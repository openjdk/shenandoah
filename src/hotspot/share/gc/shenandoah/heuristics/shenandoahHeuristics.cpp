/*
 * Copyright (c) 2018, 2020, Red Hat, Inc. All rights reserved.
 * Copyright Amazon.com Inc. or its affiliates. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "gc/shared/gcCause.hpp"
#include "gc/shenandoah/shenandoahAllocRequest.hpp"
#include "gc/shenandoah/shenandoahCollectionSet.inline.hpp"
#include "gc/shenandoah/shenandoahCollectorPolicy.hpp"
#include "gc/shenandoah/shenandoahGeneration.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahHeapRegion.inline.hpp"
#include "gc/shenandoah/shenandoahMarkingContext.inline.hpp"
#include "gc/shenandoah/shenandoahOldGeneration.hpp"
#include "gc/shenandoah/shenandoahYoungGeneration.hpp"
#include "gc/shenandoah/heuristics/shenandoahHeuristics.hpp"
#include "gc/shenandoah/mode/shenandoahMode.hpp"
#include "logging/log.hpp"
#include "logging/logTag.hpp"
#include "runtime/globals_extension.hpp"

int ShenandoahHeuristics::compare_by_garbage(RegionData a, RegionData b) {
  if (a._garbage > b._garbage)
    return -1;
  else if (a._garbage < b._garbage)
    return 1;
  else return 0;
}

ShenandoahHeuristics::ShenandoahHeuristics(ShenandoahGeneration* generation) :
  _generation(generation),
  _region_data(nullptr),
  _degenerated_cycles_in_a_row(0),
  _successful_cycles_in_a_row(0),
  _guaranteed_gc_interval(0),
  _cycle_start(os::elapsedTime()),
  _last_cycle_end(0),
  _gc_times_learned(0),
  _gc_time_penalties(0),
  _gc_cycle_time_history(new TruncatedSeq(Moving_Average_Samples, ShenandoahAdaptiveDecayFactor)),
  _metaspace_oom()
{
  // No unloading during concurrent mark? Communicate that to heuristics
  if (!ClassUnloadingWithConcurrentMark) {
    FLAG_SET_DEFAULT(ShenandoahUnloadClassesFrequency, 0);
  }

  size_t num_regions = ShenandoahHeap::heap()->num_regions();
  assert(num_regions > 0, "Sanity");

  _region_data = NEW_C_HEAP_ARRAY(RegionData, num_regions, mtGC);
}

ShenandoahHeuristics::~ShenandoahHeuristics() {
  FREE_C_HEAP_ARRAY(RegionGarbage, _region_data);
}

size_t ShenandoahHeuristics::select_aged_regions(size_t old_available, size_t num_regions, bool* preselected_regions) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  if (!heap->mode()->is_generational()) {
    // TODO: Do we need this check, or assert is enough?
    return 0;
  }

  size_t old_consumed = 0;
  for (size_t i = 0; i < num_regions; i++) {
    ShenandoahHeapRegion* region = heap->get_region(i);
    if (in_generation(region) && !region->is_empty() && region->is_regular() && (region->age() >= InitialTenuringThreshold)) {
      size_t promotion_need = (size_t) (region->get_live_data_bytes() * ShenandoahEvacWaste);
      if (old_consumed + promotion_need < old_available) {
        old_consumed += promotion_need;
        preselected_regions[i] = true;
      }
      // Note that we keep going even if one region is excluded from selection.
      // Subsequent regions may be selected if they have smaller live data.
    }
  }
  return old_consumed;
}

void ShenandoahHeuristics::choose_collection_set(ShenandoahCollectionSet* collection_set, ShenandoahOldHeuristics* old_heuristics) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  bool is_generational = heap->mode()->is_generational();

  assert(collection_set->count() == 0, "Must be empty");
  assert(!is_generational || !_generation->is_old(), "Old GC invokes ShenandoahOldHeuristics::choose_collection_set()");

  // Check all pinned regions have updated status before choosing the collection set.
  heap->assert_pinned_region_status();

  // Step 1. Build up the region candidates we care about, rejecting losers and accepting winners right away.

  size_t num_regions = heap->num_regions();

  RegionData* candidates = _region_data;

  size_t cand_idx = 0;

  size_t total_garbage = 0;

  size_t immediate_garbage = 0;
  size_t immediate_regions = 0;

  size_t free = 0;
  size_t free_regions = 0;

  for (size_t i = 0; i < num_regions; i++) {
    ShenandoahHeapRegion* region = heap->get_region(i);
    if (is_generational && !in_generation(region)) {
      continue;
    }

    size_t garbage = region->garbage();
    total_garbage += garbage;
    if (region->is_empty()) {
      free_regions++;
      free += ShenandoahHeapRegion::region_size_bytes();
    } else if (region->is_regular()) {
      if (!region->has_live()) {
        // We can recycle it right away and put it in the free set.
        immediate_regions++;
        immediate_garbage += garbage;
        region->make_trash_immediate();
      } else {
        assert(!_generation->is_old(), "OLD is handled elsewhere");
        // This is our candidate for later consideration.
        candidates[cand_idx]._region = region;
        if (is_generational && collection_set->is_preselected(i)) {
          // If region is preselected, we know mode()->is_generational() and region->age() >= InitialTenuringThreshold)
          garbage = ShenandoahHeapRegion::region_size_bytes();
        }
        candidates[cand_idx]._garbage = garbage;
        cand_idx++;
      }
    } else if (region->is_humongous_start()) {

      // Reclaim humongous regions here, and count them as the immediate garbage
#ifdef ASSERT
      bool reg_live = region->has_live();
      bool bm_live = heap->complete_marking_context()->is_marked(cast_to_oop(region->bottom()));
      assert(reg_live == bm_live,
             "Humongous liveness and marks should agree. Region live: %s; Bitmap live: %s; Region Live Words: " SIZE_FORMAT,
             BOOL_TO_STR(reg_live), BOOL_TO_STR(bm_live), region->get_live_data_words());
#endif
      if (!region->has_live()) {
        heap->trash_humongous_region_at(region);

        // Count only the start. Continuations would be counted on "trash" path
        immediate_regions++;
        immediate_garbage += garbage;
      }
    } else if (region->is_trash()) {
      // Count in just trashed collection set, during coalesced CM-with-UR
      immediate_regions++;
      immediate_garbage += garbage;
    }
  }

  // Step 2. Look back at garbage statistics, and decide if we want to collect anything,
  // given the amount of immediately reclaimable garbage. If we do, figure out the collection set.

  assert (immediate_garbage <= total_garbage,
          "Cannot have more immediate garbage than total garbage: " SIZE_FORMAT "%s vs " SIZE_FORMAT "%s",
          byte_size_in_proper_unit(immediate_garbage), proper_unit_for_byte_size(immediate_garbage),
          byte_size_in_proper_unit(total_garbage),     proper_unit_for_byte_size(total_garbage));

  size_t immediate_percent = (total_garbage == 0) ? 0 : (immediate_garbage * 100 / total_garbage);
  collection_set->set_immediate_trash(immediate_garbage);

  if (immediate_percent <= ShenandoahImmediateThreshold) {
    if (old_heuristics != nullptr) {
      old_heuristics->prime_collection_set(collection_set);
    } else {
      // This is a global collection and does not need to prime cset
    }

    // Call the subclasses to add young-gen regions into the collection set.
    choose_collection_set_from_regiondata(collection_set, candidates, cand_idx, immediate_garbage + free);
  } else {
    // We are going to skip evacuation and update refs because we reclaimed
    // sufficient amounts of immediate garbage.
    heap->shenandoah_policy()->record_abbreviated_cycle();
  }

  if (collection_set->has_old_regions()) {
    heap->shenandoah_policy()->record_mixed_cycle();
  }

  size_t cset_percent = (total_garbage == 0) ? 0 : (collection_set->garbage() * 100 / total_garbage);
  size_t collectable_garbage = collection_set->garbage() + immediate_garbage;
  size_t collectable_garbage_percent = (total_garbage == 0) ? 0 : (collectable_garbage * 100 / total_garbage);

  log_info(gc, ergo)("Collectable Garbage: " SIZE_FORMAT "%s (" SIZE_FORMAT "%%), "
                     "Immediate: " SIZE_FORMAT "%s (" SIZE_FORMAT "%%), " SIZE_FORMAT " regions, "
                     "CSet: " SIZE_FORMAT "%s (" SIZE_FORMAT "%%), " SIZE_FORMAT " regions",

                     byte_size_in_proper_unit(collectable_garbage),
                     proper_unit_for_byte_size(collectable_garbage),
                     collectable_garbage_percent,

                     byte_size_in_proper_unit(immediate_garbage),
                     proper_unit_for_byte_size(immediate_garbage),
                     immediate_percent,
                     immediate_regions,

                     byte_size_in_proper_unit(collection_set->garbage()),
                     proper_unit_for_byte_size(collection_set->garbage()),
                     cset_percent,
                     collection_set->count());

  if (collection_set->garbage() > 0) {
    size_t young_evac_bytes   = collection_set->get_young_bytes_reserved_for_evacuation();
    size_t promote_evac_bytes = collection_set->get_young_bytes_to_be_promoted();
    size_t old_evac_bytes     = collection_set->get_old_bytes_reserved_for_evacuation();
    size_t total_evac_bytes   = young_evac_bytes + promote_evac_bytes + old_evac_bytes;
    log_info(gc, ergo)("Evacuation Targets: YOUNG: " SIZE_FORMAT "%s, "
                       "PROMOTE: " SIZE_FORMAT "%s, "
                       "OLD: " SIZE_FORMAT "%s, "
                       "TOTAL: " SIZE_FORMAT "%s",
                       byte_size_in_proper_unit(young_evac_bytes),   proper_unit_for_byte_size(young_evac_bytes),
                       byte_size_in_proper_unit(promote_evac_bytes), proper_unit_for_byte_size(promote_evac_bytes),
                       byte_size_in_proper_unit(old_evac_bytes),     proper_unit_for_byte_size(old_evac_bytes),
                       byte_size_in_proper_unit(total_evac_bytes),   proper_unit_for_byte_size(total_evac_bytes));
  }
}

void ShenandoahHeuristics::record_cycle_start() {
  _cycle_start = os::elapsedTime();
}

void ShenandoahHeuristics::record_cycle_end() {
  _last_cycle_end = os::elapsedTime();
}

bool ShenandoahHeuristics::should_start_gc() {
  // Perform GC to cleanup metaspace
  if (has_metaspace_oom()) {
    // Some of vmTestbase/metaspace tests depend on following line to count GC cycles
    log_info(gc)("Trigger: %s", GCCause::to_string(GCCause::_metadata_GC_threshold));
    return true;
  }

  if (_guaranteed_gc_interval > 0) {
    double last_time_ms = (os::elapsedTime() - _last_cycle_end) * 1000;
    if (last_time_ms > _guaranteed_gc_interval) {
      log_info(gc)("Trigger (%s): Time since last GC (%.0f ms) is larger than guaranteed interval (" UINTX_FORMAT " ms)",
                   _generation->name(), last_time_ms, _guaranteed_gc_interval);
      return true;
    }
  }

  return false;
}

bool ShenandoahHeuristics::should_degenerate_cycle() {
  return _degenerated_cycles_in_a_row <= ShenandoahFullGCThreshold;
}

void ShenandoahHeuristics::adjust_penalty(intx step) {
  assert(0 <= _gc_time_penalties && _gc_time_penalties <= 100,
         "In range before adjustment: " INTX_FORMAT, _gc_time_penalties);

  intx new_val = _gc_time_penalties + step;
  if (new_val < 0) {
    new_val = 0;
  }
  if (new_val > 100) {
    new_val = 100;
  }
  _gc_time_penalties = new_val;

  assert(0 <= _gc_time_penalties && _gc_time_penalties <= 100,
         "In range after adjustment: " INTX_FORMAT, _gc_time_penalties);
}

void ShenandoahHeuristics::record_success_concurrent(bool abbreviated) {
  _degenerated_cycles_in_a_row = 0;
  _successful_cycles_in_a_row++;

  if (!(abbreviated && ShenandoahAdaptiveIgnoreShortCycles)) {
    _gc_cycle_time_history->add(elapsed_cycle_time());
    _gc_times_learned++;
  }

  adjust_penalty(Concurrent_Adjust);
}

void ShenandoahHeuristics::record_success_degenerated() {
  _degenerated_cycles_in_a_row++;
  _successful_cycles_in_a_row = 0;

  adjust_penalty(Degenerated_Penalty);
}

void ShenandoahHeuristics::record_success_full() {
  _degenerated_cycles_in_a_row = 0;
  _successful_cycles_in_a_row++;

  adjust_penalty(Full_Penalty);
}

void ShenandoahHeuristics::record_allocation_failure_gc() {
  // Do nothing.
}

void ShenandoahHeuristics::record_requested_gc() {
  // Assume users call System.gc() when external state changes significantly,
  // which forces us to re-learn the GC timings and allocation rates.
  reset_gc_learning();
}

void ShenandoahHeuristics::reset_gc_learning() {
  _gc_times_learned = 0;
}

bool ShenandoahHeuristics::can_unload_classes() {
  if (!ClassUnloading) return false;
  return true;
}

bool ShenandoahHeuristics::can_unload_classes_normal() {
  if (!can_unload_classes()) return false;
  if (has_metaspace_oom()) return true;
  if (!ClassUnloadingWithConcurrentMark) return false;
  if (ShenandoahUnloadClassesFrequency == 0) return false;
  return true;
}

bool ShenandoahHeuristics::should_unload_classes() {
  if (!can_unload_classes_normal()) return false;
  if (has_metaspace_oom()) return true;
  size_t cycle = ShenandoahHeap::heap()->shenandoah_policy()->cycle_counter();
  // Unload classes every Nth GC cycle.
  // This should not happen in the same cycle as process_references to amortize costs.
  // Offsetting by one is enough to break the rendezvous when periods are equal.
  // When periods are not equal, offsetting by one is just as good as any other guess.
  return (cycle + 1) % ShenandoahUnloadClassesFrequency == 0;
}

void ShenandoahHeuristics::initialize() {
  // Nothing to do by default.
}

double ShenandoahHeuristics::elapsed_cycle_time() const {
  return os::elapsedTime() - _cycle_start;
}

bool ShenandoahHeuristics::in_generation(ShenandoahHeapRegion* region) {
  return _generation->is_global()
          || (_generation->is_young() && region->is_young())
          || (_generation->is_old()   && region->is_old());
}

size_t ShenandoahHeuristics::min_free_threshold() {
  size_t min_free_threshold = _generation->is_old() ? ShenandoahOldMinFreeThreshold : ShenandoahMinFreeThreshold;
  return _generation->soft_max_capacity() / 100 * min_free_threshold;
}

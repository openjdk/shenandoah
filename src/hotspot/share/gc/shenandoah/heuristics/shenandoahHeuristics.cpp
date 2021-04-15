/*
 * Copyright (c) 2018, 2020, Red Hat, Inc. All rights reserved.
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
#include "gc/shenandoah/heuristics/shenandoahHeuristics.hpp"
#include "gc/shenandoah/mode/shenandoahMode.hpp"
#include "logging/log.hpp"
#include "logging/logTag.hpp"
#include "runtime/globals_extension.hpp"
#include "utilities/quickSort.hpp"

int ShenandoahHeuristics::compare_by_garbage(RegionData a, RegionData b) {
  if (a._garbage > b._garbage)
    return -1;
  else if (a._garbage < b._garbage)
    return 1;
  else return 0;
}

ShenandoahHeuristics::ShenandoahHeuristics(ShenandoahGeneration* generation,
                                           ShenandoahHeuristics* old_heuristics) :
  _generation(generation),
  _old_heuristics(old_heuristics),
  _region_data(NULL),
  _old_collection_candidates(0),
  _next_old_collection_candidate(0),
  _hidden_old_collection_candidates(0),
  _hidden_next_old_collection_candidate(0),
  _old_coalesce_and_fill_candidates(0),
  _first_coalesce_and_fill_candidate(0),
  _degenerated_cycles_in_a_row(0),
  _successful_cycles_in_a_row(0),
  _cycle_start(os::elapsedTime()),
  _last_cycle_end(0),
  _gc_times_learned(0),
  _gc_time_penalties(0),
  _gc_time_history(new TruncatedSeq(10, ShenandoahAdaptiveDecayFactor)),
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

void ShenandoahHeuristics::prime_collection_set_with_old_candidates(ShenandoahCollectionSet* collection_set) {
  uint included_old_regions = 0;
  size_t evacuated_old_bytes = 0;

  // TODO: These macro definitions represent a first approximation to desired operating parameters.
  // Eventually, these values should be determined by heuristics and should adjust dynamically based
  // on most current execution behavior.  In the interrim, we may choose to offer command-line options
  // to set the values of these configuration parameters.

  // MAX_OLD_EVACUATION_BYTES represents an "arbitrary" bound on how much evacuation effort is dedicated to
  // old-gen regions.
#define MAX_OLD_EVACUATION_BYTES (ShenandoahHeapRegion::region_size_bytes() * 8)

  // PROMOTION_BUDGET_BYTES represents an "arbitrary" bound on how many bytes can be consumed by young-gen
  // objects promoted into old-gen memory.  We need to avoid a scenario under which promotion of objects
  // depletes old-gen available memory to the point that there is insufficient memory to hold old-gen objects
  // that need to be evacuated from within the old-gen collection set.
  //
  // TODO We should probably enforce this, but there is no enforcement currently.  Key idea: if there is not
  // sufficient memory within old-gen to hold an object that wants to be promoted, defer promotion until a
  // subsequent evacuation pass.  Since enforcement may be expensive, requiring frequent synchronization
  // between mutator and GC threads, here's an alternative "greedy" mitigation strategy: Set the parameter's
  // value so overflow is "very rare".  In the case that we experience overflow, evacuate what we can from
  // within the old collection set, but don't evacuate everything.  At the end of evacuation, any collection
  // set region that was not fully evacuated cannot be recycled.  It becomes a prime candidate for the next
  // collection set selection.  Here, we'd rather fall back to this contingent behavior than force a full STW
  // collection.
#define PROMOTION_BUDGET_BYTES (ShenandoahHeapRegion::region_size_bytes() / 2)

  // If a region is put into the collection set, then this region's free (not yet used) bytes are no longer
  // "available" to hold the results of other evacuations.  This causes further decrease in the value of
  // AVAILABLE_OLD_BYTES.
  //
  // We address this by reducing the evacuation budget by the amount of live memory in that region and by the
  // amount of unallocated memory in that region if the evacuation budget is constrained by availability of
  // free memory.

  // Allow no more evacuation than exists free-space within old-gen memory
  size_t old_evacuation_budget = (_old_heuristics->_generation->available() > PROMOTION_BUDGET_BYTES)? _old_heuristics->_generation->available() - PROMOTION_BUDGET_BYTES: 0;

  // But if the amount of available free space in old-gen memory exceeds the pacing bound on how much old-gen memory can be
  // evacuated during each evacuation pass, then cut the old-gen evacuation further.  The pacing bound is designed to assure
  // that old-gen evacuations to not excessively slow the evacuation pass in order to assure that young-gen GC cadence is
  // not disrupted.

  // Represents availability of memory to hold evacuations beyond what is required to hold planned evacuations.  May go
  // negative if we choose to collect regions with large amounts of free memory.
  long long excess_free_capacity;
  if (old_evacuation_budget > MAX_OLD_EVACUATION_BYTES) {
    excess_free_capacity = old_evacuation_budget - MAX_OLD_EVACUATION_BYTES;
    old_evacuation_budget = MAX_OLD_EVACUATION_BYTES;
  } else
    excess_free_capacity = 0;

  size_t remaining_old_evacuation_budget = old_evacuation_budget;

  // The number of old-gen regions that were selected as candidates for collection at the end of the most recent old-gen
  // concurrent marking phase and have not yet been collected is represented by unprocessed_old_collection_candidates()
  while (_old_heuristics->unprocessed_old_collection_candidates() > 0) {
    // Old collection candidates are sorted in order of decreasing garbage contained therein.
    ShenandoahHeapRegion* r = _old_heuristics->next_old_collection_candidate();

    // Assuming region r is added to the collection set, what will be the remaining_old_evacuation_budget after accounting
    // for the loss of region r's free() memory.
    size_t adjusted_remaining_old_evacuation_budget;

    // If we choose region r to be collected, then we need to decrease the capacity to hold other evacuations by the size of r's free memory.
    excess_free_capacity -= r->free();
    // If subtracting r->free from excess_free_capacity() makes it go negative, that means we are going to have to decrease the
    // evacuation budget.
    if (excess_free_capacity < 0) {
      if (remaining_old_evacuation_budget < (size_t) -excess_free_capacity) {
        // By setting adjusted_remaining_old_evacuation_budget to 0, we prevent further additions to the old-gen collection set,
        // unless the region has zero live data bytes.
        adjusted_remaining_old_evacuation_budget = 0;
      } else {
        // Adding negative excess_free_capacity decreases the adjusted_remaining_old_evacuation_budget
        adjusted_remaining_old_evacuation_budget = remaining_old_evacuation_budget + excess_free_capacity;
      }
    } else {
      adjusted_remaining_old_evacuation_budget = remaining_old_evacuation_budget;
    }

    if (r->get_live_data_bytes() > adjusted_remaining_old_evacuation_budget) {
      break;
    }
    collection_set->add_region(r);
    included_old_regions++;
    evacuated_old_bytes += r->get_live_data_bytes();
    _old_heuristics->consume_old_collection_candidate();
    remaining_old_evacuation_budget = adjusted_remaining_old_evacuation_budget - r->get_live_data_bytes();
  }

  if (included_old_regions > 0) {
    log_info(gc)("Old-gen piggyback evac (%llu regions, %llu bytes)",
                 (unsigned long long) included_old_regions,
                 (unsigned long long) evacuated_old_bytes);
  }
}

void ShenandoahHeuristics::prepare_for_other_collection(ShenandoahCollectionSet* collection_set) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();

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

  ShenandoahMarkingContext* const ctx = _generation->complete_marking_context();

  for (size_t i = 0; i < num_regions; i++) {
    ShenandoahHeapRegion* region = heap->get_region(i);
    if (!in_generation(region)) {
      continue;
    }

    size_t garbage = region->garbage();
    total_garbage += garbage;

    if (region->is_empty()) {
      free_regions++;
      free += ShenandoahHeapRegion::region_size_bytes();
    } else if (region->is_regular()) {
      if (!region->has_live() && !heap->mode()->is_generational()) {
        // We can recycle it right away and put it in the free set.
        immediate_regions++;
        immediate_garbage += garbage;
        region->make_trash_immediate();
      } else {
        assert (_generation->generation_mode() != OLD, "OLD is handled elsewhere");

        // This is our candidate for later consideration.
        candidates[cand_idx]._region = region;
        candidates[cand_idx]._garbage = garbage;
        cand_idx++;
      }
    } else if (region->is_humongous_start()) {

      // Reclaim humongous regions here, and count them as the immediate garbage
#ifdef ASSERT
      bool reg_live = region->has_live();
      bool bm_live = ctx->is_marked(oop(region->bottom()));
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

  if (immediate_percent <= ShenandoahImmediateThreshold) {

    if (_old_heuristics != NULL) {
      prime_collection_set_with_old_candidates(collection_set);
    }

    // Add young-gen regions into the collection set.  This is a virtual call, implemented differently by each
    // of the heuristics subclasses.
    choose_collection_set_from_regiondata(collection_set, candidates, cand_idx, immediate_garbage + free);
  }

  size_t cset_percent = (total_garbage == 0) ? 0 : (collection_set->garbage() * 100 / total_garbage);

  size_t collectable_garbage = collection_set->garbage() + immediate_garbage;
  size_t collectable_garbage_percent = (total_garbage == 0) ? 0 : (collectable_garbage * 100 / total_garbage);
  log_info(gc, ergo)("Collectable Garbage: " SIZE_FORMAT "%s (" SIZE_FORMAT "%%), "
                     "Immediate: " SIZE_FORMAT "%s (" SIZE_FORMAT "%%), "
                     "CSet: " SIZE_FORMAT "%s (" SIZE_FORMAT "%%)",

                     byte_size_in_proper_unit(collectable_garbage),
                     proper_unit_for_byte_size(collectable_garbage),
                     collectable_garbage_percent,

                     byte_size_in_proper_unit(immediate_garbage),
                     proper_unit_for_byte_size(immediate_garbage),
                     immediate_percent,

                     byte_size_in_proper_unit(collection_set->garbage()),
                     proper_unit_for_byte_size(collection_set->garbage()),
                     cset_percent);
}

void ShenandoahHeuristics::choose_collection_set(ShenandoahCollectionSet* collection_set) {
  assert(collection_set->count() == 0, "Must be empty");

  if (_generation->generation_mode() == OLD) {
    // Old-gen doesn't actually choose a collection set to be evacuated by its own gang of worker tasks.
    // Instead, it computes the set of regions to be evacuated by subsequent young-gen evacuation passes.
    prepare_for_old_collections();
  } else {
    prepare_for_other_collection(collection_set);
  }
}

void ShenandoahHeuristics::record_cycle_start() {
  _cycle_start = os::elapsedTime();
}

void ShenandoahHeuristics::record_cycle_end() {
  _last_cycle_end = os::elapsedTime();
}

bool ShenandoahHeuristics::should_defer_gc() {
  if ((_generation->generation_mode() == OLD) && (unprocessed_old_collection_candidates() > 0)) {
    // Cannot start a new old-gen GC until previous one has finished.
    //
    // Future refinement: under certain circumstances, we might be more
    // sophisticated about this choice.  But if we choose to abandon
    // previous old collection before it has completed evacuations,
    // we would need to coalesce and fill all garbage within
    // unevacuated collection-set regions.
    return true;
  }
  return false;
}

bool ShenandoahHeuristics::should_start_gc() {

  if (should_defer_gc())
    return false;

  // Perform GC to cleanup metaspace
  if (has_metaspace_oom()) {
    // Some of vmTestbase/metaspace tests depend on following line to count GC cycles
    log_info(gc)("Trigger: %s", GCCause::to_string(GCCause::_metadata_GC_threshold));
    return true;
  }

  if (ShenandoahGuaranteedGCInterval > 0) {
    double last_time_ms = (os::elapsedTime() - _last_cycle_end) * 1000;
    if (last_time_ms > ShenandoahGuaranteedGCInterval) {
      log_info(gc)("Trigger (%s): Time since last GC (%.0f ms) is larger than guaranteed interval (" UINTX_FORMAT " ms)",
                   _generation->name(), last_time_ms, ShenandoahGuaranteedGCInterval);
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

void ShenandoahHeuristics::record_success_concurrent() {
  _degenerated_cycles_in_a_row = 0;
  _successful_cycles_in_a_row++;

  _gc_time_history->add(time_since_last_gc());
  _gc_times_learned++;

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

double ShenandoahHeuristics::time_since_last_gc() const {
  return os::elapsedTime() - _cycle_start;
}

bool ShenandoahHeuristics::in_generation(ShenandoahHeapRegion* region) {
  return (_generation->generation_mode() == GLOBAL)
    || (_generation->generation_mode() == YOUNG && region->affiliation() == YOUNG_GENERATION)
    || (_generation->generation_mode() == OLD && region->affiliation() == OLD_GENERATION);
}

void ShenandoahHeuristics::prepare_for_old_collections() {
  assert(_generation->generation_mode() == OLD, "This service only available for old-gc heuristics");
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  uint free_regions = 0;
  size_t cand_idx = 0;
  size_t total_garbage = 0;
  size_t num_regions = heap->num_regions();

  RegionData* candidates = _region_data;
  for (size_t i = 0; i < num_regions; i++) {
    ShenandoahHeapRegion* region = heap->get_region(i);
    if (!in_generation(region))
      continue;
    else {
      size_t garbage = region->garbage();
      total_garbage += garbage;

      candidates[cand_idx]._region = region;
      candidates[cand_idx]._garbage = garbage;
      cand_idx++;
    }
  }

  // Give special treatment to humongous regions.  Assume humongous regions is entirely
  // garbage or entirely non-garbage.  Assume that a head humongous region and the associated
  // humongous continuous regions are uniformly entirely garbage or entirely non-garbage.
  //
  // Sift garbage humongous regions to front, non-garbage humongous regions to end of array.
  size_t first_non_humongous_empty = 0;
  size_t first_humongous_non_empty = cand_idx;

  // This loop is written as while rather than for because of
  // suspected gcc error in translating/optimizing for-loop
  size_t i = 0;
  while (i < first_humongous_non_empty) {
    ShenandoahHeapRegion* region = candidates[i]._region;
    if (region->is_humongous()) {
      if (region->get_live_data_bytes() == 0) {
        // Humongous region is entirely garbage.  Reclaim it.
        if (i == first_non_humongous_empty) {
          first_non_humongous_empty++;
        } else {
          RegionData swap_tmp = candidates[i];
          candidates[i] = candidates[first_non_humongous_empty];
          candidates[first_non_humongous_empty++] = swap_tmp;
        }
        i++;
      } else {
        // Humongous region is non garbage.  Don't reclaim it.
        if (i + 1 == first_humongous_non_empty) {
          first_humongous_non_empty--;
          i++;
        } else {
          RegionData swap_tmp = candidates[i];
          candidates[i] = candidates[--first_humongous_non_empty];
          candidates[first_humongous_non_empty] = swap_tmp;
          // Do not increment i so we can revisit swapped entry on next iteration
        }
      }
    } else {
      i++;
    }
  }


  // Prioritize regions to select garbage-first regions
  QuickSort::sort<RegionData>(candidates + first_non_humongous_empty, (int)(first_humongous_non_empty - first_non_humongous_empty),
                              compare_by_garbage, false);

  // Any old-gen region that contains 50% garbage or more is to be
  // evacuated.  In the future, this threshold percentage may be specified on
  // the command line or preferrably determined by dynamic heuristics.
#define CollectionThresholdGarbagePercent 50

  size_t region_size = ShenandoahHeapRegion::region_size_bytes();
  for (size_t i = first_non_humongous_empty; i < first_humongous_non_empty; i++) {
    // Do approximate percent to avoid floating point math
    size_t percent_garbage = candidates[i]._garbage * 100 / region_size;

    if (percent_garbage < CollectionThresholdGarbagePercent) {
      _hidden_next_old_collection_candidate = 0;
      _hidden_old_collection_candidates = i;
      _first_coalesce_and_fill_candidate = i;
      _old_coalesce_and_fill_candidates = cand_idx - i;

      // Note that we do not coalesce and fill occupied humongous regions
      // HR: humongous regions, RR: regular regions, CF: coalesce and fill regions
      log_info(gc)("Old-gen mark evac (%u HR, %llu RR), %llu CF)",
                   (unsigned int) first_non_humongous_empty,
                   (unsigned long long) (_hidden_old_collection_candidates - first_non_humongous_empty),
                   (unsigned long long) _old_coalesce_and_fill_candidates);
      return;
    }
  }

  // If we reach here, all of non-humogous old-gen regions are candidates for collection set.
  _hidden_next_old_collection_candidate = 0;
  _hidden_old_collection_candidates = first_humongous_non_empty;
  _first_coalesce_and_fill_candidate = 0;
  _old_coalesce_and_fill_candidates = 0;

#undef CollectionThresholdGarbagePercent

  // Note that we do not coalesce and fill occupied humongous regions
  // HR: humongous regions, RR: regular regions, CF: coalesce and fill regions
  log_info(gc)("Old-gen mark evac (%u HR, %llu RR), %llu CF)",
               (unsigned int) first_non_humongous_empty,
               (unsigned long long) (_hidden_old_collection_candidates - first_non_humongous_empty),
               (unsigned long long) _old_coalesce_and_fill_candidates);
}

void ShenandoahHeuristics::start_old_evacuations() {
  assert(_generation->generation_mode() == OLD, "This service only available for old-gc heuristics");

  _old_collection_candidates = _hidden_old_collection_candidates;
  _next_old_collection_candidate = _hidden_next_old_collection_candidate;

  _hidden_old_collection_candidates = 0;}


uint ShenandoahHeuristics::unprocessed_old_collection_candidates() {
  assert(_generation->generation_mode() == OLD, "This service only available for old-gc heuristics");
  return _old_collection_candidates + _hidden_old_collection_candidates;
}

ShenandoahHeapRegion* ShenandoahHeuristics::next_old_collection_candidate() {
  assert(_generation->generation_mode() == OLD, "This service only available for old-gc heuristics");
  return _region_data[_next_old_collection_candidate]._region;
}

void ShenandoahHeuristics::consume_old_collection_candidate() {
  assert(_generation->generation_mode() == OLD, "This service only available for old-gc heuristics");
  _next_old_collection_candidate++;
  _old_collection_candidates--;
}

uint ShenandoahHeuristics::old_coalesce_and_fill_candidates() {
  assert(_generation->generation_mode() == OLD, "This service only available for old-gc heuristics");
  return _old_coalesce_and_fill_candidates;
}

void ShenandoahHeuristics::get_coalesce_and_fill_candidates(ShenandoahHeapRegion** buffer) {
  assert(_generation->generation_mode() == OLD, "This service only available for old-gc heuristics");
  uint count = _old_coalesce_and_fill_candidates;
  int index = _first_coalesce_and_fill_candidate;
  while (count-- > 0)
    *buffer++ = _region_data[index++]._region;
}

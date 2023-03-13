/*
 * Copyright (c) 2021, Amazon.com, Inc. or its affiliates. All rights reserved.
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

#include "gc/shenandoah/heuristics/shenandoahOldHeuristics.hpp"
#include "gc/shenandoah/shenandoahCollectionSet.hpp"
#include "gc/shenandoah/shenandoahHeap.hpp"
#include "gc/shenandoah/shenandoahHeapRegion.inline.hpp"
#include "gc/shenandoah/shenandoahOldGeneration.hpp"
#include "gc/shenandoah/shenandoahYoungGeneration.hpp"
#include "utilities/quickSort.hpp"

uint ShenandoahOldHeuristics::NOT_FOUND = -1U;

ShenandoahOldHeuristics::ShenandoahOldHeuristics(ShenandoahOldGeneration* generation, ShenandoahHeuristics* trigger_heuristic) :
  ShenandoahHeuristics(generation),
#ifdef ASSERT
  _start_candidate(0),
#endif
  _first_pinned_candidate(NOT_FOUND),
  _last_old_collection_candidate(0),
  _next_old_collection_candidate(0),
  _last_old_region(0),
  _trigger_heuristic(trigger_heuristic),
  _old_generation(generation),
  _promotion_failed(false),
  _cannot_expand_trigger(false),
  _fragmentation_trigger(false),
  _growth_trigger(false)
{
  assert(_generation->generation_mode() == OLD, "This service only available for old-gc heuristics");
}

bool ShenandoahOldHeuristics::prime_collection_set(ShenandoahCollectionSet* collection_set) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
#undef KELVIN_PRIME
#ifdef KELVIN_PRIME
  log_info(gc, ergo)("prime_collection_set(), old_candidates: %u, old_collection:enabled: yes",
                     unprocessed_old_collection_candidates());
#endif
  if (unprocessed_old_collection_candidates() == 0) {
    return false;
  }

  _first_pinned_candidate = NOT_FOUND;

  uint included_old_regions = 0;
  size_t evacuated_old_bytes = 0;
  size_t collected_old_bytes = 0;

  // If a region is put into the collection set, then this region's free (not yet used) bytes are no longer
  // "available" to hold the results of other evacuations.  This may cause a decrease in the remaining amount
  // of memory that can still be evacuated.  We address this by reducing the evacuation budget by the amount
  // of live memory in that region and by the amount of unallocated memory in that region if the evacuation
  // budget is constrained by availability of free memory.
  size_t old_evacuation_budget = (size_t) ((double) heap->get_old_evac_reserve() / ShenandoahOldEvacWaste);
  size_t remaining_old_evacuation_budget = old_evacuation_budget;
  size_t lost_evacuation_capacity = 0;
  log_info(gc)("Choose old regions for mixed collection: old evacuation budget: " SIZE_FORMAT "%s, candidates: %u",
               byte_size_in_proper_unit(old_evacuation_budget), proper_unit_for_byte_size(old_evacuation_budget),
               unprocessed_old_collection_candidates());

  // The number of old-gen regions that were selected as candidates for collection at the end of the most recent old-gen
  // concurrent marking phase and have not yet been collected is represented by unprocessed_old_collection_candidates()
  while (unprocessed_old_collection_candidates() > 0) {
    // Old collection candidates are sorted in order of decreasing garbage contained therein.
    ShenandoahHeapRegion* r = next_old_collection_candidate();
    if (r == nullptr) {
      break;
    }

    // If we choose region r to be collected, then we need to decrease the capacity to hold other evacuations by
    // the size of r's free memory.

    // It's probably overkill to compensate with lost_evacuation_capacity.  But it's the safe thing to do and
    //  has minimal impact on content of primed collection set.
    if (r->get_live_data_bytes() + lost_evacuation_capacity <= remaining_old_evacuation_budget) {
      // Decrement remaining evacuation budget by bytes that will be copied.
      lost_evacuation_capacity += r->free();
      remaining_old_evacuation_budget -= r->get_live_data_bytes();
      collection_set->add_region(r);
      included_old_regions++;
      evacuated_old_bytes += r->get_live_data_bytes();
      collected_old_bytes += r->garbage();
      consume_old_collection_candidate();
    } else {
      break;
    }
  }

  if (_first_pinned_candidate != NOT_FOUND) {
    // Need to deal with pinned regions
    slide_pinned_regions_to_front();
  }

  if (included_old_regions > 0) {
    log_info(gc)("Old-gen piggyback evac (" UINT32_FORMAT " regions, evacuating " SIZE_FORMAT "%s, reclaiming: " SIZE_FORMAT "%s)",
                 included_old_regions,
                 byte_size_in_proper_unit(evacuated_old_bytes), proper_unit_for_byte_size(evacuated_old_bytes),
                 byte_size_in_proper_unit(collected_old_bytes), proper_unit_for_byte_size(collected_old_bytes));
  }

  if (unprocessed_old_collection_candidates() == 0) {
    // Any triggers that occurred during mixed evacuations may no longer be valid.  They can retrigger if appropriate.
    clear_triggers();
    _old_generation->transition_to(ShenandoahOldGeneration::IDLE);
  }

  return (included_old_regions > 0);
}

void ShenandoahOldHeuristics::slide_pinned_regions_to_front() {
  // Find the leftmost unpinned region. The region in this slot will have been
  // added to the cset, so we can use it to hold pointers to regions that were
  // pinned when the cset was chosen.
  // [ r p r p p p r ]
  //          ^
  //          | first r to the left should be in the collection set now.
  uint write_index = NOT_FOUND;
  for (uint search = _next_old_collection_candidate - 1; search > _first_pinned_candidate; --search) {
    ShenandoahHeapRegion* region = _region_data[search]._region;
    if (!region->is_pinned()) {
      write_index = search;
      assert(region->is_cset(), "Expected unpinned region to be added to the collection set.");
      break;
    }
  }

  if (write_index == NOT_FOUND) {
    if (_first_pinned_candidate > 0) {
      _next_old_collection_candidate = _first_pinned_candidate;
    }
    return;
  }

  // Find pinned regions to the left and move their pointer into a slot
  // that was pointing at a region that has been added to the cset.
  // [ r p r p p p r ]
  //       ^
  //       | Write pointer is here. We know this region is already in the cset
  //       | so we can clobber it with the next pinned region we find.
  for (size_t search = write_index - 1; search > _first_pinned_candidate; --search) {
    RegionData& skipped = _region_data[search];
    if (skipped._region->is_pinned()) {
      RegionData& added_to_cset = _region_data[write_index];
      assert(added_to_cset._region->is_cset(), "Can only overwrite slots used by regions added to the collection set.");
      added_to_cset._region = skipped._region;
      added_to_cset._u._live_data = skipped._u._live_data;
      --write_index;
    }
  }

  // Everything left should already be in the cset
  // [ r x p p p p r ]
  //       ^
  //       | next pointer points at the first region which was not added
  //       | to the collection set.
#ifdef ASSERT
  for (size_t check = write_index - 1; check > _start_candidate; --check) {
    ShenandoahHeapRegion* region = _region_data[check]._region;
    assert(region->is_cset(), "All regions here should be in the collection set.");
  }
  _start_candidate = write_index;
#endif

  // Update to read from the leftmost pinned region.
  _next_old_collection_candidate = write_index;
}

// Both arguments are don't cares for old-gen collections
void ShenandoahOldHeuristics::choose_collection_set(ShenandoahCollectionSet* collection_set,
                                                    ShenandoahOldHeuristics* old_heuristics) {
  assert((collection_set == nullptr) && (old_heuristics == nullptr),
         "Expect null arguments in ShenandoahOldHeuristics::choose_collection_set()");
  // Old-gen doesn't actually choose a collection set to be evacuated by its own gang of worker tasks.
  // Instead, it computes the set of regions to be evacuated by subsequent young-gen evacuation passes.
  prepare_for_old_collections();
}

void ShenandoahOldHeuristics::prepare_for_old_collections() {
  assert(_generation->generation_mode() == OLD, "This service only available for old-gc heuristics");
  ShenandoahHeap* heap = ShenandoahHeap::heap();

  size_t cand_idx = 0;
  size_t total_garbage = 0;
  size_t num_regions = heap->num_regions();
  size_t immediate_garbage = 0;
  size_t immediate_regions = 0;
  size_t live_data = 0;

  RegionData* candidates = _region_data;
  for (size_t i = 0; i < num_regions; i++) {
    ShenandoahHeapRegion* region = heap->get_region(i);
    if (!in_generation(region)) {
      continue;
    }

    size_t garbage = region->garbage();
    size_t live_bytes = region->get_live_data_bytes();
    total_garbage += garbage;
    live_data += live_bytes;

    if (region->is_regular() || region->is_pinned()) {
      if (!region->has_live()) {
        assert(!region->is_pinned(), "Pinned region should have live (pinned) objects.");
        region->make_trash_immediate();
        immediate_regions++;
        immediate_garbage += garbage;
      } else {
        region->begin_preemptible_coalesce_and_fill();
        candidates[cand_idx]._region = region;
        candidates[cand_idx]._u._live_data = live_bytes;
        cand_idx++;
      }
    } else if (region->is_humongous_start()) {
      if (!region->has_live()) {
        // The humongous object is dead, we can just return this region and the continuations
        // immediately to the freeset - no evacuations are necessary here. The continuations
        // will be made into trash by this method, so they'll be skipped by the 'is_regular'
        // check above, but we still need to count the start region.
        immediate_regions++;
        immediate_garbage += garbage;
        size_t region_count = heap->trash_humongous_region_at(region);
        log_debug(gc)("Trashed " SIZE_FORMAT " regions for humongous object.", region_count);
      }
    } else if (region->is_trash()) {
      // Count humongous objects made into trash here.
      immediate_regions++;
      immediate_garbage += garbage;
    }
  }

  ((ShenandoahOldGeneration*) (heap->old_generation()))->set_live_bytes_after_last_mark(live_data);

  // TODO: Consider not running mixed collects if we recovered some threshold percentage of memory from immediate garbage.
  // This would be similar to young and global collections shortcutting evacuation, though we'd probably want a separate
  // threshold for the old generation.

  // Unlike young, we are more interested in efficiently packing OLD-gen than in reclaiming garbage first.  We set by live-data.
  // Note that regular regions may be promoted in place with no garbage but also with very little live data.  When we "compact"
  // old-gen, we want to pack these underutilized regions together so we can have more unaffiliated (unfragmented) free regions
  // in old-gen.
  QuickSort::sort<RegionData>(candidates, cand_idx, compare_by_live, false);

  // Any old-gen region that contains (ShenandoahOldGarbageThreshold (default value 25))% garbage or more is to
  // be evacuated.
  //
  // TODO: allow ShenandoahOldGarbageThreshold to be determined adaptively, by heuristics.

  // The convention is to collect regions that have more than this amount of garbage.
  const size_t garbage_threshold = ShenandoahHeapRegion::region_size_bytes() * ShenandoahOldGarbageThreshold / 100;

  // Englightened interpretation: collect regions that have less than this amount of live.
  const size_t live_threshold = ShenandoahHeapRegion::region_size_bytes() - garbage_threshold;

  size_t candidates_garbage = 0;
  _last_old_region = (uint)cand_idx;
  _last_old_collection_candidate = (uint)cand_idx;
  _next_old_collection_candidate = 0;

  size_t unfragmented = 0;

  for (size_t i = 0; i < cand_idx; i++) {
    size_t region_garbage = candidates[i]._region->garbage();
    size_t unused = ShenandoahHeapRegion::region_size_bytes() - candidates[i]._u._live_data;
    if (unused < garbage_threshold) {
      // Candidates are sorted in increasing order of live data, so no regions after this will be below the threshold.
      _last_old_collection_candidate = (uint)i;
      break;
    }
    candidates_garbage += region_garbage;
    unfragmented += unused;
  }

  // Note that we do not coalesce and fill occupied humongous regions
  // HR: humongous regions, RR: regular regions, CF: coalesce and fill regions
  size_t collectable_garbage = immediate_garbage + candidates_garbage;
  log_info(gc)("Old-Gen Collectable Garbage: " SIZE_FORMAT "%s consolidated with free: "
               SIZE_FORMAT "%s, over " UINT32_FORMAT " regions, "
               "Old-Gen Immediate Garbage: " SIZE_FORMAT "%s over " SIZE_FORMAT " regions.",
               byte_size_in_proper_unit(collectable_garbage), proper_unit_for_byte_size(collectable_garbage),
               byte_size_in_proper_unit(unfragmented), proper_unit_for_byte_size(unfragmented), _last_old_collection_candidate,
               byte_size_in_proper_unit(immediate_garbage), proper_unit_for_byte_size(immediate_garbage), immediate_regions);

  if (unprocessed_old_collection_candidates() == 0) {
    _old_generation->transition_to(ShenandoahOldGeneration::IDLE);
  } else {
    _old_generation->transition_to(ShenandoahOldGeneration::WAITING);
  }
}

uint ShenandoahOldHeuristics::last_old_collection_candidate_index() {
  return _last_old_collection_candidate;
}

uint ShenandoahOldHeuristics::unprocessed_old_collection_candidates() {
  return _last_old_collection_candidate - _next_old_collection_candidate;
}

ShenandoahHeapRegion* ShenandoahOldHeuristics::next_old_collection_candidate() {
  while (_next_old_collection_candidate < _last_old_collection_candidate) {
    ShenandoahHeapRegion* next = _region_data[_next_old_collection_candidate]._region;
    if (!next->is_pinned()) {
      return next;
    } else {
      assert(next->is_pinned(), "sanity");
      if (_first_pinned_candidate == NOT_FOUND) {
        _first_pinned_candidate = _next_old_collection_candidate;
      }
    }

    _next_old_collection_candidate++;
  }
  return nullptr;
}

void ShenandoahOldHeuristics::consume_old_collection_candidate() {
  _next_old_collection_candidate++;
}

uint ShenandoahOldHeuristics::last_old_region_index() const {
  return _last_old_region;
}

unsigned int ShenandoahOldHeuristics::get_coalesce_and_fill_candidates(ShenandoahHeapRegion** buffer) {
  uint end = _last_old_region;
  uint index = _next_old_collection_candidate;
  while (index < end) {
#undef KELVIN_PIP
#ifdef KELVIN_PIP
    log_info(gc, ergo)("get_coalesce_and_fill_candidates includes uncollected region " SIZE_FORMAT, _region_data[index]._region->index());
#endif
    *buffer++ = _region_data[index++]._region;
  }
  return (_last_old_region - _next_old_collection_candidate);
}

void ShenandoahOldHeuristics::abandon_collection_candidates() {
  _last_old_collection_candidate = 0;
  _next_old_collection_candidate = 0;
  _last_old_region = 0;
}

void ShenandoahOldHeuristics::handle_promotion_failure() {
#ifdef KELVIN_DEPRECATE
  if (!_promotion_failed) {
    if (ShenandoahHeap::heap()->generation_sizer()->transfer_capacity(_old_generation)) {
      log_info(gc)("Increased size of old generation due to promotion failure.");
    }
    // TODO: Increase tenuring threshold to push back on promotions.
  }
#endif
  _promotion_failed = true;
}

void ShenandoahOldHeuristics::record_cycle_start() {
  _trigger_heuristic->record_cycle_start();
}

void ShenandoahOldHeuristics::record_cycle_end() {
  _trigger_heuristic->record_cycle_end();
  // Clear triggers that might have been set during OLD marking.  Conditions are different now that this phase has finished.
  _promotion_failed = false;
  _cannot_expand_trigger = false;
  _fragmentation_trigger = false;
  _growth_trigger = false;
}

void ShenandoahOldHeuristics::clear_triggers() {
  // Clear any triggers that were set during mixed evacuations.  Conditions are different now that this phase has finished.
  _promotion_failed = false;
  _cannot_expand_trigger = false;
  _fragmentation_trigger = false;
  _growth_trigger = false;
 }

bool ShenandoahOldHeuristics::should_start_gc() {
  // Cannot start a new old-gen GC until previous one has finished.
  //
  // Future refinement: under certain circumstances, we might be more sophisticated about this choice.
  // For example, we could choose to abandon the previous old collection before it has completed evacuations.
  if (unprocessed_old_collection_candidates() > 0) {
    return false;
  }

#ifdef KELVIN_DEPRECATE
  // If there's been a promotion failure (and we don't have regions already scheduled for evacuation),
  // start a new old generation collection.
  if (_promotion_failed) {
    log_info(gc)("Trigger (OLD): Promotion Failure");
    return true;
  }
#endif

  if (_cannot_expand_trigger) {
    ShenandoahHeap* heap = ShenandoahHeap::heap();
    ShenandoahOldGeneration* old_gen = heap->old_generation();
    size_t old_gen_capacity = old_gen->max_capacity();
    size_t heap_capacity = heap->capacity();
    double percent = 100.0 * ((double) old_gen_capacity) / heap_capacity;
    log_info(gc)("Trigger (OLD): Expansion failure, current size: " SIZE_FORMAT "%s which is %.1f%% of total heap size",
                 byte_size_in_proper_unit(old_gen_capacity), proper_unit_for_byte_size(old_gen_capacity), percent);
    return true;
  }

  if (_fragmentation_trigger) {
    ShenandoahHeap* heap = ShenandoahHeap::heap();
    ShenandoahOldGeneration* old_gen = heap->old_generation();
    size_t used = old_gen->used();
    size_t used_regions_size = old_gen->used_regions_size();
    size_t used_regions = old_gen->used_regions();
    assert(used_regions_size > used_regions, "Cannot have more used than used regions");
    size_t fragmented_free = used_regions_size - used;
    double percent = 100.0 * ((double) fragmented_free) / used_regions_size;
    log_info(gc)("Trigger (OLD): Old has become fragmented: "
                 SIZE_FORMAT "%s available bytes spread between " SIZE_FORMAT " regions (%.1f%% free)",
                 byte_size_in_proper_unit(fragmented_free), proper_unit_for_byte_size(fragmented_free), used_regions, percent);
    return true;
  }

  if (_growth_trigger) {
    ShenandoahHeap* heap = ShenandoahHeap::heap();
    ShenandoahOldGeneration* old_gen = heap->old_generation();
    size_t current_usage = old_gen->used();
    size_t live_at_previous_old = old_gen->get_live_bytes_after_last_mark();
    double percent_growth = 100.0 * ((double) current_usage - live_at_previous_old) / live_at_previous_old;
    log_info(gc)("Trigger (OLD): Old has overgrown, live at end of previous OLD marking: "
                 SIZE_FORMAT "%s, current usage: " SIZE_FORMAT "%s, percent growth: %.1f%%",
                 byte_size_in_proper_unit(live_at_previous_old), proper_unit_for_byte_size(live_at_previous_old),
                 byte_size_in_proper_unit(current_usage), proper_unit_for_byte_size(current_usage), percent_growth);
    return true;
  }

  // Otherwise, defer to configured heuristic for gc trigger.
  return _trigger_heuristic->should_start_gc();
}

bool ShenandoahOldHeuristics::should_degenerate_cycle() {
  return _trigger_heuristic->should_degenerate_cycle();
}

void ShenandoahOldHeuristics::record_success_concurrent(bool abbreviated) {
  // Forget any triggers that occured while OLD GC was ongoing.  If we really need to start another, it will retrigger.
  _promotion_failed = false;
  _cannot_expand_trigger = false;
  _fragmentation_trigger = false;
  _growth_trigger = false;
  _trigger_heuristic->record_success_concurrent(abbreviated);
}

void ShenandoahOldHeuristics::record_success_degenerated() {
  // Forget any triggers that occured while OLD GC was ongoing.  If we really need to start another, it will retrigger.
  _promotion_failed = false;
  _cannot_expand_trigger = false;
  _fragmentation_trigger = false;
  _growth_trigger = false;
  _trigger_heuristic->record_success_degenerated();
}

void ShenandoahOldHeuristics::record_success_full() {
  // Forget any triggers that occured while OLD GC was ongoing.  If we really need to start another, it will retrigger.
  _promotion_failed = false;
  _cannot_expand_trigger = false;
  _fragmentation_trigger = false;
  _growth_trigger = false;
  _trigger_heuristic->record_success_full();
}

void ShenandoahOldHeuristics::record_allocation_failure_gc() {
  _trigger_heuristic->record_allocation_failure_gc();
}

void ShenandoahOldHeuristics::record_requested_gc() {
  _trigger_heuristic->record_requested_gc();
}

void ShenandoahOldHeuristics::reset_gc_learning() {
  _trigger_heuristic->reset_gc_learning();
}

bool ShenandoahOldHeuristics::can_unload_classes() {
  return _trigger_heuristic->can_unload_classes();
}

bool ShenandoahOldHeuristics::can_unload_classes_normal() {
  return _trigger_heuristic->can_unload_classes_normal();
}

bool ShenandoahOldHeuristics::should_unload_classes() {
  return _trigger_heuristic->should_unload_classes();
}

const char* ShenandoahOldHeuristics::name() {
  static char name[128];
  jio_snprintf(name, sizeof(name), "%s (OLD)", _trigger_heuristic->name());
  return name;
}

bool ShenandoahOldHeuristics::is_diagnostic() {
  return false;
}

bool ShenandoahOldHeuristics::is_experimental() {
  return true;
}

void ShenandoahOldHeuristics::choose_collection_set_from_regiondata(ShenandoahCollectionSet* set,
                                                                    ShenandoahHeuristics::RegionData* data,
                                                                    size_t data_size, size_t free) {
  ShouldNotReachHere();
}



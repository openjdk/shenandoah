/*
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

#include "gc/shenandoah/heuristics/shenandoahOldHeuristics.hpp"
#include "gc/shenandoah/heuristics/shenandoahYoungHeuristics.hpp"
#include "gc/shenandoah/shenandoahCollectorPolicy.hpp"
#include "gc/shenandoah/shenandoahGeneration.hpp"
#include "gc/shenandoah/shenandoahYoungGeneration.hpp"

#include "utilities/quickSort.hpp"

ShenandoahYoungHeuristics::ShenandoahYoungHeuristics(ShenandoahGeneration* generation)
        : ShenandoahAdaptiveHeuristics(generation) {
  assert(!_generation->is_old(), "Old GC invokes ShenandoahOldHeuristics::choose_collection_set()");
}

void ShenandoahYoungHeuristics::choose_collection_set(ShenandoahCollectionSet* collection_set, ShenandoahOldHeuristics* old_heuristics) {
  assert(collection_set->is_empty(), "Must be empty");

  ShenandoahHeap* heap = ShenandoahHeap::heap();
  size_t region_size_bytes = ShenandoahHeapRegion::region_size_bytes();


  // Check all pinned regions have updated status before choosing the collection set.
  heap->assert_pinned_region_status();

  // Step 1. Build up the region candidates we care about, rejecting losers and accepting winners right away.

  size_t num_regions = heap->num_regions();

  RegionData* candidates = _region_data;

  size_t cand_idx = 0;
  size_t preselected_candidates = 0;

  size_t total_garbage = 0;

  size_t immediate_garbage = 0;
  size_t immediate_regions = 0;

  size_t free = 0;
  size_t free_regions = 0;

  size_t old_garbage_threshold = (region_size_bytes * ShenandoahOldGarbageThreshold) / 100;
  // This counts number of humongous regions that we intend to promote in this cycle.
  size_t humongous_regions_promoted = 0;
  // This counts bytes of memory used by hunongous regions to be promoted in place.
  size_t humongous_bytes_promoted = 0;
  // This counts number of regular regions that will be promoted in place.
  size_t regular_regions_promoted_in_place = 0;
  // This counts bytes of memory used by regular regions to be promoted in place.
  size_t regular_regions_promoted_usage = 0;

  for (size_t i = 0; i < num_regions; i++) {
    ShenandoahHeapRegion* region = heap->get_region(i);
    if (!_generation->contains(region)) {
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
        bool is_candidate;
        // This is our candidate for later consideration.
        if (collection_set->is_preselected(i)) {
          // If !is_generational, we cannot ask if is_preselected.  If is_preselected, we know
          //   region->age() >= InitialTenuringThreshold).
          is_candidate = true;
          preselected_candidates++;
          // Set garbage value to maximum value to force this into the sorted collection set.
          garbage = region_size_bytes;
        } else if (region->is_young() && (region->age() >= InitialTenuringThreshold)) {
          // Note that for GLOBAL GC, region may be OLD, and OLD regions do not qualify for pre-selection

          // This region is old enough to be promoted but it was not preselected, either because its garbage is below
          // ShenandoahOldGarbageThreshold so it will be promoted in place, or because there is not sufficient room
          // in old gen to hold the evacuated copies of this region's live data.  In both cases, we choose not to
          // place this region into the collection set.
          if (region->get_top_before_promote() != nullptr) {
            regular_regions_promoted_in_place++;
            regular_regions_promoted_usage += region->used_before_promote();
          }
          is_candidate = false;
        } else {
          is_candidate = true;
        }
        if (is_candidate) {
          candidates[cand_idx]._region = region;
          candidates[cand_idx]._u._garbage = garbage;
          cand_idx++;
        }
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
      } else {
        if (region->is_young() && region->age() >= InitialTenuringThreshold) {
          oop obj = cast_to_oop(region->bottom());
          size_t humongous_regions = ShenandoahHeapRegion::required_regions(obj->size() * HeapWordSize);
          humongous_regions_promoted += humongous_regions;
          humongous_bytes_promoted += obj->size() * HeapWordSize;
        }
      }
    } else if (region->is_trash()) {
      // Count in just trashed collection set, during coalesced CM-with-UR
      immediate_regions++;
      immediate_garbage += garbage;
    }
  }
  heap->reserve_promotable_humongous_regions(humongous_regions_promoted);
  heap->reserve_promotable_humongous_usage(humongous_bytes_promoted);
  heap->reserve_promotable_regular_regions(regular_regions_promoted_in_place);
  heap->reserve_promotable_regular_usage(regular_regions_promoted_usage);
  log_info(gc, ergo)("Planning to promote in place " SIZE_FORMAT " humongous regions and " SIZE_FORMAT
                     " regular regions, spanning a total of " SIZE_FORMAT " used bytes",
                     humongous_regions_promoted, regular_regions_promoted_in_place,
                     humongous_regions_promoted * ShenandoahHeapRegion::region_size_bytes() + regular_regions_promoted_usage);

  // Step 2. Look back at garbage statistics, and decide if we want to collect anything,
  // given the amount of immediately reclaimable garbage. If we do, figure out the collection set.

  assert (immediate_garbage <= total_garbage,
          "Cannot have more immediate garbage than total garbage: " SIZE_FORMAT "%s vs " SIZE_FORMAT "%s",
          byte_size_in_proper_unit(immediate_garbage), proper_unit_for_byte_size(immediate_garbage),
          byte_size_in_proper_unit(total_garbage),     proper_unit_for_byte_size(total_garbage));

  size_t immediate_percent = (total_garbage == 0) ? 0 : (immediate_garbage * 100 / total_garbage);
  collection_set->set_immediate_trash(immediate_garbage);

  bool doing_promote_in_place = (humongous_regions_promoted + regular_regions_promoted_in_place > 0);
  if (doing_promote_in_place || (preselected_candidates > 0) || (immediate_percent <= ShenandoahImmediateThreshold)) {
    if (old_heuristics != nullptr) {
      old_heuristics->prime_collection_set(collection_set);
    } else {
      // This is a global collection and does not need to prime cset
      assert(_generation->is_global(), "Expected global collection here");
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

void ShenandoahYoungHeuristics::choose_collection_set_from_regiondata(ShenandoahCollectionSet* cset,
                                                                      RegionData* data, size_t size,
                                                                      size_t actual_free) {
  size_t garbage_threshold = ShenandoahHeapRegion::region_size_bytes() * ShenandoahGarbageThreshold / 100;
  size_t ignore_threshold = ShenandoahHeapRegion::region_size_bytes() * ShenandoahIgnoreGarbageThreshold / 100;
  ShenandoahHeap* heap = ShenandoahHeap::heap();

  // The logic for cset selection in adaptive is as follows:
  //
  //   1. We cannot get cset larger than available free space. Otherwise we guarantee OOME
  //      during evacuation, and thus guarantee full GC. In practice, we also want to let
  //      application to allocate something. This is why we limit CSet to some fraction of
  //      available space. In non-overloaded heap, max_cset would contain all plausible candidates
  //      over garbage threshold.
  //
  //   2. We should not get cset too low so that free threshold would not be met right
  //      after the cycle. Otherwise we get back-to-back cycles for no reason if heap is
  //      too fragmented. In non-overloaded non-fragmented heap min_garbage would be around zero.
  //
  // Therefore, we start by sorting the regions by garbage. Then we unconditionally add the best candidates
  // before we meet min_garbage. Then we add all candidates that fit with a garbage threshold before
  // we hit max_cset. When max_cset is hit, we terminate the cset selection. Note that in this scheme,
  // ShenandoahGarbageThreshold is the soft threshold which would be ignored until min_garbage is hit.

  // In generational mode, the sort order within the data array is not strictly descending amounts of garbage.  In
  // particular, regions that have reached tenure age will be sorted into this array before younger regions that contain
  // more garbage.  This represents one of the reasons why we keep looking at regions even after we decide, for example,
  // to exclude one of the regions because it might require evacuation of too much live data.
  // TODO: Split it in the separate methods for clarity.
  assert(heap->mode()->is_generational(), "Heuristic is only for generational mode");
  size_t capacity = heap->young_generation()->max_capacity();

  // cur_young_garbage represents the amount of memory to be reclaimed from young-gen.  In the case that live objects
  // are known to be promoted out of young-gen, we count this as cur_young_garbage because this memory is reclaimed
  // from young-gen and becomes available to serve future young-gen allocation requests.
  size_t cur_young_garbage = 0;

  // Better select garbage-first regions
  QuickSort::sort<RegionData>(data, (int) size, compare_by_garbage, false);

  for (size_t idx = 0; idx < size; idx++) {
    ShenandoahHeapRegion* r = data[idx]._region;
    if (cset->is_preselected(r->index())) {
      assert(r->age() >= InitialTenuringThreshold, "Preselected regions must have tenure age");
      // Entire region will be promoted, This region does not impact young-gen or old-gen evacuation reserve.
      // This region has been pre-selected and its impact on promotion reserve is already accounted for.

      // r->used() is r->garbage() + r->get_live_data_bytes()
      // Since all live data in this region is being evacuated from young-gen, it is as if this memory
      // is garbage insofar as young-gen is concerned.  Counting this as garbage reduces the need to
      // reclaim highly utilized young-gen regions just for the sake of finding min_garbage to reclaim
      // within young-gen memory.

      cur_young_garbage += r->garbage();
      cset->add_region(r);
    }
  }

  if (_generation->is_global()) {
    choose_global_collection_set(cset, heap, data, size, actual_free,
                                 garbage_threshold, ignore_threshold, capacity, cur_young_garbage);
  } else {
    choose_young_collection_set(cset, heap, data, size, actual_free,
                                garbage_threshold, ignore_threshold, capacity, cur_young_garbage);
  }

  size_t collected_old = cset->get_old_bytes_reserved_for_evacuation();
  size_t collected_promoted = cset->get_young_bytes_to_be_promoted();
  size_t collected_young = cset->get_young_bytes_reserved_for_evacuation();

  log_info(gc, ergo)(
          "Chosen CSet evacuates young: " SIZE_FORMAT "%s (of which at least: " SIZE_FORMAT "%s are to be promoted), "
          "old: " SIZE_FORMAT "%s",
          byte_size_in_proper_unit(collected_young), proper_unit_for_byte_size(collected_young),
          byte_size_in_proper_unit(collected_promoted), proper_unit_for_byte_size(collected_promoted),
          byte_size_in_proper_unit(collected_old), proper_unit_for_byte_size(collected_old));
}

void ShenandoahYoungHeuristics::choose_young_collection_set(ShenandoahCollectionSet* cset, const ShenandoahHeap* heap,
                                                            const ShenandoahHeuristics::RegionData* data, size_t size,
                                                            size_t actual_free,
                                                            size_t garbage_threshold, size_t ignore_threshold,
                                                            size_t capacity,
                                                            size_t cur_young_garbage) const {
  // This is young-gen collection or a mixed evacuation.
  // If this is mixed evacuation, the old-gen candidate regions have already been added.
  size_t max_cset = (size_t) (heap->get_young_evac_reserve() / ShenandoahEvacWaste);
  size_t cur_cset = 0;
  size_t free_target = (capacity * ShenandoahMinFreeThreshold) / 100 + max_cset;
  size_t min_garbage = (free_target > actual_free) ? (free_target - actual_free) : 0;

  log_info(gc, ergo)(
          "Adaptive CSet Selection for YOUNG. Max Evacuation: " SIZE_FORMAT "%s, Actual Free: " SIZE_FORMAT "%s.",
          byte_size_in_proper_unit(max_cset), proper_unit_for_byte_size(max_cset),
          byte_size_in_proper_unit(actual_free), proper_unit_for_byte_size(actual_free));

  for (size_t idx = 0; idx < size; idx++) {
    ShenandoahHeapRegion* r = data[idx]._region;
    if (cset->is_preselected(r->index())) {
      continue;
    }
    if (r->age() < InitialTenuringThreshold) {
      size_t new_cset = cur_cset + r->get_live_data_bytes();
      size_t region_garbage = r->garbage();
      size_t new_garbage = cur_young_garbage + region_garbage;
      bool add_regardless = (region_garbage > ignore_threshold) && (new_garbage < min_garbage);
      assert(r->is_young(), "Only young candidates expected in the data array");
      if ((new_cset <= max_cset) && (add_regardless || (region_garbage > garbage_threshold))) {
        cur_cset = new_cset;
        cur_young_garbage = new_garbage;
        cset->add_region(r);
      }
    }
    // Note that we do not add aged regions if they were not pre-selected.  The reason they were not preselected
    // is because there is not sufficient room in old-gen to hold their to-be-promoted live objects or because
    // they are to be promoted in place.
  }
}

void ShenandoahYoungHeuristics::choose_global_collection_set(ShenandoahCollectionSet* cset,
                                                             const ShenandoahHeap* heap,
                                                             const ShenandoahHeuristics::RegionData* data,
                                                             size_t size, size_t actual_free,
                                                             size_t garbage_threshold, size_t ignore_threshold,
                                                             size_t capacity, size_t cur_young_garbage) const {
  size_t max_young_cset = (size_t) (heap->get_young_evac_reserve() / ShenandoahEvacWaste);
  size_t young_cur_cset = 0;
  size_t max_old_cset = (size_t) (heap->get_old_evac_reserve() / ShenandoahOldEvacWaste);
  size_t old_cur_cset = 0;
  size_t free_target = (capacity * ShenandoahMinFreeThreshold) / 100 + max_young_cset;
  size_t min_garbage = (free_target > actual_free) ? (free_target - actual_free) : 0;

  log_info(gc, ergo)("Adaptive CSet Selection for GLOBAL. Max Young Evacuation: " SIZE_FORMAT
                     "%s, Max Old Evacuation: " SIZE_FORMAT "%s, Actual Free: " SIZE_FORMAT "%s.",
                     byte_size_in_proper_unit(max_young_cset), proper_unit_for_byte_size(max_young_cset),
                     byte_size_in_proper_unit(max_old_cset), proper_unit_for_byte_size(max_old_cset),
                     byte_size_in_proper_unit(actual_free), proper_unit_for_byte_size(actual_free));

  for (size_t idx = 0; idx < size; idx++) {
    ShenandoahHeapRegion* r = data[idx]._region;
    if (cset->is_preselected(r->index())) {
      continue;
    }
    bool add_region = false;
    if (r->is_old()) {
      size_t new_cset = old_cur_cset + r->get_live_data_bytes();
      if ((new_cset <= max_old_cset) && (r->garbage() > garbage_threshold)) {
        add_region = true;
        old_cur_cset = new_cset;
      }
    } else if (r->age() < InitialTenuringThreshold) {
      size_t new_cset = young_cur_cset + r->get_live_data_bytes();
      size_t region_garbage = r->garbage();
      size_t new_garbage = cur_young_garbage + region_garbage;
      bool add_regardless = (region_garbage > ignore_threshold) && (new_garbage < min_garbage);
      if ((new_cset <= max_young_cset) && (add_regardless || (region_garbage > garbage_threshold))) {
        add_region = true;
        young_cur_cset = new_cset;
        cur_young_garbage = new_garbage;
      }
    }
    // Note that we do not add aged regions if they were not pre-selected.  The reason they were not preselected
    // is because there is not sufficient room in old-gen to hold their to-be-promoted live objects.

    if (add_region) {
      cset->add_region(r);
    }
  }
}

bool ShenandoahYoungHeuristics::should_start_gc() {
  // inherited triggers have already decided to start a cycle, so no further evaluation is required
  if (ShenandoahAdaptiveHeuristics::should_start_gc()) {
    return true;
  }

  // Get through promotions and mixed evacuations as quickly as possible.  These cycles sometimes require significantly
  // more time than traditional young-generation cycles so start them up as soon as possible.  This is a "mitigation"
  // for the reality that old-gen and young-gen activities are not truly "concurrent".  If there is old-gen work to
  // be done, we start up the young-gen GC threads so they can do some of this old-gen work.  As implemented, promotion
  // gets priority over old-gen marking.
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  size_t promo_potential = heap->get_promotion_potential();
  if (promo_potential > 0) {
    // Detect unsigned arithmetic underflow
    assert(promo_potential < heap->capacity(), "Sanity");
    log_info(gc)("Trigger (%s): expedite promotion of " SIZE_FORMAT "%s",
                 _generation->name(),
                 byte_size_in_proper_unit(promo_potential),
                 proper_unit_for_byte_size(promo_potential));
    return true;
  }

  size_t promo_in_place_potential = heap->get_promotion_in_place_potential();
  if (promo_in_place_potential > 0) {
    // Detect unsigned arithmetic underflow
    assert(promo_in_place_potential < heap->capacity(), "Sanity");
    log_info(gc)("Trigger (%s): expedite promotion in place of " SIZE_FORMAT "%s",
                 _generation->name(),
                 byte_size_in_proper_unit(promo_in_place_potential),
                 proper_unit_for_byte_size(promo_in_place_potential));
    return true;
  }

  ShenandoahOldHeuristics* old_heuristics = heap->old_heuristics();
  size_t mixed_candidates = old_heuristics->unprocessed_old_collection_candidates();
  if (mixed_candidates > 0) {
    // We need to run young GC in order to open up some free heap regions so we can finish mixed evacuations.
    log_info(gc)("Trigger (%s): expedite mixed evacuation of " SIZE_FORMAT " regions",
                 _generation->name(), mixed_candidates);
    return true;
  }

  return false;
}

// Return a conservative estimate of how much memory can be allocated before we need to start GC. The estimate is based
// on memory that is currently available within young generation plus all of the memory that will be added to the young
// generation at the end of the current cycle (as represented by young_regions_to_be_reclaimed) and on the anticipated
// amount of time required to perform a GC.
size_t ShenandoahYoungHeuristics::bytes_of_allocation_runway_before_gc_trigger(size_t young_regions_to_be_reclaimed) {
  size_t max_capacity = _generation->max_capacity();
  size_t capacity = _generation->soft_max_capacity();
  size_t usage = _generation->used();
  size_t available = (capacity > usage)? capacity - usage: 0;
  size_t allocated = _generation->bytes_allocated_since_gc_start();

  size_t available_young_collected = ShenandoahHeap::heap()->collection_set()->get_young_available_bytes_collected();
  size_t anticipated_available =
          available + young_regions_to_be_reclaimed * ShenandoahHeapRegion::region_size_bytes() - available_young_collected;
  size_t allocation_headroom = anticipated_available;
  size_t spike_headroom = capacity * ShenandoahAllocSpikeFactor / 100;
  size_t penalties      = capacity * _gc_time_penalties / 100;

  double rate = _allocation_rate.sample(allocated);

  // At what value of available, would avg and spike triggers occur?
  //  if allocation_headroom < avg_cycle_time * avg_alloc_rate, then we experience avg trigger
  //  if allocation_headroom < avg_cycle_time * rate, then we experience spike trigger if is_spiking
  //
  // allocation_headroom =
  //     0, if penalties > available or if penalties + spike_headroom > available
  //     available - penalties - spike_headroom, otherwise
  //
  // so we trigger if available - penalties - spike_headroom < avg_cycle_time * avg_alloc_rate, which is to say
  //                  available < avg_cycle_time * avg_alloc_rate + penalties + spike_headroom
  //            or if available < penalties + spike_headroom
  //
  // since avg_cycle_time * avg_alloc_rate > 0, the first test is sufficient to test both conditions
  //
  // thus, evac_slack_avg is MIN2(0,  available - avg_cycle_time * avg_alloc_rate + penalties + spike_headroom)
  //
  // similarly, evac_slack_spiking is MIN2(0, available - avg_cycle_time * rate + penalties + spike_headroom)
  // but evac_slack_spiking is only relevant if is_spiking, as defined below.

  double avg_cycle_time = _gc_cycle_time_history->davg() + (_margin_of_error_sd * _gc_cycle_time_history->dsd());

  // TODO: Consider making conservative adjustments to avg_cycle_time, such as: (avg_cycle_time *= 2) in cases where
  // we expect a longer-than-normal GC duration.  This includes mixed evacuations, evacuation that perform promotion
  // including promotion in place, and OLD GC bootstrap cycles.  It has been observed that these cycles sometimes
  // require twice or more the duration of "normal" GC cycles.  We have experimented with this approach.  While it
  // does appear to reduce the frequency of degenerated cycles due to late triggers, it also has the effect of reducing
  // evacuation slack so that there is less memory available to be transferred to OLD.  The result is that we
  // throttle promotion and it takes too long to move old objects out of the young generation.

  double avg_alloc_rate = _allocation_rate.upper_bound(_margin_of_error_sd);
  size_t evac_slack_avg;
  if (anticipated_available > avg_cycle_time * avg_alloc_rate + penalties + spike_headroom) {
    evac_slack_avg = anticipated_available - (avg_cycle_time * avg_alloc_rate + penalties + spike_headroom);
  } else {
    // we have no slack because it's already time to trigger
    evac_slack_avg = 0;
  }

  bool is_spiking = _allocation_rate.is_spiking(rate, _spike_threshold_sd);
  size_t evac_slack_spiking;
  if (is_spiking) {
    if (anticipated_available > avg_cycle_time * rate + penalties + spike_headroom) {
      evac_slack_spiking = anticipated_available - (avg_cycle_time * rate + penalties + spike_headroom);
    } else {
      // we have no slack because it's already time to trigger
      evac_slack_spiking = 0;
    }
  } else {
    evac_slack_spiking = evac_slack_avg;
  }

  size_t threshold = min_free_threshold();
  size_t evac_min_threshold = (anticipated_available > threshold)? anticipated_available - threshold: 0;
  return MIN3(evac_slack_spiking, evac_slack_avg, evac_min_threshold);
}


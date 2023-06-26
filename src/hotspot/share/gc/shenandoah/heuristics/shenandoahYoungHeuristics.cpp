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

#include "gc/shenandoah/heuristics/shenandoahYoungHeuristics.hpp"
#include "gc/shenandoah/shenandoahGeneration.hpp"
#include "gc/shenandoah/shenandoahYoungGeneration.hpp"

#include "utilities/quickSort.hpp"

ShenandoahYoungHeuristics::ShenandoahYoungHeuristics(ShenandoahGeneration* generation)
        : ShenandoahAdaptiveHeuristics(generation) {}

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

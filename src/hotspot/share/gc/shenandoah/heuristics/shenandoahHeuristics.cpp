/*
 * Copyright (c) 2018, 2021, Red Hat, Inc. All rights reserved.
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
#include "utilities/quickSort.hpp"

// sort by decreasing garbage (so most garbage comes first)
int ShenandoahHeuristics::compare_by_garbage(RegionData a, RegionData b) {
  if (a._u._garbage > b._u._garbage)
    return -1;
  else if (a._u._garbage < b._u._garbage)
    return 1;
  else return 0;
}

// sort by increasing live (so least live comes first)
int ShenandoahHeuristics::compare_by_live(RegionData a, RegionData b) {
  if (a._u._live_data < b._u._live_data)
    return -1;
  else if (a._u._live_data > b._u._live_data)
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

typedef struct {
  ShenandoahHeapRegion* _region;
  size_t _live_data;
} AgedRegionData;

static int compare_by_aged_live(AgedRegionData a, AgedRegionData b) {
  if (a._live_data < b._live_data)
    return -1;
  else if (a._live_data > b._live_data)
    return 1;
  else return 0;
}

// Returns bytes of old-gen memory consumed by selected aged regions
size_t ShenandoahHeuristics::select_aged_regions(size_t old_available, size_t num_regions, bool preselected_regions[]) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  ShenandoahMarkingContext* const ctx = heap->marking_context();
  size_t old_consumed = 0;
  size_t promo_potential = 0;
  size_t anticipated_promote_in_place_live = 0;
#undef KELVIN_PRESELECT
#ifdef KELVIN_PRESELECT
  log_info(gc, ergo)("selecting aged regions with budget " SIZE_FORMAT, old_available);
#endif
  if (heap->mode()->is_generational()) {
    heap->clear_promotion_in_place_potential();
    heap->clear_promotion_potential();
    size_t candidates = 0;
    size_t candidates_live = 0;
    size_t old_garbage_threshold = (ShenandoahHeapRegion::region_size_bytes() * ShenandoahOldGarbageThreshold) / 100;
    size_t promote_in_place_regions = 0;
    size_t promote_in_place_live = 0;
    size_t promote_in_place_pad = 0;
    size_t anticipated_candidates = 0;
    size_t anticipated_promote_in_place_regions = 0;

    // sort the promotion-eligible regions according to live-data-bytes so that we can first reclaim the larger numbers
    // of regions that require less evacuation effort.  This prioritizes garbage first, expanding the allocation pool before
    // we begin the work of reclaiming regions that require more effort.
    AgedRegionData* sorted_regions = (AgedRegionData*) alloca(num_regions * sizeof(AgedRegionData));
    for (size_t i = 0; i < num_regions; i++) {
      ShenandoahHeapRegion* r = heap->get_region(i);
      if (r->is_empty() || !r->has_live() || !r->is_young() || !r->is_regular()) {
        continue;
      }
      if (r->age() >= InitialTenuringThreshold) {
#ifdef KELVIN_PRESELECT
        log_info(gc, ergo)("Consider selection of region " SIZE_FORMAT " (candidate: " SIZE_FORMAT
                           ", age: %d, garbage: " SIZE_FORMAT " < " SIZE_FORMAT ")"
                           ", min_fill: " SIZE_FORMAT ", original top: " PTR_FORMAT,
                           i, promote_in_place_regions, r->age(), r->garbage(), old_garbage_threshold,
                           ShenandoahHeap::min_fill_size(), p2i(r->top()));
#endif
        r->save_top_before_promote();
        if ((r->garbage() < old_garbage_threshold)) {
          HeapWord* tams = ctx->top_at_mark_start(r);
          HeapWord* original_top = r->top();
          if (tams == original_top) {
            // Fill the remnant memory within this region to assure no allocations prior to promote in place.  Otherwise,
            // newly allocated objects will not be parseable when promote in place tries to register them.  Furthermore, any
            // new allocations would not necessarily be eligible for promotion.  This addresses both issues.
            size_t remnant_size = r->free() / HeapWordSize;
            if (remnant_size > ShenandoahHeap::min_fill_size()) {
              ShenandoahHeap::fill_with_object(original_top, remnant_size);
              r->set_top(r->end());
	      promote_in_place_pad += remnant_size * HeapWordSize;
            }
            // else, the remnant is too small to be allocated by any thread, so we don't have a problem.
#ifdef KELVIN_PRESELECT
            log_info(gc, ergo)("Promote in place region " SIZE_FORMAT " (candidate: " SIZE_FORMAT
                               ", age: %d, garbage: " SIZE_FORMAT " < " SIZE_FORMAT ")"
                               ", remnant: " SIZE_FORMAT ", min_fill: " SIZE_FORMAT
                               ", original top: " PTR_FORMAT ", adjusted top: " PTR_FORMAT,
                               i, promote_in_place_regions, r->age(), r->garbage(), old_garbage_threshold,
                               remnant_size, ShenandoahHeap::min_fill_size(), p2i(original_top), p2i(r->top()));
#endif
            promote_in_place_regions++;
            promote_in_place_live += r->get_live_data_bytes();
          }
          // Else, we do not promote this region (either in place or by copy) because it has received new allocations.

          // During evacuation, we exclude from promotion regions for which age > tenure threshold, garbage < garbage-threshold,
          //  and get_top_before_promote() != tams
        } else {
          // After sorting and selecting best candidates below, we may decide to exclude this promotion-eligible region
          // from the current collection sets.  If this happens, we will consider this region as part of the anticipated
          // promotion potential for the next GC pass.
#ifdef KELVIN_PRESELECT
          log_info(gc, ergo)("Consider promoting region " SIZE_FORMAT " (candidate: " SIZE_FORMAT ", age: %d, garbage: " SIZE_FORMAT " >= " SIZE_FORMAT ")",
                             i, candidates, r->age(), r->garbage(), old_garbage_threshold);
#endif
          size_t live_data = r->get_live_data_bytes();
          candidates_live += live_data;
          sorted_regions[candidates]._region = r;
          sorted_regions[candidates++]._live_data = live_data;
        }
      } else {

        // Only anticipate to promote regular regions if garbage() is above threshold.  Note that certain regions that are
        // excluded from anticipated promotion because their garbage content is too low (causing us to anticipate that
        // the region would be promoted in place) may be eligible for promotion by the time promotion takes place because
        // more garbage is found within the region between now and then.  This should not happen if we are properly adapting
        // the tenure age.  We won't tenure objects until they exhibit at least one full GC pass without further decline
        // in population.
        //
        // If this does occur by accident, the most likely impact is that there will not be sufficient available space in
        // old-gen to hold the live data to be copied out of this region, so the region will not be selected for the
        // current collection set.  The region will be tallied into the anticipated promotion for the next cycle and
        // will be collected at that time.
        //
        // TODO:
        //   If we are auto-tuning the tenure age and this occurs, use this as guidance that tenure age should be increased.

        if (r->age() + 1 == InitialTenuringThreshold) {
          if (r->garbage() >= old_garbage_threshold) {
#ifdef KELVIN_PRESELECT
            log_info(gc, ergo)("Anticipating promotion of regular region " SIZE_FORMAT " (candidate: " SIZE_FORMAT ", age %u, live: " SIZE_FORMAT
                               ", garbage: " SIZE_FORMAT ")",
                               r->index(), anticipated_candidates, r->age(), r->get_live_data_bytes(), r->garbage());
#endif
            anticipated_candidates++;
            promo_potential += r->get_live_data_bytes();
          }
          else {
#ifdef KELVIN_PRESELECT
            log_info(gc, ergo)("Anticipating promote-in-place of %s region " SIZE_FORMAT " (candidate: " SIZE_FORMAT
                               ", age %u, live: " SIZE_FORMAT ", garbage: " SIZE_FORMAT ")",
                               r->is_regular()? "regular": "humongous",
                               r->index(), anticipated_promote_in_place_regions, r->age(), r->get_live_data_bytes(), r->garbage());
#endif
            anticipated_promote_in_place_regions++;
            anticipated_promote_in_place_live += r->get_live_data_bytes();
          }
        }
      }
    }
#ifdef KELVIN_PRESELECT
    log_info(gc, ergo)("Preselect midpoint: regular regions to promote in place: " SIZE_FORMAT ", representing " SIZE_FORMAT
                       " live, promo candidates: " SIZE_FORMAT ", spanning live: " SIZE_FORMAT,
                       promote_in_place_regions, promote_in_place_live, candidates, candidates_live);
    log_info(gc, ergo)("  anticipated regular regions to promote in place candidates: " SIZE_FORMAT ", representing " SIZE_FORMAT
                       " live, anticipated promote by copy candidates: " SIZE_FORMAT " with potential: " SIZE_FORMAT,
                       anticipated_promote_in_place_regions, anticipated_promote_in_place_live,
                       anticipated_candidates, promo_potential);
#endif
    // Sort in increasing order according to live data bytes.  Note that candidates represents the number of regions
    // that qualify to be promoted by evacuation.
    if (candidates > 0) {
      QuickSort::sort<AgedRegionData>(sorted_regions, candidates, compare_by_aged_live, false);
      for (size_t i = 0; i < candidates; i++) {
        size_t region_live_data = sorted_regions[i]._live_data;
        size_t promotion_need = (size_t) (region_live_data * ShenandoahPromoEvacWaste);
        if (old_consumed + promotion_need <= old_available) {
          ShenandoahHeapRegion* region = sorted_regions[i]._region;
#ifdef KELVIN_PRESELECT
          log_info(gc, ergo)("Preselecting regular region " SIZE_FORMAT " with age %u, live: " SIZE_FORMAT
                             ", garbage: " SIZE_FORMAT,
                             region->index(), region->age(), region->get_live_data_bytes(), region->garbage());
#endif
          old_consumed += promotion_need;
          preselected_regions[region->index()] = true;
        } else {
#ifdef KELVIN_PRESELECT
          ShenandoahHeapRegion* region = sorted_regions[i]._region;
          log_info(gc, ergo)("Region " SIZE_FORMAT " rejected because old_consumed: " SIZE_FORMAT ", budget: " SIZE_FORMAT
                             ", adding to future promo potential (age: %d, live: " SIZE_FORMAT ", garbage: "
                             SIZE_FORMAT " >= " SIZE_FORMAT ")", region->index(), old_consumed, old_available,
                             region->age(), region->get_live_data_bytes(),
                             region->garbage(), old_garbage_threshold);
#endif
          // We rejected this promotable region from the collection set because we had no room to hold its copy.
          // Add this region to promo potential for next GC.
          promo_potential += region_live_data;
        }
        // Note that we keep going even if one region is excluded from selection because we need to accumulate all
        // eligible regions into promo_potential if not preselected.
      }
    }
    heap->set_pad_for_promote_in_place(promote_in_place_pad);
#undef KELVIN_PROMO_POTENTIAL
#ifdef KELVIN_PROMO_POTENTIAL
    log_info(gc, ergo)("Establishing promo potential as " SIZE_FORMAT, promo_potential);
#endif
    heap->set_promotion_potential(promo_potential);
    heap->set_promotion_in_place_potential(anticipated_promote_in_place_live);
  }
#ifdef KELVIN_PRESELECT
  log_info(gc, ergo)("select_aged_regions consumed old reserve of " SIZE_FORMAT ", promo potential: " SIZE_FORMAT,
                     old_consumed, promo_potential);
#endif
  return old_consumed;
}

void ShenandoahHeuristics::choose_collection_set(ShenandoahCollectionSet* collection_set, ShenandoahOldHeuristics* old_heuristics) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  bool is_generational = heap->mode()->is_generational();

  assert(collection_set->count() == 0, "Must be empty");
  assert(_generation->generation_mode() != OLD, "Old GC invokes ShenandoahOldHeuristics::choose_collection_set()");

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
  size_t live_memory = 0;

  size_t old_garbage_threshold = (ShenandoahHeapRegion::region_size_bytes() * ShenandoahOldGarbageThreshold) / 100;
  // This counts number of humongous regions that we intend to promote in this cycle.
  size_t humongous_regions_promoted = 0;
  // This counts bytes of memory used by hunongous regions to be promoted in place.
  size_t humongous_bytes_promoted = 0;
  // This counts number of regular regions that will be promoted in place.
  size_t regular_regions_promoted_in_place = 0;
  // This counts bytes of memory used by regular regions to be promoted in place.
  size_t regular_regions_promoted_usage = 0;

#undef KELVIN_CSET
#ifdef KELVIN_CSET
  log_info(gc, ergo)("Choosing collection set, confirm that young capacity greater than used_regions, old_heuristics: %s",
                     old_heuristics? "NOT NULL": "NULL");
  heap->young_generation()->log_status("At start choose_collection set");
#endif
  for (size_t i = 0; i < num_regions; i++) {
    ShenandoahHeapRegion* region = heap->get_region(i);

    if (is_generational && !in_generation(region)) {
#ifdef KELVIN_CSET
      log_info(gc, ergo)("Ignoring %s region " SIZE_FORMAT " because not in generation",
                         affiliation_name(region->affiliation()), region->index());
#endif
      continue;
    }

    size_t garbage = region->garbage();
    total_garbage += garbage;
    if (region->is_empty()) {
#ifdef KELVIN_CSET
      log_info(gc, ergo)("Treating %s region " SIZE_FORMAT " as free",
                         affiliation_name(region->affiliation()), region->index());
#endif
      free_regions++;
      free += ShenandoahHeapRegion::region_size_bytes();
    } else if (region->is_regular()) {
      if (!region->has_live()) {
        // We can recycle it right away and put it in the free set.
#ifdef KELVIN_CSET
      log_info(gc, ergo)("Treating %s region " SIZE_FORMAT " as immediate garbage",
                         affiliation_name(region->affiliation()), region->index());
#endif
        immediate_regions++;
        immediate_garbage += garbage;
        region->make_trash_immediate();
      } else {
        assert (_generation->generation_mode() != OLD, "OLD is handled elsewhere");
        live_memory += region->get_live_data_bytes();
        bool is_candidate;
        // This is our candidate for later consideration.
        if (is_generational && collection_set->is_preselected(i)) {
          // If !is_generational, we cannot ask if is_preselected.  If is_preselected, we know
          //   region->age() >= InitialTenuringThreshold).
          // Set garbage value to maximum value to force this into the sorted collection set.
          is_candidate = true;
#ifdef KELVIN_CSET
          log_info(gc, ergo)("Preselected Region " SIZE_FORMAT " is placed in candidate set", region->index());
#endif
          preselected_candidates++;
        } else if (is_generational && region->is_young() && (region->age() >= InitialTenuringThreshold)) {
          // Note that for GLOBAL GC, region may be OLD, and OLD regions do not qualify for pre-selection

          // This region is old enough to be promoted but it was not preselected, either because its garbage is below
          // ShenandoahOldGarbageThreshold so it will be promoted in place, or because there is not sufficient room
          // in old gen to hold the evacuated copies of this region's live data.  In both cases, we choose not to
          // place this region into the collection set.
          if (region->garbage_before_padded_for_promote() < old_garbage_threshold) {
#ifdef KELVIN_CSET
            log_info(gc, ergo)("Excluding region " SIZE_FORMAT " which will be promoted in place has usage: " SIZE_FORMAT,
                               region->index(), region->used_before_promote());
#endif
            regular_regions_promoted_in_place++;
            regular_regions_promoted_usage += region->used_before_promote();
          } else {
#ifdef KELVIN_CSET
            log_info(gc, ergo)("Excluding aged region "
                               SIZE_FORMAT " for which there is not sufficient old-gen space to promote at this time",
                               region->index());
#endif
          }
          is_candidate = false;
        } else {
#ifdef KELVIN_CSET
          log_info(gc, ergo)("Regular unaged (%d) Region " SIZE_FORMAT ", is candidate for evacuation",
                             region->age(), region->index());
#endif
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
#ifdef KELVIN_CSET
        log_info(gc, ergo)("Humongous region " SIZE_FORMAT ", is immediate trash", region->index());
#endif
        heap->trash_humongous_region_at(region);

        // Count only the start. Continuations would be counted on "trash" path
        immediate_regions++;
        immediate_garbage += garbage;
      } else {
        live_memory += region->get_live_data_bytes();
        if (region->is_young() && region->age() >= InitialTenuringThreshold) {
          oop obj = cast_to_oop(region->bottom());
          size_t humongous_regions = ShenandoahHeapRegion::required_regions(obj->size() * HeapWordSize);
          humongous_regions_promoted += humongous_regions;
          humongous_bytes_promoted += obj->size() * HeapWordSize;

#undef KELVIN_REPROMOTE
#ifdef KELVIN_REPROMOTE
          HeapWord* end_addr = region->bottom() + obj->size();
          size_t waste = (region->bottom() + humongous_regions * ShenandoahHeapRegion::region_size_bytes()) - end_addr;
          waste *= HeapWordSize;
          log_info(gc, ergo)("Planning to promote " SIZE_FORMAT " %s humongous regions starting with index " SIZE_FORMAT
                             ", representing " SIZE_FORMAT " used (live) bytes, waste: " SIZE_FORMAT,
                             humongous_regions, affiliation_name(region->affiliation()),
                             region->index(),  obj->size() * HeapWordSize, waste);
#endif

#ifdef KELVIN_CSET
          log_info(gc, ergo)("Planning to promote " SIZE_FORMAT " humongous regions starting with index " SIZE_FORMAT
                             ", representing " SIZE_FORMAT " used (live) bytes",
                             humongous_regions, humongous_regions_promoted,  obj->size() * HeapWordSize);
#endif
        }
#ifdef KELVIN_CSET
        else {
          log_info(gc, ergo)("Humongous %s region " SIZE_FORMAT " is not in collection candidates",
                             affiliation_name(region->affiliation()), region->index());
        }
#endif
      }
    } else if (region->is_trash()) {
#ifdef KELVIN_CSET
      log_info(gc, ergo)("Treating %s region " SIZE_FORMAT " as trash", affiliation_name(region->affiliation()), region->index());
#endif
      // Count in just trashed collection set, during coalesced CM-with-UR
      immediate_regions++;
      immediate_garbage += garbage;
    } else {                      // region->is_humongous_cont() and !region->is_trash()
#ifdef KELVIN_CSET
      log_info(gc, ergo)("Ignoring %s region " SIZE_FORMAT " with live memory: " SIZE_FORMAT,
                         affiliation_name(region->affiliation()), region->index(), region->get_live_data_bytes());
#endif
      live_memory += region->get_live_data_bytes();
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
#ifdef KELVIN_CSET
  log_info(gc, ergo)("Regular regions promoted usage: " SIZE_FORMAT, regular_regions_promoted_usage);
#endif

  // Step 2. Look back at garbage statistics, and decide if we want to collect anything,
  // given the amount of immediately reclaimable garbage. If we do, figure out the collection set.

  assert (immediate_garbage <= total_garbage,
          "Cannot have more immediate garbage than total garbage: " SIZE_FORMAT "%s vs " SIZE_FORMAT "%s",
          byte_size_in_proper_unit(immediate_garbage), proper_unit_for_byte_size(immediate_garbage),
          byte_size_in_proper_unit(total_garbage),     proper_unit_for_byte_size(total_garbage));

  size_t immediate_percent = (total_garbage == 0) ? 0 : (immediate_garbage * 100 / total_garbage);
  collection_set->set_immediate_trash(immediate_garbage);

  ShenandoahGeneration* young_gen = heap->young_generation();
  bool doing_promote_in_place = (humongous_regions_promoted + regular_regions_promoted_in_place > 0);
#ifdef KELVIN_CSET
  log_info(gc, ergo)("Promoted in place regions: " SIZE_FORMAT ", preselected_candidates: " SIZE_FORMAT
                     ", immediate_percent: " SIZE_FORMAT ", threshold: " SIZE_FORMAT,
                     humongous_regions_promoted + regular_regions_promoted_in_place, preselected_candidates,
                     immediate_percent, ShenandoahImmediateThreshold);
#endif
  if (doing_promote_in_place || (preselected_candidates > 0) || (immediate_percent <= ShenandoahImmediateThreshold)) {
#ifdef KELVIN_CSET
    log_info(gc, ergo)("Prime the collection set here, old_heuristics: %s",
                       old_heuristics? "NOT NULL": "NULL");
#endif
    if (old_heuristics != nullptr) {
      old_heuristics->prime_collection_set(collection_set);
    }
    // else, this is non-generational or global collection and doesn't need to prime_collection_set

    // Add young-gen regions into the collection set.  This is a virtual call, implemented differently by each
    // of the heuristics subclasses.
    choose_collection_set_from_regiondata(collection_set, candidates, cand_idx, immediate_garbage + free);
  } else {
    // we're going to skip evacuation and update refs because we reclaimed sufficient amounts of immediate garbage.
    heap->shenandoah_policy()->record_abbreviated_cycle();
  }

  if (collection_set->has_old_regions()) {
    heap->shenandoah_policy()->record_mixed_cycle();
  }

  size_t cset_percent = (total_garbage == 0) ? 0 : (collection_set->garbage() * 100 / total_garbage);
  size_t collectable_garbage = collection_set->garbage() + immediate_garbage;
  size_t collectable_garbage_percent = (total_garbage == 0) ? 0 : (collectable_garbage * 100 / total_garbage);

  log_info(gc, ergo)("Collectable Garbage: " SIZE_FORMAT "%s (" SIZE_FORMAT "%%), "
                     "Immediate: " SIZE_FORMAT "%s (" SIZE_FORMAT "%%) R: " SIZE_FORMAT ", "
                     "CSet: " SIZE_FORMAT "%s (" SIZE_FORMAT "%%) R: " SIZE_FORMAT,

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

  if (!collection_set->is_empty()) {
    size_t young_evac_bytes = collection_set->get_young_bytes_reserved_for_evacuation();
    size_t promote_evac_bytes = collection_set->get_young_bytes_to_be_promoted();
    size_t old_evac_bytes = collection_set->get_old_bytes_reserved_for_evacuation();
    size_t total_evac_bytes = young_evac_bytes + promote_evac_bytes + old_evac_bytes;

    log_info(gc, ergo)("Evacuation Targets: YOUNG: " SIZE_FORMAT "%s, "
                       "PROMOTE: " SIZE_FORMAT "%s, "
                       "OLD: " SIZE_FORMAT "%s, "
                       "TOTAL: " SIZE_FORMAT "%s",
                       byte_size_in_proper_unit(young_evac_bytes), proper_unit_for_byte_size(young_evac_bytes),
                       byte_size_in_proper_unit(promote_evac_bytes), proper_unit_for_byte_size(promote_evac_bytes),
                       byte_size_in_proper_unit(old_evac_bytes), proper_unit_for_byte_size(old_evac_bytes),
                       byte_size_in_proper_unit(total_evac_bytes), proper_unit_for_byte_size(total_evac_bytes));
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

size_t ShenandoahHeuristics::evac_slack(size_t young_regions_to_be_recycled) {
  assert(false, "evac_slack() only implemented for young Adaptive Heuristics");
  return 0;
}


double ShenandoahHeuristics::elapsed_cycle_time() const {
  return os::elapsedTime() - _cycle_start;
}

bool ShenandoahHeuristics::in_generation(ShenandoahHeapRegion* region) {
  return ((_generation->generation_mode() == GLOBAL)
          || (_generation->generation_mode() == YOUNG && region->affiliation() == YOUNG_GENERATION)
          || (_generation->generation_mode() == OLD && region->affiliation() == OLD_GENERATION));
}

size_t ShenandoahHeuristics::min_free_threshold() {
  size_t min_free_threshold = ShenandoahMinFreeThreshold;
  return _generation->soft_max_capacity() / 100 * min_free_threshold;
}

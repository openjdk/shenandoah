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

#ifndef SHARE_GC_SHENANDOAH_SHENANDOAHCOLLECTIONSETPARAMETERS_HPP
#define SHARE_GC_SHENANDOAH_SHENANDOAHCOLLECTIONSETPARAMETERS_HPP

#include "utilities/globalDefinitions.hpp"

class ShenandoahCollectionSetParameters {
private:
  size_t _promotion_potential;
  size_t _pad_for_promote_in_place;
  size_t _promotable_humongous_regions;
  size_t _regular_regions_promoted_in_place;

  // Bytes reserved within old-gen to hold the results of promotion
  size_t _promoted_reserve;

  // Bytes of old-gen memory expended on promotions (is volatile necessary?)
  volatile size_t _promoted_expended;

  // Bytes reserved within old-gen to hold evacuated objects from old-gen collection set
  size_t _old_evac_reserve;

  // Bytes reserved within young-gen to hold evacuated objects from young-gen collection set
  size_t _young_evac_reserve;

public:
  ShenandoahCollectionSetParameters();

  // Used only by shFullGC.cpp
  void clear_promotion_potential() { _promotion_potential = 0; };

  // Used only by shGeneration::select_aged_regions
  void set_promotion_potential(size_t val) { _promotion_potential = val; };

  // Used:
  // ShenandoahHeap::compute_old_generation_balance - should definitely move this (probably to shOldGeneration).
  // ShenandoahYoungHeuristics::should_start_gc - to expedite promotions
  size_t get_promotion_potential() const { return _promotion_potential; };

  // Used only by shGeneration::select_aged_regions
  void set_pad_for_promote_in_place(size_t pad) { _pad_for_promote_in_place = pad; }
  // Used during verification only
  size_t get_pad_for_promote_in_place() const { return _pad_for_promote_in_place; }

  // Used in ShenandoahGenerationalHeuristics::choose_collection_set
  void reserve_promotable_humongous_regions(size_t region_count) { _promotable_humongous_regions = region_count; }
  void reserve_promotable_regular_regions(size_t region_count) { _regular_regions_promoted_in_place = region_count; }

  // Used in ShenandoahDegenGC::op_prepare_evacuation and ShenandoahConcurrentGC::op_final_mark
  // to initiate promote in place during evacuation of concurrent and degenerated cycles
  size_t get_promotable_humongous_regions() const { return _promotable_humongous_regions; }
  size_t get_regular_regions_promoted_in_place() const  { return _regular_regions_promoted_in_place; }

  // Returns previous value
  // Used in shGeneration::adjust_evacuation_budgets
  // Used in shGeneration::compute_evacuation_budgets
  // Used in ShenandoahDegenGC::op_degenerated (zero'd out)
  // Used in ShenandoahConcurrentGC::collect (zero'd out)
  size_t set_promoted_reserve(size_t new_val);

  // Used in ShenandoahHeap::report_promotion_failure (under the heap lock)
  // Used (heavily) in ShenandoahHeap::allocate_memory_under_lock
  // Used in ShenandoahFreeSet::rebuild
  size_t get_promoted_reserve() const;

  // Used in ShenandoahFreeSet::add_old_collector_free_region
  void augment_promo_reserve(size_t increment);

  // Used in shGeneration::adjust_evacuation_budgets
  void reset_promoted_expended();
  size_t expend_promoted(size_t increment);

  // ShenandoahHeap::retire_plab
  size_t unexpend_promoted(size_t decrement);

  // Used in ShenandoahHeap::report_promotion_failure (under the heap lock)
  // Used (heavily) in ShenandoahHeap::allocate_memory_under_lock
  size_t get_promoted_expended();

  // Returns previous value
  // Used in shGeneration::compute_evacuation_budgets
  // Used in ShenandoahDegenGC::op_degenerated (zero'd out)
  // Used in ShenandoahConcurrentGC::collect (zero'd out)
  // Used in ShenandoahGeneration::adjust_evacuation_budgets
  // Used in ShenandoahGlobalHeuristics::choose_global_collection_set (if regions transferred to old)
  // Used in ShenandoahOldHeuristicTest (this test is a burden at this point)
  size_t set_old_evac_reserve(size_t new_val);

  // Used in ShenandoahFreeSet::rebuild
  // Used in ShenandoahGlobalHeuristics::choose_global_collection_set
  // Used in ShenandoahGeneration::adjust_evacuation_budgets
  // Used in ShenandoahOldHeuristics::prime_collection_set
  // Used in ShenandoahHeap::allocate_memory_under_lock
  size_t get_old_evac_reserve() const;

  // Used in ShenandoahFreeSet::add_old_collector_free_region
  // Used in ShenandoahFreeSet::flip_to_old_gc
  void augment_old_evac_reserve(size_t increment);

  // Returns previous value
  // Used in shGeneration::compute_evacuation_budgets
  // Used in ShenandoahDegenGC::op_degenerated (zero'd out)
  // Used in ShenandoahConcurrentGC::collect (zero'd out)
  // Used in ShenandoahGeneration::adjust_evacuation_budgets
  // Used in ShenandoahGlobalHeuristics::choose_global_collection_set  (if regions transferred to old)
  size_t set_young_evac_reserve(size_t new_val);

  // Used in ShenandoahFreeSet::rebuild
  // Used in ShenandoahGlobalHeuristics::choose_global_collection_set
  // Used in ShenandoahYoungHeuristics::choose_young_collection_set
  size_t get_young_evac_reserve() const;

  // Used in ShenandoahGenerationalFullGC::prepare
  void reset_generation_reserves();
};


#endif //SHARE_GC_SHENANDOAH_SHENANDOAHCOLLECTIONSETPARAMETERS_HPP

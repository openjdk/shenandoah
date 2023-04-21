/*
 * Copyright (c) 2016, 2019, Red Hat, Inc. All rights reserved.
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

#ifndef SHARE_GC_SHENANDOAH_SHENANDOAHFREESET_HPP
#define SHARE_GC_SHENANDOAH_SHENANDOAHFREESET_HPP

#include "gc/shenandoah/shenandoahHeapRegionSet.hpp"
#include "gc/shenandoah/shenandoahHeap.hpp"

enum MemoryReserve {
  Mutator,
  Collector,
  OldCollector
};

class ShenandoahFreeSet : public CHeapObj<mtGC> {
private:

  ShenandoahHeap* const _heap;
  CHeapBitMap _mutator_free_bitmap;

  // The _collector_free regions hold survivor objects within young-generation and within traditional single-generation
  // collections.  In general, the _collector_free regions are at the high end of memory and mutator-free regions are at
  // the low-end of memory.  In generational mode, the young survivor regions are typically recycled after the region reaches
  // tenure age.  In the case that a young survivor region reaches tenure age and has sufficiently low amount of garbage,
  // the region will be promoted in place.  This means the region will simply be relabled as an old-generation region and
  // will not be evacuated until an old-generation collection chooses to do so.
  CHeapBitMap _collector_free_bitmap;

  // We keep the _old_collector regions separate from the young collector regions.  This allows us to pack the old regions
  // further to the right than the young collector regions.  This is desirable because the old collector regions are recycled
  // even less frequently than the young survivor regions.
  CHeapBitMap _old_collector_free_bitmap;
  size_t _max;

  // Left-most and right-most region indexes. There are no free regions outside of [left-most; right-most] index intervals.
  // For a free set of a given kind (mutator, collector, old_collector), we maintain left and right indices to limit
  // searching. The intervals represented by these extremal indices designate the lowest and highest indices at which
  // that kind of free region exists. These intervals may overlap. In particular, it is quite common for the collector
  // free interval to overlap the mutator free interval on one side (the low end) and the old_collector free interval
  // on the other (the high end).  It is also possible for the mutator interval to overlap the old_collector free
  // interval.
  size_t _mutator_leftmost, _mutator_rightmost;

  size_t _collector_leftmost, _collector_rightmost;
  size_t _old_collector_leftmost, _old_collector_rightmost;

  // _capacity represents the amount of memory that can be allocated within the mutator set at the time of the
  // most recent rebuild, as adjusted for the flipping of regions from mutator set to collector set or old collector set.
  size_t _capacity;

  // _used represents the amount of memory allocated within the mutator set since the time of the most recent rebuild.
  // _used feeds into certain ShenandoanPacing decisions.  There is no need to track of the memory consumed from
  // within the collector and old_collector sets.
  size_t _used;

  // _old_capacity represents the amount of memory that can be allocated within the old collector set at the time
  // of the most recent rebuild, as adjusted for the flipping of regions from mutator set to old collector set.
  size_t _old_capacity;

  // There is no need to compute young collector capacity.  And there is not need to consult _old_capacity once we
  // have successfully reserved the evacuation (old_collector and collector sets) requested at rebuild time.
  // TODO: A cleaner abstraction might encapsulate capacity (and used) information within a refactored set abstraction.


  // When old_collector_set regions sparsely populate the lower address ranges of the heap, we search from left to
  // right in order to consume (and remove from the old_collector set range) these sparsely distributed regions.
  // This allows us to more quickly condense the range of addresses that represent old_collector_free regions.
  bool _old_collector_search_left_to_right = true;

  // Assure leftmost and rightmost bounds are valid for the mutator_is_free, collector_is_free, and old_collector_is_free sets.
  // valid bounds honor all of the following (where max is the number of heap regions):
  //   if the set is empty, leftmost equals max and rightmost equals 0
  //   Otherwise (the set is not empty):
  //     0 <= leftmost < max and 0 <= rightmost < max
  //     the region at leftmost is in the set
  //     the region at rightmost is in the set
  //     rightmost >= leftmost
  //     for every idx that is in the set {
  //       idx >= leftmost &&
  //       idx <= rightmost
  //     }
  void assert_bounds() const NOT_DEBUG_RETURN;

  // Every region is in exactly one of four sets: mutator_free, collector_free, old_collector_free, not_free.
  // Insofar as the free-set abstraction is concerned, we are only interested in regions that are free so we provide no
  // mechanism to directly inquire as to whether a region is not_free.  not_free membership is implied by not member of
  // mutator_free, collector_free and old_collector_free sets.
  //
  // in_set() implies that the region has allocation capacity (i.e. is not yet fully allocated) as assured by assertions.
  //
  // TODO: a future implementation may replace the three bitmaps with a single array of enums to simplify the representation
  // of membership within these four mutually exclusive sets.

  template <MemoryReserve SET> inline bool in_set(size_t idx) const;

  // The following probe routine mimics the behavior is in_set() but does not assert that regions have allocation capacity.
  // This probe routine is used in assertions enforced during certain state transitions.
  template <MemoryReserve SET> inline bool probe_set(size_t idx) const;

  // The next two methods change set membership of regions
  template <MemoryReserve SET> inline void add_to_set(size_t idx);
  template <MemoryReserve SSET> inline void remove_from_set(size_t idx);

  HeapWord* try_allocate_in(ShenandoahHeapRegion* region, ShenandoahAllocRequest& req, bool& in_new_region);

  // Satisfy young-generation or single-generation collector allocation request req by finding memory that matches
  // affiliation, which either equals req.affiliation or FREE.  We know req.is_young().
  HeapWord* allocate_with_affiliation(ShenandoahAffiliation affiliation, ShenandoahAllocRequest& req, bool& in_new_region);

  // Satisfy allocation request req by finding memory that matches affiliation, which either equals req.affiliation
  // or FREE. We know req.is_old().
  HeapWord* allocate_old_with_affiliation(ShenandoahAffiliation affiliation, ShenandoahAllocRequest& req, bool& in_new_region);

  // While holding the heap lock, allocate memory for a single object which is to be entirely contained
  // within a single HeapRegion as characterized by req.  The req.size() value is known to be less than or
  // equal to ShenandoahHeapRegion::humongous_threshold_words().  The caller of allocate_single is responsible
  // for registering the resulting object and setting the remembered set card values as appropriate.  The
  // most common case is that we are allocating a PLAB in which case object registering and card dirtying
  // is managed after the PLAB is divided into individual objects.
  HeapWord* allocate_single(ShenandoahAllocRequest& req, bool& in_new_region);
  HeapWord* allocate_contiguous(ShenandoahAllocRequest& req);

  void flip_to_gc(ShenandoahHeapRegion* r);
  void flip_to_old_gc(ShenandoahHeapRegion* r);

  // Compute left-most and right-most indexes for the mutator_is_free, collector_is_free, and old_collector_is_free sets.
  void recompute_bounds();

  // Adjust left-most and right-most indexes for the mutator_is_free, collector_is_free, and old_collector_is_free sets
  //  following minor changes to at least one set membership.
  void adjust_bounds();

  // Adjust left-most and right-most indexes for the <SET> free set after adding region idx to this set.
  template <MemoryReserve SET> inline void expand_bounds_maybe(size_t idx);

  // Adjust left-most and right-most indexes for the <SET> free set after removing region idx from this set.
  template <MemoryReserve SET> bool adjust_bounds_if_touched(size_t idx);

  // Return true iff region idx was the left-most or right-most index for one of the three free sets.
  bool touches_bounds(size_t idx) const;

   // Used of free set represents the amount of is_mutator_free set that has been consumed since most recent rebuild.
  void increase_used(size_t amount);

  void clear_internal();

  void try_recycle_trashed(ShenandoahHeapRegion *r);

  bool can_allocate_from(ShenandoahHeapRegion *r) const;
  size_t alloc_capacity(ShenandoahHeapRegion *r) const;
  bool has_alloc_capacity(size_t idx) const;
  bool has_alloc_capacity(ShenandoahHeapRegion *r) const;
  bool has_no_alloc_capacity(ShenandoahHeapRegion *r) const;

public:
  ShenandoahFreeSet(ShenandoahHeap* heap, size_t max_regions);

  // Number of regions dedicated to GC allocations (for evacuation) that are at least partially free
  size_t collector_count() const { return _collector_free_bitmap.count_one_bits(); }

  // Number of regions dedicated to Old GC allocations (for evacuation or promotion) that are at least partially free
  size_t old_collector_count() const { return _old_collector_free_bitmap.count_one_bits(); }

  // Number of regions dedicated to mutator allocations that are at least partially free
  size_t mutator_count()   const { return _mutator_free_bitmap.count_one_bits();   }

  void clear();
  void rebuild();

  void recycle_trash();

  void log_status();

  size_t capacity()  const { return _capacity; }
  size_t used()      const { return _used;     }
  size_t available() const {
    assert(_used <= _capacity, "must use less than capacity");
    return _capacity - _used;
  }

  HeapWord* allocate(ShenandoahAllocRequest& req, bool& in_new_region);
  size_t unsafe_peek_free() const;

  double internal_fragmentation();
  double external_fragmentation();

  void print_on(outputStream* out) const;

  void find_regions_with_alloc_capacity();
  void reserve_regions(size_t young_reserve, size_t old_reserve);
};

#endif // SHARE_GC_SHENANDOAH_SHENANDOAHFREESET_HPP

/*
 * Copyright (c) 2016, 2020, Red Hat, Inc. All rights reserved.
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

#ifndef SHARE_GC_SHENANDOAH_SHENANDOAHCOLLECTIONSET_HPP
#define SHARE_GC_SHENANDOAH_SHENANDOAHCOLLECTIONSET_HPP

#include "memory/allocation.hpp"
#include "memory/virtualspace.hpp"
#include "gc/shenandoah/shenandoahHeap.hpp"
#include "gc/shenandoah/shenandoahHeapRegion.hpp"
#include "gc/shenandoah/shenandoahPadding.hpp"

#ifdef KELVIN_NOTES
/**
 * My plan:  Order(1)
 *
 * To remove:
 *
 * To replace:
 *   [ok] _young_bytes_to_evacuate -> _young_words_to_evacuate
 *   [ok] _young_bytes_to_promote -> _young_words_to_promote
 *   [ok] _old_bytes_to_evacuate -> _old_words_to_evacuate
 *   [ok] _live: was bytes, change to words
 *   [ok] live() returns words
 *   [ok] get_old_bytes_reserved_for_evacuation -> get_old_words_reserved_for_evacuation
 *   [ok] get_young_bytes_reserved_for_evacuation -> get_young_words_reserved_for_evacuation
 *   [ok] get_young_bytes_to_be_promoted -> get_young_words_to_be_promoted

 *   [ok] _young_available_bytes_collected -> _young_available_words_collected

 *
 * To modify:

 *   [ok] _used: was bytes, change to words
 *   [ok] used() returns words

 *   [ok] _old_garbage was bytes, change to words
 *   [ok] _garbage was bytes, change to words
 *   [ok] garbage(): used to return bytes, but now returns words
 *   [ok] get_old_garbage(): used to return bytes, but now returns words
 */

#endif

class ShenandoahCollectionSet : public CHeapObj<mtGC> {
  friend class ShenandoahHeap;
  friend class ShenandoahCollectionSetPreselector;

  void establish_preselected(bool *preselected) {
   assert(_preselected_regions == nullptr, "Over-writing");
   _preselected_regions = preselected;
  }
  void abandon_preselected() { _preselected_regions = nullptr; }

private:
  size_t const          _map_size;
  size_t const          _region_size_bytes_shift;
  ReservedSpace         _map_space;
  char* const           _cset_map;
  // Bias cset map's base address for fast test if an oop is in cset
  char* const           _biased_cset_map;

  ShenandoahHeap* const _heap;

  bool                  _has_old_regions;
  size_t                _garbage;            // words of total _garbage to be reclaimed from cset
  size_t                _old_garbage;        // words of old garbage to be reclaimed from cset

  size_t                _used;               // total used words within cset
  size_t                _live;               // total live words within cset
  size_t                _region_count;

  size_t                _young_words_to_evacuate;
  size_t                _young_words_to_promote;
  size_t                _old_words_to_evacuate;


  // Points to array identifying which tenure-age regions have been preselected
  // for inclusion in collection set. This field is only valid during brief
  // spans of time while collection set is being constructed.
  bool*                 _preselected_regions;

  // When a region having memory available to be allocated is added to the collection set, the region's available memory
  // should be subtracted from what's available.
  size_t                _young_available_words_collected;

  shenandoah_padding(0);
  volatile size_t       _current_index;
  shenandoah_padding(1);

public:
  ShenandoahCollectionSet(ShenandoahHeap* heap, ReservedSpace space, char* heap_base);

  // Add region to collection set
  void add_region(ShenandoahHeapRegion* r);

  // MT version
  ShenandoahHeapRegion* claim_next();

  // Single-thread version
  ShenandoahHeapRegion* next();

  size_t count()  const { return _region_count; }
  bool is_empty() const { return _region_count == 0; }

  void clear_current_index() {
    _current_index = 0;
  }

  inline bool is_in(ShenandoahHeapRegion* r) const;
  inline bool is_in(size_t region_idx)       const;
  inline bool is_in(oop obj)                 const;
  inline bool is_in_loc(void* loc)           const;

  void print_on(outputStream* out) const;

  // It is not known how many of these words will be promoted.
  inline size_t get_young_words_reserved_for_evacuation();
  inline size_t get_old_words_reserved_for_evacuation();

  inline size_t get_young_words_to_be_promoted();

  size_t get_young_available_words_collected() { return _young_available_words_collected; }

  // Return words of old garbage to be reclaimed from the collection set
  inline size_t get_old_garbage();

  bool is_preselected(size_t region_idx) {
    assert(_preselected_regions != nullptr, "Missing etsablish after abandon");
    return _preselected_regions[region_idx];
  }

  bool* preselected_regions() {
    assert(_preselected_regions != nullptr, "Null ptr");
    return _preselected_regions;
  }

  bool has_old_regions() const { return _has_old_regions; }

  // Return words of used data represented by collection set
  size_t used()          const { return _used; }

  // Return words of live data within the collection set
  size_t live()          const { return _live; }

  // Return words of total garbage to be reclaimed from the collection set
  size_t garbage()       const { return _garbage; }

  void clear();

private:
  char* map_address() const {
    return _cset_map;
  }
  char* biased_map_address() const {
    return _biased_cset_map;
  }
};

#endif // SHARE_GC_SHENANDOAH_SHENANDOAHCOLLECTIONSET_HPP

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

#ifndef SHARE_GC_SHENANDOAH_SHENANDOAHGENERATIONALFULLGC_HPP
#define SHARE_GC_SHENANDOAH_SHENANDOAHGENERATIONALFULLGC_HPP

#include "gc/shared/preservedMarks.hpp"
#include "memory/iterator.hpp"
#include "oops/oop.inline.hpp"
#include "utilities/growableArray.hpp"

class ShenandoahHeap;
class ShenandoahHeapRegion;

class ShenandoahGenerationalFullGC {
public:
  static void handle_completion(ShenandoahHeap* heap);
  static void rebuild_remembered_set(ShenandoahHeap* heap);
  static void balance_old_generation(ShenandoahHeap* heap);
  static void balance_generations(ShenandoahHeap* heap);
  static void log_live_in_old(ShenandoahHeap* heap);
  static void account_for_region(ShenandoahHeapRegion* r, size_t &region_count, size_t &region_usage, size_t &humongous_waste);
  static void restore_top_before_promote(ShenandoahHeap* heap);
  static void maybe_coalesce_and_fill_region(ShenandoahHeapRegion* r);
};

class ShenandoahPrepareForGenerationalCompactionObjectClosure : public ObjectClosure {
private:
  PreservedMarks*          const _preserved_marks;
  ShenandoahHeap*          const _heap;
  uint                           _tenuring_threshold;

  // _empty_regions is a thread-local list of heap regions that have been completely emptied by this worker thread's
  // compaction efforts.  The worker thread that drives these efforts adds compacted regions to this list if the
  // region has not been compacted onto itself.
  GrowableArray<ShenandoahHeapRegion*>& _empty_regions;
  int _empty_regions_pos;
  ShenandoahHeapRegion*          _old_to_region;
  ShenandoahHeapRegion*          _young_to_region;
  ShenandoahHeapRegion*          _from_region;
  ShenandoahAffiliation          _from_affiliation;
  HeapWord*                      _old_compact_point;
  HeapWord*                      _young_compact_point;
  uint                           _worker_id;

public:
  ShenandoahPrepareForGenerationalCompactionObjectClosure(PreservedMarks* preserved_marks,
                                                          GrowableArray<ShenandoahHeapRegion*>& empty_regions,
                                                          ShenandoahHeapRegion* from_region, uint worker_id);

  void set_from_region(ShenandoahHeapRegion* from_region);
  void finish();
  void finish_old_region();
  void finish_young_region();
  bool is_compact_same_region();
  int empty_regions_pos() const { return _empty_regions_pos; }

  void do_object(oop p) override;
};

#endif //SHARE_GC_SHENANDOAH_SHENANDOAHGENERATIONALFULLGC_HPP

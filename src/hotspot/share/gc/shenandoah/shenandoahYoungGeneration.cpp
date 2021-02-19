/*
 * Copyright (c) 2020, Red Hat, Inc. All rights reserved.
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

#include "gc/shenandoah/shenandoahFreeSet.hpp"
#include "gc/shenandoah/shenandoahHeap.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahUtils.hpp"
#include "gc/shenandoah/shenandoahVerifier.hpp"
#include "gc/shenandoah/shenandoahYoungGeneration.hpp"
#include "gc/shenandoah/heuristics/shenandoahHeuristics.hpp"

#undef TRACE_PROMOTION

ShenandoahYoungGeneration::ShenandoahYoungGeneration(uint max_queues) :
  ShenandoahGeneration(YOUNG, max_queues),
  _affiliated_region_count(0),
  _used(0),
  _old_gen_task_queues(nullptr) {
}

const char* ShenandoahYoungGeneration::name() const {
  return "YOUNG";
}

void ShenandoahYoungGeneration::increment_affiliated_region_count() {
  _affiliated_region_count++;
}

void ShenandoahYoungGeneration::decrement_affiliated_region_count() {
  _affiliated_region_count--;
}

void ShenandoahYoungGeneration::increase_used(size_t bytes) {
  shenandoah_assert_heaplocked();
  _used += bytes;
}

void ShenandoahYoungGeneration::decrease_used(size_t bytes) {
  shenandoah_assert_heaplocked_or_safepoint();
  assert(used() >= bytes, "cannot reduce bytes used by young generation below zero");
  _used -= bytes;
}

// There are three JVM parameters for setting young gen capacity:
//    NewSize, MaxNewSize, NewRatio.
//
// If only NewSize is set, it assigns a fixed size and the other two parameters are ignored.
// Otherwise NewRatio applies.
//
// If NewSize is set in any combination, it provides a lower bound.
//
// If MaxNewSize is set it provides an upper bound.
// If this bound is smaller than NewSize, it supersedes,
// resulting in a fixed size given by MaxNewSize.
size_t ShenandoahYoungGeneration::configured_capacity(size_t capacity) const {
  if (FLAG_IS_CMDLINE(NewSize) && !FLAG_IS_CMDLINE(MaxNewSize) && !FLAG_IS_CMDLINE(NewRatio)) {
    capacity = MIN2(NewSize, capacity);
  } else {
    capacity /= NewRatio + 1;
    if (FLAG_IS_CMDLINE(NewSize)) {
      capacity = MAX2(NewSize, capacity);
    }
    if (FLAG_IS_CMDLINE(MaxNewSize)) {
      capacity = MIN2(MaxNewSize, capacity);
    }
  }
  return capacity;
}

size_t ShenandoahYoungGeneration::soft_max_capacity() const {
  size_t capacity = ShenandoahHeap::heap()->soft_max_capacity();
  return configured_capacity(capacity);
}

size_t ShenandoahYoungGeneration::max_capacity() const {
  size_t capacity = ShenandoahHeap::heap()->max_capacity();
  return configured_capacity(capacity);
}

size_t ShenandoahYoungGeneration::used_regions_size() const {
  return _affiliated_region_count * ShenandoahHeapRegion::region_size_bytes();
}

size_t ShenandoahYoungGeneration::available() const {
  size_t in_use = used();
  size_t soft_capacity = soft_max_capacity();
  return in_use > soft_capacity ? 0 : soft_capacity - in_use;
}

void ShenandoahYoungGeneration::set_concurrent_mark_in_progress(bool in_progress) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  heap->set_concurrent_young_mark_in_progress(in_progress);
  if (_old_gen_task_queues != NULL) {
    heap->set_concurrent_old_mark_in_progress(true);
  }
}

class ShenandoahPromoteTenuredRegionsTask : public AbstractGangTask {
private:
  ShenandoahRegionIterator* _regions;
public:
  volatile size_t _used;

  ShenandoahPromoteTenuredRegionsTask(ShenandoahRegionIterator* regions) :
    AbstractGangTask("Shenandoah Promote Tenured Regions"),
    _regions(regions),
    _used(0) {
  }

  void work(uint worker_id) {
    ShenandoahParallelWorkerSession worker_session(worker_id);
    ShenandoahHeapRegion* r = _regions->next();
    while (r != NULL) {
      if (r->is_young()) {
        if (r->age() >= InitialTenuringThreshold && !r->is_humongous_continuation()) {
          r->promote();
        } else {
          Atomic::add(&_used, r->used());
        }
      }
      r = _regions->next();
    }
  }
};

void ShenandoahYoungGeneration::promote_tenured_regions() {
  ShenandoahRegionIterator regions;
  ShenandoahPromoteTenuredRegionsTask task(&regions);
  ShenandoahHeap::heap()->workers()->run_task(&task);
  _used = task._used;
}

void ShenandoahYoungGeneration::promote_all_regions() {
  // This only happens on a full stw collect. No allocations can happen here.
  shenandoah_assert_safepoint();

  ShenandoahHeap* heap = ShenandoahHeap::heap();
  for (size_t index = 0; index < heap->num_regions(); index++) {
    ShenandoahHeapRegion* r = heap->get_region(index);
    if (r->is_young()) {
#ifdef TRACE_PROMOTION
      printf("promote_all_regions(), setting region (%llx, %llx, %llx) to OLD_GENERATION\n",
             (unsigned long long) r->bottom(), (unsigned long long) r->top(), (unsigned long long) r->end());
      fflush(stdout);
#endif
      r->promote();
    }
  }
  assert(_affiliated_region_count == 0, "young generation must not have affiliated regions after reset");
  _used = 0;

  // HEY! Better to use a service of ShenandoahScanRemembered for the following.

  // We can clear the entire card table here because we've just promoted all
  // young regions to old, so there can be no old->young pointers at this point.
  ShenandoahBarrierSet::barrier_set()->card_table()->clear();
}

bool ShenandoahYoungGeneration::contains(ShenandoahHeapRegion* region) const {
  return region->affiliation() != OLD_GENERATION;
}

void ShenandoahYoungGeneration::parallel_heap_region_iterate(ShenandoahHeapRegionClosure* cl) {
  if (_old_gen_task_queues != NULL) {
    // No generation filter on regions, we need to iterate all the regions.
    ShenandoahHeap::heap()->parallel_heap_region_iterate(cl);
  } else {
    // Just the young generations here.
    ShenandoahGenerationRegionClosure<YOUNG> young_regions(cl);
    ShenandoahHeap::heap()->parallel_heap_region_iterate(&young_regions);
  }
}

bool ShenandoahYoungGeneration::is_concurrent_mark_in_progress() {
  return ShenandoahHeap::heap()->is_concurrent_young_mark_in_progress();
}

void ShenandoahYoungGeneration::reserve_task_queues(uint workers) {
  ShenandoahGeneration::reserve_task_queues(workers);
  if (_old_gen_task_queues != NULL) {
    _old_gen_task_queues->reserve(workers);
  }
}

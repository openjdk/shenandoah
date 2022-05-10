/*
 * Copyright (c) 2018, 2019, Red Hat, Inc. All rights reserved.
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

#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahSATBMarkQueueSet.hpp"
#include "gc/shenandoah/shenandoahThreadLocalData.hpp"

ShenandoahSATBMarkQueueSet::ShenandoahSATBMarkQueueSet(BufferNode::Allocator* allocator) :
  SATBMarkQueueSet(allocator)
{}

SATBMarkQueue& ShenandoahSATBMarkQueueSet::satb_queue_for_thread(Thread* const t) const {
  return ShenandoahThreadLocalData::satb_mark_queue(t);
}

class ShenandoahSATBMarkQueueFilterFn {
  ShenandoahHeap* const _heap;

public:
  ShenandoahSATBMarkQueueFilterFn(ShenandoahHeap* heap) : _heap(heap) {}

  // Return true if entry should be filtered out (removed), false if
  // it should be retained.
  bool operator()(const void* entry) const {
    if (!_heap->requires_marking(entry)) {
      // The object is already marked, don't need to mark it again
      return true;
    }

    if (_heap->mode()->is_generational() && _heap->is_concurrent_old_mark_in_progress()) {
      // The SATB barrier is left on during concurrent marking of the old generation.
      // It is possible for the barrier to record an address of a reachable object
      // which exists in the collection set of a young generation collection. Once the
      // object is evacuated, the address left in the SATB buffer is no longer valid (or safe).
      ShenandoahHeapRegion* region = _heap->heap_region_containing(entry);
      return !region->is_active() || region->is_cset();
    }

    // Keep this object in the SATB buffer.
    return false;
  }
};

void ShenandoahSATBMarkQueueSet::filter(SATBMarkQueue& queue) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  apply_filter(ShenandoahSATBMarkQueueFilterFn(heap), queue);
}

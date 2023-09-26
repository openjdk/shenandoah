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
#undef KELVIN_SCRUTINY
#ifdef KELVIN_SCRUTINY
    bool result = !_heap->requires_marking(entry);
    log_info(gc)("ShenSATBMarkQueueFilterFn::filter_out function looking at entry: " PTR_FORMAT ", returning %s",
                 p2i(entry), result? "true": "false");
    return result;
#endif
    // We filter out all references to regions that are to be promoted in place.  At the time we select a region for promotion
    // in place (final mark), the region has been entirely marked and no further changes to the marking state are allowed.
    // After the region is promoted in place (concurrently, during evacuation), there may be new allocations above TAMS.
    // Since these reside above TAMS, they are implicitly marked and do not need to be marked again by SATB.
    //
    // Purging promote-in-place regions is more than a performance optimization.  It is essential for correct operation.
    // The processing of Reference objects may overwrite references to dead objects residing in a promoted-in-place region
    // with nullptr, and the SATB write barrier may in certain cases cause the reference to the dead object to be placed
    // in to the SATB buffer.  This is a complication that occurs only when old-gen marking is concurrently active while
    // evacuating and updating references during a young-gen collection.
    //
    // TOOD: isolate this genshen code from single-generation code.
    if (_heap->mode()->is_generational()) {
      return (_heap->is_in(entry) && _heap->heap_region_containing(entry)->get_top_before_promote() != nullptr) ||
        !_heap->requires_marking(entry);
    } else {
      return !_heap->requires_marking(entry);
    }
  }
};

void ShenandoahSATBMarkQueueSet::filter(SATBMarkQueue& queue) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  apply_filter(ShenandoahSATBMarkQueueFilterFn(heap), queue);
}

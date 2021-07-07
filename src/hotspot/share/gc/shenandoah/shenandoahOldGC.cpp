/*
 * Copyright (c) 2021, Amazon.com, Inc. or its affiliates. All rights reserved.
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

#include "gc/shenandoah/shenandoahFreeSet.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahMonitoringSupport.hpp"
#include "gc/shenandoah/shenandoahOldGC.hpp"
#include "gc/shenandoah/shenandoahOopClosures.inline.hpp"
#include "gc/shenandoah/shenandoahGeneration.hpp"
#include "gc/shenandoah/heuristics/shenandoahHeuristics.hpp"
#include "gc/shenandoah/shenandoahWorkerPolicy.hpp"
#include "utilities/events.hpp"

ShenandoahOldGC::ShenandoahOldGC(ShenandoahGeneration* generation, ShenandoahSharedFlag& allow_preemption) :
  ShenandoahConcurrentGC(generation), _allow_preemption(allow_preemption) {
}

void ShenandoahOldGC::entry_old_evacuations() {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  ShenandoahOldHeuristics* old_heuristics = heap->old_heuristics();
  old_heuristics->start_old_evacuations();
}

bool ShenandoahOldGC::collect(GCCause::Cause cause) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();

  // Continue concurrent mark, do not reset regions, do not mark roots, do not collect $200.
  _allow_preemption.set();
  entry_mark();
  _allow_preemption.unset();
  if (check_cancellation_and_abort(ShenandoahDegenPoint::_degenerated_mark)) return false;

  // Complete marking under STW
  vmop_entry_final_mark();

  entry_old_evacuations();

  // We aren't dealing with old generation evacuation yet. Our heuristic
  // should not have built a cset in final mark.
  assert(!heap->is_evacuation_in_progress(), "Old gen evacuations are not supported");

  {
    ShenandoahHeapLocker locker(heap->lock());
    heap->free_set()->log_status();
  }

  // Processing strong roots
  // This may be skipped if there is nothing to update/evacuate.
  // If so, strong_root_in_progress would be unset.
  if (heap->is_concurrent_strong_root_in_progress()) {
    entry_strong_roots();
  }

  entry_rendezvous_roots();
  return true;
}

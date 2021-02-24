/*
 * Copyright (c), Amazon.com, Inc. and/or its affiliates. All rights reserved.
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

#include "gc/shenandoah/heuristics/shenandoahHeuristics.hpp"
#include "gc/shenandoah/mode/shenandoahMode.hpp"
#include "gc/shenandoah/shenandoahControlThread.hpp"
#include "gc/shenandoah/shenandoahGeneration.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahRegulatorThread.hpp"
#include "gc/shenandoah/shenandoahYoungGeneration.hpp"
#include "logging/log.hpp"

static ShenandoahHeuristics* get_heuristics(ShenandoahGeneration* nullable) {
  return nullable != NULL ? nullable->heuristics() : NULL;
}

ShenandoahRegulatorThread::ShenandoahRegulatorThread(ShenandoahControlThread* control_thread) :
  ConcurrentGCThread(),
  _control_thread(control_thread),
  _last_young_cycle(0),
  _last_old_cycle(0),
  _last_cycle(0),
  _sleep(ShenandoahControlIntervalMin),
  _last_sleep_adjust_time(os::elapsedTime()) {

  ShenandoahHeap* heap = ShenandoahHeap::heap();
  _old_heuristics = get_heuristics(heap->old_generation());
  _young_heuristics = get_heuristics(heap->young_generation());
  _global_heuristics = get_heuristics(heap->global_generation());

  create_and_start();
}

void ShenandoahRegulatorThread::run_service() {
  if (ShenandoahHeap::heap()->mode()->is_generational()) {
    if (ShenandoahAllowOldMarkingPreemption) {
      regulate_concurrent_cycles();
    } else {
      regulate_interleaved_cycles();
    }
  } else {
    regulate_heap();
  }

  log_info(gc)("%s: Done.", name());
}

void ShenandoahRegulatorThread::regulate_concurrent_cycles() {
  assert(_young_heuristics != NULL, "Need young heuristics.");
  assert(_old_heuristics != NULL, "Need old heuristics.");

  while (!should_terminate()) {
    if (should_start_old_cycle()) {
      log_info(gc)("Heuristics requesting old collection.");
      _last_old_cycle = _control_thread->get_gc_id();
      _control_thread->request_concurrent_gc(OLD);
    } else if (should_start_young_cycle()) {
      log_info(gc)("Heuristics requesting young collection.");
      _last_young_cycle = _control_thread->get_gc_id();
      _control_thread->request_concurrent_gc(YOUNG);
    }

    regulator_sleep();
  }
}

void ShenandoahRegulatorThread::regulate_interleaved_cycles() {
  assert(_young_heuristics != NULL, "Need young heuristics.");
  assert(_global_heuristics != NULL, "Need global heuristics.");

  while (!should_terminate()) {
    if (should_start_cycle(_young_heuristics, _last_cycle)) {
      log_info(gc)("Heuristics requesting young collection.");
      _last_cycle = _control_thread->get_gc_id();
      _control_thread->request_concurrent_gc(YOUNG);
    } else if (should_start_cycle(_global_heuristics, _last_cycle)) {
      log_info(gc)("Heuristics requesting global collection.");
      _last_cycle = _control_thread->get_gc_id();
      _control_thread->request_concurrent_gc(GLOBAL);
    }

    regulator_sleep();
  }
}

void ShenandoahRegulatorThread::regulate_heap() {
  assert(_global_heuristics != NULL, "Need global heuristics.");

  while (!should_terminate()) {
    if (should_start_cycle(_global_heuristics, _last_cycle)) {
      _last_cycle = _control_thread->get_gc_id();
      _control_thread->request_concurrent_gc(GLOBAL);
    }

    regulator_sleep();
  }
}

void ShenandoahRegulatorThread::regulator_sleep() {
  // Wait before performing the next action. If allocation happened during this wait,
  // we exit sooner, to let heuristics re-evaluate new conditions. If we are at idle,
  // back off exponentially.
  double current = os::elapsedTime();

  if (_heap_changed.try_unset()) {
    _sleep = ShenandoahControlIntervalMin;
  } else if ((current - _last_sleep_adjust_time) * 1000 > ShenandoahControlIntervalAdjustPeriod){
    _sleep = MIN2<int>(ShenandoahControlIntervalMax, MAX2(1, _sleep * 2));
    _last_sleep_adjust_time = current;
  }

  os::naked_short_sleep(_sleep);
}

bool ShenandoahRegulatorThread::should_start_young_cycle() {
  return ShenandoahHeap::heap()->mode()->is_generational()
      && should_start_cycle(_young_heuristics, _last_young_cycle);
}

bool ShenandoahRegulatorThread::should_start_old_cycle() {
  return should_start_cycle(_old_heuristics, _last_old_cycle);
}

bool ShenandoahRegulatorThread::should_start_cycle(ShenandoahHeuristics* heuristics, size_t last_cycle_started) {
  // We want to hold the last heuristic down so that it doesn't repeatedly try to start
  // a cycle. We might consider invoking 'should_start_gc' because in some cases it has
  // side effects (like sampling the allocation rate). On the other hand, the right thing™
  // to do is probably factor allocation rate sampling outside of heuristics evaluation.
  return (last_cycle_started == 0 || _control_thread->get_gc_id() > last_cycle_started)
      && heuristics->should_start_gc();
}

void ShenandoahRegulatorThread::stop_service() {
  log_info(gc)("%s: Stop requested.", name());
}


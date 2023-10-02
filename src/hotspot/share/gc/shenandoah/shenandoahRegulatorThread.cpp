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
#include "precompiled.hpp"

#undef KELVIN_TRACE

#include "gc/shenandoah/heuristics/shenandoahHeuristics.hpp"
#include "gc/shenandoah/mode/shenandoahMode.hpp"
#include "gc/shenandoah/shenandoahControlThread.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahOldGeneration.hpp"
#include "gc/shenandoah/shenandoahRegulatorThread.hpp"
#include "gc/shenandoah/shenandoahYoungGeneration.hpp"
#ifdef KELVIN_TRACE
#include "gc/shenandoah/heuristics/shenandoahAdaptiveHeuristics.hpp"
#endif
#include "logging/log.hpp"

static ShenandoahHeuristics* get_heuristics(ShenandoahGeneration* nullable) {
  return nullable != nullptr ? nullable->heuristics() : nullptr;
}

ShenandoahRegulatorThread::ShenandoahRegulatorThread(ShenandoahControlThread* control_thread) :
  ConcurrentGCThread(),
  _control_thread(control_thread),
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

#ifdef KELVIN_TRACE
static double _most_recent_timestamp;
static double _next_sleep_interval;
#endif

void ShenandoahRegulatorThread::regulate_concurrent_cycles() {
  assert(_young_heuristics != nullptr, "Need young heuristics.");
  assert(_old_heuristics != nullptr, "Need old heuristics.");
#undef KELVIN_EXTERNAL_TRACE
#ifdef KELVIN_EXTERNAL_TRACE
  ShenandoahAdaptiveHeuristics* adaptive_heuristics =
         (ShenandoahAdaptiveHeuristics*)ShenandoahHeap::heap()->young_heuristics();
#endif

  while (!should_terminate()) {
    ShenandoahControlThread::GCMode mode = _control_thread->gc_mode();
    if (mode == ShenandoahControlThread::none) {
      if (should_unload_classes()) {
        if (request_concurrent_gc(ShenandoahControlThread::select_global_generation())) {
          log_info(gc)("Heuristics request for global (unload classes) accepted.");
        }
      } else {
#ifdef KELVIN_EXTERNAL_TRACE
        adaptive_heuristics->timestamp_for_sample(_most_recent_timestamp, _next_sleep_interval);
#endif
        // TODO: there may be a race that results in deadlock or livelock over the ShenandoahControlThread::_regulator_lock.
        // We need to DEBUG this.  Could it be that on rare occasion, the V() is performed before the P() operation, and
        // thus the P() operation never gets released?  In one 20 minute execution of an Extremem workload, the last
        // heuristic request was accepted at time 559.076s, and this was 2.261s after sleeping 1ms following the previous
        // invocation of regulator_sleep(), which occurred at time 557.979.  After this, no more heuristics requests were
        // accepted during the remaining 700s of execution.  Rather, we limped along, repeatedly ignoring heuristics requests
        // until we experienced allocation failures, at which point we would perform degen or full GCs.

        // Give priority to starting young cycles.  If both old and young cycle are ready to run, starting the
        // old cycle first is counterproductive, because it will be immediately preempted.  On typical hosts, this
        // would result in 10 ms of context-switching overhead and less than 1 ms of old execution time.  In some
        // cases, it results in much more than 10 ms of context-switching overhead (and delays in the start of young,
        // which may result in degenerated and full gc cycles).  This effect can be exacerbated if old-gen cannot be
        // "immediately" preempted.  We have observed very rare delays of over 500 ms (some even longer than 2s) in
        // the processing of these context switch requests.

        // Because we are giving priority to young cycles over old, it is possible that we may starve old entirely.
        // TODO: It may be worthwhile to force a 10ms old timeslice once out of every ten young-cycle dispatches.
        // Similarly, we may want to force an old-gen cycle that was not forced (because young trigger was not active
        // when old wanted to fire) to run a minimum of 20 ms.  The code as is seems to run well with workloads that
        // have been tested, so I'm not introducing this change into the current patch.  Here is what the refinement
        // might look like:
        //
        //   int cycles_until_forced_old = 10;
        //   while (!should_terminate()) {
        //     if (should_unload_classes() {
        //       ...
        //     } else {
        //       if (cycles_until_forced_old-- == 0) {
        //         cycles_until_forced_old = 10;
        //         if (start_old_cycle()) {
        //           log_info(gc)("Heuristics request for forced old collection accepted");
        //           os::naked_short_sleep(10.0);  // Let the old-gc run for 10 ms before triggering its preemption
        //           continue;                     // This is not ideal, because old-gc might finish before 10 ms,
        //                                         //   but this would very rare, only on final increment of old gc effort.
        //         }
        //         // else, old cycle is not required, so fall through to normal control
        //       }
        //       if (start_young_cycle()) { ... }
        //       else if (start_old_cycle()) {
        //         // code as before, but add
        //         os::naked_short_sleep(20.0);   // Let this old-gc run for 20 ms before triggering its preemption
        //         cycles_until_forced_old = 20;  // because we just got 20 ms of execution time, we can delay longer before forced
        //       }

        if (start_young_cycle()) {
#ifdef KELVIN_TRACE
          log_info(gc)("Acceptance after sleeping %.3f following timestamp %.3f", _next_sleep_interval, _most_recent_timestamp);
#endif
          log_info(gc)("Heuristics request for young collection accepted");
        } else if (start_old_cycle()) {
#ifdef KELVIN_TRACE
          log_info(gc)("Acceptance after sleeping %.3f following timestamp %.3f", _next_sleep_interval, _most_recent_timestamp);
#endif
          log_info(gc)("Heuristics request for old collection accepted");
        }
      }
    } else if (mode == ShenandoahControlThread::servicing_old) {
#ifdef KELVIN_EXTERNAL_TRACE
      adaptive_heuristics->timestamp_for_sample(_most_recent_timestamp, _next_sleep_interval);
#endif
      if (start_young_cycle()) {
#ifdef KELVIN_TRACE
        log_info(gc)("Acceptance after sleeping %.3f following timestamp %.3f", _next_sleep_interval, _most_recent_timestamp);
#endif
        log_info(gc)("Heuristics request to interrupt old for young collection accepted");
      }
    }

    regulator_sleep();
  }
}

void ShenandoahRegulatorThread::regulate_interleaved_cycles() {
  assert(_young_heuristics != nullptr, "Need young heuristics.");
  assert(_global_heuristics != nullptr, "Need global heuristics.");

  while (!should_terminate()) {
    if (_control_thread->gc_mode() == ShenandoahControlThread::none) {
      if (start_global_cycle()) {
        log_info(gc)("Heuristics request for global collection accepted.");
      } else if (start_young_cycle()) {
        log_info(gc)("Heuristics request for young collection accepted.");
      }
    }

    regulator_sleep();
  }
}

void ShenandoahRegulatorThread::regulate_heap() {
  assert(_global_heuristics != nullptr, "Need global heuristics.");

  while (!should_terminate()) {
    if (_control_thread->gc_mode() == ShenandoahControlThread::none) {
      if (start_global_cycle()) {
        log_info(gc)("Heuristics request for global collection accepted.");
      }
    }

    regulator_sleep();
  }
}

void ShenandoahRegulatorThread::regulator_sleep() {
  // Wait before performing the next action. If allocation happened during this wait,
  // we exit sooner, to let heuristics re-evaluate new conditions. If we are at idle,
  // back off exponentially.
  double current = os::elapsedTime();
#ifdef KELVIN_TRACE
  _most_recent_timestamp = current;
#endif
  if (_heap_changed.try_unset()) {
    _sleep = ShenandoahControlIntervalMin;
  } else if ((current - _last_sleep_adjust_time) * 1000 > ShenandoahControlIntervalAdjustPeriod){
    _sleep = MIN2<int>(ShenandoahControlIntervalMax, MAX2(1, _sleep * 2));
    _last_sleep_adjust_time = current;
  }
#ifdef KELVIN_TRACE
  _next_sleep_interval = _sleep;
#endif

  os::naked_short_sleep(_sleep);
  if (LogTarget(Debug, gc, thread)::is_enabled()) {
    double elapsed = os::elapsedTime() - current;
    double hiccup = elapsed - double(_sleep);
    if (hiccup > 0.001) {
      log_debug(gc, thread)("Regulator hiccup time: %.3fs", hiccup);
    }
  }
}

bool ShenandoahRegulatorThread::start_old_cycle() {
  // TODO: These first two checks might be vestigial
  return !ShenandoahHeap::heap()->doing_mixed_evacuations()
      && !ShenandoahHeap::heap()->collection_set()->has_old_regions()
      && _old_heuristics->should_start_gc()
      && request_concurrent_gc(OLD);
}

bool ShenandoahRegulatorThread::request_concurrent_gc(ShenandoahGenerationType generation) {
  double now = os::elapsedTime();
  bool accepted = _control_thread->request_concurrent_gc(generation);
  if (LogTarget(Debug, gc, thread)::is_enabled() && accepted) {
    double wait_time = os::elapsedTime() - now;
    if (wait_time > 0.001) {
      log_debug(gc, thread)("Regulator waited %.3fs for control thread to acknowledge request.", wait_time);
    }
  }
  return accepted;
}

bool ShenandoahRegulatorThread::start_young_cycle() {
  return _young_heuristics->should_start_gc() && request_concurrent_gc(YOUNG);
}

bool ShenandoahRegulatorThread::start_global_cycle() {
  return _global_heuristics->should_start_gc() && request_concurrent_gc(ShenandoahControlThread::select_global_generation());
}

void ShenandoahRegulatorThread::stop_service() {
  log_info(gc)("%s: Stop requested.", name());
}

bool ShenandoahRegulatorThread::should_unload_classes() {
  // The heuristics delegate this decision to the collector policy, which is based on the number
  // of cycles started.
  return _global_heuristics->should_unload_classes();
}


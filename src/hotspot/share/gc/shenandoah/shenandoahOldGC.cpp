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
#include "prims/jvmtiTagMap.hpp"
#include "utilities/events.hpp"

class ShenandoahConcurrentCoalesceAndFillTask : public AbstractGangTask {
private:
  uint _nworkers;
  ShenandoahHeapRegion** _coalesce_and_fill_region_array;
  uint _coalesce_and_fill_region_count;
  ShenandoahConcurrentGC* _old_gc;
  volatile bool _is_preempted;

public:
  ShenandoahConcurrentCoalesceAndFillTask(uint nworkers, ShenandoahHeapRegion** coalesce_and_fill_region_array,
                                          uint region_count, ShenandoahConcurrentGC* old_gc) :
    AbstractGangTask("Shenandoah Concurrent Coalesce and Fill"),
    _nworkers(nworkers),
    _coalesce_and_fill_region_array(coalesce_and_fill_region_array),
    _coalesce_and_fill_region_count(region_count),
    _old_gc(old_gc),
    _is_preempted(false) {
  }

  void work(uint worker_id) {
    for (uint region_idx = worker_id; region_idx < _coalesce_and_fill_region_count; region_idx += _nworkers) {
      ShenandoahHeapRegion* r = _coalesce_and_fill_region_array[region_idx];
#undef KELVIN_VERBOSE
#ifdef KELVIN_VERBOSE
      printf("CFTask worker %u of %u looking at region %lu fetched from index %u\n", worker_id, _nworkers, r->index(), region_idx);
#endif
      if (!r->is_humongous()) {
        if (!r->oop_fill_and_coalesce()) {
          // Coalesce and fill has been preempted
          Atomic::store(&_is_preempted, true);
#ifdef KELVIN_VERBOSE
          printf("Abandoning CalesceAndFillTask::work() because worker %d was preempted at region_idx %d\n", worker_id, region_idx);
#endif
          return;
        }
      } else {
        // there's only one object in this region and it's not garbage, so no need to coalesce or fill
      }
    }
  }

  // Value returned from is_completed() is only valid after all worker thread have terminated.
  bool is_completed() {
    return !Atomic::load(&_is_preempted);
  }
};


ShenandoahOldGC::ShenandoahOldGC(ShenandoahGeneration* generation, ShenandoahSharedFlag& allow_preemption) :
    ShenandoahConcurrentGC(generation, false), _allow_preemption(allow_preemption) {
  _coalesce_and_fill_region_array = NEW_C_HEAP_ARRAY(ShenandoahHeapRegion*, ShenandoahHeap::heap()->num_regions(), mtGC);
}

void ShenandoahOldGC::entry_old_evacuations() {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  ShenandoahOldHeuristics* old_heuristics = heap->old_heuristics();
  old_heuristics->start_old_evacuations();
}


// Final mark for old-gen is different than for young or old, so we
// override the implementation.
void ShenandoahOldGC::op_final_mark() {

  ShenandoahHeap* const heap = ShenandoahHeap::heap();
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Should be at safepoint");
  assert(!heap->has_forwarded_objects(), "No forwarded objects on this path");

  if (ShenandoahVerify) {
    heap->verifier()->verify_roots_no_forwarded();
  }

  if (!heap->cancelled_gc()) {
    assert(_mark.generation()->generation_mode() == OLD, "Generation of Old-Gen GC should be OLD");
    _mark.finish_mark();
    assert(!heap->cancelled_gc(), "STW mark cannot OOM");

    // We need to do this because weak root cleaning reports the number of dead handles
    JvmtiTagMap::set_needs_cleaning();

    _generation->prepare_regions_and_collection_set(true);

    heap->set_unload_classes(false);
    heap->prepare_concurrent_roots();

    // Believe verification following old-gen concurrent mark needs to be different than verification following
    // young-gen concurrent mark, so am commenting this out for now:
    //   if (ShenandoahVerify) {
    //     heap->verifier()->verify_after_concmark();
    //   }

    if (VerifyAfterGC) {
      Universe::verify();
    }
  }
}

bool ShenandoahOldGC::collect(GCCause::Cause cause) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();

#undef KELVIN_VERBOSE
#ifdef KELVIN_VERBOSE
  printf("Starting or resuming ShenandoahOldGC::collect(), cause: %s\n", GCCause::to_string(cause));
  printf("  is_concurrent_weak_root_in_progress? %s\n", heap->is_concurrent_weak_root_in_progress()? "Yes": "No");
  printf("           is_evacuationn_in_progress? %s\n", heap->is_concurrent_weak_root_in_progress()? "Yes": "No");
#endif

  if (!heap->is_concurrent_prep_for_mixed_evacuation_in_progress()) {
    // Skip over the initial phases of old collect if we're resuming mixed evacuation preparation.

#ifdef KELVIN_VERBOSE
    printf("ShenandoahOldGC::collect() is_concurrent_prep_for_mixed_evacuation is not true\n");
    printf("  is_concurrent_weak_root_in_progress? %s\n", heap->is_concurrent_weak_root_in_progress()? "Yes": "No");
    printf("           is_evacuationn_in_progress? %s\n", heap->is_concurrent_weak_root_in_progress()? "Yes": "No");
    fflush(stdout);
#endif

    // Continue concurrent mark, do not reset regions, do not mark roots, do not collect $200.
    _allow_preemption.set();
    entry_mark();
    if (!_allow_preemption.try_unset()) {
      // The regulator thread has unset the preemption guard. That thread will shortly cancel
      // the gc, but the control thread is now racing it. Wait until this thread sees the cancellation.
      while (!heap->cancelled_gc()) {
        SpinPause();
      }
    }

    if (check_cancellation_and_abort(ShenandoahDegenPoint::_degenerated_mark)) {
#ifdef KELVIN_VERBOSE
      printf("ShenandoahOldGC::collect() old-gen marking was preempted, so gc.collect() is returning false\n");
      printf("  is_concurrent_weak_root_in_progress? %s\n", heap->is_concurrent_weak_root_in_progress()? "Yes": "No");
      printf("           is_evacuationn_in_progress? %s\n", heap->is_concurrent_weak_root_in_progress()? "Yes": "No");
#endif
      return false;
    }

    // Complete marking under STW
    vmop_entry_final_mark();

#ifdef KELVIN_VERBOSE
      printf("ShenandoahOldGC::collect() finished final_mark\n");
      printf("  is_concurrent_weak_root_in_progress? %s\n", heap->is_concurrent_weak_root_in_progress()? "Yes": "No");
      printf("           is_evacuationn_in_progress? %s\n", heap->is_concurrent_weak_root_in_progress()? "Yes": "No");
#endif

    // We aren't dealing with old generation evacuation yet. Our heuristic
    // should not have built a cset in final mark.
    assert(!heap->is_evacuation_in_progress(), "Old gen evacuations are not supported");

    // Process weak roots that might still point to regions that would be broken by cleanup
    if (heap->is_concurrent_weak_root_in_progress()) {
      entry_weak_refs();
      entry_weak_roots();
    }

#ifdef KELVIN_VERBOSE
    printf("ShenandoahOldGC::collect() did weak refs and weak roots\n");
    printf("  is_concurrent_weak_root_in_progress? %s\n", heap->is_concurrent_weak_root_in_progress()? "Yes": "No");
    printf("           is_evacuationn_in_progress? %s\n", heap->is_concurrent_weak_root_in_progress()? "Yes": "No");
#endif

    // Final mark might have reclaimed some immediate garbage, kick cleanup to reclaim
    // the space. This would be the last action if there is nothing to evacuate.
    entry_cleanup_early();

#ifdef KELVIN_VERBOSE
    printf("ShenandoahOldGC::collect() did entry cleanup early\n");
    printf("  is_concurrent_weak_root_in_progress? %s\n", heap->is_concurrent_weak_root_in_progress()? "Yes": "No");
    printf("           is_evacuationn_in_progress? %s\n", heap->is_concurrent_weak_root_in_progress()? "Yes": "No");
#endif

    {
      ShenandoahHeapLocker locker(heap->lock());
      heap->free_set()->log_status();
    }


    // TODO: Old marking doesn't support class unloading yet
    // Perform concurrent class unloading
    // if (heap->unload_classes() &&
    //     heap->is_concurrent_weak_root_in_progress()) {
    //   entry_class_unloading();
    // }

    heap->set_concurrent_prep_for_mixed_evacuation_in_progress(true);
#ifdef KELVIN_VERBOSE
    printf("ShenandoahOldGC::collect() starting prep for mixed evac with %d regions to be coalesced and filled\n",
           heap->old_heuristics()->old_coalesce_and_fill_candidates());
    printf("  is_concurrent_weak_root_in_progress? %s\n", heap->is_concurrent_weak_root_in_progress()? "Yes": "No");
    printf("           is_evacuationn_in_progress? %s\n", heap->is_concurrent_weak_root_in_progress()? "Yes": "No");
    fflush(stdout);
#endif
  }

  // Coalesce and fill objects _after_ weak root processing and class unloading.
  // Weak root and reference processing makes assertions about unmarked referents
  // that will fail if they've been overwritten with filler objects. There is also
  // a case in the LRB that permits access to from-space objects for the purpose
  // of class unloading that is unlikely to function correctly if the object has
  // been filled.

  _allow_preemption.set();

  if (check_cancellation_and_abort(ShenandoahDegenPoint::_degenerated_evac)) {
#ifdef KELVIN_VERBOSE
  printf("ShenandoahOldGC::collect() has abandoned all hope because check_cancellation_and_abort()!\n");
  printf("  is_concurrent_weak_root_in_progress? %s\n", heap->is_concurrent_weak_root_in_progress()? "Yes": "No");
  printf("           is_evacuationn_in_progress? %s\n", heap->is_concurrent_weak_root_in_progress()? "Yes": "No");
  fflush(stdout);
#endif
    return false;
  }

  // Prepare for old evacuations (actual evacuations will happen on subsequent young collects).
  entry_old_evacuations();

#ifdef KELVIN_VERBOSE
  printf("ShenandoahOldGC::collect() is back from entry_old_evacuations\n");
  printf("  is_concurrent_weak_root_in_progress? %s\n", heap->is_concurrent_weak_root_in_progress()? "Yes": "No");
  printf("           is_evacuationn_in_progress? %s\n", heap->is_concurrent_weak_root_in_progress()? "Yes": "No");
  fflush(stdout);
#endif

  assert(!heap->is_concurrent_strong_root_in_progress(), "No evacuations during old gc.");

  vmop_entry_final_roots();

#ifdef KELVIN_VERBOSE
  printf("ShenandoahOldGC::collect() is done with vmop_entry_final_roots()\n");
  printf("  is_concurrent_weak_root_in_progress? %s\n", heap->is_concurrent_weak_root_in_progress()? "Yes": "No");
  printf("           is_evacuationn_in_progress? %s\n", heap->is_concurrent_weak_root_in_progress()? "Yes": "No");
  fflush(stdout);
#endif

  if (heap->is_concurrent_prep_for_mixed_evacuation_in_progress()) {
    if (!entry_coalesce_and_fill()) {
#ifdef KELVIN_VERBOSE
      printf("ShenandoahOldGC::collect() entry_coalesce_and_fill() was preempted\n");
      printf("  is_concurrent_weak_root_in_progress? %s\n", heap->is_concurrent_weak_root_in_progress()? "Yes": "No");
      printf("           is_evacuationn_in_progress? %s\n", heap->is_concurrent_weak_root_in_progress()? "Yes": "No");
      fflush(stdout);
#endif
      // If old-gen degenerates instead of resuming, we'll just start up an out-of-cycle degenerated GC.
      // This should be a rare event.  Normally, we'll resume the coalesce-and-fill effort after the
      // preempting young-gen GC finishes.
      check_cancellation_and_abort(ShenandoahDegenPoint::_degenerated_outside_cycle);
      return false;
    }
  }
#ifdef KELVIN_VERBOSE
  printf("ShenandoahOldGC::collect() is done with coalesce_and_fill\n");
  fflush(stdout);
#endif
  if (!_allow_preemption.try_unset()) {
    // The regulator thread has unset the preemption guard. That thread will shortly cancel
    // the gc, but the control thread is now racing it. Wait until this thread sees the cancellation.
    while (!heap->cancelled_gc()) {
      SpinPause();
    }
  }
#ifdef KELVIN_VERBOSE
  printf("ShenandoahOldGC::collect() has unset allow_preemption\n");
  printf("  is_concurrent_weak_root_in_progress? %s\n", heap->is_concurrent_weak_root_in_progress()? "Yes": "No");
  printf("           is_evacuationn_in_progress? %s\n", heap->is_concurrent_weak_root_in_progress()? "Yes": "No");
  fflush(stdout);
#endif
  return true;
}

void ShenandoahOldGC::entry_coalesce_and_fill_message(char *buf, size_t len) const {
  // ShenandoahHeap* const heap = ShenandoahHeap::heap();
  jio_snprintf(buf, len, "Coalescing and filling (%s)", _generation->name());
}

bool ShenandoahOldGC::op_coalesce_and_fill() {
  ShenandoahHeap* const heap = ShenandoahHeap::heap();
  ShenandoahOldHeuristics* old_heuristics = heap->old_heuristics();
  WorkGang* workers = heap->workers();
  uint nworkers = workers->active_workers();

  assert(_generation->generation_mode() == OLD, "Only old-GC does coalesce and fill");
  log_debug(gc)("Starting (or resuming) coalesce-and-fill of old heap regions");
  uint coalesce_and_fill_regions_count = old_heuristics->old_coalesce_and_fill_candidates();
#undef KELVIN_VERBOSE
#ifdef KELVIN_VERBOSE
  printf("Starting (or resuming) coalesce-and-fill of old heap regions, count is: %d\n", coalesce_and_fill_regions_count);
#endif
  assert(coalesce_and_fill_regions_count <= heap->num_regions(), "Sanity");
  old_heuristics->get_coalesce_and_fill_candidates(_coalesce_and_fill_region_array);
  ShenandoahConcurrentCoalesceAndFillTask task(nworkers, _coalesce_and_fill_region_array, coalesce_and_fill_regions_count, this);

  workers->run_task(&task);
  if (task.is_completed()) {
#ifdef KELVIN_VERBOSE
    printf("Finished with coalesce-and-fill of old heap regions\n");
#endif
    // Remember that we're done with coalesce-and-fill.
    heap->set_concurrent_prep_for_mixed_evacuation_in_progress(false);
    return true;
  } else {
#ifdef KELVIN_VERBOSE
    printf("Suspending coalesce-and-fill of old heap regions\n");
#endif
    log_debug(gc)("Suspending coalesce-and-fill of old heap regions");
    // Otherwise, we got preempted before the work was done.
    return false;
  }
}

bool ShenandoahOldGC::entry_coalesce_and_fill() {
  char msg[1024];
  ShenandoahHeap* const heap = ShenandoahHeap::heap();

  entry_coalesce_and_fill_message(msg, sizeof(msg));
  ShenandoahConcurrentPhase gc_phase(msg, ShenandoahPhaseTimings::coalesce_and_fill);

  TraceCollectorStats tcs(heap->monitoring_support()->concurrent_collection_counters());
  EventMark em("%s", msg);
  ShenandoahWorkerScope scope(heap->workers(),
                              ShenandoahWorkerPolicy::calc_workers_for_conc_marking(),
                              "concurrent coalesce and fill");

  return op_coalesce_and_fill();
}

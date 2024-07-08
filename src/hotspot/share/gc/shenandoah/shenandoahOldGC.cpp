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

#include "gc/shenandoah/heuristics/shenandoahYoungHeuristics.hpp"
#include "gc/shenandoah/shenandoahFreeSet.hpp"
#include "gc/shenandoah/shenandoahGenerationalHeap.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahMonitoringSupport.hpp"
#include "gc/shenandoah/shenandoahOldGC.hpp"
#include "gc/shenandoah/shenandoahOopClosures.inline.hpp"
#include "gc/shenandoah/shenandoahGeneration.hpp"
#include "gc/shenandoah/shenandoahOldGeneration.hpp"
#include "gc/shenandoah/shenandoahYoungGeneration.hpp"
#include "prims/jvmtiTagMap.hpp"
#include "utilities/events.hpp"


ShenandoahOldGC::ShenandoahOldGC(ShenandoahOldGeneration* generation, ShenandoahSharedFlag& allow_preemption) :
    ShenandoahConcurrentGC(generation, false), _old_generation(generation), _allow_preemption(allow_preemption) {
}

// Final mark for old-gen is different than for young or old, so we
// override the implementation.
void ShenandoahOldGC::op_final_mark() {

  ShenandoahGenerationalHeap* const heap = ShenandoahGenerationalHeap::heap();
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Should be at safepoint");
  assert(!heap->has_forwarded_objects(), "No forwarded objects on this path");

  if (ShenandoahVerify) {
    heap->verifier()->verify_roots_no_forwarded();
  }

  if (!heap->cancelled_gc()) {
    assert(_mark.generation()->is_old(), "Generation of Old-Gen GC should be OLD");
    _mark.finish_mark();
    assert(!heap->cancelled_gc(), "STW mark cannot OOM");

    // Old collection is complete, the young generation no longer needs this
    // reference to the old concurrent mark so clean it up.
    heap->young_generation()->set_old_gen_task_queues(nullptr);

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
  auto heap = ShenandoahGenerationalHeap::heap();
  assert(!_old_generation->is_doing_mixed_evacuations(), "Should not start an old gc with pending mixed evacuations");
  assert(!_old_generation->is_preparing_for_mark(), "Old regions need to be parsable during concurrent mark.");

  // Enable preemption of old generation mark.
  _allow_preemption.set();

  // Continue concurrent mark, do not reset regions, do not mark roots, do not collect $200.
  entry_mark();

  // If we failed to unset the preemption flag, it means another thread has already unset it.
  if (!_allow_preemption.try_unset()) {
    // The regulator thread has unset the preemption guard. That thread will shortly cancel
    // the gc, but the control thread is now racing it. Wait until this thread sees the
    // cancellation.
    while (!heap->cancelled_gc()) {
      SpinPause();
    }
  }

  if (heap->cancelled_gc()) {
    return false;
  }

  // Complete marking under STW
  vmop_entry_final_mark();

  if (_generation->is_concurrent_mark_in_progress()) {
    assert(heap->cancelled_gc(), "Safepoint operation observed gc cancellation");
    // GC may have been cancelled before final mark, but after the preceding cancellation check.
    return false;
  }

  // We aren't dealing with old generation evacuation yet. Our heuristic
  // should not have built a cset in final mark.
  assert(!heap->is_evacuation_in_progress(), "Old gen evacuations are not supported");

  // Process weak roots that might still point to regions that would be broken by cleanup
  if (heap->is_concurrent_weak_root_in_progress()) {
    entry_weak_refs();
    entry_weak_roots();
  }

  // Final mark might have reclaimed some immediate garbage, kick cleanup to reclaim
  // the space. This would be the last action if there is nothing to evacuate.
  entry_cleanup_early();

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


  assert(!heap->is_concurrent_strong_root_in_progress(), "No evacuations during old gc.");

  // We must execute this vm operation if we completed final mark. We cannot return from here with weak roots in progress.
  // This is not a valid gc state for any young collections (or allocation failures) that interrupt the old collection.
  // This will reclaim immediate garbage.  vmop_entry_final_roots() will also rebuild the free set.
  vmop_entry_final_roots();

#ifdef KELVIN_DEPRECATE
  // Deprecating because vmop_entry_final_roots() does the free-set rebuild.

  // After concurrent old marking finishes, we may be able to reclaim immediate garbage from regions that are fully garbage.
  // Furthermore, we may want to expand OLD in order to make room for the first mixed evacuation that immediately follows
  // completion of OLD marking.  This is why we rebuild free set here.
  ShenandoahGenerationalHeap::TransferResult result;
  {
    // Though we did not choose a collection set above, we still may have freed up immediate garbage regions so
    // proceed with rebuilding the free set.  A second reason to rebuild free set now is to prepare for mixed evacuations
    // which are likely to follow completion of old-gen marking.  Preparation for mixed evacuations likely involves
    // expansion of the old generation.

    // Old marking does not degenerate.  It is always concurrent.  In case of out-of-cycle memory allocation failures
    // while old marking is ongoing, we will degenerate to a young GC, which may, if necessary upgrade to Full GC.
    // If the young degenerated GC upgrades to full GC, concurrent old marking will be cancelled.
    ShenandoahHeapLocker locker(heap->lock());
    size_t young_cset_regions, old_cset_regions;
    size_t first_old, last_old, num_old;
    size_t allocation_runway = heap->young_generation()->heuristics()->bytes_of_allocation_runway_before_gc_trigger(0);
    heap->free_set()->prepare_to_rebuild(young_cset_regions, old_cset_regions, first_old, last_old, num_old);
    assert((young_cset_regions == 0) && (old_cset_regions == 0), "No ongoing evacuation when concurrent mark ends");
    heap->compute_old_generation_balance(allocation_runway, 0, 0);
    result = heap->balance_generations();
    heap->free_set()->finish_rebuild(0, 0, num_old);
  }

  LogTarget(Info, gc, ergo) lt;
  if (lt.is_enabled()) {
    LogStream ls(lt);
    result.print_on("Old Mark", &ls);
  }
#endif
  return true;
}

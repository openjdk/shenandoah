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

#include "gc/shenandoah/shenandoahCollectorPolicy.hpp"
#include "gc/shenandoah/shenandoahGenerationalHeap.hpp"
#include "gc/shenandoah/shenandoahGenerationalControlThread.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahInitLogger.hpp"
#include "gc/shenandoah/shenandoahMemoryPool.hpp"
#include "gc/shenandoah/shenandoahOldGeneration.hpp"
#include "gc/shenandoah/shenandoahRegulatorThread.hpp"
#include "gc/shenandoah/shenandoahYoungGeneration.hpp"
#include "gc/shenandoah/shenandoahUtils.hpp"
#include "logging/log.hpp"

class ShenandoahConcurrentEvacuator : public ObjectClosure {
private:
  ShenandoahGenerationalHeap* const _heap;
  Thread* const _thread;
public:
  explicit ShenandoahConcurrentEvacuator(ShenandoahGenerationalHeap* heap) :
          _heap(heap), _thread(Thread::current()) {}

  void do_object(oop p) override {
    shenandoah_assert_marked(nullptr, p);
    if (!p->is_forwarded()) {
      _heap->evacuate_or_promote_object(p, _thread);
    }
  }
};

ShenandoahGenerationalEvacuationTask::ShenandoahGenerationalEvacuationTask(ShenandoahGenerationalHeap* heap,
                                       ShenandoahRegionIterator* iterator,
                                       bool concurrent) :
        WorkerTask("Shenandoah Evacuation"),
        _heap(heap),
        _regions(iterator),
        _concurrent(concurrent),
        _tenuring_threshold(0)
{
  shenandoah_assert_generational();
  _tenuring_threshold = _heap->age_census()->tenuring_threshold();
}

void ShenandoahGenerationalEvacuationTask::work(uint worker_id) {
  if (_concurrent) {
    ShenandoahConcurrentWorkerSession worker_session(worker_id);
    ShenandoahSuspendibleThreadSetJoiner stsj;
    ShenandoahEvacOOMScope oom_evac_scope;
    do_work();
  } else {
    ShenandoahParallelWorkerSession worker_session(worker_id);
    ShenandoahEvacOOMScope oom_evac_scope;
    do_work();
  }
}

void ShenandoahGenerationalEvacuationTask::do_work() {
  ShenandoahConcurrentEvacuator cl(_heap);
  ShenandoahHeapRegion* r;
  ShenandoahMarkingContext* const ctx = _heap->marking_context();
  const size_t old_garbage_threshold = (ShenandoahHeapRegion::region_size_bytes() * ShenandoahOldGarbageThreshold) / 100;
  while ((r = _regions->next()) != nullptr) {
    log_debug(gc)("GenerationalEvacuationTask do_work(), looking at %s region " SIZE_FORMAT ", (age: %d) [%s, %s, %s]",
                  r->is_old()? "old": r->is_young()? "young": "free", r->index(), r->age(),
                  r->is_active()? "active": "inactive",
                  r->is_humongous()? (r->is_humongous_start()? "humongous_start": "humongous_continuation"): "regular",
                  r->is_cset()? "cset": "not-cset");

    if (r->is_cset()) {
      assert(r->has_live(), "Region " SIZE_FORMAT " should have been reclaimed early", r->index());
      _heap->marked_object_iterate(r, &cl);
      if (ShenandoahPacing) {
        _heap->pacer()->report_evac(r->used() >> LogHeapWordSize);
      }
    } else if (r->is_young() && r->is_active() && (r->age() >= _tenuring_threshold)) {
      if (r->is_humongous_start()) {
        // We promote humongous_start regions along with their affiliated continuations during evacuation rather than
        // doing this work during a safepoint.  We cannot put humongous regions into the collection set because that
        // triggers the load-reference barrier (LRB) to copy on reference fetch.
        r->promote_humongous();
      } else if (r->is_regular() && (r->get_top_before_promote() != nullptr)) {
        assert(r->garbage_before_padded_for_promote() < old_garbage_threshold,
               "Region " SIZE_FORMAT " has too much garbage for promotion", r->index());
        assert(r->get_top_before_promote() == ctx->top_at_mark_start(r),
               "Region " SIZE_FORMAT " has been used for allocations before promotion", r->index());
        // Likewise, we cannot put promote-in-place regions into the collection set because that would also trigger
        // the LRB to copy on reference fetch.
        r->promote_in_place();
      }
      // Aged humongous continuation regions are handled with their start region.  If an aged regular region has
      // more garbage than ShenandoahOldGarbageThreshold, we'll promote by evacuation.  If there is room for evacuation
      // in this cycle, the region will be in the collection set.  If there is not room, the region will be promoted
      // by evacuation in some future GC cycle.

      // If an aged regular region has received allocations during the current cycle, we do not promote because the
      // newly allocated objects do not have appropriate age; this region's age will be reset to zero at end of cycle.
    }
    // else, region is free, or OLD, or not in collection set, or humongous_continuation,
    // or is young humongous_start that is too young to be promoted

    if (_heap->check_cancelled_gc_and_yield(_concurrent)) {
      break;
    }
  }
}

class ShenandoahGenerationalInitLogger : public ShenandoahInitLogger {
public:
  static void print() {
    ShenandoahGenerationalInitLogger logger;
    logger.print_all();
  }

  void print_heap() override {
    ShenandoahInitLogger::print_heap();

    ShenandoahGenerationalHeap* heap = ShenandoahGenerationalHeap::heap();

    ShenandoahYoungGeneration* young = heap->young_generation();
    log_info(gc, init)("Young Generation Soft Size: " EXACTFMT, EXACTFMTARGS(young->soft_max_capacity()));
    log_info(gc, init)("Young Generation Max: " EXACTFMT, EXACTFMTARGS(young->max_capacity()));

    ShenandoahOldGeneration* old = heap->old_generation();
    log_info(gc, init)("Old Generation Soft Size: " EXACTFMT, EXACTFMTARGS(old->soft_max_capacity()));
    log_info(gc, init)("Old Generation Max: " EXACTFMT, EXACTFMTARGS(old->max_capacity()));
  }

protected:
  void print_gc_specific() override {
    ShenandoahInitLogger::print_gc_specific();

    ShenandoahGenerationalHeap* heap = ShenandoahGenerationalHeap::heap();
    log_info(gc, init)("Young Heuristics: %s", heap->young_generation()->heuristics()->name());
    log_info(gc, init)("Old Heuristics: %s", heap->old_generation()->heuristics()->name());
  }
};

ShenandoahGenerationalHeap* ShenandoahGenerationalHeap::heap() {
  shenandoah_assert_generational();
  CollectedHeap* heap = Universe::heap();
  return checked_cast<ShenandoahGenerationalHeap*>(heap);
}

size_t ShenandoahGenerationalHeap::calculate_min_plab() const {
  return align_up(PLAB::min_size(), CardTable::card_size_in_words());
}

size_t ShenandoahGenerationalHeap::calculate_max_plab() const {
  size_t MaxTLABSizeWords = ShenandoahHeapRegion::max_tlab_size_words();
  return ((ShenandoahMaxEvacLABRatio > 0)?
          align_down(MIN2(MaxTLABSizeWords, PLAB::min_size() * ShenandoahMaxEvacLABRatio), CardTable::card_size_in_words()):
          align_down(MaxTLABSizeWords, CardTable::card_size_in_words()));
}

ShenandoahGenerationalHeap::ShenandoahGenerationalHeap(ShenandoahCollectorPolicy* policy) :
  ShenandoahHeap(policy),
  _min_plab_size(calculate_min_plab()),
  _max_plab_size(calculate_max_plab()),
  _regulator_thread(nullptr) {
  assert(is_aligned(_min_plab_size, CardTable::card_size_in_words()), "min_plab_size must be aligned");
  assert(is_aligned(_max_plab_size, CardTable::card_size_in_words()), "max_plab_size must be aligned");
}

void ShenandoahGenerationalHeap::print_init_logger() const {
  ShenandoahGenerationalInitLogger logger;
  logger.print_all();
}

void ShenandoahGenerationalHeap::initialize_serviceability() {
  assert(mode()->is_generational(), "Only for the generational mode");
  _young_gen_memory_pool = new ShenandoahYoungGenMemoryPool(this);
  _old_gen_memory_pool = new ShenandoahOldGenMemoryPool(this);
  cycle_memory_manager()->add_pool(_young_gen_memory_pool);
  cycle_memory_manager()->add_pool(_old_gen_memory_pool);
  stw_memory_manager()->add_pool(_young_gen_memory_pool);
  stw_memory_manager()->add_pool(_old_gen_memory_pool);
}

GrowableArray<MemoryPool*> ShenandoahGenerationalHeap::memory_pools() {
  assert(mode()->is_generational(), "Only for the generational mode");
  GrowableArray<MemoryPool*> memory_pools(2);
  memory_pools.append(_young_gen_memory_pool);
  memory_pools.append(_old_gen_memory_pool);
  return memory_pools;
}

void ShenandoahGenerationalHeap::initialize_controller() {
  auto control_thread = new ShenandoahGenerationalControlThread();
  _control_thread = control_thread;
  _regulator_thread = new ShenandoahRegulatorThread(control_thread);
}

void ShenandoahGenerationalHeap::gc_threads_do(ThreadClosure* tcl) const {
  if (!shenandoah_policy()->is_at_shutdown()) {
    ShenandoahHeap::gc_threads_do(tcl);
    tcl->do_thread(regulator_thread());
  }
}

void ShenandoahGenerationalHeap::stop() {
  regulator_thread()->stop();
  ShenandoahHeap::stop();
}

oop ShenandoahGenerationalHeap::evacuate_or_promote_object(oop p, Thread* thread) {
  assert(thread == Thread::current(), "Expected thread parameter to be current thread.");
  if (ShenandoahThreadLocalData::is_oom_during_evac(thread)) {
    // This thread went through the OOM during evac protocol and it is safe to return
    // the forward pointer. It must not attempt to evacuate anymore.
    return ShenandoahBarrierSet::resolve_forwarded(p);
  }

  assert(ShenandoahThreadLocalData::is_evac_allowed(thread), "must be enclosed in oom-evac scope");

  ShenandoahHeapRegion* r = heap_region_containing(p);
  assert(!r->is_humongous(), "never evacuate humongous objects");

  ShenandoahAffiliation target_gen = r->affiliation();
  if (active_generation()->is_young() && target_gen == YOUNG_GENERATION) {
    markWord mark = p->mark();
    if (mark.is_marked()) {
      // Already forwarded.
      return ShenandoahBarrierSet::resolve_forwarded(p);
    }

    if (mark.has_displaced_mark_helper()) {
      // We don't want to deal with MT here just to ensure we read the right mark word.
      // Skip the potential promotion attempt for this one.
    } else if (r->age() + mark.age() >= age_census()->tenuring_threshold()) {
      oop result = try_evacuate_object(p, thread, r, OLD_GENERATION);
      if (result != nullptr) {
        return result;
      }
      // If we failed to promote this aged object, we'll fall through to code below and evacuate to young-gen.
    }
  }
  return try_evacuate_object(p, thread, r, target_gen);
}

// try_evacuate_object registers the object and dirties the associated remembered set information when evacuating
// to OLD_GENERATION.
oop ShenandoahGenerationalHeap::try_evacuate_object(oop p, Thread* thread, ShenandoahHeapRegion* from_region,
                                        ShenandoahAffiliation target_gen) {
  bool alloc_from_lab = true;
  bool has_plab = false;
  HeapWord* copy = nullptr;
  size_t size = p->size();
  bool is_promotion = (target_gen == OLD_GENERATION) && from_region->is_young();

#ifdef ASSERT
  if (ShenandoahOOMDuringEvacALot &&
      (os::random() & 1) == 0) { // Simulate OOM every ~2nd slow-path call
    copy = nullptr;
  } else {
#endif
    if (UseTLAB) {
      switch (target_gen) {
        case YOUNG_GENERATION: {
          copy = allocate_from_gclab(thread, size);
          if ((copy == nullptr) && (size < ShenandoahThreadLocalData::gclab_size(thread))) {
            // GCLAB allocation failed because we are bumping up against the limit on young evacuation reserve.  Try resetting
            // the desired GCLAB size and retry GCLAB allocation to avoid cascading of shared memory allocations.
            ShenandoahThreadLocalData::set_gclab_size(thread, PLAB::min_size());
            copy = allocate_from_gclab(thread, size);
            // If we still get nullptr, we'll try a shared allocation below.
          }
          break;
        }
        case OLD_GENERATION: {
          assert(mode()->is_generational(), "OLD Generation only exists in generational mode");
          PLAB* plab = ShenandoahThreadLocalData::plab(thread);
          if (plab != nullptr) {
            has_plab = true;
          }
          copy = allocate_from_plab(thread, size, is_promotion);
          if ((copy == nullptr) && (size < ShenandoahThreadLocalData::plab_size(thread)) &&
              ShenandoahThreadLocalData::plab_retries_enabled(thread)) {
            // PLAB allocation failed because we are bumping up against the limit on old evacuation reserve or because
            // the requested object does not fit within the current plab but the plab still has an "abundance" of memory,
            // where abundance is defined as >= ShenGenHeap::plab_min_size().  In the former case, we try resetting the desired
            // PLAB size and retry PLAB allocation to avoid cascading of shared memory allocations.

            // In this situation, PLAB memory is precious.  We'll try to preserve our existing PLAB by forcing
            // this particular allocation to be shared.
            if (plab->words_remaining() < plab_min_size()) {
              ShenandoahThreadLocalData::set_plab_size(thread, plab_min_size());
              copy = allocate_from_plab(thread, size, is_promotion);
              // If we still get nullptr, we'll try a shared allocation below.
              if (copy == nullptr) {
                // If retry fails, don't continue to retry until we have success (probably in next GC pass)
                ShenandoahThreadLocalData::disable_plab_retries(thread);
              }
            }
            // else, copy still equals nullptr.  this causes shared allocation below, preserving this plab for future needs.
          }
          break;
        }
        default: {
          ShouldNotReachHere();
          break;
        }
      }
    }

    if (copy == nullptr) {
      // If we failed to allocate in LAB, we'll try a shared allocation.
      if (!is_promotion || !has_plab || (size > PLAB::min_size())) {
        ShenandoahAllocRequest req = ShenandoahAllocRequest::for_shared_gc(size, target_gen, is_promotion);
        copy = allocate_memory(req);
        alloc_from_lab = false;
      }
      // else, we leave copy equal to nullptr, signaling a promotion failure below if appropriate.
      // We choose not to promote objects smaller than PLAB::min_size() by way of shared allocations, as this is too
      // costly.  Instead, we'll simply "evacuate" to young-gen memory (using a GCLAB) and will promote in a future
      // evacuation pass.  This condition is denoted by: is_promotion && has_plab && (size <= PLAB::min_size())
    }
#ifdef ASSERT
  }
#endif

  if (copy == nullptr) {
    if (target_gen == OLD_GENERATION) {
      if (from_region->is_young()) {
        // Signal that promotion failed. Will evacuate this old object somewhere in young gen.
        old_generation()->handle_failed_promotion(thread, size);
        return nullptr;
      } else {
        // Remember that evacuation to old gen failed. We'll want to trigger a full gc to recover from this
        // after the evacuation threads have finished.
        old_generation()->handle_failed_evacuation();
      }
    }

    control_thread()->handle_alloc_failure_evac(size);

    oom_evac_handler()->handle_out_of_memory_during_evacuation();

    return ShenandoahBarrierSet::resolve_forwarded(p);
  }

  // Copy the object:
  evac_tracker()->begin_evacuation(thread, size * HeapWordSize);
  Copy::aligned_disjoint_words(cast_from_oop<HeapWord*>(p), copy, size);

  oop copy_val = cast_to_oop(copy);

  if (target_gen == YOUNG_GENERATION && is_aging_cycle()) {
    ShenandoahHeap::increase_object_age(copy_val, from_region->age() + 1);
  }

  // Try to install the new forwarding pointer.
  ContinuationGCSupport::relativize_stack_chunk(copy_val);

  oop result = ShenandoahForwarding::try_update_forwardee(p, copy_val);
  if (result == copy_val) {
    // Successfully evacuated. Our copy is now the public one!
    evac_tracker()->end_evacuation(thread, size * HeapWordSize);
    if (target_gen == OLD_GENERATION) {
      old_generation()->handle_evacuation(copy, size, from_region->is_young());
    } else {
      // When copying to the old generation above, we don't care
      // about recording object age in the census stats.
      assert(target_gen == YOUNG_GENERATION, "Error");
      // We record this census only when simulating pre-adaptive tenuring behavior, or
      // when we have been asked to record the census at evacuation rather than at mark
      if (ShenandoahGenerationalCensusAtEvac || !ShenandoahGenerationalAdaptiveTenuring) {
        evac_tracker()->record_age(thread, size * HeapWordSize, ShenandoahHeap::get_object_age(copy_val));
      }
    }
    shenandoah_assert_correct(nullptr, copy_val);
    return copy_val;
  }  else {
    // Failed to evacuate. We need to deal with the object that is left behind. Since this
    // new allocation is certainly after TAMS, it will be considered live in the next cycle.
    // But if it happens to contain references to evacuated regions, those references would
    // not get updated for this stale copy during this cycle, and we will crash while scanning
    // it the next cycle.
    if (alloc_from_lab) {
      // For LAB allocations, it is enough to rollback the allocation ptr. Either the next
      // object will overwrite this stale copy, or the filler object on LAB retirement will
      // do this.
      switch (target_gen) {
        case YOUNG_GENERATION: {
          ShenandoahThreadLocalData::gclab(thread)->undo_allocation(copy, size);
          break;
        }
        case OLD_GENERATION: {
          ShenandoahThreadLocalData::plab(thread)->undo_allocation(copy, size);
          if (is_promotion) {
            ShenandoahThreadLocalData::subtract_from_plab_promoted(thread, size * HeapWordSize);
          } else {
            ShenandoahThreadLocalData::subtract_from_plab_evacuated(thread, size * HeapWordSize);
          }
          break;
        }
        default: {
          ShouldNotReachHere();
          break;
        }
      }
    } else {
      // For non-LAB allocations, we have no way to retract the allocation, and
      // have to explicitly overwrite the copy with the filler object. With that overwrite,
      // we have to keep the fwdptr initialized and pointing to our (stale) copy.
      assert(size >= ShenandoahHeap::min_fill_size(), "previously allocated object known to be larger than min_size");
      fill_with_object(copy, size);
      shenandoah_assert_correct(nullptr, copy_val);
      // For non-LAB allocations, the object has already been registered
    }
    shenandoah_assert_correct(nullptr, result);
    return result;
  }
}


ShenandoahGenerationalHeap::TransferResult ShenandoahGenerationalHeap::balance_generations() {
  shenandoah_assert_heaplocked_or_safepoint();

  ShenandoahOldGeneration* old_gen = old_generation();
  const ssize_t old_region_balance = old_gen->get_region_balance();
  old_gen->set_region_balance(0);

  if (old_region_balance > 0) {
    const auto old_region_surplus = checked_cast<size_t>(old_region_balance);
    const bool success = generation_sizer()->transfer_to_young(old_region_surplus);
    return TransferResult {
      success, old_region_surplus, "young"
    };
  }

  if (old_region_balance < 0) {
    const auto old_region_deficit = checked_cast<size_t>(-old_region_balance);
    const bool success = generation_sizer()->transfer_to_old(old_region_deficit);
    if (!success) {
      old_gen->handle_failed_transfer();
    }
    return TransferResult {
      success, old_region_deficit, "old"
    };
  }

  return TransferResult {true, 0, "none"};
}

// Make sure old-generation is large enough, but no larger than is necessary, to hold mixed evacuations
// and promotions, if we anticipate either. Any deficit is provided by the young generation, subject to
// xfer_limit, and any surplus is transferred to the young generation.
// xfer_limit is the maximum we're able to transfer from young to old.
void ShenandoahGenerationalHeap::compute_old_generation_balance(size_t old_xfer_limit, size_t old_cset_regions) {

  // We can limit the old reserve to the size of anticipated promotions:
  // max_old_reserve is an upper bound on memory evacuated from old and promoted to old,
  // clamped by the old generation space available.
  //
  // Here's the algebra.
  // Let SOEP = ShenandoahOldEvacRatioPercent,
  //     OE = old evac,
  //     YE = young evac, and
  //     TE = total evac = OE + YE
  // By definition:
  //            SOEP/100 = OE/TE
  //                     = OE/(OE+YE)
  //  => SOEP/(100-SOEP) = OE/((OE+YE)-OE)      // componendo-dividendo: If a/b = c/d, then a/(b-a) = c/(d-c)
  //                     = OE/YE
  //  =>              OE = YE*SOEP/(100-SOEP)

  // We have to be careful in the event that SOEP is set to 100 by the user.
  assert(ShenandoahOldEvacRatioPercent <= 100, "Error");
  const size_t old_available = old_generation()->available();
  // The free set will reserve this amount of memory to hold young evacuations
  const size_t young_reserve = (young_generation()->max_capacity() * ShenandoahEvacReserve) / 100;

  // In the case that ShenandoahOldEvacRatioPercent equals 100, max_old_reserve is limited only by xfer_limit.

  const size_t bound_on_old_reserve = old_available + old_xfer_limit + young_reserve;
  const size_t max_old_reserve = (ShenandoahOldEvacRatioPercent == 100)?
                                 bound_on_old_reserve: MIN2((young_reserve * ShenandoahOldEvacRatioPercent) / (100 - ShenandoahOldEvacRatioPercent),
                                                            bound_on_old_reserve);

  const size_t region_size_bytes = ShenandoahHeapRegion::region_size_bytes();

  // Decide how much old space we should reserve for a mixed collection
  size_t reserve_for_mixed = 0;
  if (old_generation()->has_unprocessed_collection_candidates()) {
    // We want this much memory to be unfragmented in order to reliably evacuate old.  This is conservative because we
    // may not evacuate the entirety of unprocessed candidates in a single mixed evacuation.
    const size_t max_evac_need = (size_t)
            (old_generation()->unprocessed_collection_candidates_live_memory() * ShenandoahOldEvacWaste);
    assert(old_available >= old_generation()->free_unaffiliated_regions() * region_size_bytes,
           "Unaffiliated available must be less than total available");
    const size_t old_fragmented_available =
            old_available - old_generation()->free_unaffiliated_regions() * region_size_bytes;
    reserve_for_mixed = max_evac_need + old_fragmented_available;
    if (reserve_for_mixed > max_old_reserve) {
      reserve_for_mixed = max_old_reserve;
    }
  }

  // Decide how much space we should reserve for promotions from young
  size_t reserve_for_promo = 0;
  const size_t promo_load = old_generation()->get_promotion_potential();
  const bool doing_promotions = promo_load > 0;
  if (doing_promotions) {
    // We're promoting and have a bound on the maximum amount that can be promoted
    assert(max_old_reserve >= reserve_for_mixed, "Sanity");
    const size_t available_for_promotions = max_old_reserve - reserve_for_mixed;
    reserve_for_promo = MIN2((size_t)(promo_load * ShenandoahPromoEvacWaste), available_for_promotions);
  }

  // This is the total old we want to ideally reserve
  const size_t old_reserve = reserve_for_mixed + reserve_for_promo;
  assert(old_reserve <= max_old_reserve, "cannot reserve more than max for old evacuations");

  // We now check if the old generation is running a surplus or a deficit.
  const size_t max_old_available = old_generation()->available() + old_cset_regions * region_size_bytes;
  if (max_old_available >= old_reserve) {
    // We are running a surplus, so the old region surplus can go to young
    const size_t old_surplus = (max_old_available - old_reserve) / region_size_bytes;
    const size_t unaffiliated_old_regions = old_generation()->free_unaffiliated_regions() + old_cset_regions;
    const size_t old_region_surplus = MIN2(old_surplus, unaffiliated_old_regions);
    old_generation()->set_region_balance(checked_cast<ssize_t>(old_region_surplus));
  } else {
    // We are running a deficit which we'd like to fill from young.
    // Ignore that this will directly impact young_generation()->max_capacity(),
    // indirectly impacting young_reserve and old_reserve.  These computations are conservative.
    // Note that deficit is rounded up by one region.
    const size_t old_need = (old_reserve - max_old_available + region_size_bytes - 1) / region_size_bytes;
    const size_t max_old_region_xfer = old_xfer_limit / region_size_bytes;

    // Round down the regions we can transfer from young to old. If we're running short
    // on young-gen memory, we restrict the xfer. Old-gen collection activities will be
    // curtailed if the budget is restricted.
    const size_t old_region_deficit = MIN2(old_need, max_old_region_xfer);
    old_generation()->set_region_balance(0 - checked_cast<ssize_t>(old_region_deficit));
  }
}

void ShenandoahGenerationalHeap::reset_generation_reserves() {
  young_generation()->set_evacuation_reserve(0);
  old_generation()->set_evacuation_reserve(0);
  old_generation()->set_promoted_reserve(0);
}

void ShenandoahGenerationalHeap::TransferResult::print_on(const char* when, outputStream* ss) const {
  auto heap = ShenandoahGenerationalHeap::heap();
  ShenandoahYoungGeneration* const young_gen = heap->young_generation();
  ShenandoahOldGeneration* const old_gen = heap->old_generation();
  const size_t young_available = young_gen->available();
  const size_t old_available = old_gen->available();
  ss->print_cr("After %s, %s " SIZE_FORMAT " regions to %s to prepare for next gc, old available: "
                     PROPERFMT ", young_available: " PROPERFMT,
                     when,
                     success? "successfully transferred": "failed to transfer", region_count, region_destination,
                     PROPERFMTARGS(old_available), PROPERFMTARGS(young_available));
}

/*
 * Copyright (c) 2016, 2021, Red Hat, Inc. All rights reserved.
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
#include "gc/shared/tlab_globals.hpp"
#include "gc/shenandoah/shenandoahBarrierSet.hpp"
#include "gc/shenandoah/shenandoahFreeSet.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahHeapRegionSet.hpp"
#include "gc/shenandoah/shenandoahMarkingContext.inline.hpp"
#include "gc/shenandoah/shenandoahOldGeneration.hpp"
#include "gc/shenandoah/shenandoahScanRemembered.inline.hpp"
#include "gc/shenandoah/shenandoahYoungGeneration.hpp"
#include "logging/logStream.hpp"
#include "memory/resourceArea.hpp"
#include "runtime/orderAccess.hpp"

// In the existing implementatation, plab allocations are taken from is-mutator-free regions and/or existing old regions.
// 
// The plan is to introduce a new is_old_collector_free qualifier in order to distinguish the regions dedicated to old-promotions
// and evacuations from regions that are dedicated to mutator allocations and young-evacuations.  This macro symbol identifies
// some of the code segments that will be affected when we incorporate this change.
#define REMOVE_WHEN_FREESET_DOES_OLD_COLLECTED_FREE 1

ShenandoahFreeSet::ShenandoahFreeSet(ShenandoahHeap* heap, size_t max_regions) :
  _heap(heap),
  _mutator_free_bitmap(max_regions, mtGC),
  _collector_free_bitmap(max_regions, mtGC),
  _max(max_regions)
{
  clear_internal();
}

void ShenandoahFreeSet::increase_used(size_t num_bytes) {
  shenandoah_assert_heaplocked();
  _used += num_bytes;

#ifndef REMOVE_WHEN_FREESET_DOES_OLD_COLLECTED_FREE
  // The intention as originally implemented is that free-set capacity represents the budget for mutator allocations.
  // This assert is currently disabled because plab and old shared allocations are taken from mutator-free regions and/or
  // from existing old-gen regions which are not mutator-free and may not be collector-free.  Work is in progress to
  // create a new mode for regions, which is "old-collector-free".  When that's implemented and we properly distinguish
  // regions that are dedicated to old-gen allocations, we'll reenable this assert.
  assert(_used <= _capacity, "must not use more than we have: used: " SIZE_FORMAT
         ", capacity: " SIZE_FORMAT ", num_bytes: " SIZE_FORMAT, _used, _capacity, num_bytes);
#endif
}

void ShenandoahFreeSet::add_old_collector_free_region(ShenandoahHeapRegion* region) {
  shenandoah_assert_heaplocked();
  size_t idx = region->index();
  assert(!is_mutator_free(idx) & !is_collector_free(idx), "Promoted in place region should not be mutator_free or collector_free");
  // TODO: we really want to label this as old-collector-free but that is not yet implemented.
  _mutator_free_bitmap.set_bit(region->index());
  // This region was previously not 
  adjust_bounds_for_additional_old_collector_free_region(idx);
}

void ShenandoahFreeSet::adjust_bounds_for_additional_old_collector_free_region(size_t idx) {
  // TODO: this should modify _old_collector_leftmost and _old_collector_rightmost, when they are implemented,
  ShenandoahHeapRegion* r = _heap->get_region(idx);

  // TODO: add available to _old_capacity rather than _capacity below.
  _capacity += r->free();
  // Only adjust _mutator_leftmost and _mutator_rightmost.
  if (idx < _mutator_leftmost) {
    _mutator_leftmost = idx;
  } else if (idx > _mutator_rightmost) {
    _mutator_rightmost = idx;
  }
}


bool ShenandoahFreeSet::is_mutator_free(size_t idx) const {
  assert (idx < _max, "index is sane: " SIZE_FORMAT " < " SIZE_FORMAT " (left: " SIZE_FORMAT ", right: " SIZE_FORMAT ")",
          idx, _max, _mutator_leftmost, _mutator_rightmost);
  return _mutator_free_bitmap.at(idx);
}

bool ShenandoahFreeSet::is_collector_free(size_t idx) const {
  assert (idx < _max, "index is sane: " SIZE_FORMAT " < " SIZE_FORMAT " (left: " SIZE_FORMAT ", right: " SIZE_FORMAT ")",
          idx, _max, _collector_leftmost, _collector_rightmost);
  return _collector_free_bitmap.at(idx);
}

// This is a temporary solution to work around a shortcoming with the existing free set implementation.
// TODO:
//   Remove this function after restructing FreeSet representation.  A problem in the existing implementation is that old-gen
//   regions are not considered to reside within the is_collector_free range.
//   Eventually, we'll keep a separate range for old_collector_free_range and we'll give preference to allocating from
//     a region that is_old_collector_free().  If that's not available, we may try to flip a region that is_mutator_free.
//
HeapWord* ShenandoahFreeSet::allocate_with_old_affiliation(ShenandoahAllocRequest& req, bool& in_new_region) {
  ShenandoahRegionAffiliation affiliation = ShenandoahRegionAffiliation::OLD_GENERATION;

  size_t o_rightmost = MAX2(_collector_rightmost, _mutator_rightmost);
  size_t o_leftmost = MIN2(_collector_leftmost, _mutator_leftmost);
  size_t rightmost = _heap->num_regions() - 1;
  size_t leftmost = 0;

  for (size_t c = rightmost + 1; c > leftmost; c--) {
    // size_t is unsigned, need to dodge underflow when _leftmost = 0
    size_t idx = c - 1;
    ShenandoahHeapRegion* r = _heap->get_region(idx);
    if (r->affiliation() == affiliation && !r->is_humongous()) {
      if (!r->is_cset() && !has_no_alloc_capacity(r)) {
#undef KELVIN_AWOA
#ifdef KELVIN_AWOA
        log_info(gc, ergo)("awoa tries region " SIZE_FORMAT " which has " SIZE_FORMAT " free", r->index(), r->free());
#endif
        HeapWord* result = try_allocate_in(r, req, in_new_region);
        if (result != NULL) {
#ifdef KELVIN_AWOA
          log_info(gc, ergo)("awoa(size: " SIZE_FORMAT ", plab: %s, min_size: " SIZE_FORMAT ") succeeds @" PTR_FORMAT
                             ", region " SIZE_FORMAT ", actual_size: " SIZE_FORMAT,
                             req.size(), req.is_lab_alloc()? "yes": "no", req.is_lab_alloc()? req.min_size(): 0L, p2i(result),
                             r->index(), req.actual_size());
          if ((idx < o_leftmost) || (idx > o_rightmost)) {
            log_info(gc, ergo)(" Surprise! Surprise! Surprise!  o_leftmost: " SIZE_FORMAT ", o_rightmost: " SIZE_FORMAT,
                               o_leftmost, o_rightmost);
          }
#endif
          return result;
        }
      }
    }
  }
#ifdef KELVIN_AWOA
  log_info(gc, ergo)("awoa(size: " SIZE_FORMAT ", plab: %s, min_size: " SIZE_FORMAT ") fails",
                     req.size(), req.is_lab_alloc()? "yes": "no", req.is_lab_alloc()? req.min_size(): 0L);
#endif
  return nullptr;
}

HeapWord* ShenandoahFreeSet::allocate_with_affiliation(ShenandoahRegionAffiliation affiliation, ShenandoahAllocRequest& req, bool& in_new_region) {
  for (size_t c = _collector_rightmost + 1; c > _collector_leftmost; c--) {
    // size_t is unsigned, need to dodge underflow when _leftmost = 0
    size_t idx = c - 1;
    if (is_collector_free(idx)) {
      ShenandoahHeapRegion* r = _heap->get_region(idx);
      if (r->affiliation() == affiliation) {
        HeapWord* result = try_allocate_in(r, req, in_new_region);
        if (result != NULL) {
          return result;
        }
      }
    }
  }
  return NULL;
}

HeapWord* ShenandoahFreeSet::allocate_single(ShenandoahAllocRequest& req, bool& in_new_region) {
  // Scan the bitmap looking for a first fit.
  //
  // Leftmost and rightmost bounds provide enough caching to walk bitmap efficiently. Normally,
  // we would find the region to allocate at right away.
  //
  // Allocations are biased: new application allocs go to beginning of the heap, and GC allocs
  // go to the end. This makes application allocation faster, because we would clear lots
  // of regions from the beginning most of the time.
  //
  // Free set maintains mutator and collector views, and normally they allocate in their views only,
  // unless we special cases for stealing and mixed allocations.

  // Overwrite with non-zero (non-NULL) values only if necessary for allocation bookkeeping.

  bool allow_new_region = true;
  switch (req.affiliation()) {
    case ShenandoahRegionAffiliation::OLD_GENERATION:
      // Note: unsigned result from adjusted_unaffiliated_regions() will never be less than zero, but it may equal zero.
      if (_heap->old_generation()->adjusted_unaffiliated_regions() <= 0) {
        allow_new_region = false;
      }
      break;

    case ShenandoahRegionAffiliation::YOUNG_GENERATION:
      // Note: unsigned result from adjusted_unaffiliated_regions() will never be less than zero, but it may equal zero.
      if (_heap->young_generation()->adjusted_unaffiliated_regions() <= 0) {
        allow_new_region = false;
      }
      break;

    case ShenandoahRegionAffiliation::FREE:
    default:
      ShouldNotReachHere();
      break;
  }
#undef KELVIN_PLAB_ALLOCATE
#ifdef KELVIN_PLAB_ALLOCATE
  log_info(gc, ergo)("alloc_single(), allow_new_region is %d", allow_new_region);
#endif
  switch (req.type()) {
    case ShenandoahAllocRequest::_alloc_tlab:
    case ShenandoahAllocRequest::_alloc_shared: {
      // Try to allocate in the mutator view
#undef KELVIN_ALLOCATE
#ifdef KELVIN_ALLOCATE
      size_t largest_available = 0;
      size_t used_candidates = 0;
      size_t free_candidates = 0;
#endif
      for (size_t idx = _mutator_leftmost; idx <= _mutator_rightmost; idx++) {
        ShenandoahHeapRegion* r = _heap->get_region(idx);
        if (is_mutator_free(idx) && (allow_new_region || r->affiliation() != ShenandoahRegionAffiliation::FREE)) {
#ifdef KELVIN_ALLOCATE
          if (r->affiliation() == req.affiliation()) {
            used_candidates++;
            // KELVIN RECOMMENDS: allocate_with_affiliation(req.affiliation(), req, in_new_region)
            //  followed by test of allow_new_region to try allocation with affiliation(FREE).
            size_t region_available = (r->end() - r->top()) * HeapWordSize;
            if (region_available > largest_available) {
              largest_available = region_available;
            }
          } else if (r->affiliation() == ShenandoahRegionAffiliation::FREE) {
            free_candidates++;
          }
#endif

          // try_allocate_in() increases used if the allocation is successful.
          HeapWord* result = try_allocate_in(r, req, in_new_region);
          if (result != NULL) {
            return result;
          }
        }
      }
#ifdef KELVIN_ALLOCATE
      log_info(gc, ergo)("allocate_single(" SIZE_FORMAT ", min: " SIZE_FORMAT
                         ") of tlab/shared failed to fulfill in range from " SIZE_FORMAT " to "
                         SIZE_FORMAT " with " SIZE_FORMAT " free candidates and " SIZE_FORMAT
                         " used candidates, largest available: " SIZE_FORMAT
                         ", allow_new_region is: %d, young available: " SIZE_FORMAT,
                         req.size(), req.is_lab_alloc()? req.min_size(): req.size(),
                         _mutator_leftmost, _mutator_rightmost, free_candidates, used_candidates,
                         largest_available, allow_new_region, _heap->young_generation()->available());
      ShenandoahGeneration* young_gen = _heap->young_generation();
      log_info(gc, ergo)(" young_capacity: " SIZE_FORMAT ", young_used: " SIZE_FORMAT ", young_regions_used: " SIZE_FORMAT
                         ", young_unaffiliated_regions: " SIZE_FORMAT
                         ", adjusted_unaffiliated_regions: " SIZE_FORMAT ", young available: " SIZE_FORMAT,
                         young_gen->max_capacity(), young_gen->used(), young_gen->used_regions_size(),
                         young_gen->free_unaffiliated_regions(),
                         young_gen->adjusted_unaffiliated_regions(), young_gen->available());
      ShenandoahGeneration* old_gen = _heap->old_generation();
      log_info(gc, ergo)(" old_capacity: " SIZE_FORMAT ", old_used: " SIZE_FORMAT ", old_regions_used: " SIZE_FORMAT
                         ", old_unaffiliated_regions: " SIZE_FORMAT
                         ", adjusted_unaffiliated_regions: " SIZE_FORMAT ", old available: " SIZE_FORMAT,
                         old_gen->max_capacity(), old_gen->used(), old_gen->used_regions_size(),
                         old_gen->free_unaffiliated_regions(),
                         old_gen->adjusted_unaffiliated_regions(), old_gen->available());

      unsafe_peek_free();
#endif
      // There is no recovery. Mutator does not touch collector view at all.
      break;
    }
    case ShenandoahAllocRequest::_alloc_gclab:
      // GCLABs are for evacuation so we must be in evacuation phase.  If this allocation is successful, increment
      // the relevant evac_expended rather than used value.

    case ShenandoahAllocRequest::_alloc_plab:
      // PLABs always reside in old-gen and are only allocated during evacuation phase.

    case ShenandoahAllocRequest::_alloc_shared_gc: {
      // First try to fit into a region that is already in use in the same generation.
      HeapWord* result;
      if (req.affiliation() == ShenandoahRegionAffiliation::OLD_GENERATION) {
#ifdef KELVIN_PLAB_ALLOCATE
        log_info(gc, ergo)("Found way to allocate_with_old_affiliation");
#endif
        // TODO: this is a work around to address a deficiency in FreeSet representation.  A better solution fixes
        // the FreeSet implementation to deal more efficiently with old-gen regions as being in the "collector free set"
        result = allocate_with_old_affiliation(req, in_new_region);
      } else {
        result = allocate_with_affiliation(req.affiliation(), req, in_new_region);
      }
      if (result != NULL) {
        return result;
      }
#ifdef KELVIN_TRACE
      if (req.affiliation() == ShenandoahRegionAffiliation::OLD_GENERATION) {
        ShenandoahGeneration* old_gen = _heap->old_generation();
        log_info(gc, ergo)("Since awoa failed, will try new region maybe: %s", allow_new_region? "yes": "not even");
        log_info(gc, ergo)(" old_used: " SIZE_FORMAT ", old_regions_used: " SIZE_FORMAT ", old_unaffiliated_regions: " SIZE_FORMAT
                           ", adjusted_unaffiliated_regions: " SIZE_FORMAT ", old available: " SIZE_FORMAT,
                           old_gen->used(), old_gen->used_regions_size(), old_gen->free_unaffiliated_regions(),
                           old_gen->adjusted_unaffiliated_regions(), old_gen->available());
                           
      }
#endif
      if (allow_new_region) {
        // Then try a free region that is dedicated to GC allocations.
        result = allocate_with_affiliation(FREE, req, in_new_region);
        if (result != NULL) {
          return result;
        }
      }
#ifdef KELVIN_TRACE
      if (req.affiliation() == ShenandoahRegionAffiliation::OLD_GENERATION) {
        log_info(gc, ergo)("%sGoing to try the mutator view", allow_new_region? "": "NOT ");
      }
#endif
      // No dice. Can we borrow space from mutator view?
      if (!ShenandoahEvacReserveOverflow) {
        return NULL;
      }

      if (allow_new_region) {
        // Try to steal an empty region from the mutator view.
        for (size_t c = _mutator_rightmost + 1; c > _mutator_leftmost; c--) {
          size_t idx = c - 1;
          if (is_mutator_free(idx)) {
            ShenandoahHeapRegion* r = _heap->get_region(idx);
            if (can_allocate_from(r)) {
              flip_to_gc(r);
              HeapWord *result = try_allocate_in(r, req, in_new_region);
              if (result != NULL) {
                return result;
              }
            }
          }
        }
      }

      // No dice. Do not try to mix mutator and GC allocations, because
      // URWM moves due to GC allocations would expose unparsable mutator
      // allocations.
      break;
    }
    default:
      ShouldNotReachHere();
  }
  return NULL;
}

HeapWord* ShenandoahFreeSet::try_allocate_in(ShenandoahHeapRegion* r, ShenandoahAllocRequest& req, bool& in_new_region) {
  assert (!has_no_alloc_capacity(r), "Performance: should avoid full regions on this path: " SIZE_FORMAT, r->index());

  if (_heap->is_concurrent_weak_root_in_progress() &&
      r->is_trash()) {
    return NULL;
  }
  try_recycle_trashed(r);
  if (r->affiliation() == ShenandoahRegionAffiliation::FREE) {
    ShenandoahMarkingContext* const ctx = _heap->complete_marking_context();
    r->set_affiliation(req.affiliation(), false);
    if (r->is_old()) {
      // Any OLD region allocated during concurrent coalesce-and-fill does not need to be coalesced and filled because
      // all objects allocated within this region are above TAMS (and thus are implicitly marked).  In case this is an
      // OLD region and concurrent preparation for mixed evacuations visits this region before the start of the next
      // old-gen concurrent mark (i.e. this region is allocated following the start of old-gen concurrent mark but before
      // concurrent preparations for mixed evacuations are completed), we mark this region as not requiring any
      // coalesce-and-fill processing.
      r->end_preemptible_coalesce_and_fill();
      _heap->clear_cards_for(r);
    }

    assert(ctx->top_at_mark_start(r) == r->bottom(), "Newly established allocation region starts with TAMS equal to bottom");
    assert(ctx->is_bitmap_clear_range(ctx->top_bitmap(r), r->end()), "Bitmap above top_bitmap() must be clear");

    // Leave top_bitmap alone.  The first time a heap region is put into service, top_bitmap should equal end.
    // Thereafter, it should represent the upper bound on parts of the bitmap that need to be cleared.
    log_debug(gc)("NOT clearing bitmap for region " SIZE_FORMAT ", top_bitmap: "
                  PTR_FORMAT " at transition from FREE to %s",
                  r->index(), p2i(ctx->top_bitmap(r)), affiliation_name(req.affiliation()));
  } else if (r->affiliation() != req.affiliation()) {
    return NULL;
  }

  in_new_region = r->is_empty();
  HeapWord* result = NULL;
  size_t size = req.size();

  // req.size() is in words, r->free() is in bytes.
  if (ShenandoahElasticTLAB && req.is_lab_alloc()) {
    if (req.type() == ShenandoahAllocRequest::_alloc_plab) {
#ifndef REMOVE_WHEN_FREESET_DOES_OLD_COLLECTED_FREE
      assert(is_old_collector_free(r->index()), "PLABS must be allocated in old_collector_free regions");
#endif
      // Need to assure that plabs are aligned on multiple of card region.
      size_t free = r->free();
      size_t usable_free = (free / CardTable::card_size()) << CardTable::card_shift();
      if ((free != usable_free) && (free - usable_free < ShenandoahHeap::min_fill_size() * HeapWordSize)) {
        // We'll have to add another card's memory to the padding
        if (usable_free > CardTable::card_size()) {
          usable_free -= CardTable::card_size();
        } else {
          assert(usable_free == 0, "usable_free is a multiple of card_size and card_size > min_fill_size");
        }
      }
      free /= HeapWordSize;
      usable_free /= HeapWordSize;
      size_t remnant = size % CardTable::card_size_in_words();
      if (remnant > 0) {
        // Since we have Elastic TLABs, align size up.  This is consistent with aligning min_size up.
        size = size - remnant + CardTable::card_size_in_words();
      }
      if (size > usable_free) {
        size = usable_free;
        assert(size % CardTable::card_size_in_words() == 0, "usable_free is a multiple of card table size");
      }

      size_t adjusted_min_size = req.min_size();
      remnant = adjusted_min_size % CardTable::card_size_in_words();
      if (remnant > 0) {
        // Round up adjusted_min_size to a multiple of alignment size
        adjusted_min_size = adjusted_min_size - remnant + CardTable::card_size_in_words();
      }
      if (size >= adjusted_min_size) {
        result = r->allocate_aligned(size, req, CardTable::card_size());
        assert(result != nullptr, "Allocation cannot fail");
        size = req.actual_size();
        assert(r->top() <= r->end(), "Allocation cannot span end of region");
        // actual_size() will be set to size below.
        assert((result == nullptr) || (size % CardTable::card_size_in_words() == 0),
               "PLAB size must be multiple of card size");
        assert((result == nullptr) || (((uintptr_t) result) % CardTable::card_size_in_words() == 0),
               "PLAB start must align with card boundary");
        if (free > usable_free) {
          // Account for the alignment padding
          size_t padding = (free - usable_free) * HeapWordSize;
#ifdef REMOVE_WHEN_FREESET_DOES_OLD_COLLECTED_FREE
          // PLABS reside in old-gen.  Their regions should not be is_mutator_free.  So we should not increase_used().
          increase_used(padding);
#endif
          assert(r->affiliation() == ShenandoahRegionAffiliation::OLD_GENERATION, "All PLABs reside in old-gen");
          _heap->old_generation()->increase_used(padding);
          // For verification consistency, we need to report this padding to _heap
#undef KELVIN_HEAP_USAGE
#ifdef KELVIN_HEAP_USAGE
          log_info(gc, ergo)("Increasing heap->_used by " SIZE_FORMAT " for plab padding, result: " SIZE_FORMAT,
                             padding, _heap->used());
#endif    
          _heap->increase_used(padding);
        }
      }
      // Otherwise, leave result == NULL because the adjusted size is smaller than min size.
    } else {
      // This is a GCLAB or a TLAB allocation
      size_t free = align_down(r->free() >> LogHeapWordSize, MinObjAlignment);
      if (size > free) {
        size = free;
      }
      if (size >= req.min_size()) {
        result = r->allocate(size, req);
        if (result != nullptr) {
          // Record actual allocation size
          req.set_actual_size(size);
        }
        assert (result != NULL, "Allocation must succeed: free " SIZE_FORMAT ", actual " SIZE_FORMAT, free, size);
      } else {
        log_trace(gc, ergo)("Failed to shrink TLAB or GCLAB request (" SIZE_FORMAT ") in region " SIZE_FORMAT " to " SIZE_FORMAT
                           " because min_size() is " SIZE_FORMAT, req.size(), r->index(), size, req.min_size());
      }
    }
  } else if (req.is_lab_alloc() && req.type() == ShenandoahAllocRequest::_alloc_plab) {
    // inelastic PLAB
    size_t free = r->free();
    size_t usable_free = (free / CardTable::card_size()) << CardTable::card_shift();
    free /= HeapWordSize;
    usable_free /= HeapWordSize;
    if ((free != usable_free) && (free - usable_free < ShenandoahHeap::min_fill_size() * HeapWordSize)) {
      // We'll have to add another card's memory to the padding
      if (usable_free > CardTable::card_size_in_words()) {
        usable_free -= CardTable::card_size_in_words();
      } else {
        assert(usable_free == 0, "usable_free is a multiple of card_size and card_size > min_fill_size");
      }
    }
    assert(size % CardTable::card_size_in_words() == 0, "PLAB size must be multiple of remembered set card size");
    if (size <= usable_free) {
      result = r->allocate_aligned(size, req, CardTable::card_size());
      size = req.actual_size();
      assert(result != nullptr, "Allocation cannot fail");
      assert(r->top() <= r->end(), "Allocation cannot span end of region");
      assert(req.actual_size() % CardTable::card_size_in_words() == 0, "PLAB start must align with card boundary");
      assert(((uintptr_t) result) % CardTable::card_size_in_words() == 0, "PLAB start must align with card boundary");
      if (free > usable_free) {
        // Account for the alignment padding
        size_t padding = (free - usable_free) * HeapWordSize;

#ifdef REMOVE_WHEN_FREESET_DOES_OLD_COLLECTED_FREE
        // PLAB allocations are collector_is_free.  We only increase_Used for mutator allocations.
        increase_used(padding);
#endif
        assert(r->affiliation() == ShenandoahRegionAffiliation::OLD_GENERATION, "All PLABs reside in old-gen");
        _heap->old_generation()->increase_used(padding);
        // For verification consistency, we need to report this padding to _heap
        _heap->increase_used(padding);
#undef KELVIN_HEAP_USAGE
#ifdef KELVIN_HEAP_USAGE
        log_info(gc, ergo)("Increasing heap->_used by " SIZE_FORMAT " plab padding, result: " SIZE_FORMAT,
                           padding, _heap->used());
#endif
      }
    }
  } else {
    result = r->allocate(size, req);
    if (result != nullptr) {
      // Record actual allocation size
      req.set_actual_size(size);
    }
  }

  if (result != NULL) {
    // Allocation successful, bump stats:
    if (req.is_mutator_alloc()) {
      // Mutator allocations always pull from young gen.
      _heap->young_generation()->increase_used(size * HeapWordSize);
      increase_used(size * HeapWordSize);
    } else {
      assert(req.is_gc_alloc(), "Should be gc_alloc since req wasn't mutator alloc");

      // For GC allocations, we advance update_watermark because the objects relocated into this memory during
      // evacuation are not updated during evacuation.  For both young and old regions r, it is essential that all
      // PLABs be made parsable at the end of evacuation.  This is enabled by retiring all plabs at end of evacuation.
      // TODO: Making a PLAB parsable involves placing a filler object in its remnant memory but does not require
      // that the PLAB be disabled for all future purposes.  We may want to introduce a new service to make the
      // PLABs parsable while still allowing the PLAB to serve future allocation requests that arise during the
      // next evacuation pass.
      r->set_update_watermark(r->top());

      if (r->affiliation() == ShenandoahRegionAffiliation::YOUNG_GENERATION) {
        _heap->young_generation()->increase_used(size * HeapWordSize);
      } else {
        assert(r->affiliation() == ShenandoahRegionAffiliation::OLD_GENERATION, "GC Alloc was not YOUNG so must be OLD");
        assert(req.type() != ShenandoahAllocRequest::_alloc_gclab, "old-gen allocations use PLAB or shared allocation");
        _heap->old_generation()->increase_used(size * HeapWordSize);
        // for plabs, we'll sort the difference between evac and promotion usage when we retire the plab
      }
    }
  }
  if (result == NULL || has_no_alloc_capacity(r)) {
    // Region cannot afford this or future allocations. Retire it.
    //
    // While this seems a bit harsh, especially in the case when this large allocation does not
    // fit, but the next small one would, we are risking to inflate scan times when lots of
    // almost-full regions precede the fully-empty region where we want to allocate the entire TLAB.
    // TODO: Record first fully-empty region, and use that for large allocations and/or organize
    // available free segments within regions for more efficient searches for "good fit".

    // Record the remainder as allocation waste
    if (req.is_mutator_alloc()) {
      size_t waste = r->free();
      if (waste > 0) {
        increase_used(waste);
        _heap->generation_for(req.affiliation())->increase_allocated(waste);
        _heap->notify_mutator_alloc_words(waste >> LogHeapWordSize, true);
      }
    }

    size_t num = r->index();
    _collector_free_bitmap.clear_bit(num);
    _mutator_free_bitmap.clear_bit(num);
    // Touched the bounds? Need to update:
    if (touches_bounds(num)) {
      adjust_bounds();
    }
    assert_bounds();
  }
  return result;
}

bool ShenandoahFreeSet::touches_bounds(size_t num) const {
  return num == _collector_leftmost || num == _collector_rightmost || num == _mutator_leftmost || num == _mutator_rightmost;
}

void ShenandoahFreeSet::recompute_bounds() {
  // Reset to the most pessimistic case:
  _mutator_rightmost = _max - 1;
  _mutator_leftmost = 0;
  _collector_rightmost = _max - 1;
  _collector_leftmost = 0;

  // ...and adjust from there
  adjust_bounds();
}

void ShenandoahFreeSet::adjust_bounds() {
  // Rewind both mutator bounds until the next bit.
  while (_mutator_leftmost < _max && !is_mutator_free(_mutator_leftmost)) {
    _mutator_leftmost++;
  }
  while (_mutator_rightmost > 0 && !is_mutator_free(_mutator_rightmost)) {
    _mutator_rightmost--;
  }
  // Rewind both collector bounds until the next bit.
  while (_collector_leftmost < _max && !is_collector_free(_collector_leftmost)) {
    _collector_leftmost++;
  }
  while (_collector_rightmost > 0 && !is_collector_free(_collector_rightmost)) {
    _collector_rightmost--;
  }
}

HeapWord* ShenandoahFreeSet::allocate_contiguous(ShenandoahAllocRequest& req) {
  shenandoah_assert_heaplocked();

  size_t words_size = req.size();
  size_t num = ShenandoahHeapRegion::required_regions(words_size * HeapWordSize);

  assert(req.affiliation() == ShenandoahRegionAffiliation::YOUNG_GENERATION, "Humongous regions always allocated in YOUNG");
  size_t avail_young_regions = _heap->young_generation()->adjusted_unaffiliated_regions();

  // No regions left to satisfy allocation, bye.
  if (num > mutator_count() || (num > avail_young_regions)) {
    return NULL;
  }

  // Find the continuous interval of $num regions, starting from $beg and ending in $end,
  // inclusive. Contiguous allocations are biased to the beginning.

  size_t beg = _mutator_leftmost;
  size_t end = beg;

  while (true) {
    if (end >= _max) {
      // Hit the end, goodbye
      return NULL;
    }

    // If regions are not adjacent, then current [beg; end] is useless, and we may fast-forward.
    // If region is not completely free, the current [beg; end] is useless, and we may fast-forward.
    if (!is_mutator_free(end) || !can_allocate_from(_heap->get_region(end))) {
      end++;
      beg = end;
      continue;
    }

    if ((end - beg + 1) == num) {
      // found the match
      break;
    }

    end++;
  };

  size_t remainder = words_size & ShenandoahHeapRegion::region_size_words_mask();
  ShenandoahMarkingContext* const ctx = _heap->complete_marking_context();

  // Initialize regions:
  for (size_t i = beg; i <= end; i++) {
    ShenandoahHeapRegion* r = _heap->get_region(i);
    try_recycle_trashed(r);

    assert(i == beg || _heap->get_region(i - 1)->index() + 1 == r->index(), "Should be contiguous");
    assert(r->is_empty(), "Should be empty");

    if (i == beg) {
      r->make_humongous_start();
    } else {
      r->make_humongous_cont();
    }

    // Trailing region may be non-full, record the remainder there
    size_t used_words;
    if ((i == end) && (remainder != 0)) {
      used_words = remainder;
    } else {
      used_words = ShenandoahHeapRegion::region_size_words();
    }

    r->set_affiliation(req.affiliation(), false);
    r->set_update_watermark(r->bottom());
    r->set_top(r->bottom());    // Set top to bottom so we can capture TAMS
    ctx->capture_top_at_mark_start(r);
    r->set_top(r->bottom() + used_words); // Then change top to reflect allocation of humongous object.
    assert(ctx->top_at_mark_start(r) == r->bottom(), "Newly established allocation region starts with TAMS equal to bottom");
    assert(ctx->is_bitmap_clear_range(ctx->top_bitmap(r), r->end()), "Bitmap above top_bitmap() must be clear");

    // Leave top_bitmap alone.  The first time a heap region is put into service, top_bitmap should equal end.
    // Thereafter, it should represent the upper bound on parts of the bitmap that need to be cleared.
    // ctx->clear_bitmap(r);
    log_debug(gc)("NOT clearing bitmap for Humongous region [" PTR_FORMAT ", " PTR_FORMAT "], top_bitmap: "
                  PTR_FORMAT " at transition from FREE to %s",
                  p2i(r->bottom()), p2i(r->end()), p2i(ctx->top_bitmap(r)), affiliation_name(req.affiliation()));

    _mutator_free_bitmap.clear_bit(r->index());
  }

  // While individual regions report their true use, all humongous regions are
  // marked used in the free set.
  size_t total_humongous_size = ShenandoahHeapRegion::region_size_bytes() * num;
  size_t humongous_waste = total_humongous_size - words_size * HeapWordSize;
  increase_used(total_humongous_size);
  if (req.affiliation() == ShenandoahRegionAffiliation::YOUNG_GENERATION) {
    _heap->young_generation()->increase_used(words_size * HeapWordSize);
    _heap->young_generation()->increase_humongous_waste(humongous_waste);
  } else if (req.affiliation() == ShenandoahRegionAffiliation::OLD_GENERATION) {
    _heap->old_generation()->increase_used(words_size * HeapWordSize);
    _heap->old_generation()->increase_humongous_waste(humongous_waste);
  }

  if (remainder != 0) {
    // Record this remainder as allocation waste
    size_t waste = ShenandoahHeapRegion::region_size_words() - remainder;
    _heap->notify_mutator_alloc_words(waste, true);
    _heap->generation_for(req.affiliation())->increase_allocated(waste * HeapWordSize);
  }

  // Allocated at left/rightmost? Move the bounds appropriately.
  if (beg == _mutator_leftmost || end == _mutator_rightmost) {
    adjust_bounds();
  }
  assert_bounds();
  req.set_actual_size(words_size);
  return _heap->get_region(beg)->bottom();
}

bool ShenandoahFreeSet::can_allocate_from(ShenandoahHeapRegion *r) {
  return r->is_empty() || (r->is_trash() && !_heap->is_concurrent_weak_root_in_progress());
}

size_t ShenandoahFreeSet::alloc_capacity(ShenandoahHeapRegion *r) const {
  if (r->is_trash()) {
    // This would be recycled on allocation path
    return ShenandoahHeapRegion::region_size_bytes();
  } else {
    return r->free();
  }
}

bool ShenandoahFreeSet::has_no_alloc_capacity(ShenandoahHeapRegion *r) {
  return alloc_capacity(r) == 0;
}

void ShenandoahFreeSet::try_recycle_trashed(ShenandoahHeapRegion *r) {
  if (r->is_trash()) {
    _heap->decrease_used(r->used());
#undef KELVIN_HEAP_USAGE
#ifdef KELVIN_HEAP_USAGE
    log_info(gc, ergo)("decreasing heap used by " SIZE_FORMAT " for recycle trash of region " SIZE_FORMAT ", result: " SIZE_FORMAT,
                       r->used(), r->index(), _heap->used());
#endif
    r->recycle();
  }
}

void ShenandoahFreeSet::recycle_trash() {
  // lock is not reentrable, check we don't have it
  shenandoah_assert_not_heaplocked();
  for (size_t i = 0; i < _heap->num_regions(); i++) {
    ShenandoahHeapRegion* r = _heap->get_region(i);
    if (r->is_trash()) {
      ShenandoahHeapLocker locker(_heap->lock());
      try_recycle_trashed(r);
    }
    SpinPause(); // allow allocators to take the lock
  }
}

void ShenandoahFreeSet::flip_to_gc(ShenandoahHeapRegion* r) {
  size_t idx = r->index();

  assert(_mutator_free_bitmap.at(idx), "Should be in mutator view");
  assert(can_allocate_from(r), "Should not be allocated");

  _mutator_free_bitmap.clear_bit(idx);
  _collector_free_bitmap.set_bit(idx);
  _collector_leftmost = MIN2(idx, _collector_leftmost);
  _collector_rightmost = MAX2(idx, _collector_rightmost);

  _capacity -= alloc_capacity(r);

  if (touches_bounds(idx)) {
    adjust_bounds();
  }
  assert_bounds();

  // We do not ensure that the region is no longer trash,
  // relying on try_allocate_in(), which always comes next,
  // to recycle trash before attempting to allocate anything in the region.
}

void ShenandoahFreeSet::clear() {
  shenandoah_assert_heaplocked();
  clear_internal();
}

void ShenandoahFreeSet::clear_internal() {
  _mutator_free_bitmap.clear();
  _collector_free_bitmap.clear();
  _mutator_leftmost = _max;
  _mutator_rightmost = 0;
  _collector_leftmost = _max;
  _collector_rightmost = 0;
  _capacity = 0;
  _used = 0;
}

// Return the amount of young-gen memory that is about to be reycled
void ShenandoahFreeSet::prepare_to_rebuild(size_t &young_cset_regions, size_t &old_cset_regions) {
  size_t region_size_bytes = ShenandoahHeapRegion::region_size_bytes();
  shenandoah_assert_heaplocked();
  clear();
#undef KELVIN_REBUILD
#ifdef KELVIN_REBUILD
  log_info(gc, ergo)("prepare for freeset->rebuild");
  young_cset_regions = 0;
  old_cset_regions = 0;
#endif
  log_debug(gc)("Rebuilding FreeSet");
  for (size_t idx = 0; idx < _heap->num_regions(); idx++) {
    ShenandoahHeapRegion* region = _heap->get_region(idx);
    if (region->is_alloc_allowed() || region->is_trash()) {
      assert(!region->is_cset(), "Shouldn't be adding those to the free set");

      // Do not add regions that would surely fail allocation
      if (has_no_alloc_capacity(region)) continue;
      if (region->is_old() && !region->is_trash()) {
#ifdef KELVIN_REBUILD
        log_info(gc, ergo)("OJO! Should not add treasured old region " SIZE_FORMAT " to mutator-is-free", region->index());
#endif        
        continue;
      }

      if (region->is_trash()) {
        if (region->is_young()) {
          young_cset_regions++;
        } else {
          assert(region->is_old(), "Better be old if not young");
          old_cset_regions++;
        }
      }

      _capacity += alloc_capacity(region);
      assert(_used <= _capacity, "must not use more than we have");

      assert(!is_mutator_free(idx), "We are about to add it, it shouldn't be there already");
      _mutator_free_bitmap.set_bit(idx);

#ifdef KELVIN_REBUILD
      log_info(gc, ergo)("Setting Region " SIZE_FORMAT " _mutator_free_bitmap bit to true with available: " SIZE_FORMAT,
                         idx, region->free());
#endif
      log_debug(gc)("  Setting Region " SIZE_FORMAT " _mutator_free_bitmap bit to true", idx);
    }
  }
#undef KELVIN_REGION_TALLIES
#ifdef KELVIN_REGION_TALLIES
  log_info(gc, ergo)("Young cset regions: " SIZE_FORMAT ", Old cset regions: " SIZE_FORMAT,
                    young_cset_regions, old_cset_regions);
#endif
}

// If young_reserve equals zero, compute young reserve from ShenandoahEvacReserve.  Otherwise, use the value supplied
// as input (which may be smaller than ShenandoanEvacReserve, as calculated from the know size of collection set.
void ShenandoahFreeSet::rebuild(size_t young_reserve) {
  shenandoah_assert_heaplocked();

#ifdef KELVIN_REBUILD
  log_info(gc, ergo)("free_set->rebuild");
#endif
  // Evac reserve: reserve trailing space for evacuations
  if (!_heap->mode()->is_generational()) {
    size_t to_reserve = (_heap->max_capacity() / 100) * ShenandoahEvacReserve;
    reserve_regions(to_reserve);
  } else {
    if (young_reserve == 0) {
      young_reserve = (_heap->young_generation()->max_capacity() / 100) * ShenandoahEvacReserve;
    }
#ifdef KELVIN_REBUILD
    log_info(gc, ergo)("Rebuild reserving " SIZE_FORMAT ", default: " SIZE_FORMAT,
                       young_reserve, (_heap->young_generation()->max_capacity() / 100) * ShenandoahEvacReserve);

#endif
    // Note that all allocations performed from old-gen are performed by GC, generally using PLABs for both
    // promotions and evacuations.  The partition between which old memory is reserved for evacuation and
    // which is reserved for promotion is enforced using thread-local variables that prescribe intentons within
    // each PLAB.  We do not reserve any of old-gen memory in order to facilitate the loaning of old-gen memory
    // to young-gen purposes.

    // All old allocations are performed by the GC rather than the mutator, so these allocations need to be
    // satisfied by is_collector_free regions.
    size_t old_reserve = 0;
    size_t to_reserve = young_reserve + old_reserve;
    reserve_regions(to_reserve);
  }

  recompute_bounds();
  assert_bounds();
}

void ShenandoahFreeSet::reserve_regions(size_t to_reserve) {
  size_t reserved = 0;
#ifdef KELVIN_REBUILD
  size_t total_young_available = 0;
  size_t young_available_reserved = 0;
  size_t young_available_mutatable = 0;
  size_t num_regions = _heap->num_regions();
  size_t first_mutator = num_regions;
  size_t last_mutator = 0;
  size_t first_collector = num_regions;
  size_t last_collector = 0;
  size_t lost_available = 0;
  size_t old_available = 0;
#endif
  for (size_t count = _heap->num_regions(); count > 0; count--) {
    size_t idx = count - 1;
    ShenandoahHeapRegion* region = _heap->get_region(idx);
#ifdef KELVIN_REBUILD
    if (reserved >= to_reserve) {
      if (_mutator_free_bitmap.at(idx)) {
        if (idx > last_mutator) {
          last_mutator = idx;
        }
        if (idx < first_mutator) {
          first_mutator = idx;
        }
        size_t ac = alloc_capacity(region);
        if (can_allocate_from(region)) {
          young_available_mutatable += ac;
          total_young_available += ac;
          log_info(gc, ergo)("Mutator free %s region " SIZE_FORMAT " with capacity " SIZE_FORMAT " remains",
                             affiliation_name(region->affiliation()), idx, ac);
        } else {
          size_t available = (region->end() - region->top()) * HeapWordSize;
          if (region->is_old()) {
            old_available += available;
          } else {
            lost_available += available;
          }
          log_info(gc, ergo)("Cannot allocate from mutator-free region " SIZE_FORMAT
                             " (%s, %s, %s, age: %d), available: " SIZE_FORMAT,
                             idx, region->is_old()? "old": region->is_young()? "young": region->affiliation() == FREE? "free": "unaffiliated",
                             region->is_empty()? "empty": "not empty", region->is_trash()? "trash": "treasure", region->age(),
                             available);
        }
      } else {
        size_t available = region->free();
        if (region->is_old()) {
          old_available += available;
        } else {
          lost_available += available;
        }
        log_info(gc, ergo)("Region " SIZE_FORMAT " is not mutator_free (%s, %s, %s, age: %d), available: " SIZE_FORMAT,
                           idx, region->is_old()? "old": region->is_young()? "young": region->affiliation() == FREE? "free": "unaffiliated",
                           region->is_empty()? "empty": "not empty", region->is_trash()? "trash": "treasure", region->age(),
                           available);
      }
    } else
#else
    if (reserved >= to_reserve) break;
#endif
#ifdef KELVIN_REBUILD
    if (_mutator_free_bitmap.at(idx)) {
      if (region->free() > 0) {
        _mutator_free_bitmap.clear_bit(idx);
        _collector_free_bitmap.set_bit(idx);
        size_t ac = alloc_capacity(region);
        _capacity -= ac;
        reserved += ac;

        // kelvin logging
        if (idx > last_collector) {
          last_collector = idx;
        }
        if (idx < first_collector) {
          first_collector = idx;
        }

        young_available_reserved += ac;
        total_young_available += ac;
        log_info(gc, ergo)("Shifting region " SIZE_FORMAT " with capacity " SIZE_FORMAT " from mutator_free to collector_free",
                           idx, ac);
      } else {
        size_t available = (region->end() - region->top()) * HeapWordSize;
        if (region->is_old()) {
          old_available += available;
        } else {
          lost_available += available;
        }
        log_info(gc, ergo)("Cannot allocate from mutator-free region " SIZE_FORMAT
                           " (%s, %s, %s, age: %d), available: " SIZE_FORMAT,
                           idx, region->is_old()? "old": region->is_young()? "young": region->affiliation() == FREE? "free": "unaffiliated",
                           region->is_empty()? "empty": "not empty", region->is_trash()? "trash": "treasure", region->age(),
                           available);

        // leave this as mutator_Free
        if (idx > last_mutator) {
          last_mutator = idx;
        }
        if (idx < first_mutator) {
          first_mutator = idx;
        }
      }
    } else {
      size_t available = (region->end() - region->top()) * HeapWordSize;
      if (region->is_old()) {
        old_available += available;
      } else {
        lost_available += available;
      }
      log_info(gc, ergo)("Region " SIZE_FORMAT " is not mutator_free (%s, %s, %s, age: %d), available: " SIZE_FORMAT,
                         idx, region->is_old()? "old": region->is_young()? "young": region->affiliation() == FREE? "free": "unaffiliated",
                         region->is_empty()? "empty": "not empty", region->is_trash()? "trash": "treasure", region->age(),
                         available);
    }
#else
    if (_mutator_free_bitmap.at(idx) && (region->free() > 0)) {
      _mutator_free_bitmap.clear_bit(idx);
      _collector_free_bitmap.set_bit(idx);
      size_t ac = alloc_capacity(region);
      _capacity -= ac;
      reserved += ac;
      log_debug(gc)("  Shifting region " SIZE_FORMAT " from mutator_free to collector_free", idx);
    }
#endif
  }
#ifdef KELVIN_REBUILD
  log_info(gc, ergo)("after reserve regions, total young available: " SIZE_FORMAT ", of which: " SIZE_FORMAT
                     " is reserved for collector, lost available: " SIZE_FORMAT ", old_available: " SIZE_FORMAT,
                     total_young_available, young_available_reserved, lost_available, old_available);
  log_info(gc, ergo)("  first_mutator: " SIZE_FORMAT " last_mutator: " SIZE_FORMAT
                     ", first_collector: " SIZE_FORMAT " last_collector: " SIZE_FORMAT,
                     first_mutator, last_mutator, first_collector, last_collector);
#endif
}

void ShenandoahFreeSet::log_status() {
  shenandoah_assert_heaplocked();

  LogTarget(Info, gc, ergo) lt;
  if (lt.is_enabled()) {
    ResourceMark rm;
    LogStream ls(lt);

    {
      size_t last_idx = 0;
      size_t max = 0;
      size_t max_contig = 0;
      size_t empty_contig = 0;

      size_t total_used = 0;
      size_t total_free = 0;
      size_t total_free_ext = 0;

      for (size_t idx = _mutator_leftmost; idx <= _mutator_rightmost; idx++) {
        if (is_mutator_free(idx)) {
          ShenandoahHeapRegion *r = _heap->get_region(idx);
          size_t free = alloc_capacity(r);

          max = MAX2(max, free);

          if (r->is_empty()) {
            total_free_ext += free;
            if (last_idx + 1 == idx) {
              empty_contig++;
            } else {
              empty_contig = 1;
            }
          } else {
            empty_contig = 0;
          }

          total_used += r->used();
          total_free += free;

          max_contig = MAX2(max_contig, empty_contig);
          last_idx = idx;
        }
      }

      size_t max_humongous = max_contig * ShenandoahHeapRegion::region_size_bytes();
      size_t free = capacity() - used();

      ls.print("Free: " SIZE_FORMAT "%s, Max: " SIZE_FORMAT "%s regular, " SIZE_FORMAT "%s humongous, ",
               byte_size_in_proper_unit(total_free),    proper_unit_for_byte_size(total_free),
               byte_size_in_proper_unit(max),           proper_unit_for_byte_size(max),
               byte_size_in_proper_unit(max_humongous), proper_unit_for_byte_size(max_humongous)
      );

      ls.print("Frag: ");
      size_t frag_ext;
      if (total_free_ext > 0) {
        frag_ext = 100 - (100 * max_humongous / total_free_ext);
      } else {
        frag_ext = 0;
      }
      ls.print(SIZE_FORMAT "%% external, ", frag_ext);

      size_t frag_int;
      if (mutator_count() > 0) {
        frag_int = (100 * (total_used / mutator_count()) / ShenandoahHeapRegion::region_size_bytes());
      } else {
        frag_int = 0;
      }
      ls.print(SIZE_FORMAT "%% internal; ", frag_int);
    }

    {
      size_t max = 0;
      size_t total_free = 0;

      for (size_t idx = _collector_leftmost; idx <= _collector_rightmost; idx++) {
        if (is_collector_free(idx)) {
          ShenandoahHeapRegion *r = _heap->get_region(idx);
          size_t free = alloc_capacity(r);
          max = MAX2(max, free);
          total_free += free;
        }
      }

      ls.print_cr("Reserve: " SIZE_FORMAT "%s, Max: " SIZE_FORMAT "%s",
                  byte_size_in_proper_unit(total_free), proper_unit_for_byte_size(total_free),
                  byte_size_in_proper_unit(max),        proper_unit_for_byte_size(max));
    }
  }
}

HeapWord* ShenandoahFreeSet::allocate(ShenandoahAllocRequest& req, bool& in_new_region) {
  shenandoah_assert_heaplocked();
  assert_bounds();

  // Allocation request is known to satisfy all memory budgeting constraints.
  if (req.size() > ShenandoahHeapRegion::humongous_threshold_words()) {
    switch (req.type()) {
      case ShenandoahAllocRequest::_alloc_shared:
      case ShenandoahAllocRequest::_alloc_shared_gc:
        in_new_region = true;
        return allocate_contiguous(req);
      case ShenandoahAllocRequest::_alloc_plab:
      case ShenandoahAllocRequest::_alloc_gclab:
      case ShenandoahAllocRequest::_alloc_tlab:
        in_new_region = false;
        assert(false, "Trying to allocate TLAB larger than the humongous threshold: " SIZE_FORMAT " > " SIZE_FORMAT,
               req.size(), ShenandoahHeapRegion::humongous_threshold_words());
        return NULL;
      default:
        ShouldNotReachHere();
        return NULL;
    }
  } else {
    return allocate_single(req, in_new_region);
  }
}

size_t ShenandoahFreeSet::unsafe_peek_free() const {
  // Deliberately not locked, this method is unsafe when free set is modified.

#ifdef KELVIN_ALLOCATE
  {
    size_t num_regions = _heap->num_regions();
    size_t left_mutator = num_regions;
    size_t right_mutator = 0;
    size_t left_collector = num_regions;
    size_t right_collector = 0;
    size_t mutatable_free = 0;
    size_t collectable_free = 0;

    size_t total_old_regions = 0;
    size_t total_young_regions = 0;
    size_t total_free_regions = 0;
    size_t total_old_used = 0;
    size_t total_young_used = 0;
    size_t total_old_avail_fragments = 0;
    size_t total_young_avail_fragments = 0;

    size_t total_mutator_regions = 0;
    size_t total_collector_regions = 0;
    size_t total_mutator_used = 0;
    size_t total_collector_used = 0;
    size_t total_mutator_avail_fragments = 0;
    size_t total_collector_avail_fragments = 0;


    {
      // When called from allocate_single, we already own the lock I think
      // ShenandoahHeapLocker locker(_heap->lock());
      //
      for (size_t index = 0; index < num_regions; index++) {
        ShenandoahHeapRegion *r = _heap->get_region(index);
        size_t available = r->free();
        size_t usage = r->used();
        if (r->is_old()) {
          total_old_regions++;
          total_old_used += usage;
          total_old_avail_fragments += available;
        } else if (r->is_young()) {
          total_young_regions++;
          total_young_used += usage;
          total_young_avail_fragments += available;
        } else if (r->affiliation() == FREE) {
          total_free_regions++;
        } else {
          log_info(gc, ergo)("OJO! Region " SIZE_FORMAT " has unexpected affiliation: %s",
                             r->index(), affiliation_name(r->affiliation()));
        }

        if (is_mutator_free(index)) {
          log_info(gc, ergo)("Region " SIZE_FORMAT " is mutator_free (%s, %s, %s, age: %d), available: " SIZE_FORMAT,
                             r->index(), affiliation_name(r->affiliation()),
                             r->is_empty()? "empty": "not empty", r->is_trash()? "trash": "treasure", r->age(),
                             available);
          mutatable_free += alloc_capacity(r);
          if (index > right_mutator) {
            right_mutator = index;
          }
          if (index < left_mutator) {
            left_mutator = index;
          }
        } else if (is_collector_free(index)) {
          log_info(gc, ergo)("Region " SIZE_FORMAT " is collector_free (%s, %s, %s, age: %d), available: " SIZE_FORMAT,
                             r->index(), affiliation_name(r->affiliation()),
                             r->is_empty()? "empty": "not empty", r->is_trash()? "trash": "treasure", r->age(),
                             available);
          collectable_free += alloc_capacity(r);
          if (index > right_collector) {
            right_collector = index;
          }
          if (index < left_collector) {
            left_collector = index;
          }
        } else {
          log_info(gc, ergo)("Region " SIZE_FORMAT " is neither_free (%s, %s, %s, age: %d), available: " SIZE_FORMAT,
                             r->index(), affiliation_name(r->affiliation()),
                             r->is_empty()? "empty": "not empty", r->is_trash()? "trash": "treasure", r->age(),
                             available);
        }
      }
    }
    log_info(gc, ergo)(" mutator_free: " SIZE_FORMAT " collector_free: " SIZE_FORMAT, mutatable_free, collectable_free);
    log_info(gc, ergo)("  computed ranges for mutator (" SIZE_FORMAT ", " SIZE_FORMAT "), collector: (" SIZE_FORMAT ", " SIZE_FORMAT ")",
                       left_mutator, right_mutator, left_collector, right_collector);
    log_info(gc, ergo)("  remembered  ranges for mutator (" SIZE_FORMAT ", " SIZE_FORMAT "), collector: (" SIZE_FORMAT ", " SIZE_FORMAT ")",
                       _mutator_leftmost, _mutator_rightmost, _collector_leftmost, _collector_rightmost);
    log_info(gc, ergo)("  Tallied old regions: " SIZE_FORMAT " with " SIZE_FORMAT " used and " SIZE_FORMAT " available",
                       total_old_regions, total_old_used, total_old_avail_fragments);
    log_info(gc, ergo)("Tallied young regions: " SIZE_FORMAT " with " SIZE_FORMAT " used and " SIZE_FORMAT " available",
                       total_young_regions, total_young_used, total_young_avail_fragments);
    log_info(gc, ergo)(" Tallied free regions: " SIZE_FORMAT " with " SIZE_FORMAT " available",
                       total_free_regions, total_free_regions * ShenandoahHeapRegion::region_size_bytes());
  }
#endif

  for (size_t index = _mutator_leftmost; index <= _mutator_rightmost; index++) {
    if (index < _max && is_mutator_free(index)) {
      ShenandoahHeapRegion* r = _heap->get_region(index);
      if (r->free() >= MinTLABSize) {
        return r->free();
      }
    }
  }

  // It appears that no regions left
  return 0;
}

void ShenandoahFreeSet::print_on(outputStream* out) const {
  out->print_cr("Mutator Free Set: " SIZE_FORMAT "", mutator_count());
  for (size_t index = _mutator_leftmost; index <= _mutator_rightmost; index++) {
    if (is_mutator_free(index)) {
      _heap->get_region(index)->print_on(out);
    }
  }
  out->print_cr("Collector Free Set: " SIZE_FORMAT "", collector_count());
  for (size_t index = _collector_leftmost; index <= _collector_rightmost; index++) {
    if (is_collector_free(index)) {
      _heap->get_region(index)->print_on(out);
    }
  }
}

/*
 * Internal fragmentation metric: describes how fragmented the heap regions are.
 *
 * It is derived as:
 *
 *               sum(used[i]^2, i=0..k)
 *   IF = 1 - ------------------------------
 *              C * sum(used[i], i=0..k)
 *
 * ...where k is the number of regions in computation, C is the region capacity, and
 * used[i] is the used space in the region.
 *
 * The non-linearity causes IF to be lower for the cases where the same total heap
 * used is densely packed. For example:
 *   a) Heap is completely full  => IF = 0
 *   b) Heap is half full, first 50% regions are completely full => IF = 0
 *   c) Heap is half full, each region is 50% full => IF = 1/2
 *   d) Heap is quarter full, first 50% regions are completely full => IF = 0
 *   e) Heap is quarter full, each region is 25% full => IF = 3/4
 *   f) Heap has one small object per each region => IF =~ 1
 */
double ShenandoahFreeSet::internal_fragmentation() {
  double squared = 0;
  double linear = 0;
  int count = 0;

  for (size_t index = _mutator_leftmost; index <= _mutator_rightmost; index++) {
    if (is_mutator_free(index)) {
      ShenandoahHeapRegion* r = _heap->get_region(index);
      size_t used = r->used();
      squared += used * used;
      linear += used;
      count++;
    }
  }

  if (count > 0) {
    double s = squared / (ShenandoahHeapRegion::region_size_bytes() * linear);
    return 1 - s;
  } else {
    return 0;
  }
}

/*
 * External fragmentation metric: describes how fragmented the heap is.
 *
 * It is derived as:
 *
 *   EF = 1 - largest_contiguous_free / total_free
 *
 * For example:
 *   a) Heap is completely empty => EF = 0
 *   b) Heap is completely full => EF = 0
 *   c) Heap is first-half full => EF = 1/2
 *   d) Heap is half full, full and empty regions interleave => EF =~ 1
 */
double ShenandoahFreeSet::external_fragmentation() {
  size_t last_idx = 0;
  size_t max_contig = 0;
  size_t empty_contig = 0;

  size_t free = 0;

  for (size_t index = _mutator_leftmost; index <= _mutator_rightmost; index++) {
    if (is_mutator_free(index)) {
      ShenandoahHeapRegion* r = _heap->get_region(index);
      if (r->is_empty()) {
        free += ShenandoahHeapRegion::region_size_bytes();
        if (last_idx + 1 == index) {
          empty_contig++;
        } else {
          empty_contig = 1;
        }
      } else {
        empty_contig = 0;
      }

      max_contig = MAX2(max_contig, empty_contig);
      last_idx = index;
    }
  }

  if (free > 0) {
    return 1 - (1.0 * max_contig * ShenandoahHeapRegion::region_size_bytes() / free);
  } else {
    return 0;
  }
}

#ifdef ASSERT
void ShenandoahFreeSet::assert_bounds() const {
  // Performance invariants. Failing these would not break the free set, but performance
  // would suffer.
  assert (_mutator_leftmost <= _max, "leftmost in bounds: "  SIZE_FORMAT " < " SIZE_FORMAT, _mutator_leftmost,  _max);
  assert (_mutator_rightmost < _max, "rightmost in bounds: " SIZE_FORMAT " < " SIZE_FORMAT, _mutator_rightmost, _max);

  assert (_mutator_leftmost == _max || is_mutator_free(_mutator_leftmost),  "leftmost region should be free: " SIZE_FORMAT,  _mutator_leftmost);
  assert (_mutator_rightmost == 0   || is_mutator_free(_mutator_rightmost), "rightmost region should be free: " SIZE_FORMAT, _mutator_rightmost);

  size_t beg_off = _mutator_free_bitmap.get_next_one_offset(0);
  size_t end_off = _mutator_free_bitmap.get_next_one_offset(_mutator_rightmost + 1);
  assert (beg_off >= _mutator_leftmost, "free regions before the leftmost: " SIZE_FORMAT ", bound " SIZE_FORMAT, beg_off, _mutator_leftmost);
  assert (end_off == _max,      "free regions past the rightmost: " SIZE_FORMAT ", bound " SIZE_FORMAT,  end_off, _mutator_rightmost);

  assert (_collector_leftmost <= _max, "leftmost in bounds: "  SIZE_FORMAT " < " SIZE_FORMAT, _collector_leftmost,  _max);
  assert (_collector_rightmost < _max, "rightmost in bounds: " SIZE_FORMAT " < " SIZE_FORMAT, _collector_rightmost, _max);

  assert (_collector_leftmost == _max || is_collector_free(_collector_leftmost),  "leftmost region should be free: " SIZE_FORMAT,  _collector_leftmost);
  assert (_collector_rightmost == 0   || is_collector_free(_collector_rightmost), "rightmost region should be free: " SIZE_FORMAT, _collector_rightmost);

  beg_off = _collector_free_bitmap.get_next_one_offset(0);
  end_off = _collector_free_bitmap.get_next_one_offset(_collector_rightmost + 1);
  assert (beg_off >= _collector_leftmost, "free regions before the leftmost: " SIZE_FORMAT ", bound " SIZE_FORMAT, beg_off, _collector_leftmost);
  assert (end_off == _max,      "free regions past the rightmost: " SIZE_FORMAT ", bound " SIZE_FORMAT,  end_off, _collector_rightmost);
}
#endif

/*
 * Copyright (c) 2016, 2021, Red Hat, Inc. All rights reserved.
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
#include "gc/shared/tlab_globals.hpp"
#include "gc/shenandoah/shenandoahAffiliation.hpp"
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

ShenandoahFreeSet::ShenandoahFreeSet(ShenandoahHeap* heap, size_t max_regions) :
  _heap(heap),
  _mutator_free_bitmap(max_regions, mtGC),
  _collector_free_bitmap(max_regions, mtGC),
  _old_collector_free_bitmap(max_regions, mtGC),
  _max(max_regions)
{
  clear_internal();
}

inline void ShenandoahFreeSet::increase_used(size_t num_bytes) {
  shenandoah_assert_heaplocked();
  _used += num_bytes;
  assert(_used <= _capacity, "must not use (" SIZE_FORMAT ") more than we have (" SIZE_FORMAT ") after increase by " SIZE_FORMAT,
         _used, _capacity, num_bytes);
}

template <MemoryReserve SET> inline bool ShenandoahFreeSet::probe_set(size_t idx) const {
  assert (idx < _max, "index is sane: " SIZE_FORMAT " < " SIZE_FORMAT " (left: " SIZE_FORMAT ", right: " SIZE_FORMAT ")",
          idx, _max, _collector_leftmost, _collector_rightmost);
  switch(SET) {
    case Mutator:
      return _mutator_free_bitmap.at(idx);
    case Collector:
      return _collector_free_bitmap.at(idx);
    case OldCollector:
      return _old_collector_free_bitmap.at(idx);
  }
}

template <MemoryReserve SET> inline bool ShenandoahFreeSet::in_set(size_t idx) const {
  assert (idx < _max, "index is sane: " SIZE_FORMAT " < " SIZE_FORMAT " (left: " SIZE_FORMAT ", right: " SIZE_FORMAT ")",
          idx, _max, _mutator_leftmost, _mutator_rightmost);
  bool is_free;
  switch(SET) {
    case Mutator:
      is_free = _mutator_free_bitmap.at(idx);
      break;
    case Collector:
      is_free = _collector_free_bitmap.at(idx);;
      break;
    case OldCollector:
      is_free = _old_collector_free_bitmap.at(idx);
      break;
  }
  assert(!is_free || has_alloc_capacity(idx), "Free set should contain useful regions");
  return is_free;
}

template <MemoryReserve SET> inline void ShenandoahFreeSet::expand_bounds_maybe(size_t idx) {
  assert (idx < _max, "index is sane: " SIZE_FORMAT " < " SIZE_FORMAT " (left: " SIZE_FORMAT ", right: " SIZE_FORMAT ")",
          idx, _max, _mutator_leftmost, _mutator_rightmost);
  switch(SET) {
    case Mutator:
      if (idx < _mutator_leftmost) {
        _mutator_leftmost = idx;
      }
      if (idx > _mutator_rightmost) {
        _mutator_rightmost = idx;
      }
      break;
    case Collector:
      if (idx < _collector_leftmost) {
        _collector_leftmost = idx;
      }
      if (idx > _collector_rightmost) {
        _collector_rightmost = idx;
      }
      break;
    case OldCollector:
      if (idx < _old_collector_leftmost) {
        _old_collector_leftmost = idx;
      }
      if (idx > _old_collector_rightmost) {
        _old_collector_rightmost = idx;
      }
      break;
  }
}

template <MemoryReserve SET> inline void ShenandoahFreeSet::add_to_set(size_t idx) {
  assert (idx < _max, "index is sane: " SIZE_FORMAT " < " SIZE_FORMAT " (left: " SIZE_FORMAT ", right: " SIZE_FORMAT ")",
          idx, _max, _mutator_leftmost, _mutator_rightmost);
  assert(has_alloc_capacity(idx), "Regions added to free set should have allocation capacity");
  switch(SET) {
    case Mutator:
      assert(!_collector_free_bitmap.at(idx) && !_old_collector_free_bitmap.at(idx), "Freeset membership is mutually exclusive");
      _mutator_free_bitmap.set_bit(idx);
      break;
    case Collector:
      assert(!_mutator_free_bitmap.at(idx) && !_old_collector_free_bitmap.at(idx), "Freeset membership is mutually exclusive");
      _collector_free_bitmap.set_bit(idx);
      break;
    case OldCollector:
      assert(!_mutator_free_bitmap.at(idx) && !_collector_free_bitmap.at(idx), "Freeset membership is mutually exclusive");
      _old_collector_free_bitmap.set_bit(idx);
      break;
  }
  expand_bounds_maybe<SET>(idx);
}

template <MemoryReserve SET> inline void ShenandoahFreeSet::remove_from_set(size_t idx) {
  assert (idx < _max, "index is sane: " SIZE_FORMAT " < " SIZE_FORMAT " (left: " SIZE_FORMAT ", right: " SIZE_FORMAT ")",
          idx, _max, _mutator_leftmost, _mutator_rightmost);
  switch(SET) {
    case Mutator:
      _mutator_free_bitmap.clear_bit(idx);
      break;
    case Collector:
      _collector_free_bitmap.clear_bit(idx);
      break;
    case OldCollector:
      _old_collector_free_bitmap.clear_bit(idx);
      break;
  }
  adjust_bounds_if_touched<SET>(idx);
}

// If idx represents a mutator bound, recompute the mutator bounds, returning true iff bounds were adjusted.
template <MemoryReserve SET> bool ShenandoahFreeSet::adjust_bounds_if_touched(size_t idx) {
  assert (idx < _max, "index is sane: " SIZE_FORMAT " < " SIZE_FORMAT " (left: " SIZE_FORMAT ", right: " SIZE_FORMAT ")",
          idx, _max, _mutator_leftmost, _mutator_rightmost);
  switch(SET) {
    case Mutator:
      if (idx == _mutator_leftmost || idx == _mutator_rightmost) {
        // Rewind both mutator bounds until the next bit.
        while (_mutator_leftmost < _max && !_mutator_free_bitmap.at(_mutator_leftmost)) {
          _mutator_leftmost++;
        }
        while (_mutator_rightmost > 0 && !_mutator_free_bitmap.at(_mutator_rightmost)) {
          _mutator_rightmost--;
        }
        return true;
      }
      break;        
    case Collector:
      if (idx == _collector_leftmost || idx == _collector_rightmost) {
        // Rewind both collector bounds until the next bit.
        while (_collector_leftmost < _max && !_collector_free_bitmap.at(_collector_leftmost)) {
          _collector_leftmost++;
        }
        while (_collector_rightmost > 0 && !_collector_free_bitmap.at(_collector_rightmost)) {
          _collector_rightmost--;
        }
        return true;
      }
      break;        
    case OldCollector:
      if (idx == _old_collector_leftmost || idx == _old_collector_rightmost) {
        // Rewind both old_collector bounds until the next bit.
        while (_old_collector_leftmost < _max && !_old_collector_free_bitmap.at(_old_collector_leftmost)) {
          _old_collector_leftmost++;
        }
        while (_old_collector_rightmost > 0 && !_old_collector_free_bitmap.at(_old_collector_rightmost)) {
          _old_collector_rightmost--;
        }
        return true;
      }
      break;        
  }
  return false;
}

// This allocates from a region within the old_collector_set.  If affiliation equals OLD, the allocation must be taken
// from a region that is_old().  Otherwise, affiliation should be FREE, in which case this will put a previously unaffiliated
// region into service.
HeapWord* ShenandoahFreeSet::allocate_old_with_affiliation(ShenandoahAffiliation affiliation,
                                                           ShenandoahAllocRequest& req, bool& in_new_region) {
  shenandoah_assert_heaplocked();
  size_t rightmost = _old_collector_rightmost;
  size_t leftmost = _old_collector_leftmost;
  if (_old_collector_search_left_to_right) {
    // This mode picks up stragglers left by a full GC
    for (size_t c = leftmost; c <= rightmost; c++) {
      if (in_set<OldCollector>(c)) {
        ShenandoahHeapRegion* r = _heap->get_region(c);
        assert(r->is_trash() || !r->is_affiliated() || r->is_old(), "old_collector_set region has bad affiliation");
        if (r->affiliation() == affiliation) {
          HeapWord* result = try_allocate_in(r, req, in_new_region);
          if (result != nullptr) {
            return result;
          }
        }
      }
    }
  } else {
    // This mode picks up stragglers left by a previous concurrent GC
    for (size_t c = rightmost + 1; c > leftmost; c--) {
      // size_t is unsigned, need to dodge underflow when _leftmost = 0
      size_t idx = c - 1;
      if (in_set<OldCollector>(idx)) {
        ShenandoahHeapRegion* r = _heap->get_region(idx);
        assert(r->is_trash() || !r->is_affiliated() || r->is_old(), "old_collector_set region has bad affiliation");
        if (r->affiliation() == affiliation) {
          HeapWord* result = try_allocate_in(r, req, in_new_region);
          if (result != nullptr) {
            return result;
          }
        }
      }
    }
  }
  return nullptr;
}

HeapWord* ShenandoahFreeSet::allocate_with_affiliation(ShenandoahAffiliation affiliation, ShenandoahAllocRequest& req, bool& in_new_region) {
  shenandoah_assert_heaplocked();
  for (size_t c = _collector_rightmost + 1; c > _collector_leftmost; c--) {
    // size_t is unsigned, need to dodge underflow when _leftmost = 0
    size_t idx = c - 1;
    if (in_set<Collector>(idx)) {
      ShenandoahHeapRegion* r = _heap->get_region(idx);
      if (r->affiliation() == affiliation) {
        HeapWord* result = try_allocate_in(r, req, in_new_region);
        if (result != nullptr) {
          return result;
        }
      }
    }
  }
  log_debug(gc, free)("Could not allocate collector region with affiliation: %s for request " PTR_FORMAT, shenandoah_affiliation_name(affiliation), p2i(&req));
  return nullptr;
}

HeapWord* ShenandoahFreeSet::allocate_single(ShenandoahAllocRequest& req, bool& in_new_region) {
  shenandoah_assert_heaplocked();

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
  if (_heap->mode()->is_generational()) {
    switch (req.affiliation()) {
      case ShenandoahAffiliation::OLD_GENERATION:
        // Note: unsigned result from adjusted_unaffiliated_regions() will never be less than zero, but it may equal zero.
        if (_heap->old_generation()->adjusted_unaffiliated_regions() <= 0) {
          allow_new_region = false;
        }
        break;

      case ShenandoahAffiliation::YOUNG_GENERATION:
        // Note: unsigned result from adjusted_unaffiliated_regions() will never be less than zero, but it may equal zero.
        if (_heap->young_generation()->adjusted_unaffiliated_regions() <= 0) {
          allow_new_region = false;
        }
        break;

      case ShenandoahAffiliation::FREE:
        fatal("Should request affiliation");

      default:
        ShouldNotReachHere();
        break;
    }
  }

  switch (req.type()) {
    case ShenandoahAllocRequest::_alloc_tlab:
    case ShenandoahAllocRequest::_alloc_shared: {
      // Try to allocate in the mutator view
      for (size_t idx = _mutator_leftmost; idx <= _mutator_rightmost; idx++) {
        ShenandoahHeapRegion* r = _heap->get_region(idx);
        if (in_set<Mutator>(idx) && (allow_new_region || r->is_affiliated())) {
          // try_allocate_in() increases used if the allocation is successful.
          HeapWord* result = try_allocate_in(r, req, in_new_region);
          if (result != nullptr) {
            return result;
          }
        }
      }
      // There is no recovery. Mutator does not touch collector view at all.
      break;
    }
    case ShenandoahAllocRequest::_alloc_gclab:
      // GCLABs are for evacuation so we must be in evacuation phase.  If this allocation is successful, increment
      // the relevant evac_expended rather than used value.

    case ShenandoahAllocRequest::_alloc_plab:
      // PLABs always reside in old-gen and are only allocated during evacuation phase.

    case ShenandoahAllocRequest::_alloc_shared_gc: {
      if (!_heap->mode()->is_generational()) {
        // size_t is unsigned, need to dodge underflow when _leftmost = 0
        // Fast-path: try to allocate in the collector view first
        for (size_t c = _collector_rightmost + 1; c > _collector_leftmost; c--) {
          size_t idx = c - 1;
          if (in_set<Collector>(idx)) {
            HeapWord* result = try_allocate_in(_heap->get_region(idx), req, in_new_region);
            if (result != nullptr) {
              return result;
            }
          }
        }
      } else {
        // First try to fit into a region that is already in use in the same generation.
        HeapWord* result;
        if (req.is_old()) {
          result = allocate_old_with_affiliation(req.affiliation(), req, in_new_region);
        } else {
          result = allocate_with_affiliation(req.affiliation(), req, in_new_region);
        }
        if (result != nullptr) {
          return result;
        }
        if (allow_new_region) {
          // Then try a free region that is dedicated to GC allocations.
          if (req.is_old()) {
            result = allocate_old_with_affiliation(FREE, req, in_new_region);
          } else {
            result = allocate_with_affiliation(FREE, req, in_new_region);
          }
          if (result != nullptr) {
            return result;
          }
        }
      }

      // No dice. Can we borrow space from mutator view?
      if (!ShenandoahEvacReserveOverflow) {
        return nullptr;
      }

      // TODO:
      // if (!allow_new_region && req.is_old() && (young_generation->adjusted_unaffiliated_regions() > 0)) {
      //   transfer a region from young to old;
      //   allow_new_region = true;
      //   heap->set_old_evac_reserve(heap->get_old_evac_reserve() + region_size_bytes);
      // }
      //
      // We should expand old-gen if this can prevent an old-gen evacuation failure.  We don't care so much about
      // promotion failures since they can be mitigated in a subsequent GC pass.  Would be nice to know if this
      // allocation request is for evacuation or promotion.  Individual threads limit their use of PLAB memory for
      // promotions, so we already have an assurance that any additional memory set aside for old-gen will be used
      // only for old-gen evacuations.

      if (allow_new_region) {
        // Try to steal an empty region from the mutator view.
        for (size_t c = _mutator_rightmost + 1; c > _mutator_leftmost; c--) {
          size_t idx = c - 1;
          if (in_set<Mutator>(idx)) {
            ShenandoahHeapRegion* r = _heap->get_region(idx);
            if (can_allocate_from(r)) {
              if (req.is_old()) {
                flip_to_old_gc(r);
              } else {
                flip_to_gc(r);
              }
              HeapWord *result = try_allocate_in(r, req, in_new_region);
              if (result != nullptr) {
                log_debug(gc, free)("Flipped region " SIZE_FORMAT " to gc for request: " PTR_FORMAT, idx, p2i(&req));
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
  return nullptr;
}

HeapWord* ShenandoahFreeSet::try_allocate_in(ShenandoahHeapRegion* r, ShenandoahAllocRequest& req, bool& in_new_region) {
  assert (has_alloc_capacity(r), "Performance: should avoid full regions on this path: " SIZE_FORMAT, r->index());
  if (_heap->is_concurrent_weak_root_in_progress() &&
      r->is_trash()) {
    return nullptr;
  }
  try_recycle_trashed(r);
  if (!r->is_affiliated()) {
    ShenandoahMarkingContext* const ctx = _heap->complete_marking_context();
    r->set_affiliation(req.affiliation());
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
  } else if (r->affiliation() != req.affiliation()) {
    assert(_heap->mode()->is_generational(), "Request for %s from %s region should only happen in generational mode.",
           req.affiliation_name(), r->affiliation_name());
    return nullptr;
  }

  in_new_region = r->is_empty();
  HeapWord* result = nullptr;
  size_t size = req.size();

  if (in_new_region) {
    log_debug(gc, free)("Using new region (" SIZE_FORMAT ") for %s (" PTR_FORMAT ").",
                       r->index(), ShenandoahAllocRequest::alloc_type_to_string(req.type()), p2i(&req));
  }

  // req.size() is in words, r->free() is in bytes.
  if (ShenandoahElasticTLAB && req.is_lab_alloc()) {
    if (req.type() == ShenandoahAllocRequest::_alloc_plab) {
      assert(_heap->mode()->is_generational(), "PLABs are only for generational mode");
      // Need to assure that plabs are aligned on multiple of card region.
      size_t free = r->free();
      // e.g. card_size is 512, card_shift is 9, min_fill_size() is 8
      //      free is 514
      //      usable_free is 512, which is decreased to 0
      size_t usable_free = (free / CardTable::card_size()) << CardTable::card_shift();
      if ((free != usable_free) && (free - usable_free < ShenandoahHeap::min_fill_size() * HeapWordSize)) {
        // We'll have to add another card's memory to the padding
        if (usable_free >= CardTable::card_size()) {
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
           assert(r->is_old(), "All PLABs reside in old-gen");
          _heap->old_generation()->increase_used(padding);
          // For verification consistency, we need to report this padding to _heap
          _heap->increase_used(padding);
        }
      }
      // Otherwise, leave result == nullptr because the adjusted size is smaller than min size.
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
        assert (result != nullptr, "Allocation must succeed: free " SIZE_FORMAT ", actual " SIZE_FORMAT, free, size);
      } else {
        log_trace(gc, free)("Failed to shrink TLAB or GCLAB request (" SIZE_FORMAT ") in region " SIZE_FORMAT " to " SIZE_FORMAT
                           " because min_size() is " SIZE_FORMAT, req.size(), r->index(), size, req.min_size());
      }
    }
  } else if (req.is_lab_alloc() && req.type() == ShenandoahAllocRequest::_alloc_plab) {
    assert(_heap->mode()->is_generational(), "PLABs are only for generational mode");
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
        assert(r->is_old(), "All PLABs reside in old-gen");
        _heap->old_generation()->increase_used(padding);
        // For verification consistency, we need to report this padding to _heap
        _heap->increase_used(padding);
      }
    }
  } else {
    result = r->allocate(size, req);
    if (result != nullptr) {
      // Record actual allocation size
      req.set_actual_size(size);
    }
  }

  ShenandoahGeneration* generation = _heap->generation_for(req.affiliation());
  if (result != nullptr) {
    // Allocation successful, bump stats:
    if (req.is_mutator_alloc()) {
      assert(req.is_young(), "Mutator allocations always come from young generation.");
      generation->increase_used(size * HeapWordSize);
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
      generation->increase_used(size * HeapWordSize);
      if (r->is_old()) {
        assert(req.type() != ShenandoahAllocRequest::_alloc_gclab, "old-gen allocations use PLAB or shared allocation");
        // for plabs, we'll sort the difference between evac and promotion usage when we retire the plab
      }
    }
  }

  if (result == nullptr || has_no_alloc_capacity(r)) {
    // Region cannot afford this and is likely to not afford future allocations. Retire it.
    //
    // While this seems a bit harsh, especially in the case when this large allocation does not
    // fit but the next small one would, we are risking to inflate scan times when lots of
    // almost-full regions precede the fully-empty region where we want to allocate the entire TLAB.
    // TODO: Record first fully-empty region, and use that for large allocations and/or organize
    // available free segments within regions for more efficient searches for "good fit".

    // Record the remainder as allocation waste
    size_t idx = r->index();
    if (req.is_mutator_alloc()) {
      size_t waste = r->free();
      if (waste > 0) {
        increase_used(waste);
        generation->increase_allocated(waste);
        _heap->notify_mutator_alloc_words(waste >> LogHeapWordSize, true);
      }
      assert(probe_set<Mutator>(idx), "Must be mutator free: " SIZE_FORMAT, idx);
      remove_from_set<Mutator>(idx);
      assert(!in_set<Collector>(idx) && !in_set<OldCollector>(idx), "Region cannot be in multiple free sets");
    } else if (r->free() < PLAB::min_size() * HeapWordSize) {
      // Permanently retire this region if there's room for a fill object.  By permanently retiring the region,
      // we simplify future allocation efforts.  Regions with "very limited" available will not be added to 
      // future collector or old_collector sets.  This allows the sets to be more represented more compactly, with
      // a smaller delta between leftmost and rightmost indexes.  It reduces the effort required to find a region
      // with sufficient memory to satisfy future allocation requests.  It reduces the need to rediscover that
      // this region has insufficient memory, eliminates the need to retire the region multiple times, and
      // reduces the need to adjust bounds each time a region is retired.
      size_t waste = r->free();
      HeapWord* fill_addr = r->top();
      size_t fill_size = waste / HeapWordSize;
      if (fill_size >= ShenandoahHeap::min_fill_size()) {
        ShenandoahHeap::fill_with_object(fill_addr, fill_size);
        r->set_top(r->end());
        // Since we have filled the waste with an empty object, account for increased usage
        _heap->increase_used(waste);
      } else {
        // We'll retire the region until the freeset is rebuilt. Since retiement is not permanent, we do not account for waste.
        waste = 0;
      }
      if (probe_set<OldCollector>(idx)) {
        assert(_heap->mode()->is_generational(), "Old collector free regions only present in generational mode");
        if (waste > 0) {
          _heap->old_generation()->increase_used(waste);
          _heap->card_scan()->register_object(fill_addr);
        }
        remove_from_set<OldCollector>(idx);
        assert(!in_set<Collector>(idx) && !in_set<Mutator>(idx), "Region cannot be in multiple free sets");
      } else {
        assert(probe_set<Collector>(idx), "Region that is not mutator free must be collector free or old collector free");
        if ((waste > 0) && _heap->mode()->is_generational()) {
          _heap->young_generation()->increase_used(waste);
        }
        // This applies to both generational and non-generational mode
        remove_from_set<Collector>(idx);
        assert(!in_set<Mutator>(idx) && !in_set<OldCollector>(idx), "Region cannot be in multiple free sets");
      }
    }
    assert_bounds();
  }
  return result;
}

bool ShenandoahFreeSet::touches_bounds(size_t num) const {
  return (num == _collector_leftmost || num == _collector_rightmost ||
          num == _old_collector_leftmost || num == _old_collector_rightmost ||
          num == _mutator_leftmost || num == _mutator_rightmost);
}

void ShenandoahFreeSet::recompute_bounds() {
  // Reset to the most pessimistic case:
  _mutator_rightmost = _max - 1;
  _mutator_leftmost = 0;
  _collector_rightmost = _max - 1;
  _collector_leftmost = 0;
  _old_collector_rightmost = _max - 1;
  _old_collector_leftmost = 0;

  // ...and adjust from there
  adjust_bounds();
  if (_heap->mode()->is_generational()) {
    size_t old_collector_middle = (_old_collector_leftmost + _old_collector_rightmost) / 2;
    size_t old_collector_available_in_first_half = 0;
    size_t old_collector_available_in_second_half = 0;

    for (size_t index = _old_collector_leftmost; index < old_collector_middle; index++) {
      if (in_set<OldCollector>(index)) {
        ShenandoahHeapRegion* r = _heap->get_region(index);
        old_collector_available_in_first_half += r->free();
      }
    }
    for (size_t index = old_collector_middle; index <= _old_collector_rightmost; index++) {
      if (in_set<OldCollector>(index)) {
        ShenandoahHeapRegion* r = _heap->get_region(index);
        old_collector_available_in_second_half += r->free();
      }
    }
    // We desire to first consume the sparsely distributed old-collector regions in order that the remaining old-collector
    // regions are densely packed.  Densely packing old-collector regions reduces the effort to search for a region that
    // has sufficient memory to satisfy a new allocation request.  Old-collector regions become sparsely distributed following
    // a Full GC, which tends to slide old-gen regions to the front of the heap rather than allowing them to remain
    // at the end of the heap where we intend for them to congregate.  In the future, we may modify Full GC so that it
    // slides old objects to the end of the heap and young objects to the start of the heap. If this is done, we can
    // always search right to left.
    _old_collector_search_left_to_right = (old_collector_available_in_second_half > old_collector_available_in_first_half);
  }
}

void ShenandoahFreeSet::adjust_bounds() {
  // Rewind both mutator bounds until the next bit.
  while (_mutator_leftmost < _max && !in_set<Mutator>(_mutator_leftmost)) {
    _mutator_leftmost++;
  }
  while (_mutator_rightmost > 0 && !in_set<Mutator>(_mutator_rightmost)) {
    _mutator_rightmost--;
  }
  // Rewind both collector bounds until the next bit.
  while (_collector_leftmost < _max && !in_set<Collector>(_collector_leftmost)) {
    _collector_leftmost++;
  }
  while (_collector_rightmost > 0 && !in_set<Collector>(_collector_rightmost)) {
    _collector_rightmost--;
  }
  // Rewind both old collector bounds until the next bit.
  while (_old_collector_leftmost < _max && !in_set<OldCollector>(_old_collector_leftmost)) {
    _old_collector_leftmost++;
  }
  while (_old_collector_rightmost > 0 && !in_set<OldCollector>(_old_collector_rightmost)) {
    _old_collector_rightmost--;
  }
}

HeapWord* ShenandoahFreeSet::allocate_contiguous(ShenandoahAllocRequest& req) {
  shenandoah_assert_heaplocked();

  size_t words_size = req.size();
  size_t num = ShenandoahHeapRegion::required_regions(words_size * HeapWordSize);

  assert(req.is_young(), "Humongous regions always allocated in YOUNG");
  ShenandoahGeneration* generation = _heap->generation_for(req.affiliation());

  // Check if there are enough regions left to satisfy allocation.
  if (_heap->mode()->is_generational()) {
    size_t avail_young_regions = generation->adjusted_unaffiliated_regions();
    if (num > mutator_count() || (num > avail_young_regions)) {
      return nullptr;
    }
  } else {
    if (num > mutator_count()) {
      return nullptr;
    }
  }

  // Find the continuous interval of $num regions, starting from $beg and ending in $end,
  // inclusive. Contiguous allocations are biased to the beginning.

  size_t beg = _mutator_leftmost;
  size_t end = beg;

  while (true) {
    if (end >= _max) {
      // Hit the end, goodbye
      return nullptr;
    }

    // If regions are not adjacent, then current [beg; end] is useless, and we may fast-forward.
    // If region is not completely free, the current [beg; end] is useless, and we may fast-forward.
    if (!in_set<Mutator>(end) || !can_allocate_from(_heap->get_region(end))) {
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

    r->set_affiliation(req.affiliation());
    r->set_update_watermark(r->bottom());
    r->set_top(r->bottom());    // Set top to bottom so we can capture TAMS
    ctx->capture_top_at_mark_start(r);
    r->set_top(r->bottom() + used_words); // Then change top to reflect allocation of humongous object.
    assert(ctx->top_at_mark_start(r) == r->bottom(), "Newly established allocation region starts with TAMS equal to bottom");
    assert(ctx->is_bitmap_clear_range(ctx->top_bitmap(r), r->end()), "Bitmap above top_bitmap() must be clear");

    // Leave top_bitmap alone.  The first time a heap region is put into service, top_bitmap should equal end.
    // Thereafter, it should represent the upper bound on parts of the bitmap that need to be cleared.
    // ctx->clear_bitmap(r);
    log_debug(gc, free)("NOT clearing bitmap for Humongous region [" PTR_FORMAT ", " PTR_FORMAT "], top_bitmap: "
                        PTR_FORMAT " at transition from FREE to %s",
                        p2i(r->bottom()), p2i(r->end()), p2i(ctx->top_bitmap(r)), req.affiliation_name());
    // While individual regions report their true use, all humongous regions are marked used in the free set.
    remove_from_set<Mutator>(r->index());
  }
  size_t total_humongous_size = ShenandoahHeapRegion::region_size_bytes() * num;
  increase_used(total_humongous_size);
  if (_heap->mode()->is_generational()) {
    size_t humongous_waste = total_humongous_size - words_size * HeapWordSize;
    _heap->global_generation()->increase_used(words_size * HeapWordSize);
    _heap->global_generation()->increase_humongous_waste(humongous_waste);
    if (req.is_young()) {
      _heap->young_generation()->increase_used(words_size * HeapWordSize);
      _heap->young_generation()->increase_humongous_waste(humongous_waste);
    } else if (req.is_old()) {
      _heap->old_generation()->increase_used(words_size * HeapWordSize);
      _heap->old_generation()->increase_humongous_waste(humongous_waste);
    }
  }

  if (remainder != 0) {
    // Record this remainder as allocation waste
    size_t waste = ShenandoahHeapRegion::region_size_words() - remainder;
    _heap->notify_mutator_alloc_words(waste, true);
    generation->increase_allocated(waste * HeapWordSize);
  }

  // Allocated at left/rightmost? Move the bounds appropriately.
  if (beg == _mutator_leftmost || end == _mutator_rightmost) {
    adjust_bounds();
  }
  assert_bounds();

  req.set_actual_size(words_size);
  return _heap->get_region(beg)->bottom();
}

// Returns true iff this region is entirely available, either because it is empty() or because it has been found to represent
// immediate trash and we'll be able to immediately recycle it.  Note that we cannot recycle immediate trash if
// concurrent weak root processing is in progress.
bool ShenandoahFreeSet::can_allocate_from(ShenandoahHeapRegion *r) const {
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

bool ShenandoahFreeSet::has_alloc_capacity(ShenandoahHeapRegion *r) const {
  return alloc_capacity(r) > 0;
}

bool ShenandoahFreeSet::has_alloc_capacity(size_t idx) const {
  ShenandoahHeapRegion* r = _heap->get_region(idx);
  return alloc_capacity(r) > 0;
}

bool ShenandoahFreeSet::has_no_alloc_capacity(ShenandoahHeapRegion *r) const {
  return alloc_capacity(r) == 0;
}

void ShenandoahFreeSet::try_recycle_trashed(ShenandoahHeapRegion *r) {
  if (r->is_trash()) {
    _heap->decrease_used(r->used());
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

void ShenandoahFreeSet::flip_to_old_gc(ShenandoahHeapRegion* r) {
  size_t idx = r->index();

  assert(_mutator_free_bitmap.at(idx), "Should be in mutator view");
  assert(can_allocate_from(r), "Should not be allocated");

  remove_from_set<Mutator>(idx);
  add_to_set<OldCollector>(idx);

  size_t region_capacity = alloc_capacity(r);
  _capacity -= region_capacity;
  _old_capacity += region_capacity;
  assert_bounds();

  // We do not ensure that the region is no longer trash,
  // relying on try_allocate_in(), which always comes next,
  // to recycle trash before attempting to allocate anything in the region.
}

void ShenandoahFreeSet::flip_to_gc(ShenandoahHeapRegion* r) {
  size_t idx = r->index();

  assert(_mutator_free_bitmap.at(idx), "Should be in mutator view");
  assert(can_allocate_from(r), "Should not be allocated");

  remove_from_set<Mutator>(idx);
  add_to_set<Collector>(idx);

  _capacity -= alloc_capacity(r);
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
  _old_collector_free_bitmap.clear();
  _mutator_leftmost = _max;
  _mutator_rightmost = 0;
  _collector_leftmost = _max;
  _collector_rightmost = 0;
  _old_collector_leftmost = _max;
  _old_collector_rightmost = 0;
  _capacity = 0;
  _old_capacity = 0;
  _used = 0;
}

// This function places all is_old() regions that have allocation capacity into the old_collector set.  It places
// all other regions (not is_old()) that have allocation capacity into the mutator_set.  Subsequently, we will
// move some of the mutator regions into the collector set or old_collector set with the intent of packing
// old_collector memory into the highest (rightmost) addresses of the heap and the collector memory into the
// next highest addresses of the heap, with mutator memory consuming the lowest addresses of the heap.
void ShenandoahFreeSet::find_regions_with_alloc_capacity() {

  for (size_t idx = 0; idx < _heap->num_regions(); idx++) {
    ShenandoahHeapRegion* region = _heap->get_region(idx);
    if (region->is_alloc_allowed() || region->is_trash()) {
      assert(!region->is_cset(), "Shouldn't be adding cset regions to the free set");

      // Do not add regions that would surely fail allocation
      if (has_no_alloc_capacity(region)) continue;

      if (region->is_old()) {
        _old_capacity += alloc_capacity(region);
        assert(!in_set<OldCollector>(idx), "We are about to add it, it shouldn't be there already");
        add_to_set<OldCollector>(idx);
        log_debug(gc, free)(
          "  Adding Region " SIZE_FORMAT  " (Free: " SIZE_FORMAT "%s, Used: " SIZE_FORMAT "%s) to old collector set",
          idx, byte_size_in_proper_unit(region->free()), proper_unit_for_byte_size(region->free()),
          byte_size_in_proper_unit(region->used()), proper_unit_for_byte_size(region->used()));

      } else {
        _capacity += alloc_capacity(region);
        assert(!in_set<Mutator>(idx), "We are about to add it, it shouldn't be there already");
        add_to_set<Mutator>(idx);
        log_debug(gc, free)(
          "  Adding Region " SIZE_FORMAT " (Free: " SIZE_FORMAT "%s, Used: " SIZE_FORMAT "%s) to mutator set",
          idx, byte_size_in_proper_unit(region->free()), proper_unit_for_byte_size(region->free()),
          byte_size_in_proper_unit(region->used()), proper_unit_for_byte_size(region->used()));
      }
    }
  }
}

void ShenandoahFreeSet::rebuild() {
  shenandoah_assert_heaplocked();
  // This resets all state information, removing all regions from all sets.
  clear();

  log_debug(gc, free)("Rebuilding FreeSet");

  // This places regions that have alloc_capacity into the old_collector set if they identify as is_old() or the
  // mutator set otherwise.
  find_regions_with_alloc_capacity();

  // Evac reserve: reserve trailing space for evacuations, with regions reserved for old evacuations placed to the right
  // of regions reserved of young evacuations.
  size_t young_reserve, old_reserve;
  if (!_heap->mode()->is_generational()) {
    young_reserve = (_heap->max_capacity() / 100) * ShenandoahEvacReserve;
    old_reserve = 0;
  } else {
    // All allocations taken from the old collector set are performed by GC, generally using PLABs for both
    // promotions and evacuations.  The partition between which old memory is reserved for evacuation and
    // which is reserved for promotion is enforced using thread-local variables that prescribe intentons for
    // each PLAB's available memory.
    if (_heap->has_evacuation_reserve_quantities()) {
      // We are rebuilding at the end of final mark, having already established evacuation budgets for this GC pass.
      young_reserve = _heap->get_young_evac_reserve();
      old_reserve = _heap->get_promoted_reserve() + _heap->get_old_evac_reserve();
    } else {
      // We are rebuilding at end of GC, so we set aside budgets specified on command line (or defaults)
      young_reserve = (_heap->young_generation()->max_capacity() * ShenandoahEvacReserve) / 100;
      old_reserve = MAX2((_heap->old_generation()->max_capacity() * ShenandoahOldEvacReserve) / 100,
                         ShenandoahOldCompactionReserve * ShenandoahHeapRegion::region_size_bytes());
    }
  }
  reserve_regions(young_reserve, old_reserve);
  assert_bounds();
  log_status();
}

// Having placed all regions that have allocation capacity into the mutator set if they identify as is_young()
// or into the old collector set if they identify as is_old(), move some of these regions from the mutator set
// into the collector set or old collector set in order to assure that the memory available for allocations within
// the collector set is at least to_reserve, and the memory available for allocations within the old collector set
// is at least to_reserve_old.
void ShenandoahFreeSet::reserve_regions(size_t to_reserve, size_t to_reserve_old) {
  size_t reserved = 0;
  for (size_t idx = _heap->num_regions() - 1; idx > 0; idx--) {
    ShenandoahHeapRegion* r = _heap->get_region(idx);
    if (_mutator_free_bitmap.at(idx) && (alloc_capacity(r) > 0)) {
      assert(!r->is_old(), "mutator_is_free regions should not be affiliated OLD");
      // OLD regions that have available memory are already in the old_collector free set
      if ((_old_capacity < to_reserve_old) && (r->is_trash() || !r->is_affiliated())) {
        remove_from_set<Mutator>(idx);
        add_to_set<OldCollector>(idx);
        size_t ac = alloc_capacity(r);
        _capacity -= ac;
        _old_capacity += ac;
        log_debug(gc, free)("  Shifting region " SIZE_FORMAT " from mutator_free to old_collector_free", idx);
      } else if (reserved < to_reserve) {
        // Note: In a previous implementation, regions were only placed into the survivor space (collector_is_free) if
        // they were entirely empty.  I'm not sure I understand the rational for that.  That alternative behavior would
        // tend to mix survivor objects with ephemeral objects, making it more difficult to reclaim the memory for the
        // ephemeral objects.  It also delays aging of regions, causing promotion in place to be delayed.
        remove_from_set<Mutator>(idx);
        add_to_set<Collector>(idx);
        size_t ac = alloc_capacity(r);
        _capacity -= ac;
        reserved += ac;
        log_debug(gc)("  Shifting region " SIZE_FORMAT " from mutator_free to collector_free", idx);
      } else {
        // We've satisfied both to_reserve and to_reserved_old
        break;
      }
    }
  }
}

void ShenandoahFreeSet::log_status() {
  shenandoah_assert_heaplocked();

#ifdef ASSERT
  // Dump of the FreeSet details is only enabled if assertions are enabled
  {
#define BUFFER_SIZE 80
    size_t retired_old = 0;
    size_t retired_old_humongous = 0;
    size_t retired_young = 0;
    size_t retired_young_humongous = 0;
    size_t region_size_bytes = ShenandoahHeapRegion::region_size_bytes();
    char buffer[BUFFER_SIZE];
    for (uint i = 0; i < BUFFER_SIZE; i++) {
      buffer[i] = '\0';
    }
    log_info(gc, free)("FreeSet map legend (see source for unexpected codes: *, $, !, #):\n"
                       " m:mutator_free c:collector_free C:old_collector_free"
                       " h:humongous young H:humongous old ~:retired old _:retired young");
    log_info(gc, free)(" mutator free range [" SIZE_FORMAT ".." SIZE_FORMAT "], "
                       " collector free range [" SIZE_FORMAT ".." SIZE_FORMAT "], "
                       "old collector free range [" SIZE_FORMAT ".." SIZE_FORMAT "] allocates from %s",
                       _mutator_leftmost, _mutator_rightmost, _collector_leftmost, _collector_rightmost,
                       _old_collector_leftmost, _old_collector_rightmost,
                       _old_collector_search_left_to_right? "left to right": "right to left");
    for (uint i = 0; i < _heap->num_regions(); i++) {
      ShenandoahHeapRegion *r = _heap->get_region(i);
      uint idx = i % 64;
      if ((i != 0) && (idx == 0)) {
        log_info(gc, free)(" %6u: %s", i-64, buffer);
      }
      if (in_set<Mutator>(i) && in_set<Collector>(i) && in_set<OldCollector>(i)) {
        buffer[idx] = '*';
      } else if (in_set<Mutator>(i) && in_set<Collector>(i)) {
        assert(!r->is_old(), "Old regions should not be in collector_free set");
        buffer[idx] = '$';
      } else if (in_set<Mutator>(i) && in_set<OldCollector>(i)) {
        // Note that young regions may be in the old_collector_free set.
        buffer[idx] = '!';
      } else if (in_set<Collector>(i) && in_set<OldCollector>(i)) {
        buffer[idx] = '#';
      } else if (in_set<Mutator>(i)) {
        assert(!r->is_old(), "Old regions should not be in mutator_free set");
        buffer[idx] = 'm';
      } else if (in_set<Collector>(i)) {
        assert(!r->is_old(), "Old regions should not be in collector_free set");
        buffer[idx] = 'c';
      } else if (in_set<OldCollector>(i)) {
        buffer[idx] = 'C';
      } else if (r->is_humongous()) {
        if (r->is_old()) {
          buffer[idx] = 'H';
          retired_old_humongous += region_size_bytes;
        } else {
          buffer[idx] = 'h';
          retired_young_humongous += region_size_bytes;
        }
      } else {
        if (r->is_old()) {
          buffer[idx] = '~';
          retired_old += region_size_bytes;
        } else {
          buffer[idx] = '_';
          retired_young += region_size_bytes;
        }

      }
    }
    uint remnant = _heap->num_regions() % 64;
    if (remnant > 0) {
      buffer[remnant] = '\0';
    } else {
      remnant = 64;
    }
    log_info(gc, free)(" %6u: %s", (uint) (_heap->num_regions() - remnant), buffer);
    size_t total_young = retired_young + retired_young_humongous;
    size_t total_old = retired_old + retired_old_humongous;
    log_info(gc, free)("Retired young: " SIZE_FORMAT "%s (including humongous: " SIZE_FORMAT "%s), old: " SIZE_FORMAT
                       "%s (including humongous: " SIZE_FORMAT "%s)",
                       byte_size_in_proper_unit(total_young),             proper_unit_for_byte_size(total_young),
                       byte_size_in_proper_unit(retired_young_humongous), proper_unit_for_byte_size(retired_young_humongous),
                       byte_size_in_proper_unit(total_old),               proper_unit_for_byte_size(total_old),
                       byte_size_in_proper_unit(retired_old_humongous),   proper_unit_for_byte_size(retired_old_humongous));
  }
#endif

  LogTarget(Info, gc, free) lt;
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
        if (in_set<Mutator>(idx)) {
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

      assert(free == total_free, "Sum of free within mutator regions (" SIZE_FORMAT
             ") should match mutator capacity (" SIZE_FORMAT ") minus mutator used (" SIZE_FORMAT ")",
             total_free, capacity(), used());

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
      ls.print("Used: " SIZE_FORMAT "%s, Mutator Free: " SIZE_FORMAT,
               byte_size_in_proper_unit(total_used), proper_unit_for_byte_size(total_used), mutator_count());
    }

    {
      size_t max = 0;
      size_t total_free = 0;
      size_t total_used = 0;

      for (size_t idx = _collector_leftmost; idx <= _collector_rightmost; idx++) {
        if (in_set<Collector>(idx)) {
          ShenandoahHeapRegion *r = _heap->get_region(idx);
          size_t free = alloc_capacity(r);
          max = MAX2(max, free);
          total_free += free;
          total_used += r->used();
        }
      }
      ls.print(" Collector Reserve: " SIZE_FORMAT "%s, Max: " SIZE_FORMAT "%s; Used: " SIZE_FORMAT "%s",
               byte_size_in_proper_unit(total_free), proper_unit_for_byte_size(total_free),
               byte_size_in_proper_unit(max),        proper_unit_for_byte_size(max),
               byte_size_in_proper_unit(total_used), proper_unit_for_byte_size(total_used));
    }

    if (_heap->mode()->is_generational()) {
      size_t max = 0;
      size_t total_free = 0;
      size_t total_used = 0;

      for (size_t idx = _old_collector_leftmost; idx <= _old_collector_rightmost; idx++) {
        if (in_set<OldCollector>(idx)) {
          ShenandoahHeapRegion *r = _heap->get_region(idx);
          size_t free = alloc_capacity(r);
          max = MAX2(max, free);
          total_free += free;
          total_used += r->used();
        }
      }
      ls.print_cr(" Old Collector Reserve: " SIZE_FORMAT "%s, Max: " SIZE_FORMAT "%s; Used: " SIZE_FORMAT "%s",
                  byte_size_in_proper_unit(total_free), proper_unit_for_byte_size(total_free),
                  byte_size_in_proper_unit(max),        proper_unit_for_byte_size(max),
                  byte_size_in_proper_unit(total_used), proper_unit_for_byte_size(total_used));
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
        return nullptr;
      default:
        ShouldNotReachHere();
        return nullptr;
    }
  } else {
    return allocate_single(req, in_new_region);
  }
}

size_t ShenandoahFreeSet::unsafe_peek_free() const {
  // Deliberately not locked, this method is unsafe when free set is modified.

  for (size_t index = _mutator_leftmost; index <= _mutator_rightmost; index++) {
    if (index < _max && in_set<Mutator>(index)) {
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
    if (in_set<Mutator>(index)) {
      _heap->get_region(index)->print_on(out);
    }
  }
  out->print_cr("Collector Free Set: " SIZE_FORMAT "", collector_count());
  for (size_t index = _collector_leftmost; index <= _collector_rightmost; index++) {
    if (in_set<Collector>(index)) {
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
    if (in_set<Mutator>(index)) {
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
    if (in_set<Mutator>(index)) {
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

  assert (_mutator_leftmost == _max || in_set<Mutator>(_mutator_leftmost),  "leftmost region should be free: " SIZE_FORMAT,  _mutator_leftmost);
  assert (_mutator_rightmost == 0   || in_set<Mutator>(_mutator_rightmost), "rightmost region should be free: " SIZE_FORMAT, _mutator_rightmost);

  size_t beg_off = _mutator_free_bitmap.find_first_set_bit(0);
  size_t end_off = _mutator_free_bitmap.find_first_set_bit(_mutator_rightmost + 1);
  assert (beg_off >= _mutator_leftmost, "free regions before the leftmost: " SIZE_FORMAT ", bound " SIZE_FORMAT, beg_off, _mutator_leftmost);
  assert (end_off == _max,      "free regions past the rightmost: " SIZE_FORMAT ", bound " SIZE_FORMAT,  end_off, _mutator_rightmost);

  assert (_collector_leftmost <= _max, "leftmost in bounds: "  SIZE_FORMAT " < " SIZE_FORMAT, _collector_leftmost,  _max);
  assert (_collector_rightmost < _max, "rightmost in bounds: " SIZE_FORMAT " < " SIZE_FORMAT, _collector_rightmost, _max);

  assert (_collector_leftmost == _max || in_set<Collector>(_collector_leftmost),  "leftmost region should be free: " SIZE_FORMAT,  _collector_leftmost);
  assert (_collector_rightmost == 0   || in_set<Collector>(_collector_rightmost), "rightmost region should be free: " SIZE_FORMAT, _collector_rightmost);

  beg_off = _collector_free_bitmap.find_first_set_bit(0);
  end_off = _collector_free_bitmap.find_first_set_bit(_collector_rightmost + 1);
  assert (beg_off >= _collector_leftmost, "free regions before the leftmost: " SIZE_FORMAT ", bound " SIZE_FORMAT, beg_off, _collector_leftmost);
  assert (end_off == _max,      "free regions past the rightmost: " SIZE_FORMAT ", bound " SIZE_FORMAT,  end_off, _collector_rightmost);

  assert (_old_collector_leftmost <= _max, "leftmost in bounds: "  SIZE_FORMAT " < " SIZE_FORMAT, _old_collector_leftmost,  _max);
  assert (_old_collector_rightmost < _max, "rightmost in bounds: " SIZE_FORMAT " < " SIZE_FORMAT, _old_collector_rightmost, _max);

  assert (_old_collector_leftmost == _max || in_set<OldCollector>(_old_collector_leftmost),  "leftmost region should be free: " SIZE_FORMAT,  _old_collector_leftmost);
  assert (_old_collector_rightmost == 0   || in_set<OldCollector>(_old_collector_rightmost), "rightmost region should be free: " SIZE_FORMAT, _old_collector_rightmost);

  beg_off = _old_collector_free_bitmap.find_first_set_bit(0);
  end_off = _old_collector_free_bitmap.find_first_set_bit(_old_collector_rightmost + 1);
  assert (beg_off >= _old_collector_leftmost, "free regions before the leftmost: " SIZE_FORMAT ", bound " SIZE_FORMAT, beg_off, _old_collector_leftmost);
  assert (end_off == _max,      "free regions past the rightmost: " SIZE_FORMAT ", bound " SIZE_FORMAT,  end_off, _old_collector_rightmost);
}
#endif

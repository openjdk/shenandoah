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

#include "gc/shared/preservedMarks.inline.hpp"
#include "gc/shenandoah/shenandoahGenerationalFullGC.hpp"
#include "gc/shenandoah/shenandoahGeneration.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahHeapRegion.hpp"
#include "gc/shenandoah/shenandoahYoungGeneration.hpp"
#include "gc/shenandoah/shenandoahOldGeneration.hpp"
#include "gc/shenandoah/shenandoahUtils.hpp"

#ifdef ASSERT
void assert_regions_used_not_more_than_capacity(ShenandoahGeneration* generation) {
  assert(generation->used_regions_size() <= generation->max_capacity(),
         "%s generation affiliated regions must be less than capacity", generation->name());
}

void assert_usage_not_more_than_regions_used(ShenandoahGeneration* generation) {
  assert(generation->used_including_humongous_waste() <= generation->used_regions_size(),
         "%s consumed can be no larger than span of affiliated regions", generation->name());
}
#else
void assert_regions_used_not_more_than_capacity(ShenandoahGeneration* generation) {}
void assert_usage_not_more_than_regions_used(ShenandoahGeneration* generation) {}
#endif


void ShenandoahGenerationalFullGC::prepare(ShenandoahHeap* heap) {
  // Since we may arrive here from degenerated GC failure of either young or old, establish generation as GLOBAL.
  heap->set_gc_generation(heap->global_generation());

  // No need for old_gen->increase_used() as this was done when plabs were allocated.
  heap->reset_generation_reserves();

  // Full GC supersedes any marking or coalescing in old generation.
  heap->cancel_old_gc();
}

void ShenandoahGenerationalFullGC::handle_completion(ShenandoahHeap* heap) {
  // Full GC should reset time since last gc for young and old heuristics
  ShenandoahYoungGeneration* young = heap->young_generation();
  ShenandoahOldGeneration* old = heap->old_generation();
  young->heuristics()->record_cycle_end();
  old->heuristics()->record_cycle_end();

  heap->mmu_tracker()->record_full(GCId::current());
  heap->log_heap_status("At end of Full GC");

  assert(old->state() == ShenandoahOldGeneration::WAITING_FOR_BOOTSTRAP,
         "After full GC, old generation should be waiting for bootstrap.");

  // Since we allow temporary violation of these constraints during Full GC, we want to enforce that the assertions are
  // made valid by the time Full GC completes.
  assert_regions_used_not_more_than_capacity(old);
  assert_regions_used_not_more_than_capacity(young);
  assert_usage_not_more_than_regions_used(old);
  assert_usage_not_more_than_regions_used(young);

  // Establish baseline for next old-has-grown trigger.
  old->set_live_bytes_after_last_mark(old->used_including_humongous_waste());
}

void ShenandoahGenerationalFullGC::rebuild_remembered_set(ShenandoahHeap* heap) {
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::full_gc_reconstruct_remembered_set);
  ShenandoahRegionIterator regions;
  ShenandoahReconstructRememberedSetTask task(&regions);
  heap->workers()->run_task(&task);
}

void ShenandoahGenerationalFullGC::balance_generations_after_gc(ShenandoahHeap* heap) {
  size_t old_usage = heap->old_generation()->used_regions_size();
  size_t old_capacity = heap->old_generation()->max_capacity();

  assert(old_usage % ShenandoahHeapRegion::region_size_bytes() == 0, "Old usage must align with region size");
  assert(old_capacity % ShenandoahHeapRegion::region_size_bytes() == 0, "Old capacity must align with region size");

  if (old_capacity > old_usage) {
    size_t excess_old_regions = (old_capacity - old_usage) / ShenandoahHeapRegion::region_size_bytes();
    heap->generation_sizer()->transfer_to_young(excess_old_regions);
  } else if (old_capacity < old_usage) {
    size_t old_regions_deficit = (old_usage - old_capacity) / ShenandoahHeapRegion::region_size_bytes();
    heap->generation_sizer()->force_transfer_to_old(old_regions_deficit);
  }

  log_info(gc)("FullGC done: young usage: " PROPERFMT ", old usage: " PROPERFMT,
               PROPERFMTARGS(heap->young_generation()->used()),
               PROPERFMTARGS(heap->old_generation()->used()));
}

void ShenandoahGenerationalFullGC::balance_generations_after_rebuilding_free_set(ShenandoahHeap* heap) {
  bool success;
  size_t region_xfer;
  const char* region_destination;
  ShenandoahYoungGeneration* young_gen = heap->young_generation();
  ShenandoahGeneration* old_gen = heap->old_generation();

  size_t old_region_surplus = heap->get_old_region_surplus();
  size_t old_region_deficit = heap->get_old_region_deficit();
  if (old_region_surplus) {
    success = heap->generation_sizer()->transfer_to_young(old_region_surplus);
    region_destination = "young";
    region_xfer = old_region_surplus;
  } else if (old_region_deficit) {
    success = heap->generation_sizer()->transfer_to_old(old_region_deficit);
    region_destination = "old";
    region_xfer = old_region_deficit;
    if (!success) {
      ((ShenandoahOldHeuristics *) old_gen->heuristics())->trigger_cannot_expand();
    }
  } else {
    region_destination = "none";
    region_xfer = 0;
    success = true;
  }
  heap->set_old_region_surplus(0);
  heap->set_old_region_deficit(0);
  size_t young_available = young_gen->available();
  size_t old_available = old_gen->available();
  log_info(gc, ergo)("After cleanup, %s " SIZE_FORMAT " regions to %s to prepare for next gc, old available: "
                     PROPERFMT ", young_available: " PROPERFMT,
                     success? "successfully transferred": "failed to transfer", region_xfer, region_destination,
                     PROPERFMTARGS(old_available), PROPERFMTARGS(young_available));
}

void ShenandoahGenerationalFullGC::log_live_in_old(ShenandoahHeap* heap) {
  LogTarget(Info, gc) lt;
  if (lt.is_enabled()) {
    size_t live_bytes_in_old = 0;
    for (size_t i = 0; i < heap->num_regions(); i++) {
      ShenandoahHeapRegion* r = heap->get_region(i);
      if (r->is_old()) {
        live_bytes_in_old += r->get_live_data_bytes();
      }
    }
    log_info(gc)("Live bytes in old after STW mark: " PROPERFMT, PROPERFMTARGS(live_bytes_in_old));
  }
}

void ShenandoahGenerationalFullGC::restore_top_before_promote(ShenandoahHeap* heap) {
  for (size_t i = 0; i < heap->num_regions(); i++) {
    ShenandoahHeapRegion* r = heap->get_region(i);
    if (r->get_top_before_promote() != nullptr) {
      r->restore_top_before_promote();
    }
  }
}

void ShenandoahGenerationalFullGC::account_for_region(ShenandoahHeapRegion* r, size_t &region_count, size_t &region_usage, size_t &humongous_waste) {
  region_count++;
  region_usage += r->used();
  if (r->is_humongous_start()) {
    // For each humongous object, we take this path once regardless of how many regions it spans.
    HeapWord* obj_addr = r->bottom();
    oop obj = cast_to_oop(obj_addr);
    size_t word_size = obj->size();
    size_t region_size_words = ShenandoahHeapRegion::region_size_words();
    size_t overreach = word_size % region_size_words;
    if (overreach != 0) {
      humongous_waste += (region_size_words - overreach) * HeapWordSize;
    }
    // else, this humongous object aligns exactly on region size, so no waste.
  }
}

void ShenandoahGenerationalFullGC::maybe_coalesce_and_fill_region(ShenandoahHeapRegion* r) {
  if (r->is_pinned() && r->is_old() && r->is_active() && !r->is_humongous()) {
    r->begin_preemptible_coalesce_and_fill();
    r->oop_fill_and_coalesce_without_cancel();
  }
}

ShenandoahPrepareForGenerationalCompactionObjectClosure::ShenandoahPrepareForGenerationalCompactionObjectClosure(PreservedMarks* preserved_marks,
                                                          GrowableArray<ShenandoahHeapRegion*>& empty_regions,
                                                          ShenandoahHeapRegion* from_region, uint worker_id) :
        _preserved_marks(preserved_marks),
        _heap(ShenandoahHeap::heap()),
        _tenuring_threshold(0),
        _empty_regions(empty_regions),
        _empty_regions_pos(0),
        _old_to_region(nullptr),
        _young_to_region(nullptr),
        _from_region(nullptr),
        _from_affiliation(ShenandoahAffiliation::FREE),
        _old_compact_point(nullptr),
        _young_compact_point(nullptr),
        _worker_id(worker_id) {
  assert(from_region != nullptr, "Worker needs from_region");
  // assert from_region has live?
  if (from_region->is_old()) {
    _old_to_region = from_region;
    _old_compact_point = from_region->bottom();
  } else if (from_region->is_young()) {
    _young_to_region = from_region;
    _young_compact_point = from_region->bottom();
  }

  _tenuring_threshold = _heap->age_census()->tenuring_threshold();
}

void ShenandoahPrepareForGenerationalCompactionObjectClosure::set_from_region(ShenandoahHeapRegion* from_region) {
  log_debug(gc)("Worker %u compacting %s Region " SIZE_FORMAT " which had used " SIZE_FORMAT " and %s live",
                _worker_id, from_region->affiliation_name(),
                from_region->index(), from_region->used(), from_region->has_live()? "has": "does not have");

  _from_region = from_region;
  _from_affiliation = from_region->affiliation();
  if (_from_region->has_live()) {
    if (_from_affiliation == ShenandoahAffiliation::OLD_GENERATION) {
      if (_old_to_region == nullptr) {
        _old_to_region = from_region;
        _old_compact_point = from_region->bottom();
      }
    } else {
      assert(_from_affiliation == ShenandoahAffiliation::YOUNG_GENERATION, "from_region must be OLD or YOUNG");
      if (_young_to_region == nullptr) {
        _young_to_region = from_region;
        _young_compact_point = from_region->bottom();
      }
    }
  } // else, we won't iterate over this _from_region so we don't need to set up to region to hold copies
}

void ShenandoahPrepareForGenerationalCompactionObjectClosure::finish() {
  finish_old_region();
  finish_young_region();
}

void ShenandoahPrepareForGenerationalCompactionObjectClosure::finish_old_region() {
  if (_old_to_region != nullptr) {
    log_debug(gc)("Planned compaction into Old Region " SIZE_FORMAT ", used: " SIZE_FORMAT " tabulated by worker %u",
            _old_to_region->index(), _old_compact_point - _old_to_region->bottom(), _worker_id);
    _old_to_region->set_new_top(_old_compact_point);
    _old_to_region = nullptr;
  }
}

void ShenandoahPrepareForGenerationalCompactionObjectClosure::finish_young_region() {
  if (_young_to_region != nullptr) {
    log_debug(gc)("Worker %u planned compaction into Young Region " SIZE_FORMAT ", used: " SIZE_FORMAT,
            _worker_id, _young_to_region->index(), _young_compact_point - _young_to_region->bottom());
    _young_to_region->set_new_top(_young_compact_point);
    _young_to_region = nullptr;
  }
}

bool ShenandoahPrepareForGenerationalCompactionObjectClosure::is_compact_same_region() {
  return (_from_region == _old_to_region) || (_from_region == _young_to_region);
}

void ShenandoahPrepareForGenerationalCompactionObjectClosure::do_object(oop p) {
  assert(_from_region != nullptr, "must set before work");
  assert((_from_region->bottom() <= cast_from_oop<HeapWord*>(p)) && (cast_from_oop<HeapWord*>(p) < _from_region->top()),
         "Object must reside in _from_region");
  assert(_heap->complete_marking_context()->is_marked(p), "must be marked");
  assert(!_heap->complete_marking_context()->allocated_after_mark_start(p), "must be truly marked");

  size_t obj_size = p->size();
  uint from_region_age = _from_region->age();
  uint object_age = p->age();

  bool promote_object = false;
  if ((_from_affiliation == ShenandoahAffiliation::YOUNG_GENERATION) &&
      (from_region_age + object_age >= _tenuring_threshold)) {
    if ((_old_to_region != nullptr) && (_old_compact_point + obj_size > _old_to_region->end())) {
      finish_old_region();
      _old_to_region = nullptr;
    }
    if (_old_to_region == nullptr) {
      if (_empty_regions_pos < _empty_regions.length()) {
        ShenandoahHeapRegion* new_to_region = _empty_regions.at(_empty_regions_pos);
        _empty_regions_pos++;
        new_to_region->set_affiliation(OLD_GENERATION);
        _old_to_region = new_to_region;
        _old_compact_point = _old_to_region->bottom();
        promote_object = true;
      }
      // Else this worker thread does not yet have any empty regions into which this aged object can be promoted so
      // we leave promote_object as false, deferring the promotion.
    } else {
      promote_object = true;
    }
  }

  if (promote_object || (_from_affiliation == ShenandoahAffiliation::OLD_GENERATION)) {
    assert(_old_to_region != nullptr, "_old_to_region should not be nullptr when evacuating to OLD region");
    if (_old_compact_point + obj_size > _old_to_region->end()) {
      ShenandoahHeapRegion* new_to_region;

      log_debug(gc)("Worker %u finishing old region " SIZE_FORMAT ", compact_point: " PTR_FORMAT ", obj_size: " SIZE_FORMAT
      ", &compact_point[obj_size]: " PTR_FORMAT ", region end: " PTR_FORMAT,  _worker_id, _old_to_region->index(),
              p2i(_old_compact_point), obj_size, p2i(_old_compact_point + obj_size), p2i(_old_to_region->end()));

      // Object does not fit.  Get a new _old_to_region.
      finish_old_region();
      if (_empty_regions_pos < _empty_regions.length()) {
        new_to_region = _empty_regions.at(_empty_regions_pos);
        _empty_regions_pos++;
        new_to_region->set_affiliation(OLD_GENERATION);
      } else {
        // If we've exhausted the previously selected _old_to_region, we know that the _old_to_region is distinct
        // from _from_region.  That's because there is always room for _from_region to be compacted into itself.
        // Since we're out of empty regions, let's use _from_region to hold the results of its own compaction.
        new_to_region = _from_region;
      }

      assert(new_to_region != _old_to_region, "must not reuse same OLD to-region");
      assert(new_to_region != nullptr, "must not be nullptr");
      _old_to_region = new_to_region;
      _old_compact_point = _old_to_region->bottom();
    }

    // Object fits into current region, record new location:
    assert(_old_compact_point + obj_size <= _old_to_region->end(), "must fit");
    shenandoah_assert_not_forwarded(nullptr, p);
    _preserved_marks->push_if_necessary(p, p->mark());
    p->forward_to(cast_to_oop(_old_compact_point));
    _old_compact_point += obj_size;
  } else {
    assert(_from_affiliation == ShenandoahAffiliation::YOUNG_GENERATION,
           "_from_region must be OLD_GENERATION or YOUNG_GENERATION");
    assert(_young_to_region != nullptr, "_young_to_region should not be nullptr when compacting YOUNG _from_region");

    // After full gc compaction, all regions have age 0.  Embed the region's age into the object's age in order to preserve
    // tenuring progress.
    if (_heap->is_aging_cycle()) {
      ShenandoahHeap::increase_object_age(p, from_region_age + 1);
    } else {
      ShenandoahHeap::increase_object_age(p, from_region_age);
    }

    if (_young_compact_point + obj_size > _young_to_region->end()) {
      ShenandoahHeapRegion* new_to_region;

      log_debug(gc)("Worker %u finishing young region " SIZE_FORMAT ", compact_point: " PTR_FORMAT ", obj_size: " SIZE_FORMAT
      ", &compact_point[obj_size]: " PTR_FORMAT ", region end: " PTR_FORMAT,  _worker_id, _young_to_region->index(),
              p2i(_young_compact_point), obj_size, p2i(_young_compact_point + obj_size), p2i(_young_to_region->end()));

      // Object does not fit.  Get a new _young_to_region.
      finish_young_region();
      if (_empty_regions_pos < _empty_regions.length()) {
        new_to_region = _empty_regions.at(_empty_regions_pos);
        _empty_regions_pos++;
        new_to_region->set_affiliation(YOUNG_GENERATION);
      } else {
        // If we've exhausted the previously selected _young_to_region, we know that the _young_to_region is distinct
        // from _from_region.  That's because there is always room for _from_region to be compacted into itself.
        // Since we're out of empty regions, let's use _from_region to hold the results of its own compaction.
        new_to_region = _from_region;
      }

      assert(new_to_region != _young_to_region, "must not reuse same OLD to-region");
      assert(new_to_region != nullptr, "must not be nullptr");
      _young_to_region = new_to_region;
      _young_compact_point = _young_to_region->bottom();
    }

    // Object fits into current region, record new location:
    assert(_young_compact_point + obj_size <= _young_to_region->end(), "must fit");
    shenandoah_assert_not_forwarded(nullptr, p);
    _preserved_marks->push_if_necessary(p, p->mark());
    p->forward_to(cast_to_oop(_young_compact_point));
    _young_compact_point += obj_size;
  }
}

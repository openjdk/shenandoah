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

#include "gc/shared/preservedMarks.inline.hpp"
#include "gc/shenandoah/shenandoahGenerationalFullGC.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahHeapRegion.inline.hpp"

ShenandoahPrepareForGenerationalCompactionObjectClosure::ShenandoahPrepareForGenerationalCompactionObjectClosure(PreservedMarks* preserved_marks,
                                                          GrowableArray<ShenandoahHeapRegion*>& empty_regions,
                                                          ShenandoahHeapRegion* old_to_region,
                                                          ShenandoahHeapRegion* young_to_region, uint worker_id) :
        _preserved_marks(preserved_marks),
        _heap(ShenandoahHeap::heap()),
        _tenuring_threshold(0),
        _empty_regions(empty_regions),
        _empty_regions_pos(0),
        _old_to_region(old_to_region),
        _young_to_region(young_to_region),
        _from_region(nullptr),
        _from_affiliation(ShenandoahAffiliation::FREE),
        _old_compact_point((old_to_region != nullptr)? old_to_region->bottom(): nullptr),
        _young_compact_point((young_to_region != nullptr)? young_to_region->bottom(): nullptr),
        _worker_id(worker_id) {
  if (_heap->mode()->is_generational()) {
    _tenuring_threshold = _heap->age_census()->tenuring_threshold();
  }
}

void ShenandoahPrepareForGenerationalCompactionObjectClosure::set_from_region(ShenandoahHeapRegion* from_region) {
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

int ShenandoahPrepareForGenerationalCompactionObjectClosure::empty_regions_pos() {
  return _empty_regions_pos;
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
      _heap->increase_object_age(p, from_region_age + 1);
    } else {
      _heap->increase_object_age(p, from_region_age);
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

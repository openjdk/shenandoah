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

#include "gc/shenandoah/shenandoahCollectionSetParameters.hpp"
#include "runtime/atomic.hpp"
#include "shenandoahAsserts.hpp"

ShenandoahCollectionSetParameters::ShenandoahCollectionSetParameters()
  : _promotion_potential(0)
  , _pad_for_promote_in_place(0)
  , _promotable_humongous_regions(0)
  , _regular_regions_promoted_in_place(0)
  , _promoted_reserve(0)
  , _promoted_expended(0)
  , _old_evac_reserve(0)
  , _young_evac_reserve(0)
  {
  }

size_t ShenandoahCollectionSetParameters::set_promoted_reserve(size_t new_val) {
  size_t orig = _promoted_reserve;
  _promoted_reserve = new_val;
  return orig;
}

size_t ShenandoahCollectionSetParameters::get_promoted_reserve() const {
  return _promoted_reserve;
}

size_t ShenandoahCollectionSetParameters::set_old_evac_reserve(size_t new_val) {
  size_t orig = _old_evac_reserve;
  _old_evac_reserve = new_val;
  return orig;
}

size_t ShenandoahCollectionSetParameters::get_old_evac_reserve() const {
  return _old_evac_reserve;
}

void ShenandoahCollectionSetParameters::augment_old_evac_reserve(size_t increment) {
  _old_evac_reserve += increment;
}

void ShenandoahCollectionSetParameters::augment_promo_reserve(size_t increment) {
  _promoted_reserve += increment;
}

void ShenandoahCollectionSetParameters::reset_promoted_expended() {
  Atomic::store(&_promoted_expended, (size_t) 0);
}

size_t ShenandoahCollectionSetParameters::expend_promoted(size_t increment) {
  shenandoah_assert_heaplocked();
  assert(get_promoted_expended() + increment <= get_promoted_reserve(), "Do not expend more promotion than budgeted");
  return Atomic::add(&_promoted_expended, increment);
}

size_t ShenandoahCollectionSetParameters::unexpend_promoted(size_t decrement) {
  return Atomic::sub(&_promoted_expended, decrement);
}

size_t ShenandoahCollectionSetParameters::get_promoted_expended() {
  return Atomic::load(&_promoted_expended);
}

size_t ShenandoahCollectionSetParameters::set_young_evac_reserve(size_t new_val) {
  size_t orig = _young_evac_reserve;
  _young_evac_reserve = new_val;
  return orig;
}

size_t ShenandoahCollectionSetParameters::get_young_evac_reserve() const {
  return _young_evac_reserve;
}

void ShenandoahCollectionSetParameters::reset_generation_reserves() {
  set_young_evac_reserve(0);
  set_old_evac_reserve(0);
  set_promoted_reserve(0);
}
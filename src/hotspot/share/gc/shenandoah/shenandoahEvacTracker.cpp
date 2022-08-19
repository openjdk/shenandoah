/*
 * Copyright (c) 2022, Amazon, Inc. All rights reserved.
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

#include "gc/shenandoah/shenandoahEvacTracker.hpp"
#include "runtime/atomic.hpp"

ShenandoahEvacuationStats::ShenandoahEvacuationStats()
  : _evacuations_completed(0), _bytes_completed(0),
    _evacuations_attempted(0), _bytes_attempted(0) {}

void ShenandoahEvacuationStats::begin_evacuation(size_t bytes) {
  Atomic::inc(&_evacuations_attempted);
  Atomic::add(&_bytes_attempted, bytes);
}

void ShenandoahEvacuationStats::end_evacuation(size_t bytes) {
  Atomic::inc(&_evacuations_completed);
  Atomic::add(&_bytes_completed, bytes);
}

void ShenandoahEvacuationStats::accumulate(const ShenandoahEvacuationStats &other) {
  _evacuations_completed += other._evacuations_completed;
  _bytes_completed += other._bytes_completed;
  _evacuations_attempted += other._evacuations_attempted;
  _bytes_attempted += other._bytes_attempted;
}

void ShenandoahEvacuationStats::reset() {
  _evacuations_completed = _evacuations_attempted = 0;
  _bytes_completed = _bytes_attempted = 0;
}

void ShenandoahEvacuationStats::print_on(outputStream* st) const {
  size_t abandoned_size = _bytes_attempted - _bytes_completed;
  size_t abandoned_count = _evacuations_attempted - _evacuations_completed;
  st->print("Evacuated " SIZE_FORMAT "%s across " SIZE_FORMAT " objects, "
            "abandoned " SIZE_FORMAT " %s across " SIZE_FORMAT " objects.",
            byte_size_in_proper_unit(_bytes_completed),
            proper_unit_for_byte_size(_bytes_completed), _evacuations_completed,
            byte_size_in_proper_unit(abandoned_size),
            proper_unit_for_byte_size(abandoned_size), abandoned_count);
}

void ShenandoahEvacuationTracker::print_cycle_on(outputStream* st) const {
  print_evacuations_on(st, _workers_cycle, _mutators_cycle);
}

void ShenandoahEvacuationTracker::print_global_on(outputStream* st) const {
  print_evacuations_on(st, _workers_global, _mutators_global);
}

void ShenandoahEvacuationTracker::print_evacuations_on(outputStream* st,
                                                       const ShenandoahEvacuationStats &workers,
                                                       const ShenandoahEvacuationStats &mutators) {
  st->print("Workers: ");
  workers.print_on(st);
  st->cr();
  st->print("Mutators: ");
  mutators.print_on(st);
  st->cr();
}
void ShenandoahEvacuationTracker::flush_cycle_to_global() {
  _mutators_global.accumulate(_mutators_cycle);
  _workers_global.accumulate(_workers_cycle);
  _mutators_cycle.reset();
  _workers_cycle.reset();
}

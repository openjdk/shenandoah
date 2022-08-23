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

#include "gc/shared/workerThread.hpp"
#include "gc/shenandoah/shenandoahEvacTracker.hpp"
#include "runtime/atomic.hpp"
#include "runtime/thread.hpp"

ShenandoahEvacuationStats::ShenandoahEvacuationStats()
  : _evacuations_completed(0), _bytes_completed(0),
    _evacuations_attempted(0), _bytes_attempted(0) {}

void ShenandoahEvacuationStats::begin_evacuation(size_t bytes) {
  Atomic::inc(&_evacuations_attempted);
  Atomic::add(&_bytes_attempted, bytes);
}

void ShenandoahEvacuationStats::begin_worker_evacuation(size_t bytes) {
  ++_evacuations_attempted;
  _bytes_attempted += bytes;
}

void ShenandoahEvacuationStats::end_evacuation(size_t bytes) {
  Atomic::inc(&_evacuations_completed);
  Atomic::add(&_bytes_completed, bytes);
}

void ShenandoahEvacuationStats::end_worker_evacuation(size_t bytes) {
  ++_evacuations_completed;
  _bytes_completed += bytes;
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
            "abandoned " SIZE_FORMAT "%s across " SIZE_FORMAT " objects.",
            byte_size_in_proper_unit(_bytes_completed),
            proper_unit_for_byte_size(_bytes_completed), _evacuations_completed,
            byte_size_in_proper_unit(abandoned_size),
            proper_unit_for_byte_size(abandoned_size), abandoned_count);
}

ShenandoahEvacuationTracker::ShenandoahEvacuationTracker(uint max_workers)
  : _max_workers(max_workers) {
  _workers_cycle = NEW_C_HEAP_ARRAY(ShenandoahEvacuationStats, max_workers, mtGC);
  for (uint i = 0; i < _max_workers; ++i) {
    _workers_cycle[i].reset();
  }
}

void ShenandoahEvacuationTracker::print_cycle_on(outputStream* st) const {
  ShenandoahEvacuationStats workers;
  for (uint i = 0; i < _max_workers; ++i) {
    ShenandoahEvacuationStats& worker = _workers_cycle[i];
    workers.accumulate(worker);
  }
  print_evacuations_on(st, workers, _mutators_cycle);
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
  for (uint i = 0; i < _max_workers; ++i) {
    ShenandoahEvacuationStats& worker = _workers_cycle[i];
      _workers_global.accumulate(worker);
      worker.reset();
  }

  _mutators_global.accumulate(_mutators_cycle);
  _mutators_cycle.reset();
}

void ShenandoahEvacuationTracker::begin_evacuation(Thread* thread, size_t bytes) {
  if (thread->is_Java_thread()) {
    _mutators_cycle.begin_evacuation(bytes);
  } else if (thread->is_Worker_thread()) {
    uint worker_id = WorkerThread::worker_id();
    ShenandoahEvacuationStats& stats = _workers_cycle[worker_id];
    stats.begin_worker_evacuation(bytes);
  } else {
    // control thread?
    ShenandoahEvacuationStats& stats = _workers_cycle[0];
    stats.begin_worker_evacuation(bytes);
  }
}

void ShenandoahEvacuationTracker::end_evacuation(Thread* thread, size_t bytes) {
  if (thread->is_Java_thread()) {
    _mutators_cycle.end_evacuation(bytes);
  } else if (thread->is_Worker_thread()) {
    uint worker_id = WorkerThread::worker_id();
    ShenandoahEvacuationStats& stats = _workers_cycle[worker_id];
    stats.end_worker_evacuation(bytes);
  } else {
    // control thread?
    ShenandoahEvacuationStats& stats = _workers_cycle[0];
    stats.end_worker_evacuation(bytes);
  }
}

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

#include "gc/shenandoah/shenandoahMmuTracker.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahOldGeneration.hpp"
#include "gc/shenandoah/shenandoahYoungGeneration.hpp"
#include "runtime/os.hpp"
#include "logging/log.hpp"

class ThreadTimeAccumulator : public ThreadClosure {
 public:
  size_t total_time;
  ThreadTimeAccumulator() : total_time(0) {}
  virtual void do_thread(Thread* thread) override {
    size_t time = os::thread_cpu_time(thread);
    log_info(gc)("%s: " SIZE_FORMAT "ns.", thread->name(), time);
    total_time += time;
  }
};

double ShenandoahMmuTracker::gc_thread_time_seconds() {
  ThreadTimeAccumulator cl;
  ShenandoahHeap::heap()->gc_threads_do(&cl);
  // Include VM thread? Compiler threads? or no - because there
  // is nothing the collector can do about those threads.
  return double(cl.total_time) / NANOSECS_PER_SEC;
}

double ShenandoahMmuTracker::process_time_seconds() {
  double process_real_time(0.0), process_user_time(0.0), process_system_time(0.0);
  bool valid = os::getTimesSecs(&process_real_time, &process_user_time, &process_system_time);
  if (valid) {
    return process_user_time + process_system_time;
  }
  return 0.0;
}

ShenandoahMmuTracker::ShenandoahMmuTracker() :
  _initial_collector_time_s(0.0),
  _initial_process_time_s(0.0),
  _mmu_lock(Mutex::nosafepoint - 2, "ShenandoahMMU_lock", false) {
}

void ShenandoahMmuTracker::record(ShenandoahGeneration* generation) {
  MonitorLocker lock(&_mmu_lock, Mutex::_no_safepoint_check_flag);
  double collector_time_s = gc_thread_time_seconds();
  double elapsed_gc_time_s = collector_time_s - _initial_collector_time_s;
  generation->add_collection_time(elapsed_gc_time_s);
  _initial_collector_time_s = collector_time_s;
}

void ShenandoahMmuTracker::report() {
  MonitorLocker lock(&_mmu_lock, Mutex::_no_safepoint_check_flag);
  double process_time_s = process_time_seconds();
  double elapsed_process_time_s = process_time_s - _initial_process_time_s;
  _initial_process_time_s = process_time_s;

  ShenandoahHeap* heap = ShenandoahHeap::heap();
  double old_time_s = heap->old_generation()->reset_collection_time();
  double young_time_s = heap->young_generation()->reset_collection_time();
  double global_time_s = heap->global_generation()->reset_collection_time();
  double thread_time_s = old_time_s + young_time_s + global_time_s;

  double verify_time_s = gc_thread_time_seconds();
  double verify_elapsed = verify_time_s - _initial_verify_collector_time_s;
  _initial_verify_collector_time_s = verify_time_s;

  double mmu = ((elapsed_process_time_s - thread_time_s) / elapsed_process_time_s) * 100;
  double verify_mmu = ((elapsed_process_time_s - verify_elapsed) / elapsed_process_time_s) * 100;
  log_info(gc)("Usr+Sys process: %.3f, YOUNG = %.3f, OLD = %.3f, GLOBAL = %.3f, mmu = %.2f%%, VERIFY = %.3f, mmu = %.2f%%",
               elapsed_process_time_s, young_time_s, old_time_s, global_time_s, mmu, verify_elapsed, verify_mmu);
}

void ShenandoahMmuTracker::initialize() {
  _initial_process_time_s = process_time_seconds();
  _initial_collector_time_s = gc_thread_time_seconds();
  _initial_verify_collector_time_s = _initial_collector_time_s;
}
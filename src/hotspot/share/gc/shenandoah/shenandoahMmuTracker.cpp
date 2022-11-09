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
#include "runtime/os.hpp"
#include "logging/log.hpp"

class ThreadTimeAccumulator : public ThreadClosure {
 public:
  size_t total_time;
  ThreadTimeAccumulator() : total_time(0) {}
  virtual void do_thread(Thread* thread) override {
    total_time += os::thread_cpu_time(thread);
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
  _initial_gc_time_s(0.0), _initial_time_s(0.0) {
}

void ShenandoahMmuTracker::update() {
  double total_gc_time_s = gc_thread_time_seconds();
  double total_time_s = process_time_seconds();
  if (_initial_time_s != 0.0 && _initial_gc_time_s != 0.0) {
    double elapsed_gc_s = total_gc_time_s - _initial_gc_time_s;
    double elapsed_s = total_time_s - _initial_time_s;
    double mmu = ((elapsed_s - elapsed_gc_s) / elapsed_s) * 100;
    log_info(gc)("Usr+Sys process: %.3f gc threads = %.3f, mmu = %.2f%%", elapsed_s, elapsed_gc_s, mmu);
  }

  _initial_gc_time_s = total_gc_time_s;
  _initial_time_s = total_time_s;
}

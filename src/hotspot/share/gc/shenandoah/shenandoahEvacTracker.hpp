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

#ifndef SHARE_GC_SHENANDOAH_SHENANDOAHEVACTRACKER_HPP
#define SHARE_GC_SHENANDOAH_SHENANDOAHEVACTRACKER_HPP

#include "utilities/ostream.hpp"

class ShenandoahEvacuationStats {
 private:
  size_t _evacuations_completed;
  size_t _bytes_completed;
  size_t _evacuations_attempted;
  size_t _bytes_attempted;

 public:
  ShenandoahEvacuationStats();
  void begin_evacuation(size_t bytes);
  void begin_worker_evacuation(size_t bytes);
  void end_evacuation(size_t bytes);
  void end_worker_evacuation(size_t bytes);

  void print_on(outputStream* st) const;
  void accumulate(const ShenandoahEvacuationStats& other);
  void reset();
};

class ShenandoahEvacuationTracker : public CHeapObj<mtGC> {
 private:
  ShenandoahEvacuationStats* _workers_cycle;
  ShenandoahEvacuationStats  _mutators_cycle;
  ShenandoahEvacuationStats  _workers_global;
  ShenandoahEvacuationStats  _mutators_global;
  uint _max_workers;

 public:
  explicit ShenandoahEvacuationTracker(uint max_workers);

  void begin_evacuation(Thread* thread, size_t bytes);
  void end_evacuation(Thread* thread, size_t bytes);

  void print_cycle_on(outputStream* st) const;
  void print_global_on(outputStream* st) const;
  void flush_cycle_to_global();

 private:
  static void print_evacuations_on(outputStream* st, const ShenandoahEvacuationStats& workers, const ShenandoahEvacuationStats& mutators);
};

#endif //SHARE_GC_SHENANDOAH_SHENANDOAHEVACTRACKER_HPP

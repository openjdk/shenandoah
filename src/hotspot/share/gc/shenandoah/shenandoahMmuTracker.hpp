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

#ifndef SHARE_GC_SHENANDOAH_SHENANDOAHMMUTRACKER_HPP
#define SHARE_GC_SHENANDOAH_SHENANDOAHMMUTRACKER_HPP

#include "memory/iterator.hpp"
#include "runtime/mutex.hpp"
#include "utilities/numberSeq.hpp"

class ShenandoahGeneration;

class ShenandoahMmuTracker {

  double _initial_collector_time_s;
  double _initial_process_time_s;
  double _initial_verify_collector_time_s;

  double _resize_increment;

  Monitor _mmu_lock;
  TruncatedSeq _mmu_average;

  bool transfer_capacity(ShenandoahGeneration* from, ShenandoahGeneration* to);

 public:
  static double gc_thread_time_seconds();
  static double process_time_seconds();

  explicit ShenandoahMmuTracker();
  void record(ShenandoahGeneration* generation);
  void report();
  void initialize();
  bool adjust_generation_sizes();
};



#endif //SHARE_GC_SHENANDOAH_SHENANDOAHMMUTRACKER_HPP

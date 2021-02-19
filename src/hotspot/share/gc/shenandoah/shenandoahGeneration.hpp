/*
 * Copyright (c) 2020, Amazon.com, Inc. and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_GC_SHENANDOAH_SHENANDOAHGENERATION_HPP
#define SHARE_VM_GC_SHENANDOAH_SHENANDOAHGENERATION_HPP

#include "memory/allocation.hpp"
#include "gc/shenandoah/shenandoahLock.hpp"
#include "gc/shenandoah/shenandoahMarkingContext.hpp"

class ShenandoahHeapRegion;

class ShenandoahGeneration : public CHeapObj<mtGC> {
private:
  GenerationMode const _generation_mode;
  ShenandoahHeuristics* _heuristics;

  ShenandoahSharedFlag _is_marking_complete;
  // Marking task queues
  ShenandoahObjToScanQueueSet* _task_queues;
public:
  ShenandoahGeneration(GenerationMode generation_mode, uint max_queues);
  ~ShenandoahGeneration();

  inline GenerationMode generation_mode() const { return _generation_mode; }

  inline ShenandoahHeuristics* heuristics() const { return _heuristics; }

  virtual const char* name() const = 0;

  void initialize_heuristics(ShenandoahMode* gc_mode);

  virtual size_t soft_max_capacity() const = 0;
  virtual size_t max_capacity() const = 0;
  virtual size_t used_regions_size() const = 0;
  virtual size_t used() const = 0;
  virtual size_t available() const = 0;

  virtual size_t bytes_allocated_since_gc_start();

  void log_status() const;

  // Used directly by FullGC
  void reset_mark_bitmap();

  // Used by concurrent and degenerated GC to reset regions.
  void prepare_gc();
  void prepare_regions_and_collection_set(bool concurrent);

  // Cancel marking (used by Full collect and when cancelling cycle).
  void cancel_marking();

  // Return true if this region is affiliated with this generation.
  virtual bool contains(ShenandoahHeapRegion* region) const = 0;

  // Apply closure to all regions affiliated with this generation.
  virtual void parallel_heap_region_iterate(ShenandoahHeapRegionClosure* cl) = 0;

  // This is public to support cancellation of marking when a Full cycle is started.
  virtual void set_concurrent_mark_in_progress(bool in_progress) = 0;

  // Check the bitmap only for regions belong to this generation.
  bool is_bitmap_clear();

  // We need to track the status of marking for different generations.
  bool is_mark_complete();
  void set_mark_complete();
  void set_mark_incomplete();

  ShenandoahMarkingContext* complete_marking_context();

  // Task queues
  ShenandoahObjToScanQueueSet* task_queues() const { return _task_queues; }
  virtual void reserve_task_queues(uint workers);
  virtual ShenandoahObjToScanQueueSet* old_gen_task_queues() const;

  void scan_remembered_set();

 protected:

  virtual bool is_concurrent_mark_in_progress() = 0;
};

#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAHGENERATION_HPP

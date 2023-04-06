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
#include "gc/shenandoah/heuristics/shenandoahAdaptiveHeuristics.hpp"
#include "gc/shenandoah/heuristics/shenandoahHeuristics.hpp"
#include "gc/shenandoah/mode/shenandoahGenerationalMode.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "logging/log.hpp"
#include "logging/logTag.hpp"
#include "runtime/globals_extension.hpp"

void ShenandoahGenerationalMode::initialize_flags() const {
  if (ClassUnloading) {
    FLAG_SET_DEFAULT(ShenandoahSuspendibleWorkers, true);
    FLAG_SET_DEFAULT(VerifyBeforeExit, false);
  }

  SHENANDOAH_ERGO_OVERRIDE_DEFAULT(GCTimeRatio, 70);
  SHENANDOAH_ERGO_OVERRIDE_DEFAULT(ShenandoahUnloadClassesFrequency, 0);
  SHENANDOAH_ERGO_ENABLE_FLAG(ExplicitGCInvokesConcurrent);
  SHENANDOAH_ERGO_ENABLE_FLAG(ShenandoahImplicitGCInvokesConcurrent);

  // This helps most multi-core hardware hosts, enable by default
  SHENANDOAH_ERGO_ENABLE_FLAG(UseCondCardMark);

  // Final configuration checks
  SHENANDOAH_CHECK_FLAG_SET(ShenandoahLoadRefBarrier);
  SHENANDOAH_CHECK_FLAG_UNSET(ShenandoahIUBarrier);
  SHENANDOAH_CHECK_FLAG_SET(ShenandoahSATBBarrier);
  SHENANDOAH_CHECK_FLAG_SET(ShenandoahCASBarrier);
  SHENANDOAH_CHECK_FLAG_SET(ShenandoahCloneBarrier);
}

ShenandoahHeuristics* ShenandoahGenerationalMode::initialize_heuristics(ShenandoahGeneration* generation) const {
  if (ShenandoahGCHeuristics == nullptr) {
    vm_exit_during_initialization("Unknown -XX:ShenandoahGCHeuristics option (null)");
  }

  if (strcmp(ShenandoahGCHeuristics, "adaptive") != 0) {
    vm_exit_during_initialization("Generational mode requires the (default) adaptive heuristic");
  }

  return new ShenandoahAdaptiveHeuristics(generation);
}

const char* affiliation_name(oop ptr) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  assert(heap->is_in(ptr), "Oop must be in the heap.");
  ShenandoahHeapRegion* region = heap->heap_region_containing(ptr);
  return affiliation_name(region->affiliation());
}

const char affiliation_code(ShenandoahRegionAffiliation type) {
  switch(type) {
    case ShenandoahRegionAffiliation::FREE:
      return 'F';
    case ShenandoahRegionAffiliation::YOUNG_GENERATION:
      return 'Y';
    case ShenandoahRegionAffiliation::OLD_GENERATION:
      return 'O';
    default:
      ShouldNotReachHere();
      return 'X';
  }
}

const char* affiliation_name(ShenandoahRegionAffiliation type) {
  switch (type) {
    case ShenandoahRegionAffiliation::FREE:
      return "FREE";
    case ShenandoahRegionAffiliation::YOUNG_GENERATION:
      return "YOUNG";
    case ShenandoahRegionAffiliation::OLD_GENERATION:
      return "OLD";
    default:
      ShouldNotReachHere();
      return nullptr;
  }
}

const char* generation_name(GenerationMode mode) {
  switch (mode) {
    case GenerationMode::GLOBAL:
      return "Global";
    case GenerationMode::OLD:
      return "Old";
    case GenerationMode::YOUNG:
      return "Young";
    default:
      ShouldNotReachHere();
      return nullptr;
  }
}

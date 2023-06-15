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

#ifndef SHARE_GC_SHENANDOAH_SHENANDOAHAGECENSUS_HPP
#define SHARE_GC_SHENANDOAH_SHENANDOAHAGECENSUS_HPP

#include "gc/shared/ageTable.hpp"

// A class for tracking a sequence of cohort population vectors (or,
// interchangeably, age tables) for up to C=MAX_COHORTS age cohorts, where a cohort
// represents the set of objects allocated during a specific inter-GC epoch.
// Epochs are demarcated by GC cycles, with those surviving a cycle aging by
// an epoch. The census tracks the historical variation of cohort demographics
// across N=MAX_SNAPSHOTS recent epochs. Since there are at most C age cohorts in
// the population, we need only track at most N=C epochal snapshots to track a
// maximal longitudinal demographics of every object's longitudinal cohort in
// the young generation. The _global_age_table is thus, currently, a C x N (row-major)
// matrix, with C=16, and, for now N=C=16, currently.
// In theory, we might decide to track even longer (N=MAX_SNAPSHOTS) demographic
// histories, but that isn't the case today. In particular, the current tenuring
// threshold algorithm uses only 2 most recent snapshots, with the remaining
// MAX_SNAPSHOTS-2=14 reserved for research purposes.
//
// In addition, this class also maintains per worker population vectors into which
// census for the current minor GC is accumulated (during marking or, optionally, during
// evacuation). These are cleared after each marking (resectively, evacuation) cycle,
// once the per-worker data is consolidated into the appropriate population vector
// per minor collection. The _local_age_table is thus C x N, for N GC workers.
class ShenandoahAgeCensus: public CHeapObj<mtGC> {
  AgeTable** _global_age_table;      // Global age table used for adapting tenuring threshold, one per snapshot
  AgeTable** _local_age_table;       // Local scratch age tables to track object ages, one per worker

  size_t* _global_skip_table;        // Size of objects skipped in census, one per snapshot
  size_t* _local_skip_table;         // Local scratch table for size of objects skipped in census, one per worker

  uint _epoch;                       // Current epoch (modulo max age)
  uint *_tenuring_threshold;         // An array of the last N tenuring threshold values we
                                     // computed.

  // A private work method invoked by the public compute_tenuring_threshold() method.
  // This uses the data in the ShenandoahAgeCensus object's _global_age_table and the
  // current _epoch to compute a new tenuring threshold, which will be remembered
  // until the next invocation of compute_tenuring_threshold.
  uint compute_tenuring_threshold_work();

  // Mortality rate of a cohort, given its population in 
  // previous and current epochs
  double mortality_rate(size_t prev_pop, size_t cur_pop);

 public:
  enum {
    MAX_COHORTS = AgeTable::table_size,    // = markWord::max_age
    MAX_SNAPSHOTS = MAX_COHORTS            // May change in the future
  };

  ShenandoahAgeCensus();

  // Return the local age table (population vector) for worker_id.
  // Only used in the case of !GenShenCensusAtEvac
  AgeTable* get_local_age_table(uint worker_id) {
    return (AgeTable*) _local_age_table[worker_id];
  }

  // Return the global age table (population vector) for the current epoch.
  // Where is this used?
  AgeTable* get_age_table() {
    return (AgeTable*) _global_age_table[_epoch];
  }

  // Update the local age table for worker_id by size for age
  void add(uint age, size_t size, uint worker_id);
  // Update the local skip table for worker_id by size
  void add_skipped(size_t size, uint worker_id);

  // Update to a new epoch, creating a slot for new census
  void update_epoch();
  // Reset the epocj, clearing accumulated census history
  void reset_epoch();

  void ingest(AgeTable* population_vector);
  void compute_tenuring_threshold();           // TODO: Add a strategy parameter

  // Return the most recently computed tenuring threshold at previous epoch.
  uint tenuring_threshold() const { return _tenuring_threshold[_epoch]; }

  // Print the age census information
  void print();
};

#endif // SHARE_GC_SHENANDOAH_SHENANDOAHAGECENSUS_HPP

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

// A class for tracking a sequence of cohort population vectors (age tables)
// for up to C age cohorts. We track up to C historical population vectors,
// to track temporal variation of cohort demographics. Since there are at most
// C age cohorts, we need only track at most C snapshots to track a maximal
// pre-promotion demographics of any object in the young generation.
// The _global_matrix is thus a C x C matrix, with C = 16, currently, see
// MAX_COHORTS below.
//
// In addition, we maintain per worker vectors into which census for the current
// minor GC is tracked during marking. These are cleared after each marking cycle,
// once the per-worker data is consolidated into the appropriate population vector
// at each minor collection. The _local_matrix is thus C x N, for N GC workers.
class ShenandoahAgeCensus: public CHeapObj<mtGC> {
  AgeTable** _global_age_table;      // Global age table used for adapting tenuring threshold
  AgeTable** _local_age_table;       // Local scratch age tables to track object ages
  uint _epoch;                       // Current epoch (modulo max age)
  uint *_tenuring_threshold;         // An array of the last N tenuring threshold values we
                                     // computed.

  // A private work method invoked by the public compute_tenuring_threshold() method.
  // This uses the data in the ShenandoahAgeCensus object's _global_age_table and the
  // current _epoch to compute a new tenuring threshold, which will be remembered
  // until the next invocation of compute_tenuring_threshold.
  uint compute_tenuring_threshold_work();

  // The fraction of prev_pop that survived in cur_pop
  double survival_rate(size_t prev_pop, size_t cur_pop);

 public:
  enum {
    MAX_COHORTS = AgeTable::table_size
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

  void update_epoch();
  void reset_epoch();

  void ingest(AgeTable* population_vector);
  void compute_tenuring_threshold();           // Add a strategy parameter

  // Return the most recently computed tenuring threshold at previous epoch.
  uint tenuring_threshold() const { return _tenuring_threshold[_epoch]; }
};

#endif // SHARE_GC_SHENANDOAH_SHENANDOAHAGECENSUS_HPP

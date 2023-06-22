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

#include "gc/shenandoah/mode/shenandoahGenerationalMode.hpp"
#include "gc/shenandoah/shenandoahAgeCensus.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"

ShenandoahAgeCensus::ShenandoahAgeCensus() {
  assert(ShenandoahHeap::heap()->mode()->is_generational(), "Only in generational mode");

  _global_age_table    = NEW_C_HEAP_ARRAY(AgeTable*, MAX_SNAPSHOTS, mtGC);
  CENSUS_NOISE(_global_noise        = NEW_C_HEAP_ARRAY(ShenandoahNoiseStats, MAX_SNAPSHOTS, mtGC);)
  _tenuring_threshold  = NEW_C_HEAP_ARRAY(uint, MAX_SNAPSHOTS, mtGC);

  for (int i = 0; i < MAX_SNAPSHOTS; i++) {
    // Note that we don't now get perfdata from age_table
    _global_age_table[i] = new AgeTable(false);
    CENSUS_NOISE(_global_noise[i].clear();)
    // Sentinel value
    _tenuring_threshold[i] = MAX_COHORTS;
  }
  if (!GenShenCensusAtEvac) {
    size_t max_workers = ShenandoahHeap::heap()->max_workers();
    _local_age_table = NEW_C_HEAP_ARRAY(AgeTable*, max_workers, mtGC);
    CENSUS_NOISE(_local_noise     = NEW_C_HEAP_ARRAY(ShenandoahNoiseStats, max_workers, mtGC);)
    for (uint i = 0; i < max_workers; i++) {
      _local_age_table[i] = new AgeTable(false);
      CENSUS_NOISE(_local_noise[i].clear();)
    }
  }
  _epoch = MAX_SNAPSHOTS - 1;  // see update_epoch()
}

void ShenandoahAgeCensus::add(uint obj_age, uint region_age, size_t size, uint worker_id) {
  if (obj_age <= markWord::max_age) {
    assert(obj_age < MAX_COHORTS && region_age < MAX_COHORTS, "Should have been tenured");
#ifdef SHENANDOAH_CENSUS_NOISE
    // Region ageing is stochastic and non-monotonic; this vitiates mortality
    // demographics in ways that might defeat our algorithms. Marking may be a
    // time when we might be able to correct this, but we currently do not do
    // this. Like skipped statistics further below, we want to track the
    // impact of this noise to see if this may be worthwhile. JDK-<TBD>.
    uint age = obj_age;
    if (region_age > 0) {
      add_aged(size, worker_id);   // this tracking is coarse for now
      age += region_age;
      if (age >= MAX_COHORTS) {
        age = (uint)(MAX_COHORTS - 1);  // clamp
        add_clamped(size, worker_id);
      }
    }
#else   // SHENANDOAH_CENSUS_NOISE
    uint age = MIN2(age, (uint)(MAX_COHORTS - 1));  // clamp
#endif  // SHENANDOAH_CENSUS_NOISE
    get_local_age_table(worker_id)->add(age, size);
  } else {
    // update skipped statistics
    CENSUS_NOISE(add_skipped(size, worker_id);)
  }
}

#ifdef SHENANDOAH_CENSUS_NOISE
void ShenandoahAgeCensus::add_skipped(size_t size, uint worker_id) {
  _local_noise[worker_id].skipped += size;
}

void ShenandoahAgeCensus::add_aged(size_t size, uint worker_id) {
  _local_noise[worker_id].aged += size;
}

void ShenandoahAgeCensus::add_clamped(size_t size, uint worker_id) {
  _local_noise[worker_id].clamped += size;
}
#endif // SHENANDOAH_CENSUS_NOISE

// Update the epoch for the global age tables,
// and merge local age tables into the global age table.
// If appropriate, compute the new tenuring threshold for
// this epoch.
void ShenandoahAgeCensus::update_epoch() {
  assert(_epoch < MAX_SNAPSHOTS, "Out of bounds");
  if (++_epoch >= MAX_SNAPSHOTS) {
    _epoch=0;
  }
  // Merge data from local age tables into the global age table for the epoch,
  // clearing the local tables.
  _global_age_table[_epoch]->clear();
  CENSUS_NOISE(_global_noise[_epoch].clear();)
  if (!GenShenCensusAtEvac) {
    size_t max_workers = ShenandoahHeap::heap()->max_workers();
    for (uint i = 0; i < max_workers; i++) {
      // age stats
      _global_age_table[_epoch]->merge(_local_age_table[i]);
      _local_age_table[i]->clear();   // clear for next census
      // Merge noise stats
      CENSUS_NOISE(_global_noise[_epoch].merge(_local_noise[i]);)
      CENSUS_NOISE(_local_noise[i].clear();)
    }

    compute_tenuring_threshold();
  }
}


// Reset the epoch for the global age tables,
// clearing all history.
// TBD: do this for noise & local tables too?
void ShenandoahAgeCensus::reset_epoch() {
  assert(_epoch < MAX_SNAPSHOTS, "Out of bounds");
  for (uint i = 0; i < MAX_SNAPSHOTS; i++) {
    _global_age_table[i]->clear();
  }
  _epoch = MAX_SNAPSHOTS;
  assert(_epoch < MAX_SNAPSHOTS, "Error");
}

// Ingest a population vector into the global age table for
// the current epoch, merging it with any current data already
// present.
void ShenandoahAgeCensus::ingest(AgeTable* population_vector) {
  _global_age_table[_epoch]->merge(population_vector);
}

void ShenandoahAgeCensus::compute_tenuring_threshold() {
  if (!GenShenAdaptiveTenuring) {
    _tenuring_threshold[_epoch] = InitialTenuringThreshold;
  } else {
    uint tt = compute_tenuring_threshold_work();
    assert(tt <= MAX_COHORTS, "Out of bounds");
    _tenuring_threshold[_epoch] = tt;
  }
  print();
  log_trace(gc, age)("New tenuring threshold " UINTX_FORMAT
    " (min " UINTX_FORMAT ", max " UINTX_FORMAT")",
    (uintx) _tenuring_threshold[_epoch], GenShenMinTenuringThreshold, GenShenMaxTenuringThreshold);
}

uint ShenandoahAgeCensus::compute_tenuring_threshold_work() {
  // Starting with the largest non-zero population by age cohort
  // and working down in age of cohorts,find the lowest age such
  // that all higher ages have a mortality rate that is below a
  // pre-specified threshold. We consider this to be the adaptive
  // tenuring age to be used for the next cohort.
  // Results are clamped between user-specified mix & max guardrails,
  // so we ignore any cohorts outside [min,max].

  // Current and previous epoch in ring
  const uint cur_epoch = _epoch;
  const uint prev_epoch = cur_epoch > 0  ? cur_epoch - 1 : markWord::max_age;
  uint tenuring_threshold = GenShenMaxTenuringThreshold;

  // Current and previous population vectors in ring
  const AgeTable* cur_pv = _global_age_table[cur_epoch];
  const AgeTable* prev_pv = _global_age_table[prev_epoch];
  for (uint i = GenShenMaxTenuringThreshold - 1; i >= MAX2((uint)GenShenMinTenuringThreshold - 1, (uint)1); i--) {
    assert(i > 0, "Error");
    // Population & mortality rate of current cohort
    const size_t cur_pop = cur_pv->sizes[i];
    const size_t prev_pop = prev_pv->sizes[i-1];
    const double mr = mortality_rate(prev_pop, cur_pop);
    // We ignore any cohorts that had a very low population count, or
    // that have a lower mortality rate than we care to age in young; these
    // cohorts are considered eligible for tenuring when all older
    // cohorts are.
    if (i > 1 && (prev_pop < GenShenTenuringCohortPopulationThreshold ||
        mr < GenShenTenuringMortalityRateThreshold)) {
      tenuring_threshold = i;
      continue;
    }
    return tenuring_threshold;
  }
  return tenuring_threshold;
}

// Mortality rate of a cohort, given its previous and current population
double ShenandoahAgeCensus::mortality_rate(size_t prev_pop, size_t cur_pop) {
  // The following also covers the case where both entries are 0
  if (prev_pop <= cur_pop) {
    // adjust for inaccurate censuses by finessing the
    // reappearance of dark matter as normal matter;
    // mortality rate is 0 if population remained the same
    // or increased.
    if (cur_pop > prev_pop) {
      log_trace(gc, age)(" (dark matter) Cohort population "
                         SIZE_FORMAT " to " SIZE_FORMAT, prev_pop, cur_pop);
    }
    return 0.0;
  }
  assert(prev_pop > 0 && prev_pop > cur_pop, "Error");
  return 1.0 - (((double)cur_pop)/((double)prev_pop));
}

void ShenandoahAgeCensus::print() {
  // Print the population vector for the current epoch, and
  // for the previous epoch, as well as the computed mortality
  // ratio for each extant cohort.
  const uint cur_epoch = _epoch;
  const uint prev_epoch = cur_epoch > 0 ? cur_epoch - 1: markWord::max_age;

  const AgeTable* cur_pv = _global_age_table[cur_epoch];
  const AgeTable* prev_pv = _global_age_table[prev_epoch];

  const uint tt = tenuring_threshold();

  log_trace(gc, age)("Epoch: previous: " UINTX_FORMAT ", \t current: " UINTX_FORMAT,
                     (uintx)prev_epoch, (uintx)cur_epoch);
  log_info(gc, age)("\t\t ---- Population ---- \t\t -- Mortality -- ");

  size_t total= 0;
  for (uint i = 1; i < MAX_COHORTS; i++) {
    const size_t prev_pop = prev_pv->sizes[i-1];  // (i-1) OK because i >= 1
    const size_t cur_pop  = cur_pv->sizes[i];
    double mr = mortality_rate(prev_pop, cur_pop);
    // Suppress printing when everything is zero
    if (prev_pop + cur_pop > 0) {
      log_info(gc, age)(UINTX_FORMAT "\t\t " SIZE_FORMAT "\t\t " SIZE_FORMAT "\t\t %.2f " ,
                        (uintx)i, prev_pop, cur_pop, mr);
    }
    total += cur_pop;
    if (i == tt) {
      // Underline the cohort for tenuring threshold (if < MAX_COHORTS)
      log_info(gc, age)("----------------------------------------------------------------------------");
    }
  }
  CENSUS_NOISE(_global_noise[cur_epoch].print(total);)
}

#ifdef SHENANDOAH_CENSUS_NOISE
void ShenandoahNoiseStats::print(size_t total) {
  if (total > 0) {
    float f_skipped = (float)skipped/(float)total;
    float f_aged    = (float)aged/(float)total;
    float f_clamped = (float)clamped/(float)total;
    log_info(gc, age)("Skipped: " SIZE_FORMAT " (%.2f),  R-Aged: " SIZE_FORMAT " (%.2f), Clamped: " SIZE_FORMAT " (%.2f)",
                    skipped, f_skipped, aged, f_aged, clamped, f_clamped);
  }
}
#endif // SHENANDOAH_CENSUS_NOISE

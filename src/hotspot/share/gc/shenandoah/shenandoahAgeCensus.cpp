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
  const int max_age = markWord::max_age;
  _global_age_table = NEW_C_HEAP_ARRAY(AgeTable*, max_age, mtGC);
  _tenuring_threshold = NEW_C_HEAP_ARRAY(uint, max_age, mtGC);
  for (int i = 0; i <= max_age; i++) {
    // Note that we don't now get perfdata from age_table
    _global_age_table[i] = new AgeTable(false);
    // Sentinel value
    _tenuring_threshold[i] = max_age + 1;
  }
  if (!GenShenCensusAtEvac) {
    size_t max_workers = ShenandoahHeap::heap()->max_workers();
    _local_age_table = NEW_C_HEAP_ARRAY(AgeTable*, max_workers, mtGC);
    for (uint i = 0; i < max_workers; i++) {
      _local_age_table[i] = new AgeTable(false);
    }
  }
  _epoch = max_age;  // see update_epoch()
  assert(_epoch <= markWord::max_age, "Error");
}

// Update the epoch for the global age tables,
// and merge local age tables into the global age table.
void ShenandoahAgeCensus::update_epoch() {
  assert(_epoch <= markWord::max_age, "Error");
  if (++_epoch > markWord::max_age) {
    _epoch=0;
  }
  // Merge data from local age tables into the global age table for the epoch,
  // clearing the local tables.
  _global_age_table[_epoch]->clear();
  if (!GenShenCensusAtEvac) {
    size_t max_workers = ShenandoahHeap::heap()->max_workers();
    for (uint i = 0; i < max_workers; i++) {
      _global_age_table[_epoch]->merge(_local_age_table[i]);
      _local_age_table[i]->clear();
    }
    _global_age_table[_epoch]->print_age_table(InitialTenuringThreshold);
  }
}


// Reset the epoch for the global age tables,
// clearing all history.
void ShenandoahAgeCensus::reset_epoch() {
  assert(_epoch <= markWord::max_age, "Error");
  for (uint i = 0; i <= markWord::max_age; i++) {
    _global_age_table[i]->clear();
  }
  _epoch = markWord::max_age;
  assert(_epoch <= markWord::max_age, "Error");
}

void ShenandoahAgeCensus::ingest(AgeTable* population_vector) {
  _global_age_table[_epoch]->merge(population_vector);
}

void ShenandoahAgeCensus::compute_tenuring_threshold() { 
  if (!GenShenAdaptiveTenuring) {
    _tenuring_threshold[_epoch] = InitialTenuringThreshold;
  } else {
    guarantee(false, "NYI");
  }
}


/*
 * Copyright (c) 2021, Amazon.com, Inc. or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_SHENANDOAH_HEURISTICS_SHENANDOAHOLDHEURISTICS_HPP
#define SHARE_GC_SHENANDOAH_HEURISTICS_SHENANDOAHOLDHEURISTICS_HPP

#include "gc/shenandoah/shenandoahCollectionSet.inline.hpp"
#include "gc/shenandoah/shenandoahGeneration.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahHeapRegion.inline.hpp"
#include "gc/shenandoah/heuristics/shenandoahHeuristics.hpp"

class ShenandoahOldHeuristics : public ShenandoahHeuristics {

protected:

  // if (_generation->generation_mode() == OLD) _old_collection_candidates
  //  represent the number of regions selected for collection following the
  //  most recently completed old-gen mark that have not yet been selected
  //  for evacuation and _next_collection_candidate is the index within
  //  _region_data of the next candidate region to be selected for evacuation.
  // if (_generation->generation_mode() != OLD) these two variables are
  //  not used.
  uint _old_collection_candidates;
  uint _next_old_collection_candidate;

  // At the time we select the old-gen collection set, _hidden_old_collection_candidates
  // and _hidden_next_old_collection_candidates are set to remember the intended old-gen
  // collection set.  After all old-gen regions not in the old-gen collection set have been
  // coalesced and filled, the content of these variables is copied to _old_collection_candidates
  // and _next_old_collection_candidates so that evacuations can begin evacuating these regions.
  uint _hidden_old_collection_candidates;
  uint _hidden_next_old_collection_candidate;

  // Prepare for evacuation of old-gen regions by capturing the mark results of a recently completed concurrent mark pass.
  void prepare_for_old_collections();

public:
  ShenandoahOldHeuristics(ShenandoahGeneration* generation);

  virtual void choose_collection_set(ShenandoahCollectionSet* collection_set, ShenandoahOldHeuristics* old_heuristics);

  void prime_collection_set(ShenandoahCollectionSet* set);

  // Having coalesced and filled all old-gen heap regions that are not part of the old-gen collection set, begin
  // evacuating the collection set.
  void start_old_evacuations();

  // How many old-collection candidates have not yet been processed?
  uint unprocessed_old_collection_candidates();

  // Return the next old-collection candidate in order of decreasing amounts of garbage.  (We process most-garbage regions
  // first.)  This does not consume the candidate.  If the candidate is selected for inclusion in a collection set, then
  // the candidate is consumed by invoking consume_old_collection_candidate().
  ShenandoahHeapRegion* next_old_collection_candidate();

  // Adjust internal state to reflect that one fewer old-collection candidate remains to be processed.
  void consume_old_collection_candidate();

  bool should_defer_gc();

  // If a GLOBAL gc occurs, it will collect the entire heap which invalidates any collection candidates being
  // held by this heuristic for supplying mixed collections.
  void abandon_collection_candidates();

};

#endif // SHARE_GC_SHENANDOAH_HEURISTICS_SHENANDOAHOLDHEURISTICS_HPP

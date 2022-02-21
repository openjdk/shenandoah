/*
 * Copyright (c) 2021, Amazon.com, Inc. or its affiliates.  All rights reserved.
 *
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

#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahOopClosures.inline.hpp"
#include "gc/shenandoah/shenandoahReferenceProcessor.hpp"
#include "gc/shenandoah/shenandoahScanRemembered.inline.hpp"

ShenandoahDirectCardMarkRememberedSet::ShenandoahDirectCardMarkRememberedSet(ShenandoahCardTable* card_table, size_t total_card_count) {
  _heap = ShenandoahHeap::heap();
  _card_table = card_table;
  _total_card_count = total_card_count;
  _cluster_count = total_card_count / ShenandoahCardCluster<ShenandoahDirectCardMarkRememberedSet>::CardsPerCluster;
  _card_shift = CardTable::card_shift();

  _byte_map = _card_table->byte_for_index(0);

  _whole_heap_base = _card_table->addr_for(_byte_map);
  _whole_heap_end = _whole_heap_base + total_card_count * CardTable::card_size();

  _byte_map_base = _byte_map - (uintptr_t(_whole_heap_base) >> _card_shift);

  _overreach_map = (uint8_t *) malloc(total_card_count);
  _overreach_map_base = (_overreach_map -
                         (uintptr_t(_whole_heap_base) >> _card_shift));

  assert(total_card_count % ShenandoahCardCluster<ShenandoahDirectCardMarkRememberedSet>::CardsPerCluster == 0, "Invalid card count.");
  assert(total_card_count > 0, "Card count cannot be zero.");
  // assert(_overreach_cards != NULL);
}

ShenandoahDirectCardMarkRememberedSet::~ShenandoahDirectCardMarkRememberedSet() {
  free(_overreach_map);
}

void ShenandoahDirectCardMarkRememberedSet::initialize_overreach(size_t first_cluster, size_t count) {

  // We can make this run faster in the future by explicitly
  // unrolling the loop and doing wide writes if the compiler
  // doesn't do this for us.
  size_t first_card_index = first_cluster * ShenandoahCardCluster<ShenandoahDirectCardMarkRememberedSet>::CardsPerCluster;
  uint8_t* omp = &_overreach_map[first_card_index];
  uint8_t* endp = omp + count * ShenandoahCardCluster<ShenandoahDirectCardMarkRememberedSet>::CardsPerCluster;
  while (omp < endp)
    *omp++ = CardTable::clean_card_val();
}

void ShenandoahDirectCardMarkRememberedSet::merge_overreach(size_t first_cluster, size_t count) {

  // We can make this run faster in the future by explicitly unrolling the loop and doing wide writes if the compiler
  // doesn't do this for us.
  size_t first_card_index = first_cluster * ShenandoahCardCluster<ShenandoahDirectCardMarkRememberedSet>::CardsPerCluster;
  uint8_t* bmp = &_byte_map[first_card_index];
  uint8_t* endp = bmp + count * ShenandoahCardCluster<ShenandoahDirectCardMarkRememberedSet>::CardsPerCluster;
  uint8_t* omp = &_overreach_map[first_card_index];

  // dirty_card is 0, clean card is 0xff; if either *bmp or *omp is dirty, we need to mark it as dirty
  while (bmp < endp)
    *bmp++ &= *omp++;
}

#ifdef KELVIN_DEPRECATED
ShenandoahScanRememberedTask::ShenandoahScanRememberedTask(ShenandoahObjToScanQueueSet* queue_set,
                                                           ShenandoahObjToScanQueueSet* old_queue_set,
                                                           ShenandoahReferenceProcessor* rp,
                                                           ShenandoahRegionIterator* regions) :
  WorkerTask("Scan Remembered Set"),
  _queue_set(queue_set), _old_queue_set(old_queue_set), _rp(rp), _regions(regions) {}
#else
ShenandoahScanRememberedTask::ShenandoahScanRememberedTask(ShenandoahObjToScanQueueSet* queue_set,
                                                           ShenandoahObjToScanQueueSet* old_queue_set,
                                                           ShenandoahReferenceProcessor* rp,
                                                           ShenandoahRegionChunkIterator* work_list, bool is_concurrent) :
  WorkerTask("Scan Remembered Set"),
  _queue_set(queue_set), _old_queue_set(old_queue_set), _rp(rp), _work_list(work_list), _is_concurrent(is_concurrent) {}
#endif

void ShenandoahScanRememberedTask::work(uint worker_id) {
  if (_is_concurrent) {
    ShenandoahConcurrentWorkerSession worker_session(worker_id);
    ShenandoahSuspendibleThreadSetJoiner stsj(ShenandoahSuspendibleWorkers);
    do_work(worker_id);
  } else {
    ShenandoahParallelWorkerSession worker_session(worker_id);
    do_work(worker_id);
  }
}

void ShenandoahScanRememberedTask::do_work(uint worker_id) {
  // This sets up a thread local reference to the worker_id which is necessary
  // the weak reference processor.
  ShenandoahParallelWorkerSession worker_session(worker_id);
  ShenandoahWorkerTimingsTracker x(ShenandoahPhaseTimings::init_scan_rset, ShenandoahPhaseTimings::ScanClusters, worker_id);

  ShenandoahObjToScanQueue* q = _queue_set->queue(worker_id);
  ShenandoahObjToScanQueue* old = _old_queue_set == NULL ? NULL : _old_queue_set->queue(worker_id);
  ShenandoahMarkRefsClosure<YOUNG> cl(q, _rp, old);
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  RememberedScanner* scanner = heap->card_scan();

  // set up thread local closure for shen ref processor
  _rp->set_mark_closure(worker_id, &cl);
#ifdef KELVIN_DEPRECATED
  ShenandoahHeapRegion* region = _regions->next();
  while (region != NULL) {
    log_debug(gc)("ShenandoahScanRememberedTask::work(%u), looking at region " SIZE_FORMAT, worker_id, region->index());
    if (region->affiliation() == OLD_GENERATION) {
      scanner->process_region(region, &cl, _is_concurrent);
    }
    region = _regions->next();
  }
#else
  work_chunk assignment;
  bool has_work = _work_list->next(&assignment);
#undef KELVIN_TRACE_CANCEL
  while (has_work) {
#ifdef KELVIN_TRACE_CANCEL
      printf("ShenandoahScanRememberedTask::work(%u) checking for cancellation\n", worker_id);
      fflush(stdout);
#endif  
    if (heap->check_cancelled_gc_and_yield(_is_concurrent)) {
#ifdef KELVIN_TRACE_CANCEL
      printf("ShenandoahScanRememberedTask::work(%u) aborting while iterating over chunks\n", worker_id);
      fflush(stdout);
#endif  
      return;
    }
#ifdef KELVIN_TRACE_CANCEL
    printf("ShenandoahScanRememberedTask::work(%u) not cancelled\n", worker_id);
    fflush(stdout);
#endif  

    ShenandoahHeapRegion* region = assignment._r;
#undef KELVIN_VERBOSE
#ifdef KELVIN_VERBOSE
    printf("ShenandoahScanRememberedTask::work(%u), region " SIZE_FORMAT ", offset: " SIZE_FORMAT ", size: " SIZE_FORMAT "\n",
           worker_id, region->index(), assignment._chunk_offset, assignment._chunk_size);

#endif
    log_debug(gc)("ShenandoahScanRememberedTask::work(%u), processing slice of region "
                  SIZE_FORMAT " at offset " SIZE_FORMAT ", size: " SIZE_FORMAT,
                  worker_id, region->index(), assignment._chunk_offset, assignment._chunk_size);
    if (region->affiliation() == OLD_GENERATION) {
#ifdef KELVIN_TRACE_CANCEL
      printf("ShenandoahScanRememberedTask::work(%u) processing region slice\n", worker_id);
      fflush(stdout);
#endif  
      size_t cluster_size =
        CardTable::card_size_in_words() * ShenandoahCardCluster<ShenandoahDirectCardMarkRememberedSet>::CardsPerCluster;
      size_t clusters = assignment._chunk_size / cluster_size;
      assert(clusters * cluster_size == assignment._chunk_size, "Chunk assignments must align on cluster boundaries");
      HeapWord* end_of_range = region->bottom() + assignment._chunk_offset + assignment._chunk_size;

      // During concurrent mark, region->top() equals TAMS with respect to the current young-gen pass.  */
      if (end_of_range > region->top()) {
        end_of_range = region->top();
      }
      scanner->process_region_slice(region, assignment._chunk_offset, clusters, end_of_range, &cl, false, _is_concurrent);
    }
#ifdef KELVIN_TRACE_CANCEL
    printf("ShenandoahScanRememberedTask::work(%u) checking for another work assignment\n", worker_id);
    fflush(stdout);
#endif  
    has_work = _work_list->next(&assignment);
  }
#endif
}

#ifndef KELVIN_DEPRECATED
size_t ShenandoahRegionChunkIterator::calc_group_size() {
  // First group does roughly half of heap, one region at a time.  
  // Second group does roughly one quarter of heap, half of a region at a time, and so on.
  // Last group does the remnant of heap, one _smallest_chunk_size at a time.  
  // Round down.
  return _heap->num_regions() / 2;
}

size_t ShenandoahRegionChunkIterator::calc_first_group_chunk_size() {
  size_t words_in_region = ShenandoahHeapRegion::region_size_words();
  return words_in_region;
}

size_t ShenandoahRegionChunkIterator::calc_num_groups() {
  size_t total_heap_size = _heap->num_regions() * ShenandoahHeapRegion::region_size_words();
  size_t num_groups = 0;
  size_t cumulative_group_span = 0;
  size_t current_group_span = _first_group_chunk_size * _group_size;
  size_t smallest_group_span = _smallest_chunk_size * _group_size;
  while ((num_groups < _maximum_groups) && (cumulative_group_span + current_group_span <= total_heap_size)) {
    num_groups++;
    cumulative_group_span += current_group_span;
    if (current_group_span <= smallest_group_span) {
      break;
    } else {
      current_group_span /= 2;    // Each group spans half of what the preceding group spanned.
    }
  }
  // Loop post condition:
  //   num_groups <= _maximum_groups
  //   cumulative_group_span is the memory spanned by num_groups
  //   current_group_span is the span of the last fully populated group (assuming loop iterates at least once)
  //   each of num_groups is fully populated with _group_size chunks in each
  // Non post conditions:
  //   cumulative_group_span may be less than total_heap size for one or more of the folowing reasons
  //   a) The number of regions remaining to be spanned is smaller than a complete group, or
  //   b) We have filled up all groups through _maximum_groups and still have not spanned all regions

  if (cumulative_group_span < total_heap_size) {
    // We've got more regions to span
    if ((num_groups < _maximum_groups) && (current_group_span > smallest_group_span)) {
      num_groups++;             // Place all remaining regions into a new not-full group (chunk_size half that of previous group)
    }
    // Else we are unable to create a new group because we've exceed the number of allowed groups or have reached the
    // minimum chunk size.

    // Any remaining regions will be treated as if they are part of the most recently created group.  This group will
    // have more than _group_size chunks within it.
  }
  return num_groups;
}

#undef KELVIN_MAKE_NOISE

size_t ShenandoahRegionChunkIterator::calc_total_chunks() {
  size_t region_size_words = ShenandoahHeapRegion::region_size_words();
  size_t unspanned_heap_size = _heap->num_regions() * region_size_words;
  size_t num_chunks = 0;
  size_t num_groups = 0;
  size_t cumulative_group_span = 0;
  size_t current_group_span = _first_group_chunk_size * _group_size;
  size_t smallest_group_span = _smallest_chunk_size * _group_size;
  while (unspanned_heap_size > 0) {
    if (current_group_span <= unspanned_heap_size) {
      unspanned_heap_size -= current_group_span;
      num_chunks += _group_size;
      num_groups++;
#ifdef KELVIN_MAKE_NOISE
      printf("calc_total_chunks, group " SIZE_FORMAT " spans " SIZE_FORMAT ", which is " SIZE_FORMAT " regions\n",
             num_groups, current_group_span, current_group_span / region_size_words);
#endif

      if (num_groups >= _num_groups) {
        // The last group has more than _group_size entries.
        size_t chunk_span = current_group_span / _group_size;
        size_t extra_chunks = unspanned_heap_size / chunk_span;
        assert (extra_chunks * chunk_span == unspanned_heap_size, "Chunks must precisely span regions");
#ifdef KELVIN_MAKE_NOISE
        printf("calc_total_chunk() maxed out num_groups: " SIZE_FORMAT ", num_chunks: " SIZE_FORMAT
               ", unspanned_heap: " SIZE_FORMAT ", chunk_span: " SIZE_FORMAT ", extra_chunks: " SIZE_FORMAT "\n",
               num_groups, num_chunks, unspanned_heap_size, chunk_span, extra_chunks);
#endif
        num_chunks += extra_chunks;
        return num_chunks;
      } else if (current_group_span <= smallest_group_span) {
        // We cannot introduce new groups because we've reached the lower bound on group size
        size_t chunk_span = _smallest_chunk_size;
        size_t extra_chunks = unspanned_heap_size / chunk_span;
        assert (extra_chunks * chunk_span == unspanned_heap_size, "Chunks must precisely span regions");
#ifdef KELVIN_MAKE_NOISE
        printf("calc_total_chunk() maxed out chunk_span: " SIZE_FORMAT ", num_chunks: " SIZE_FORMAT
               ", unspanned_heap: " SIZE_FORMAT ", num_groups: " SIZE_FORMAT ", extra_chunks: " SIZE_FORMAT "\n",
               chunk_span, num_chunks, unspanned_heap_size, num_groups, extra_chunks);
#endif
        num_chunks += extra_chunks;
        return num_chunks;
      } else {
        current_group_span /= 2;
      }
    } else {
      // The last group has fewer than _group_size entries.
      size_t chunk_span = current_group_span / _group_size;
      size_t last_group_size = unspanned_heap_size / chunk_span;
      assert (last_group_size * chunk_span == unspanned_heap_size, "Chunks must precisely span regions");
      num_chunks += last_group_size;
#ifdef KELVIN_MAKE_NOISE
      printf("calc_total_chunk() last group not full, chunk_span: " SIZE_FORMAT ", num_chunks: " SIZE_FORMAT
               ", unspanned_heap: " SIZE_FORMAT ", num_groups: " SIZE_FORMAT ", last_group_size" SIZE_FORMAT "\n",
               chunk_span, num_chunks, unspanned_heap_size, num_groups, last_group_size);
#endif
      return num_chunks;
    }      
  }
#ifdef KELVIN_MAKE_NOISE
  printf("calc_total_chunk() normal return, num_chunks: " SIZE_FORMAT "\n", num_chunks);
#endif
  return num_chunks;
}

#undef KELVIN_VERBOSE

ShenandoahRegionChunkIterator::ShenandoahRegionChunkIterator(size_t worker_count) :
    _heap(ShenandoahHeap::heap()),
    _group_size(calc_group_size()),
    _first_group_chunk_size(calc_first_group_chunk_size()),
    _num_groups(calc_num_groups()),
    _total_chunks(calc_total_chunks()),
    _index(0)
{
  size_t words_in_region = ShenandoahHeapRegion::region_size_words();
  size_t group_span = _first_group_chunk_size * _group_size;

#ifdef KELVIN_MAKE_NOISE
  printf("ShenandoahChunkIterator, _num_groups: " SIZE_FORMAT ", _total_chunks: " SIZE_FORMAT "\n",
         _num_groups, _total_chunks);
#endif
  
  _region_index[0] = 0;
  _group_offset[0] = 0;
  for (size_t i = 1; i < _num_groups; i++) {
    _region_index[i] = _region_index[i-1] + (_group_offset[i-1] + group_span) / words_in_region;
    _group_offset[i] = (_group_offset[i-1] + group_span) % words_in_region;
    group_span /= 2;
  }
  // Not necessary, but keeps things tidy
  for (size_t i = _num_groups; i < _maximum_groups; i++) {
    _region_index[i] = 0;
    _group_offset[i] = 0;
  }
#ifdef KELVIN_VERBOSE
  printf("_num_groups: " SIZE_FORMAT "\n", _num_groups);
  for (size_t i = 0; i < _maximum_groups; i++) {
    printf("@ " SIZE_FORMAT ", _region_index[]: " SIZE_FORMAT ", _group_offset[]: " SIZE_FORMAT "\n",
           i, _region_index[i], _group_offset[i]);
  }
#endif
}

ShenandoahRegionChunkIterator::ShenandoahRegionChunkIterator(ShenandoahHeap* heap, size_t worker_count) :
    _heap(heap),
    _group_size(calc_group_size()),
    _first_group_chunk_size(calc_first_group_chunk_size()),
    _num_groups(calc_num_groups()),
    _total_chunks(calc_total_chunks()),
    _index(0)
{
  size_t words_in_region = ShenandoahHeapRegion::region_size_words();
  size_t group_span = _first_group_chunk_size * _group_size;
  
#ifdef KELVIN_MAKE_NOISE
  printf("ShenandoahChunkIterator, _num_groups: " SIZE_FORMAT ", _total_chunks: " SIZE_FORMAT "\n",
         _num_groups, _total_chunks);
#endif
  
  _region_index[0] = 0;
  _group_offset[0] = 0;
  for (size_t i = 1; i < _num_groups; i++) {
    _region_index[i] = _region_index[i-1] + (_group_offset[i-1] + group_span) / words_in_region;
    _group_offset[i] = (_group_offset[i-1] + group_span) % words_in_region;
    group_span /= 2;
  }
  // Not necessary, but keeps things tidy
  for (size_t i = _num_groups; i < _maximum_groups; i++) {
    _region_index[i] = 0;
    _group_offset[i] = 0;
  }
#ifdef KELVIN_VERBOSE
  printf("_num_groups: " SIZE_FORMAT "\n", _num_groups);
  for (size_t i = 0; i < _maximum_groups; i++) {
    printf("@ " SIZE_FORMAT ", _region_index[]: " SIZE_FORMAT ", _group_offset[]: " SIZE_FORMAT "\n",
           i, _region_index[i], _group_offset[i]);
  }
#endif
}

void ShenandoahRegionChunkIterator::reset() {
  _index = 0;
}

#endif


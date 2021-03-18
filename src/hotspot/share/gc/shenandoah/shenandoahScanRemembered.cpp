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

ShenandoahDirectCardMarkRememberedSet::ShenandoahDirectCardMarkRememberedSet(CardTable *card_table, size_t total_card_count) {
  _heap = ShenandoahHeap::heap();
  _card_table = card_table;
  _total_card_count = total_card_count;
  _cluster_count = total_card_count / ShenandoahCardCluster<ShenandoahDirectCardMarkRememberedSet>::CardsPerCluster;
  _card_shift = CardTable::card_shift;

  _byte_map = _card_table->byte_for_index(0);

  _whole_heap_base = _card_table->addr_for(_byte_map);
  _whole_heap_end = _whole_heap_base + total_card_count * CardTable::card_size;

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
  uint8_t *omp = &_overreach_map[first_card_index];
  uint8_t *endp = omp + count * ShenandoahCardCluster<ShenandoahDirectCardMarkRememberedSet>::CardsPerCluster;
  while (omp < endp)
    *omp++ = CardTable::clean_card_val();
}

void ShenandoahDirectCardMarkRememberedSet::merge_overreach(size_t first_cluster, size_t count) {

  // We can make this run faster in the future by explicitly unrolling the loop and doing wide writes if the compiler
  // doesn't do this for us.
  size_t first_card_index = first_cluster * ShenandoahCardCluster<ShenandoahDirectCardMarkRememberedSet>::CardsPerCluster;
  uint8_t *bmp = &_byte_map[first_card_index];
  uint8_t *endp = bmp + count * ShenandoahCardCluster<ShenandoahDirectCardMarkRememberedSet>::CardsPerCluster;
  uint8_t *omp = &_overreach_map[first_card_index];

  // dirty_card is 0, clean card is 0xff; if either *bmp or *omp is dirty, we need to mark it as dirty
  while (bmp < endp)
    *bmp++ &= *omp++;
}

ShenandoahScanRememberedTask::ShenandoahScanRememberedTask(ShenandoahObjToScanQueueSet* queue_set,
                                                           ShenandoahObjToScanQueueSet* old_queue_set,
                                                           ShenandoahReferenceProcessor* rp,
                                                           ShenandoahRegionIterator* regions) :
  AbstractGangTask("Scan Remembered Set"),
  _queue_set(queue_set), _old_queue_set(old_queue_set), _rp(rp), _regions(regions) {}

void ShenandoahScanRememberedTask::work(uint worker_id) {
  // This sets up a thread local reference to the worker_id which is necessary
  // the weak reference processor.
  ShenandoahParallelWorkerSession worker_session(worker_id);

  ShenandoahObjToScanQueue* q = _queue_set->queue(worker_id);
  ShenandoahObjToScanQueue* old = _old_queue_set == NULL ? NULL : _old_queue_set->queue(worker_id);
  ShenandoahMarkRefsClosure<YOUNG> cl(q, _rp, old);
  RememberedScanner *rs = ShenandoahHeap::heap()->card_scan();

  // set up thread local closure for shen ref processor
  _rp->set_mark_closure(worker_id, &cl);

  ShenandoahHeapRegion* region = _regions->next();
  while (region != NULL) {
    if (region->affiliation() == OLD_GENERATION) {
      rs->process_region(region, &cl);
    }
    region = _regions->next();
  }
}

/*
 * Copyright (c) Amazon.com, Inc. or its affiliates.  All rights reserved.
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

#include "shenandoahScanRemembered.hpp"


ShenandoahDirectCardMarkRememberedSet::ShenandoahDirectCardMarkRememberedSet(
    CardTable *card_table, size_t count)
{
  _heap = ShenandoahHeap::heap();
  _card_table = card_table;
  _card_count = count;
  _cluster_count = (
      _card_count /
      ShenandoahCardCluster<ShenandoahDirectCardMarkRememberedSet>::CardsPerCluster);
  _card_shift = CardTable::card_shift;

  _whole_heap_base = _card_table->addr_for(_byte_map);
  _whole_heap_end = _card_table->addr_for(_byte_map + _card_count);


  _byte_map = _card_table->byte_for_index(0);
  _byte_map_base = _byte_map - (uintptr_t(_whole_heap_base) >> _card_shift);

  _overreach_map = (uint8_t *) malloc(_card_count);
  _overreach_map_base = (_overreach_map -
			 (uintptr_t(_whole_heap_base) >> _card_shift));
      
  assert(_card_count % ShenandoahCardCluster<ShenandoahDirectCardMarkRememberedSet>::CardsPerCluster == 0, "Invalid card count.");
  assert(_card_count > 0, "Card count cannot be zero.");
  // assert(_overreach_cards != NULL);
}

ShenandoahDirectCardMarkRememberedSet::~ShenandoahDirectCardMarkRememberedSet()
{
  free(_overreach_map);
}

void ShenandoahDirectCardMarkRememberedSet::initializeOverreach(
    uint32_t first_cluster, uint32_t count) {

  // We can make this run faster in the future by explicitly
  // unrolling the loop and doing wide writes if the compiler
  // doesn't do this for us.
  uint32_t first_card_no =
      first_cluster * ShenandoahCardCluster<ShenandoahDirectCardMarkRememberedSet>::CardsPerCluster;
  uint8_t *omp = &_overreach_map[first_card_no];
  uint8_t *endp = omp + count * ShenandoahCardCluster<ShenandoahDirectCardMarkRememberedSet>::CardsPerCluster;
  while (omp < endp)
    *omp++ = CardTable::clean_card_val();
}

void ShenandoahDirectCardMarkRememberedSet::mergeOverreach(
    uint32_t first_cluster, uint32_t count) {

  // We can make this run faster in the future by explicitly
  // unrolling the loop and doing wide writes if the compiler
  // doesn't do this for us.
    uint32_t first_card_no = (
      first_cluster *
      ShenandoahCardCluster<ShenandoahDirectCardMarkRememberedSet>::CardsPerCluster);
  uint8_t *bmp = &_byte_map[first_card_no];
  uint8_t *endp = bmp + count * ShenandoahCardCluster<ShenandoahDirectCardMarkRememberedSet>::CardsPerCluster;
  uint8_t *omp = &_overreach_map[first_card_no];

  // dirty_card is 0, clean card is 0xff
  // if either *bmp or *omp is dirty, we need to mark it as dirty
  while (bmp < endp)
    *bmp++ &= *omp++;
}


/* Implementation is not correct.  ShenandoahBufferWithSATBRememberedSet
 * is a placeholder for future planned improvements.
 */
ShenandoahBufferWithSATBRememberedSet::ShenandoahBufferWithSATBRememberedSet(
    size_t card_count)
{
  _heap = ShenandoahHeap::heap();

  _card_count = card_count;
  _cluster_count = _card_count /
      ShenandoahCardCluster<ShenandoahBufferWithSATBRememberedSet>::CardsPerCluster;
  _card_shift = CardTable::card_shift;

  _whole_heap_base = _heap->base();
  _whole_heap_end = _whole_heap_base + _card_count * 
      ShenandoahCardCluster<ShenandoahBufferWithSATBRememberedSet>::CardsPerCluster;

}

/* Implementation is not correct.  ShenandoahBufferWithSATBRememberedSet
 * is a placeholder for future planned improvements.
 */
ShenandoahBufferWithSATBRememberedSet::~ShenandoahBufferWithSATBRememberedSet()
{
}

/* Implementation is not correct.  ShenandoahBufferWithSATBRememberedSet
 * is a placeholder for future planned improvements.
 */
void ShenandoahBufferWithSATBRememberedSet::initializeOverreach(
    uint32_t first_cluster, uint32_t count) {
}

/* Implementation is not correct.  ShenandoahBufferWithSATBRememberedSet
 * is a placeholder for future planned improvements.
 */
void ShenandoahBufferWithSATBRememberedSet::mergeOverreach(
    uint32_t first_cluster, uint32_t count) {
}

#ifdef IMPLEMENT_THIS_OPTIMIZATION_LATER

template <class RememberedSet>
bool ShenandoahCardCluster<RememberedSet>::hasObject(uint32_t card_no) {
  return (object_starts[card_no] & ObjectStartsInCardRegion)? true: false;
}

template <class RememberedSet>
uint32_t ShenandoahCardCluster<RememberedSet>::getFirstStart(uint32_t card_no)
{
  assert(object_starts[card_no] & ObjectStartsInCardRegion);
  return (((object_starts[card_no] & FirstStartBits) >> FirstStartShift) *
	  CardWordOffsetMultiplier);
}

template <class RememberedSet>
uint32_t ShenandoahCardCluster<RememberedSet>::getLastStart(uint32_t card_no) {
  assert(object_starts[card_no] & ObjectStartsInCardRegion);
  return (((object_starts[card_no] & LastStartBits) >> LastStartShift) *
	  CardWordOffsetMultiplier);
}

template <class RememberedSet>
uint8_t ShenandoahCardCluster<RememberedSet>::getCrossingObjectStart(
    uint32_t card_no) {
  assert((object_starts[card_no] & ObjectStartsInCardRegion) == 0);
  return object_starts[card_no] * CardWordOffsetMultiplier;
}

#else

// This implementation of services is slow but "sure" (in theory).
// Significant performance improvement is planned, with not a huge
// amount of further effort.

template <class RememberedSet>
bool ShenandoahCardCluster<RememberedSet>::hasObject(uint32_t card_no) {
  HeapWord *addr = _rs->addrForCardNo(card_no);
  ShenandoahHeap *heap = ShenandoahHeap::heap();
  ShenandoahHeapRegion *region = heap->heap_region_containing(addr);
  HeapWord *obj = region->block_start(addr);

  assert(obj != NULL, "Object cannot be null");
  if (obj >= addr)
    return true;
  else {
    HeapWord *end_addr = addr + CardTable::card_size_in_words;
    obj += oop(obj)->size();
    if (obj < end_addr)
      return true;
    else
      return false;
  }
}

template <class RememberedSet>
uint32_t ShenandoahCardCluster<RememberedSet>::getFirstStart(uint32_t card_no)
{
  HeapWord *addr = _rs->addrForCardNo(card_no);
  ShenandoahHeap *heap = ShenandoahHeap::heap();
  ShenandoahHeapRegion *region = heap->heap_region_containing(addr);
  HeapWord *obj = region->block_start(addr);

  assert(obj != NULL, "Object cannot be null.");
  if (obj >= addr)
    return obj - addr;
  else {
    HeapWord *end_addr = addr + CardTable::card_size_in_words;
    obj += oop(obj)->size();
    if (obj < end_addr)
      return obj - addr;
  }
}

template <class RememberedSet>
uint32_t ShenandoahCardCluster<RememberedSet>::getLastStart(uint32_t card_no) {
  HeapWord *addr = _rs->addrForCardNo(card_no);
  HeapWord *end_addr = addr + CardTable::card_size_in_words;
  ShenandoahHeap *heap = ShenandoahHeap::heap();
  ShenandoahHeapRegion *region = heap->heap_region_containing(addr);
  HeapWord *obj = region->block_start(addr);

  assert(obj != NULL, "Object cannot be null.");

  HeapWord *end_obj = obj + oop(obj)->size();
  while (end_obj < end_addr) {
    obj = end_obj;
    end_obj = obj + oop(obj)->size();
  }

  assert(obj >= addr, "Object out of range.");
  return obj - addr;
}

template <class RememberedSet>
uint32_t ShenandoahCardCluster<RememberedSet>::getCrossingObjectStart(
    uint32_t card_no) {
  HeapWord *addr = _rs->addrForCardNo(card_no);
  uint32_t cluster_no =
      card_no / ShenandoahCardCluster<RememberedSet>::CardsPerCluster;
  HeapWord *cluster_addr = _rs->addrForCardNo(cluster_no * CardsPerCluster);

  HeapWord *end_addr = addr + CardTable::card_size_in_words;
  ShenandoahHeap *heap = ShenandoahHeap::heap();
  ShenandoahHeapRegion *region = heap->heap_region_containing(addr);
  HeapWord *obj = region->block_start(addr);

  if (obj > cluster_addr)
    return obj - cluster_addr;
  else
    return 0x7fff;
}
#endif


template <typename RememberedSet>
template <typename ClosureType>
void ShenandoahScanRemembered<RememberedSet>::processClusters(
    uint32_t first_cluster, uint32_t count, ClosureType *oops) {

  // Unlike traditional Shenandoah marking, the old-gen resident
  // objects that are examined as part of the remembered set are not
  // themselves marked.  Each such object will be scanned only once.
  // Any young-gen objects referenced from the remembered set will
  // be marked and then subsequently scanned.

  while (count-- > 0) {

    uint32_t card_no = first_cluster *
	ShenandoahCardCluster<ShenandoahBufferWithSATBRememberedSet>::CardsPerCluster;
    uint32_t end_card_no = card_no +
	ShenandoahCardCluster<ShenandoahBufferWithSATBRememberedSet>::CardsPerCluster;

    while (card_no < end_card_no) {
      if (_scc->isCardDirty(card_no)) {
        if (_scc->hasObject(card_no)) {
          // Scan all objects that start within this card region.
          uint32_t start_offset = _scc->getFirstStart(card_no);
          HeapWord *p = _scc->getAddrForCard(card_no);
          HeapWord *endp = p + CardTable::card_size_in_words;
          p += start_offset;

          while (p < endp) {
            oop obj = oop(p);

            // Future TODO:
	    // For improved efficiency, we might want to give
            // special handling of obj->is_objArray().  In
            // particular, in that case, we might want to divide the
            // effort for scanning of a very long object array
            // between multiple threads.
            if (obj->is_objArray()) {
              objArrayOop array = objArrayOop(obj);
	      int len = array->length();
	      array->oop_iterate_range(oops, 0, len);
	    } else
	      oops->do_oop(&obj);
	    p += obj->size();
	  }
	  // p either points to start of next card region, or to
	  // the next object that needs to be scanned, which may
	  // reside in some successor card region.
	  card_no = _scc->cardAtAddress(p);
	} else {
          // otherwise, this card will have been scanned during
          // scan of a previous cluster.
          card_no++;
        }
      } else if (_scc->hasObject(card_no)) {
        // Scan the last object that starts within this card memory if
        // it spans at least one dirty card within this cluster or
        // if it reaches into the next cluster. 
        uint32_t start_offset = _scc->getFirstStart(card_no);
        HeapWord *p = _scc->getAddrForCard(card_no) + start_offset;
        oop obj = oop(p);
        HeapWord *nextp = p + obj->size();
        uint32_t last_card = _scc->cardAtAddress(nextp);

        bool reaches_next_cluster = (last_card > end_card_no);
        bool spans_dirty_within_this_cluster = false;
        if (!reaches_next_cluster) {
          uint32_t span_card;
          for (span_card = card_no+1; span_card < end_card_no; span_card++)
            if (_scc->isCardDirty(span_card)) {
              spans_dirty_within_this_cluster = true;
	      break;
	    }
	}
	if (reaches_next_cluster || spans_dirty_within_this_cluster) {
	  if (obj->is_objArray()) {
	    objArrayOop array = objArrayOop(obj);
	    int len = array->length();
	    array->oop_iterate_range(oops, 0, len);
          } else
            oops->do_oop(&obj);
	}
	// Increment card_no to account for the spanning object,
	// even if we didn't scan it.
	card_no = _scc->cardAtAddress(end_card_no);
      } else
	card_no++;
    }
  }
}

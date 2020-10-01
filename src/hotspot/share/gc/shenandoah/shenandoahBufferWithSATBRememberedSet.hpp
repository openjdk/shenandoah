//
// The anticipated benefits of an alternative SATB-based implementation of the remembered set are several fold:
//
//  1. The representation of remembered set is more concise (1 bit per card) and there is no remembered set maintenance
//     required for ShenandoahHeapRegions that correspond to young-gen memory.  Besides requiring less memory overall,
//     additional benefits of the more concise remembered set representation are improved cache hit rates and more efficient
//     scanning and maintenance of remembered set information by GC threads.
//  2. While the mutator overhead is similar between the modified SATB barrier mechanism and direct card marking, the
//     SATB mechanism offers the potential of improved accuracy within the remembered set.  This is because direct card
//     marking unconditionally marks every old-gen page that is overwritten with a pointer value, even when the new pointer
//     value might refer to old-gen memory.  With the modified SATB mechanism, background GC threads process the addresses
//     logged within the SATB buffer and mark cards as dirty only if the pointer found at an overwritten old-gen address
//     refers to young-gen memory.  There are (at least) two options here:
//
//      a) Just blindly dirty each card that is overwritten with a pointer value, regardless of the written value, as with
//         the implementation of traditional direct card marking.  When this card's memory region is eventually scanned, the
//         the implementation of remembered set scanning will clear the page if it no longer holds references to young-gen
//         memory.
//      b) When the thread-local SATB becomes full, the thread examines the content of each overwritten address and only
//         forwards the address to be marked as dirty if the address holds a young-gen reference.  Presumably, the value
//         just recently written by this same thread will be available in the L1 cache and fetching this single reference
//         "now" is more efficient than reading the entire card's memory at remembered set scanning time only to discover
//         then that the card represents no references to young-gen memory.
//
//     Experiments with each approach may help decide between approaches.
//
//  3. Potentially, the incremental overhead of adding remembered set logging to the existing SATB barrier is lower than
//     the incremental overhead of adding an independent and distinct new write barrier for card marking.  This is especially
//     true during times that the SATB barrier is already enabled (which might represent 30-50% of execution for GC-intensive
//     workloads).  It may even be true during times that the SATB is disabled.  This is because even when the SATB is disabled,
//     register allocation is constrained by the reality that SATB will be enabled some of the time, so registers allocated for
//     SATB-buffer maintenance will sit idle during times when the SATB barrier is disabled.  In tight loops that write pointer
//     values, for example, the SATB implementation might dedicate registers to holding thread-local information associated with
//     maintenance of the SATB buffer such as the address of the SATB buffer and the next available buffer slot within this buffer.
//     In the traditional card-marking remembered set implementation, an additional register might be dedicated to holding a
//     reference to the base of the card table.
//
// Note that a ShenandoahBufferWithSATBRememberedSet maintains the remembered set only for memory regions that correspond to
// old-generation memory.  Anticipate that the implementation will use double indirection.
//
// To Do:
//
//  1. Figure out which region an old-gen address belongs to
//  2. Find the cluster and card numbers that corresponds to that region
//
// Old-gen memory is not necessarily contiguous.  It may be comprised of multiple heap regions.
//
// The memory overhead for crossing map and card-table entries is given by the following analysis:
//   For each 512-byte card-entry span, we have the following overhead:
//    2 bytes for the object_starts map
//    1 bit for the card-table entry
//  =====
//    2 bytes (rounded down) out of 512 bytes is 0.39% bookkeeping overhead


// ShenandoahBufferWithSATBRememberedSet is not implemented correctly in its current form.
//
// This class is a placeholder to support a possible future implementation of remembered set that uses a generalization of the
// existing SATB pre-write barrier to maintain remembered set information rather than using unconditional direct card marking in
// a post-write barrier.
class ShenandoahBufferWithSATBRememberedSet: public CHeapObj<mtGC> {

  // The current implementation is simply copied from the implementation of class ShenandoahDirectCardMarkRememberedSet

  // Use symbolic constants defined in cardTable.hpp
  //  CardTable::card_shift = 9;
  //  CardTable::card_size = 512;
  //  CardTable::card_size_in_words = 64;

  //  CardTable::clean_card_val()
  //  CardTable::dirty_card_val()

  ShenandoahHeap *_heap;
  uint32_t _card_shift;
  size_t _card_count;
  uint32_t _cluster_count;
  HeapWord *_whole_heap_base;
  HeapWord *_whole_heap_end;

public:
  ShenandoahBufferWithSATBRememberedSet(size_t card_count);
  ~ShenandoahBufferWithSATBRememberedSet();

  uint32_t card_index_for_addr(HeapWord *p);
  HeapWord *addr_for_card_index(uint32_t card_index);
  bool is_card_dirty(uint32_t card_index);
  void mark_card_as_dirty(uint32_t card_index);
  void mark_card_as_clean(uint32_t card_index);
  void mark_overreach_card_as_dirty(uint32_t card_index);
  bool is_card_dirty(HeapWord *p);
  void mark_card_as_dirty(HeapWord *p);
  void mark_card_as_clean(HeapWord *p);
  void mark_overreach_card_as_dirty(void *p);
  uint32_t cluster_count();

  // Called by multiple GC threads at start of concurrent mark and/ evacuation phases.  Each parallel GC thread typically
  // initializes a different subranges of all overreach entries.
  void initialize_overreach(uint32_t first_cluster, uint32_t count);

  // Called by GC thread at end of concurrent mark or evacuation phase./ Each parallel GC thread typically merges different
  // subranges of all overreach entries.
  void merge_overreach(uint32_t first_cluster, uint32_t count);
};

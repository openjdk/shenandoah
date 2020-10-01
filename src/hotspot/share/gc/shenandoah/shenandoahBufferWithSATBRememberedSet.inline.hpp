
// ShenandoahBufferWithSATBRemberedSet is not currently implemented

inline uint32_t
ShenandoahBufferWithSATBRememberedSet::card_index_for_addr(HeapWord *p) {
  return 0;
}

inline HeapWord *
ShenandoahBufferWithSATBRememberedSet::addr_for_card_index(uint32_t card_index) {
  return NULL;
}

inline bool
ShenandoahBufferWithSATBRememberedSet::is_card_dirty(uint32_t card_index) {
  return false;
}

inline void
ShenandoahBufferWithSATBRememberedSet::mark_card_as_dirty(uint32_t card_index) {
}

inline void
ShenandoahBufferWithSATBRememberedSet::mark_card_as_clean(uint32_t card_index) {
}

inline void
ShenandoahBufferWithSATBRememberedSet::mark_overreach_card_as_dirty(uint32_t card_index) {
}

inline bool
ShenandoahBufferWithSATBRememberedSet::is_card_dirty(HeapWord *p) {
  return false;
}


inline void
ShenandoahBufferWithSATBRememberedSet::mark_card_as_dirty(HeapWord *p) {
}

inline void
ShenandoahBufferWithSATBRememberedSet::mark_card_as_clean(HeapWord *p) {
}

inline void
ShenandoahBufferWithSATBRememberedSet::mark_overreach_card_as_dirty(void *p) {
}

inline uint32_t
ShenandoahBufferWithSATBRememberedSet::cluster_count() {
  return 0;
}

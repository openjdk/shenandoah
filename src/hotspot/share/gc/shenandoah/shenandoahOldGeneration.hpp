
#ifndef SHARE_VM_GC_SHENANDOAH_SHENANDOAHOLDGENERATION_HPP
#define SHARE_VM_GC_SHENANDOAH_SHENANDOAHOLDGENERATION_HPP

#include "gc/shenandoah/shenandoahGeneration.hpp"

class ShenandoahOldGeneration : public ShenandoahGeneration {
 public:
  ShenandoahOldGeneration(uint max_queues, size_t max_capacity, size_t soft_max_capacity);

  const char* name() const;

  bool contains(ShenandoahHeapRegion* region) const;
  void parallel_heap_region_iterate(ShenandoahHeapRegionClosure* cl);
  void set_concurrent_mark_in_progress(bool in_progress);

 protected:
  bool is_concurrent_mark_in_progress();
};


#endif //SHARE_VM_GC_SHENANDOAH_SHENANDOAHOLDGENERATION_HPP

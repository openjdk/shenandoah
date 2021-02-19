#include "gc/shenandoah/shenandoahHeap.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahHeapRegion.hpp"
#include "gc/shenandoah/shenandoahOldGeneration.hpp"

ShenandoahOldGeneration::ShenandoahOldGeneration(uint max_queues)
  : ShenandoahGeneration(OLD, max_queues) {}

const char* ShenandoahOldGeneration::name() const {
  return "OLD";
}

size_t ShenandoahOldGeneration::soft_max_capacity() const {
  return 0;
}

size_t ShenandoahOldGeneration::max_capacity() const {
  return 0;
}

size_t ShenandoahOldGeneration::used_regions_size() const {
  return 0;
}

size_t ShenandoahOldGeneration::used() const {
  return 0;
}

size_t ShenandoahOldGeneration::available() const {
  return 0;
}

bool ShenandoahOldGeneration::contains(ShenandoahHeapRegion* region) const {
  return region->affiliation() != YOUNG_GENERATION;
}

void ShenandoahOldGeneration::parallel_heap_region_iterate(ShenandoahHeapRegionClosure* cl) {
  ShenandoahGenerationRegionClosure<OLD> old_regions(cl);
  ShenandoahHeap::heap()->parallel_heap_region_iterate(&old_regions);
}

void ShenandoahOldGeneration::set_concurrent_mark_in_progress(bool in_progress) {
  ShenandoahHeap::heap()->set_concurrent_old_mark_in_progress(in_progress);
}

bool ShenandoahOldGeneration::is_concurrent_mark_in_progress() {
  return ShenandoahHeap::heap()->is_concurrent_old_mark_in_progress();
}


#ifndef SRC_SHARE_GC_SHENANDOAH_SHENANDOAHHEAPREGIONCLOSURES_HPP
#define SRC_SHARE_GC_SHENANDOAH_SHENANDOAHHEAPREGIONCLOSURES_HPP


#include "gc/shenandoah/shenandoahHeap.hpp"
#include "gc/shenandoah/shenandoahHeapRegion.inline.hpp"

// Applies the given closure to all regions with the given affiliation
template<ShenandoahAffiliation AFFILIATION>
class ShenandoahIncludeRegionClosure : public ShenandoahHeapRegionClosure {
private:
  ShenandoahHeapRegionClosure* _closure;

public:
  explicit ShenandoahIncludeRegionClosure(ShenandoahHeapRegionClosure* closure): _closure(closure) {}

  void heap_region_do(ShenandoahHeapRegion* r) override {
    if (r->affiliation() == AFFILIATION) {
      _closure->heap_region_do(r);
    }
  }

  bool is_thread_safe() override {
    return _closure->is_thread_safe();
  }
};

// Applies the given closure to all regions without the given affiliation
template<ShenandoahAffiliation AFFILIATION>
class ShenandoahExcludeRegionClosure : public ShenandoahHeapRegionClosure {
private:
  ShenandoahHeapRegionClosure* _closure;

public:
  explicit ShenandoahExcludeRegionClosure(ShenandoahHeapRegionClosure* closure): _closure(closure) {}

  void heap_region_do(ShenandoahHeapRegion* r) override {
    if (r->affiliation() != AFFILIATION) {
      _closure->heap_region_do(r);
    }
  }

  bool is_thread_safe() override {
    return _closure->is_thread_safe();
  }
};

#endif //SRC_SHARE_GC_SHENANDOAH_SHENANDOAHHEAPREGIONCLOSURES_HPP

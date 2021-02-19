#ifndef SHARE_GC_SHENANDOAH_SHENANDOAHOLDGC_HPP
#define SHARE_GC_SHENANDOAH_SHENANDOAHOLDGC_HPP

#include "gc/shared/gcCause.hpp"
#include "gc/shenandoah/shenandoahConcurrentMark.hpp"
#include "gc/shenandoah/shenandoahConcurrentGC.hpp"
#include "gc/shenandoah/shenandoahHeap.hpp"

class ShenandoahGeneration;

class ShenandoahOldGC : public ShenandoahConcurrentGC {
 public:
  ShenandoahOldGC(ShenandoahGeneration* generation);
  bool collect(GCCause::Cause cause);
};


#endif //SHARE_GC_SHENANDOAH_SHENANDOAHOLDGC_HPP

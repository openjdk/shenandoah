/*
 * Copyright (c) 2021, Red Hat, Inc. All rights reserved.
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

#ifndef SHARE_GC_SHENANDOAH_SHENANDOAHBITMAPREGION_HPP
#define SHARE_GC_SHENANDOAH_SHENANDOAHBITMAPREGION_HPP

#include "memory/memRegion.hpp"

class ShenandoahHeapRegion;

/*
 * The purpose of this class to encapsulate operations on
 * the memory backing instances of Shenandoah's mark bitmap.
 * The encapsulation allows Shenandoah to use a secondary
 * mark bitmap to support remembered set scans during
 * concurrent marking of the old generation.
 */
class ShenandoahBitmapRegion {
 public:

  ShenandoahBitmapRegion();

  void initialize(size_t bitmap_size,
                  size_t bitmap_bytes_per_region,
                  size_t num_committed_regions);

  bool commit_bitmap_slice(ShenandoahHeapRegion *r);
  bool uncommit_bitmap_slice(ShenandoahHeapRegion *r);
  bool is_bitmap_slice_committed(ShenandoahHeapRegion *r, bool skip_self = false);

  void pretouch(size_t pretouch_bitmap_page_size);

  MemRegion bitmap_region() { return _bitmap_region; }

 private:
  MemRegion _bitmap_region;
  bool _bitmap_region_special;

  size_t _bitmap_size;
  size_t _bitmap_regions_per_slice;
  size_t _bitmap_bytes_per_slice;
  size_t _pretouch_bitmap_page_size;
};


#endif //SHENANDOAHBITMAPREGION_HPP

/*
 * Copyright Amazon.com Inc. or its affiliates. All Rights Reserved.
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

#ifndef SHARE_GC_SHENANDOAH_SHENANDOAHEVACINFO_HPP
#define SHARE_GC_SHENANDOAH_SHENANDOAHEVACINFO_HPP

#include "memory/allocation.hpp"

class ShenandoahEvacInfo : public StackObj {
  uint   _collection_set_regions;
  size_t _collection_set_used_before;
  size_t _collection_set_used_after;
  size_t _collected_old;
  size_t _collected_promoted;
  size_t _collected_young;
  uint   _regions_freed;
  uint   _regions_immediate;
  size_t _immediate_size;

public:
  ShenandoahEvacInfo() :
    _collection_set_regions(0), _collection_set_used_before(0), _collection_set_used_after(0),
    _collected_old(0), _collected_promoted(0), _collected_young(0),
    _regions_freed(0), _regions_immediate(0), _immediate_size(0) { }

  void set_collection_set_regions(uint collection_set_regions) {
    _collection_set_regions = collection_set_regions;
  }

  void set_collection_set_used_before(size_t used) {
    _collection_set_used_before = used;
  }

  void set_collection_set_used_after(size_t used) {
    _collection_set_used_after = used;
  }

  void set_collected_old(size_t collected) {
    _collected_old = collected;
  }

  void set_collected_promoted(size_t collected) {
    _collected_promoted = collected;
  }

  void set_collected_young(size_t collected) {
    _collected_young = collected;
  }

  void set_regions_freed(uint freed) {
    _regions_freed = freed;
  }

  void set_regions_immediate(uint immediate) {
    _regions_immediate = immediate;
  }

  void set_immediate_size(size_t size) {
    _immediate_size = size;
  }

  uint   collection_set_regions()     { return _collection_set_regions; }
  size_t collection_set_used_before() { return _collection_set_used_before; }
  size_t collection_set_used_after()  { return _collection_set_used_after; }
  size_t collected_old()              { return _collected_old; }
  size_t collected_promoted()         { return _collected_promoted; }
  size_t collected_young()            { return _collected_young; }
  uint   regions_freed()              { return _regions_freed; }
  uint   regions_immediate()          { return _regions_immediate; }
  size_t immediate_size()             { return _immediate_size; }
};

#endif // SHARE_GC_SHENANDOAH_SHENANDOAHEVACINFO_HPP

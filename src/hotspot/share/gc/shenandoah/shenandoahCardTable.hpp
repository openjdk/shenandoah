/*
 * Copyright (c) 2020, Amazon.com, Inc. and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_GC_SHENANDOAH_SHENANDOAHCARDTABLE_HPP
#define SHARE_VM_GC_SHENANDOAH_SHENANDOAHCARDTABLE_HPP

#include "gc/g1/g1RegionToSpaceMapper.hpp"
#include "gc/shared/cardTable.hpp"
#include "oops/oopsHierarchy.hpp"
#include "utilities/macros.hpp"

class ShenandoahCardTable: public CardTable {
  friend class VMStructs;

protected:
  CardValue* _read_byte_map;
  CardValue* _write_byte_map;
  CardValue* _read_byte_map_base;
  CardValue* _write_byte_map_base;

public:
  ShenandoahCardTable(MemRegion whole_heap) : CardTable(whole_heap) { }

  virtual void initialize();

  virtual bool is_in_young(oop obj) const;

  bool is_dirty(MemRegion mr);

  void clear();

  // After remembered set scanning has completed, clear the read_card_table
  // so that it can instantaneously become the write_card_table at the
  // time of next swap_card_tables() invocation.
  //
  // Since this effort is memory bound, we do not expect the work to
  // be divided between many concurrent GC worker threads.  Following
  // remembered set scanning, one worker thread clears the read table
  // while the other GC worker threads begin processing the content of
  // the mark closures.
  void clear_read_table();

  // Exchange the roles of the read and write card tables.
  void swap_card_tables();

  CardValue* read_byte_map() {
    return _read_byte_map;
  }

  CardValue* write_byte_map() {
    return _write_byte_map;
  }

  CardValue* read_byte_map_base() {
    return _read_byte_map_base;
  }

  CardValue* write_byte_map_base() {
    return _write_byte_map_base;
  }
};

#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAHCARDTABLE_HPP

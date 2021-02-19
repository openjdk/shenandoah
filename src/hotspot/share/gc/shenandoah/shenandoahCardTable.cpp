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

#include "precompiled.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahCardTable.hpp"

void ShenandoahCardTable::initialize() {
  CardTable::initialize();
  resize_covered_region(_whole_heap);
}

bool ShenandoahCardTable::is_in_young(oop obj) const {
  return ShenandoahHeap::heap()->is_in_young(obj);
}

bool ShenandoahCardTable::is_dirty(MemRegion mr) {
  for (size_t i = index_for(mr.start()); i <= index_for(mr.end() - 1); i++) {
    CardValue* byte = byte_for_index(i);
    if (*byte == CardTable::dirty_card_val()) {
      return true;
    }
  }
  return false;
}

void ShenandoahCardTable::clear() {
  CardTable::clear(_whole_heap);
}

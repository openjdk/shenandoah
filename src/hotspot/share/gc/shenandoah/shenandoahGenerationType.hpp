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

#ifndef SHARE_GC_SHENANDOAH_SHENANDOAHGENERATIONTYPE_HPP
#define SHARE_GC_SHENANDOAH_SHENANDOAHGENERATIONTYPE_HPP

enum ShenandoahGenerationType {
    NON_GEN,         // non-generational
    GLOBAL,          // generational: Global
    YOUNG,           // generational: Young
    OLD              // generational: Old
};

inline const char* shenandoah_generation_name(ShenandoahGenerationType mode) {
  switch (mode) {
    case NON_GEN:
      return "Non-Generational";
    case GLOBAL:
      return "Global";
    case OLD:
      return "Old";
    case YOUNG:
      return "Young";
    default:
      ShouldNotReachHere();
      return "Unknown";
  }
}

#endif // SHARE_GC_SHENANDOAH_SHENANDOAHGENERATIONTYPE_HPP

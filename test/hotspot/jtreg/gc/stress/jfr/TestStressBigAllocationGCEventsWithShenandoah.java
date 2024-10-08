/*
 * Copyright (c) 2015, 2023, Oracle and/or its affiliates. All rights reserved.
 * Copyright Amazon.com Inc. or its affiliates. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
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
 */
package jdk.jfr.event.gc.detailed;

/**
 * @test id=default
 * @key randomness
 * @requires vm.hasJFR
 * @requires vm.gc.Shenandoah
 * @library /test/lib /test/jdk
 * @run main/othervm -XX:+UseShenandoahGC -Xmx256m jdk.jfr.event.gc.detailed.TestStressBigAllocationGCEventsWithShenandoah 1048576
 */

 /**
  * @test id=generational
  * @key randomness
  * @requires vm.hasJFR
  * @requires vm.gc.Shenandoah
  * @library /test/lib /test/jdk
  * @run main/othervm -XX:+UseShenandoahGC -XX:ShenandoahGCMode=generational -Xmx256m jdk.jfr.event.gc.detailed.TestStressBigAllocationGCEventsWithShenandoah 1048576
  */
public class TestStressBigAllocationGCEventsWithShenandoah {

    public static void main(String[] args) throws Exception {
        new StressAllocationGCEvents().run(args);
    }
}

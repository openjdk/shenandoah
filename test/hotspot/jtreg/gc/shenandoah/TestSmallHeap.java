/*
 * Copyright (c) 2017, 2018, Red Hat, Inc. All rights reserved.
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

/*
 * @test id=default
 * @requires vm.gc.Shenandoah
 *
 * @run main/othervm -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC         TestSmallHeap
 * @run main/othervm -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -Xmx64m TestSmallHeap
 * @run main/othervm -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -Xmx32m TestSmallHeap
 * @run main/othervm -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -Xmx16m TestSmallHeap
 * @run main/othervm -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -Xmx8m  TestSmallHeap
 * @run main/othervm -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -Xmx4m  TestSmallHeap
 */

/*
 * @test id=generational
 * @requires vm.gc.Shenandoah
 *
 * @run main/othervm -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -XX:ShenandoahGCMode=generational         TestSmallHeap
 * @run main/othervm -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -XX:ShenandoahGCMode=generational -Xmx64m TestSmallHeap
 * @run main/othervm -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -XX:ShenandoahGCMode=generational -Xmx32m TestSmallHeap
 * @run main/othervm -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -XX:ShenandoahGCMode=generational -Xmx16m TestSmallHeap
 * @run main/othervm -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -XX:ShenandoahGCMode=generational -Xmx8m  TestSmallHeap
 * @run main/othervm -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -XX:ShenandoahGCMode=generational -Xmx4m  TestSmallHeap
 */
public class TestSmallHeap {

    public static void main(String[] args) throws Exception {
        System.out.println("Hello World!");
    }

}

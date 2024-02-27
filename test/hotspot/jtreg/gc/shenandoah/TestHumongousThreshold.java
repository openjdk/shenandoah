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
 * @key randomness
 * @requires vm.gc.Shenandoah
 * @library /test/lib
 *
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -Xmx1g
 *                   -XX:+ShenandoahVerify
 *                   TestHumongousThreshold
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -Xmx1g
 *                   -XX:+ShenandoahVerify -XX:ShenandoahHumongousThreshold=50
 *                   TestHumongousThreshold
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -Xmx1g
 *                   -XX:+ShenandoahVerify -XX:ShenandoahHumongousThreshold=90
 *                   TestHumongousThreshold
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -Xmx1g
 *                   -XX:+ShenandoahVerify -XX:ShenandoahHumongousThreshold=99
 *                   TestHumongousThreshold
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -Xmx1g
 *                   -XX:+ShenandoahVerify -XX:ShenandoahHumongousThreshold=100
 *                   TestHumongousThreshold
 *
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -Xmx1g
 *                   -XX:-UseTLAB -XX:+ShenandoahVerify
 *                   TestHumongousThreshold
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -Xmx1g
 *                   -XX:-UseTLAB -XX:+ShenandoahVerify -XX:ShenandoahHumongousThreshold=50
 *                   TestHumongousThreshold
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -Xmx1g
 *                   -XX:-UseTLAB -XX:+ShenandoahVerify -XX:ShenandoahHumongousThreshold=90
 *                   TestHumongousThreshold
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -Xmx1g
 *                   -XX:-UseTLAB -XX:+ShenandoahVerify -XX:ShenandoahHumongousThreshold=99
 *                   TestHumongousThreshold
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -Xmx1g
 *                   -XX:-UseTLAB -XX:+ShenandoahVerify -XX:ShenandoahHumongousThreshold=100
 *                   TestHumongousThreshold
 *
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -Xmx1g
 *                   -XX:ShenandoahHumongousThreshold=90 -XX:ShenandoahGCHeuristics=aggressive
 *                   TestHumongousThreshold
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -Xmx1g
 *                   -XX:-UseTLAB -XX:ShenandoahHumongousThreshold=90 -XX:ShenandoahGCHeuristics=aggressive
 *                   TestHumongousThreshold
 */

/*
 * @test id=16b
 * @key randomness
 * @requires vm.gc.Shenandoah
 * @requires vm.bits == "64"
 * @library /test/lib
 *
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -Xmx1g
 *                   -XX:ObjectAlignmentInBytes=16 -XX:+ShenandoahVerify
 *                   TestHumongousThreshold
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -Xmx1g
 *                   -XX:ObjectAlignmentInBytes=16 -XX:+ShenandoahVerify -XX:ShenandoahHumongousThreshold=50
 *                   TestHumongousThreshold
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -Xmx1g
 *                   -XX:ObjectAlignmentInBytes=16 -XX:+ShenandoahVerify -XX:ShenandoahHumongousThreshold=90
 *                   TestHumongousThreshold
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -Xmx1g
 *                   -XX:ObjectAlignmentInBytes=16 -XX:+ShenandoahVerify -XX:ShenandoahHumongousThreshold=99
 *                   TestHumongousThreshold
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -Xmx1g
 *                   -XX:ObjectAlignmentInBytes=16 -XX:+ShenandoahVerify -XX:ShenandoahHumongousThreshold=100
 *                   TestHumongousThreshold
 *
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -Xmx1g
 *                   -XX:-UseTLAB -XX:ObjectAlignmentInBytes=16 -XX:+ShenandoahVerify
 *                   TestHumongousThreshold
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -Xmx1g
 *                   -XX:-UseTLAB -XX:ObjectAlignmentInBytes=16 -XX:+ShenandoahVerify -XX:ShenandoahHumongousThreshold=50
 *                   TestHumongousThreshold
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -Xmx1g
 *                   -XX:-UseTLAB -XX:ObjectAlignmentInBytes=16 -XX:+ShenandoahVerify -XX:ShenandoahHumongousThreshold=90
 *                   TestHumongousThreshold
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -Xmx1g
 *                   -XX:-UseTLAB -XX:ObjectAlignmentInBytes=16 -XX:+ShenandoahVerify -XX:ShenandoahHumongousThreshold=99
 *                   TestHumongousThreshold
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -Xmx1g
 *                   -XX:-UseTLAB -XX:ObjectAlignmentInBytes=16 -XX:+ShenandoahVerify -XX:ShenandoahHumongousThreshold=100
 *                   TestHumongousThreshold
 */

/*
 * @test id=generational
 * @key randomness
 * @requires vm.gc.Shenandoah
 * @library /test/lib
 *
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -XX:ShenandoahGCMode=generational -Xmx1g
 *                   -XX:+ShenandoahVerify
 *                   TestHumongousThreshold
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -XX:ShenandoahGCMode=generational -Xmx1g
 *                   -XX:+ShenandoahVerify -XX:ShenandoahHumongousThreshold=50
 *                   TestHumongousThreshold
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -XX:ShenandoahGCMode=generational -Xmx1g
 *                   -XX:+ShenandoahVerify -XX:ShenandoahHumongousThreshold=90
 *                   TestHumongousThreshold
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -XX:ShenandoahGCMode=generational -Xmx1g
 *                   -XX:+ShenandoahVerify -XX:ShenandoahHumongousThreshold=99
 *                   TestHumongousThreshold
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -XX:ShenandoahGCMode=generational -Xmx1g
 *                   -XX:+ShenandoahVerify -XX:ShenandoahHumongousThreshold=100
 *                   TestHumongousThreshold
 *
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -XX:ShenandoahGCMode=generational -Xmx1g
 *                   -XX:-UseTLAB -XX:+ShenandoahVerify
 *                   TestHumongousThreshold
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -XX:ShenandoahGCMode=generational -Xmx1g
 *                   -XX:-UseTLAB -XX:+ShenandoahVerify -XX:ShenandoahHumongousThreshold=50
 *                   TestHumongousThreshold
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -XX:ShenandoahGCMode=generational -Xmx1g
 *                   -XX:-UseTLAB -XX:+ShenandoahVerify -XX:ShenandoahHumongousThreshold=90
 *                   TestHumongousThreshold
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -XX:ShenandoahGCMode=generational -Xmx1g
 *                   -XX:-UseTLAB -XX:+ShenandoahVerify -XX:ShenandoahHumongousThreshold=99
 *                   TestHumongousThreshold
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -XX:ShenandoahGCMode=generational -Xmx1g
 *                   -XX:-UseTLAB -XX:+ShenandoahVerify -XX:ShenandoahHumongousThreshold=100
 *                   TestHumongousThreshold
 */

/*
 * @test id=generational-16b
 * @key randomness
 * @requires vm.gc.Shenandoah
 * @requires vm.bits == "64"
 * @library /test/lib
 *
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -XX:ShenandoahGCMode=generational -Xmx1g
 *                   -XX:ObjectAlignmentInBytes=16 -XX:+ShenandoahVerify
 *                   TestHumongousThreshold
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -XX:ShenandoahGCMode=generational -Xmx1g
 *                   -XX:ObjectAlignmentInBytes=16 -XX:+ShenandoahVerify -XX:ShenandoahHumongousThreshold=50
 *                   TestHumongousThreshold
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -XX:ShenandoahGCMode=generational -Xmx1g
 *                   -XX:ObjectAlignmentInBytes=16 -XX:+ShenandoahVerify -XX:ShenandoahHumongousThreshold=90
 *                   TestHumongousThreshold
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -XX:ShenandoahGCMode=generational -Xmx1g
 *                   -XX:ObjectAlignmentInBytes=16 -XX:+ShenandoahVerify -XX:ShenandoahHumongousThreshold=99
 *                   TestHumongousThreshold
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -XX:ShenandoahGCMode=generational -Xmx1g
 *                   -XX:ObjectAlignmentInBytes=16 -XX:+ShenandoahVerify -XX:ShenandoahHumongousThreshold=100
 *                   TestHumongousThreshold
 *
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -XX:ShenandoahGCMode=generational -Xmx1g
 *                   -XX:-UseTLAB -XX:ObjectAlignmentInBytes=16 -XX:+ShenandoahVerify
 *                   TestHumongousThreshold
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -XX:ShenandoahGCMode=generational -Xmx1g
 *                   -XX:-UseTLAB -XX:ObjectAlignmentInBytes=16 -XX:+ShenandoahVerify -XX:ShenandoahHumongousThreshold=50
 *                   TestHumongousThreshold
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -XX:ShenandoahGCMode=generational -Xmx1g
 *                   -XX:-UseTLAB -XX:ObjectAlignmentInBytes=16 -XX:+ShenandoahVerify -XX:ShenandoahHumongousThreshold=90
 *                   TestHumongousThreshold
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -XX:ShenandoahGCMode=generational -Xmx1g
 *                   -XX:-UseTLAB -XX:ObjectAlignmentInBytes=16 -XX:+ShenandoahVerify -XX:ShenandoahHumongousThreshold=99
 *                   TestHumongousThreshold
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:+UseShenandoahGC -XX:ShenandoahGCMode=generational -Xmx1g
 *                   -XX:-UseTLAB -XX:ObjectAlignmentInBytes=16 -XX:+ShenandoahVerify -XX:ShenandoahHumongousThreshold=100
 *                   TestHumongousThreshold
 */

import java.util.Random;
import jdk.test.lib.Utils;

public class TestHumongousThreshold {

    static final long TARGET_MB = Long.getLong("target", 20_000); // 20 Gb allocation
    static final boolean WeightLargerAllocations = true;

    static volatile Object sink;

    public static int random_int(Random r, int max) {
      if (WeightLargerAllocations) {
        int n = r.nextInt(max * 8);
        if (n >= max * 6) {
          return n / 8;
        } else if (n >= max * 4) {
          return n / 6;
        } else if (n >= max * 2) {
          return n / 4;
        } else if (n >= max) {
          return n/2;
        } else if (n >= max / 2) {
          return n;
        } else {
          return n * 2;
        }
      } else {
        return r.nextInt(max);
      }
    }

    public static void main(String[] args) throws Exception {
        final int min = 0;
        final int max = 384 * 1024;

        // Total number of arrays to be allocated is count = TARGET_NUMBER_OF_BYTES / AVERAGE_ARRAY_SIZE
        long count;
        if (WeightLargerAllocations) {
          // With non-uniform distribution of array sizes, the goal is for allocated arrays to still span the
          // full range of sizes spanned with uniform distribution of sizes, but to allocate more arrays of a
          // larger size, so that we can satisfy the target size more quickly.  If not WeightLargerAllocations,
          // this test frequently violated timetous.
          //
          // Let N represent the range of sizes (max - min).  Randomly generate a number n between 0 and 8N
          //  if (n >= 6N), array_size is n/8         (1/4 of samples will average 0.875N)    (aka 7/8)
          //  else if (n >= 4N), array_size is n/6    (1/4 of samples will average 0.833333N) (aka 5/6)
          //  else if (n >= 2N), array_size is n/4    (1/4 of samples will average 0.75N)     (aka 3/4)
          //  else if (n >= N), array size is n/2     (1/8 of samples will average 0.75N)     (aka 3/4)
          //  else if (n >= .5N, array size is n     (1/16 of samples will average 0.75N)     (aka 3/4)
          //  else, array size is 2n                 (1/16 of samples will average 0.5N)      (aka 1/2)
          // The weighted average array size is 0.7865.                                     (aka 151/192)

          //  The smallest allocated array has min elements
          //  The largest allocated array has max elements
          //  The average number of elements in allocated array is:  min + 0.7865 * (max - min)
          //  Assuming 4 bytes to represent each integer, the bytes consumed by an average array are 4*(min + .7685*(max-min))
          //  With 16 bytes for each array header, the total bytes consumed by an average array are 16 + 4*(min + .7685*(max-min))
          //  1 MB equals 1024 * 1024

          count = TARGET_MB * 1024 * 1025 / (16 + 4 * (min + 151 * (max - min) / 192));
        } else {
          // With uniform distribution of array sizes:
          //
          //  The smallest allocated array has min elements
          //  The largest allocated array has max elements
          //  Assuming uniform distribution of sizes, the average number of elements in allocated array is:  min + (max - min) / 2
          //  Assuming 4 bytes to represent each integer, the bytes consumed by an average array are 4 * (min + (max - min) / 2)
          //  Assume 16 bytes for each array header,
          //     the total bytes consumed by an average array are 16 + 4 * (min + (max - min) / 2)
          //  1 MB equals 1024 * 1024
          
          count = TARGET_MB * 1024 * 1024 / (16 + 4 * (min + (max - min) / 2));
        }

        Random r = Utils.getRandomInstance();
        for (long c = 0; c < count; c++) {
            sink = new int[min + random_int(r, max - min)];
        }
    }

}

/*
 * Copyright (c) 2018, Red Hat, Inc. All rights reserved.
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

/**
 * @test
 * @summary Test OOME in separate thread is recoverable
 * @requires vm.gc.Shenandoah
 * @library /test/lib
 * @run driver TestThreadFailure
 */

import java.util.*;

import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;

public class TestThreadFailure {

    static final int SIZE = 1024;
    static final int COUNT = 16;

    static int thread_count = 0;

    static synchronized final int get_thread_id() {
      return ++thread_count;
    }

    static class NastyThread extends Thread {
        @Override
        public void run() {
            List<Object> root = new ArrayList<Object>();
            int my_thread_id = get_thread_id();
            String msg = "NastyThread " + my_thread_id + " is shutting down";
            try {
              while (true) {
                root.add(new Object[SIZE]);
              }
            } catch (Exception x) {
              // This is probably a java.lang.OutOfMemoryError exception
              System.out.println(msg);
            }
        }
    }

    public static void main(String[] args) throws Exception {
        if (args.length > 0) {
            for (int t = 0; t < COUNT; t++) {
                // If we experience OutOfMemoryError during our attempt to instantiate NastyThread, we'll abort
                // main and will not print "All good".  We'll also report a non-zero termination code.  In the 
                // case that the previously instantiated NastyThread accumulated more than SheanndoahNoProgressThreshold
                // unproductive GC cycles before failing, the main thread may not try a Full GC before it experiences
                // OutOfMemoryError exception.
                Thread thread = new NastyThread();
                thread.start();
                thread.join();
                // Having joined thread, we know the memory consumed by thread is now garbage, and will eventually be
                // collected.  Some or all of that memory may have been promoted, so we may need to perform a Full GC
                // in order to reclaim it quickly.
            }
            System.out.println("All good");
            return;
        }

        {
            OutputAnalyzer analyzer = ProcessTools.executeLimitedTestJava(
                    "-Xmx32m",
                    "-XX:+UnlockExperimentalVMOptions",
                    "-XX:+UseShenandoahGC",
                    "-Xlog:gc*=info",
                    TestThreadFailure.class.getName(),
                    "test");

            analyzer.shouldHaveExitValue(0);
            analyzer.shouldContain("java.lang.OutOfMemoryError");
            analyzer.shouldContain("All good");
        }

        {
            // Generational Shenandoah needs a higher value of ShenandaohNoProgressThreshold than traditional
            // Shenandoah.  This is because Generational Shenandoah may experience many failed young-generation
            // GC's which degenerate and increment the count of no-progress collections in the same time that a
            // global collection occurs in traditional Shenandoah.
            ProcessBuilder pb = ProcessTools.createLimitedTestJavaProcessBuilder(
                    "-Xmx32m",
                    "-XX:+UnlockExperimentalVMOptions", "-XX:ShenandoahNoProgressThreshold=24",
                    "-XX:+UseShenandoahGC", "-XX:ShenandoahGCMode=generational",
                    "-Xlog:gc*=info",
                    TestThreadFailure.class.getName(),
                    "test");

            OutputAnalyzer analyzer = new OutputAnalyzer(pb.start());
            analyzer.shouldHaveExitValue(0);
            analyzer.shouldContain("java.lang.OutOfMemoryError");
            analyzer.shouldContain("All good");
        }
    }
}

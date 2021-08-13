/*
 * Copyright (c) 2017, 2019, Red Hat, Inc. All rights reserved.
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
 * @test TestRegionSamplingLogging
 * @summary Test that logging file is created and populated
 * @requires vm.gc.Shenandoah
 * @library /test/lib
 */

import java.util.*;
import java.nio.file.*;
import java.io.File;

import jdk.test.lib.Asserts;
import jdk.test.lib.process.ProcessTools;
import java.lang.ProcessBuilder;
import jdk.test.lib.process.OutputAnalyzer;

public class TestRegionSamplingLogging {
    static final long TARGET_MB = Long.getLong("target", 1_000); // 2 Gb allocation
    static volatile Object sink;
    static String dirName = "";

    public static long testWith(String file_path,String ... args) throws Exception {
        String[] cmds = null;
        if (file_path == null || file_path.isEmpty()) {
            cmds = Arrays.copyOfRange(args, 1, args.length);
        } else {
            cmds = Arrays.copyOf(args, args.length);
            cmds[0] = "-XX:ShenandoahRegionSamplingFile=\"" + cmds[0] + "\"";
        }
        ProcessBuilder pb = ProcessTools.createJavaProcessBuilder(cmds);

        File dir = pb.directory();
        dirName = dir.getAbsolutePath();

        Process p = pb.start();
        long pid = p.pid();
//         OutputAnalyzer oa = ProcessTools.executeProcess(pb);

        long count = TARGET_MB * 1024 * 1024 / 16;
        for (long c = 0; c < count; c++) {
            sink = new Object();
        }
        return pid;
    }

    public static void checkLogs(String file_path, long id) throws Exception {
        if (file_path == null || file_path.isEmpty()) {
            file_path = String.format("./shenandoahSnapshots_pid%d.log", id);
        }

        File f = new File(file_path);
        Asserts.assertTrue(f.exists() && !f.isDirectory(), "File '" + file_path +  "' was not created.");
        Asserts.assertTrue(isSnapshot(f), "File '" + file_path +  "' is not a valid Shenandoah log file.");
    }

    private static boolean isSnapshot(File f) throws Exception {
        Scanner sc = new Scanner(f);

        while (sc.hasNextLine()) {
            String metadata = sc.nextLine();
            String[] mdArray = metadata.split(" ");
            Asserts.assertTrue(mdArray.length == 4, "Header line has length=" + mdArray.length + ". Expected length=4.");
            for (String md : mdArray) {
                try {
                    long a = Long.parseLong(md);
                }
                catch (NumberFormatException e) {
                    Asserts.fail("Value '" + md + "' in region data line is not of type long.",e);
                }
            }

            Asserts.assertTrue(sc.hasNextLine(), "File is not a valid Shenandoah log file. Missing region data line after header data line.");

            String regionData = sc.nextLine();
            String[] rData = regionData.split(" ");
            for (String r : rData) {
                try {
                    long a = Long.parseLong(r);
                }
                catch (NumberFormatException e) {
                    Asserts.fail("Value '" + r + "' in region data line is not of type long.",e);
                }

            }
        }

        f.delete();
        sc.close();
        return true;
    }

    public static void main(String[] args) throws Exception {


        long id_3 = testWith("./test_shenandoah_logging.log",
                "-XX:+UnlockExperimentalVMOptions",
                "-XX:+UseShenandoahGC",
                "-XX:+ShenandoahRegionSampling",
                "-XX:+UsePerfData",
                "-XX:+ShenandoahLogRegionSampling");
        checkLogs("./test_shenandoah_logging.log", id_3);
    }
}
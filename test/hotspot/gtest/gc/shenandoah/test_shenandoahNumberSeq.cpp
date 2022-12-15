/*
 * Copyright Amazon.com Inc. or its affiliates. All Rights Reserved.
 * Copyright (c) 2022, Oracle and/or its affiliates. All rights reserved.
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
 */

#include "precompiled.hpp"
#include "gc/shenandoah/shenandoahNumberSeq.hpp"
#include <iostream>
#include <string>
#include "unittest.hpp"
#include "utilities/ostream.hpp"

class ShenandoahNumberSeqTest: public ::testing::Test {
  const double err = 0.5;

 protected:
  HdrSeq seq1;
  HdrSeq seq2;
  HdrSeq seq3;

  void print() {
    if (seq1.num() > 0) {
      print(seq1, "seq1");
    }
    if (seq2.num() > 0) {
      print(seq2, "seq2");
    }
    if (seq3.num() > 0) {
      print(seq3, "seq3");
    }
  }

  void print(HdrSeq& seq, const char* msg) {
    std::cout << msg;
    std::cout << " p0 = " << seq.percentile(0);
    std::cout << " p10 = " << seq.percentile(10);
    std::cout << " p20 = " << seq.percentile(20);
    std::cout << " p30 = " << seq.percentile(30);
    std::cout << " p50 = " << seq.percentile(50);
    std::cout << " p80 = " << seq.percentile(80);
    std::cout << " p90 = " << seq.percentile(90);
    std::cout << " p100 = " << seq.percentile(100);
  }
};

class BasicShenandoahNumberSeqTest: public ShenandoahNumberSeqTest {
  BasicShenandoahNumberSeqTest() {
    seq1.add(0);
    seq1.add(1);
    seq1.add(10);
    for (int i = 0; i < 7; i++) {
      seq1.add(100);
    }
    print();
  }
};

TEST_VM_F(BasicShenandoahNumberSeqTest, maximum_test) {
  EXPECT_EQ(seq1.maximum(), 100);
}

TEST_VM_F(BasicShenandoahNumberSeqTest, minimum_test) {
  EXPECT_EQ(0, seq1.percentile(0));
}

TEST_VM_F(BasicShenandoahNumberSeqTest, percentile_test) {
  EXPECT_NEAR(0, seq1.percentile(10), err);
  EXPECT_NEAR(1, seq1.percentile(20), err);
  EXPECT_NEAR(10, seq1.percentile(30), err);
  EXPECT_NEAR(100, seq1.percentile(40), err);
  EXPECT_NEAR(100, seq1.percentile(50), err);
  EXPECT_NEAR(100, seq1.percentile(75), err);
  EXPECT_NEAR(100, seq1.percentile(90), err);
  EXPECT_NEAR(100, seq1.percentile(100), err);
}


class ShenandoahNumberSeqMergeTest: public ShenandoahNumberSeqTest {
  ShenandoahNumberSeqMergeTest() {
    for (int i = 0; i < 80; i++) {
      seq1.add(1);
      seq3.add(1);
    }

    for (int i = 0; i < 20; i++) {
      seq2.add(100);
      seq3.add(100);
    }
    print();
  }
};

TEST_VM_F(ShenandoahNumberSeqMergeTest, maximum_test) {
  seq1.merge(seq2);    // clears seq1, after merging into seq2
  EXPECT_EQ(seq2.maximum(), seq3.maximum());
  EXPECT_EQ(seq2.percentile(), seq3.percentile());
  for (i = 0, i <= 100; i++) {
    EXPECT_NEAR(seq2.percentile(i), seq3.percentile(i), err);
  }
}

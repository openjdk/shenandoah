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

#ifndef LINUX_X86_64_SERVER_SLOWDEBUG_SHENANDOAHCONTROLLER_HPP
#define LINUX_X86_64_SERVER_SLOWDEBUG_SHENANDOAHCONTROLLER_HPP

#include "gc/shared/gcCause.hpp"
#include "gc/shared/concurrentGCThread.hpp"
#include "gc/shenandoah/shenandoahAllocRequest.hpp"

class ShenandoahController: public ConcurrentGCThread {
public:
  ShenandoahController() : ConcurrentGCThread() { }

  virtual void notify_heap_changed() = 0;
  virtual void handle_alloc_failure_evac(size_t words) = 0;
  virtual void handle_alloc_failure(ShenandoahAllocRequest& req, bool block) = 0;
  virtual void pacing_notify_alloc(size_t words) = 0;
  virtual void request_gc(GCCause::Cause cause) = 0;
  virtual void prepare_for_graceful_shutdown() = 0;
  virtual size_t get_gc_id() = 0;
};
#endif //LINUX_X86_64_SERVER_SLOWDEBUG_SHENANDOAHCONTROLLER_HPP

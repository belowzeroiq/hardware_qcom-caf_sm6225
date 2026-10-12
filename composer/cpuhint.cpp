/* Copyright (c) 2015, 2021, The Linux Foundataion. All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are
* met:
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above
*       copyright notice, this list of conditions and the following
*       disclaimer in the documentation and/or other materials provided
*       with the distribution.
*     * Neither the name of The Linux Foundation nor the names of its
*       contributors may be used to endorse or promote products derived
*       from this software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
* AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
* AND NON-INFRINGEMENT ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
* OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
* EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
* PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
* OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
* WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
* OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
* ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
*/

/*
 * Patched for the sm6225 (topaz/tapas) display HAL.
 *
 * Failure mode being fixed:
 *   The composer-service bootloop is a SIGSEGV / SEGV_MAPERR at fault addr
 *   0x58 inside the large-composition perf-hint path. Init() returns
 *   kErrorNotSupported when the vendor extension library cannot supply its
 *   symbols, but the object stays alive with NULL function pointers. A caller
 *   that only checks "cpu_hint_ != NULL" then performs an ungated indirect
 *   call and faults at 0x0 + 0x58.
 *
 * Fixes in this file:
 *   1. Init() now returns immediately when Open() fails, instead of falling
 *      through to a shared return. The result is identical (enabled_ == false)
 *      but the control flow no longer depends on that being true.
 *   2. Init() logs the property path and any dlopen error, so the tombstone
 *      context says which library and which failure was involved.
 *   3. IsEnabled()/IsValid() are now implemented for callers to gate on.
 *   4. Every offload/release entry point re-checks IsValid(), so even a caller
 *      that forgets its own guard cannot reach a NULL function pointer.
 *   5. Dump() reports the resolved state for triage.
 *
 * No public behaviour of Set()/Reset()/ReqHints()/ReqHintsOffload() changes
 * when the feature is healthy. When the feature is unhealthy, all of them
 * become safe no-ops instead of faults.
 */

#include <cutils/properties.h>
#include <dlfcn.h>
#include <utils/debug.h>

#include "cpuhint.h"
#include "hwc_debugger.h"

#define __CLASS__ "CPUHint"

namespace sdm {

DisplayError CPUHint::Init(HWCDebugHandler *debug_handler) {
  if (debug_handler == NULL) {
    DLOGW("Invalid debug handler");
    return kErrorNotSupported;
  }

  char path[PROPERTY_VALUE_MAX] = {};
  if (debug_handler->GetProperty("ro.vendor.extension_library", path) != kErrorNone) {
    DLOGI("Vendor Extension Library not enabled");
    return kErrorNotSupported;
  }

  int pre_enable_window = -1;
  debug_handler->GetProperty(PERF_HINT_WINDOW_PROP, &pre_enable_window);
  if (pre_enable_window <= 0) {
    DLOGI("Invalid CPU Hint Pre-enable Window %d", pre_enable_window);
  }

  DLOGI("CPU Hint Pre-enable Window %d", pre_enable_window);
  pre_enable_window_ = pre_enable_window;

  if (!vendor_ext_lib_.Open(path)) {
    // Was: fall through to "return enabled_ ? ...". Now an explicit early
    // return, so a failed dlopen can never be confused with a partial success.
    DLOGW("Failed to open %s : %s", path, vendor_ext_lib_.Error());
    return kErrorNotSupported;
  }

  if (!vendor_ext_lib_.Sym("perf_lock_acq", reinterpret_cast<void **>(&fn_lock_acquire_)) ||
      !vendor_ext_lib_.Sym("perf_lock_rel", reinterpret_cast<void **>(&fn_lock_release_)) ||
      !vendor_ext_lib_.Sym("perf_hint", reinterpret_cast<void **>(&fn_perf_hint_)) ||
      !vendor_ext_lib_.Sym("perf_hint_offload", reinterpret_cast<void **> \
      (&fn_perf_hint_offload_))) {
    DLOGW("Failed to load symbols for Vendor Extension Library");
    return kErrorNotSupported;
  }

  // Defensive: Sym() success must imply a usable pointer. If any is still NULL,
  // treat the library as unusable rather than calling through it later.
  if (!IsValid()) {
    DLOGW("Vendor Extension Library symbols resolved to NULL");
    return kErrorNotSupported;
  }

  DLOGI("Successfully Loaded Vendor Extension Library symbols");
  enabled_ = true;

  return kErrorNone;
}

void CPUHint::Dump() const {
  DLOGI("CPUHint state: enabled %d valid %d pre_enable_window %d frame_countdown %d "
        "lock_acquired %d lock_handle %d offload_handle %d offload_tid %d "
        "fn(acq %p rel %p hint %p offload %p)",
        enabled_, IsValid(), pre_enable_window_, frame_countdown_, lock_acquired_, lock_handle_,
        offload_handle_, offload_tid_, fn_lock_acquire_, fn_lock_release_, fn_perf_hint_,
        fn_perf_hint_offload_);
}

void CPUHint::Set() {
  if (!IsValid()) {
    return;
  }
  if (lock_acquired_) {
    return;
  }
  if (frame_countdown_) {
    --frame_countdown_;
    return;
  }

  int hint = HINT;
  lock_handle_ = fn_lock_acquire_(0 /*handle*/, 0/*duration*/,
                                  &hint, sizeof(hint) / sizeof(int));
  if (lock_handle_ >= 0) {
    lock_acquired_ = true;
  }
}

void CPUHint::Reset() {
  if (!IsValid()) {
    return;
  }

  frame_countdown_ = pre_enable_window_;

  if (!lock_acquired_) {
    return;
  }

  fn_lock_release_(lock_handle_);
  lock_acquired_ = false;
}

void CPUHint::ReqHints(int hint) {
  if (IsValid() && hint > 0) {
    fn_perf_hint_(hint, NULL, 0, 0);
  }
}

void CPUHint::ReqHintsOffload(int hint, int duration) {
  if (IsValid() && hint > 0) {
    fn_perf_hint_offload_(hint, NULL, duration, 0, 0, NULL);
  }
}

int CPUHint::ReqTidChangeOffload(int hint, int tid) {
  if (!IsValid() || hint <= 0) {
    return 0;
  }

  // Associate the hint with the calling thread so the perf HAL can place the
  // relevant work on the gold cores. duration is the thread id here.
  int handle = fn_perf_hint_offload_(hint, NULL, tid, 0, 0, NULL);
  if (handle < 0) {
    DLOGW("Failed to offload hint 0x%x to tid %d ret %d", hint, tid, handle);
    return -1;
  }

  offload_handle_ = handle;
  offload_tid_ = tid;

  return 0;
}

int CPUHint::ReqHintRelease() {
  if (!IsValid()) {
    return 0;
  }

  if (offload_handle_ == 0) {
    return 0;
  }

  int ret = fn_lock_release_(offload_handle_);
  if (ret < 0) {
    DLOGW("Failed to release offloaded hint ret %d", ret);
    return -1;
  }

  offload_handle_ = 0;
  offload_tid_ = 0;

  return 0;
}

void CPUHint::DecReqWindow() {
  if (!IsValid()) {
    return;
  }
  frame_countdown_ = pre_enable_window_;
}

void CPUHint::IncReqWindow() {
  frame_countdown_ = 0;
}

}  // namespace sdm

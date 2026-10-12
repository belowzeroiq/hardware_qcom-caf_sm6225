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
 * What changed and why:
 *   - CPUHint::Init() can now be queried with IsEnabled()/IsValid(). Callers
 *     MUST gate on this, not on the CPUHint pointer being non-NULL.
 *   - A failed Init() leaves a live object whose function pointers are NULL.
 *     hwc_display_builtin.cpp only nulls its cpu_hint_ pointer on failure in
 *     some revisions; where it does not, an ungated indirect call through
 *     fn_perf_hint_offload_ faults at 0x0 + 0x58 (SIGSEGV / SEGV_MAPERR,
 *     fault addr 0x58), which is exactly the composer-service bootloop.
 *   - Added IsValid() to defensively cover objects that were half-built.
 *   - Added Dump() so the resolved state is visible in a tombstone/logcat.
 *   - The legacy API below (ReqTidChangeOffload/ReqHintRelease/IncReqWindow/
 *     DecReqWindow) is additive only. It exists so a caller that offloads the
 *     perf hint to a specific thread can do so safely. Nothing in here
 *     changes the behaviour of Set/Reset/ReqHints/ReqHintsOffload.
 *
 * Requires the matching patched cpuhint.cpp.
 */

#ifndef __CPUHINT_H__
#define __CPUHINT_H__

#include <core/sdm_types.h>
#include <utils/sys.h>

namespace sdm {

class HWCDebugHandler;

class CPUHint {
 public:
  CPUHint() = default;
  ~CPUHint() = default;

  // Not copyable: owns dlopen'd function pointers and a lock handle.
  CPUHint(const CPUHint &) = delete;
  CPUHint &operator=(const CPUHint &) = delete;

  DisplayError Init(HWCDebugHandler *debug_handler);
  void Set();
  void Reset();
  void ReqHints(int hint);
  void ReqHintsOffload(int hint, int duration);

  // True only when the vendor extension library was opened AND every required
  // symbol resolved. Callers must use this to decide whether the perf hint
  // feature is usable at all.
  bool IsEnabled() const { return enabled_; }

  // True when the object is safe to call into: enabled and every function
  // pointer this class can invoke is non-NULL. Use this before any offload or
  // release call. It is deliberately stricter than IsEnabled().
  bool IsValid() const {
    return enabled_ && fn_lock_acquire_ != NULL && fn_lock_release_ != NULL &&
           fn_perf_hint_ != NULL && fn_perf_hint_offload_ != NULL;
  }

  // Diagnostics: log the object's resolved state. No-op if debug_handler has
  // not been supplied via Init().
  void Dump() const;

  // ---- Additive legacy/offload helpers -------------------------------------
  // Offload a hint against a specific thread id. Returns 0 on success,
  // -1 on failure, and is a no-op returning 0 when the feature is unusable.
  int ReqTidChangeOffload(int hint, int tid);

  // Release a previously acquired hint. Safe to call unconditionally.
  int ReqHintRelease();

  // Defer/re-allow hint acquisition. DecReqWindow() arms frame_countdown_ so
  // Set() waits pre_enable_window_ frames before acquiring; IncReqWindow()
  // disarms it so the next Set() acquires immediately.
  void DecReqWindow();
  void IncReqWindow();
  // -------------------------------------------------------------------------

 private:
  enum { HINT =  0x4501 /* 45-display layer hint, 01-Enable */ };

  bool enabled_ = false;
  // frames to wait before setting this hint
  int pre_enable_window_ = 0;
  int frame_countdown_ = 0;
  int lock_handle_ = 0;
  bool lock_acquired_ = false;
  // Handle/thread of the last offloaded hint, for ReqTidChangeOffload().
  int offload_handle_ = 0;
  int offload_tid_ = 0;
  DynLib vendor_ext_lib_;
  int (*fn_lock_acquire_)(int handle, int duration, int *hints, int num_args) = NULL;
  int (*fn_lock_release_)(int value) = NULL;
  int (*fn_perf_hint_)(int hint, const char *pkg, int duration, int type) = NULL;
  int (*fn_perf_hint_offload_)(int hint, const char *pkg, int duration, int type,
                               int numArgs, int *) = NULL;
};

}  // namespace sdm

#endif  // __CPUHINT_H__

#pragma once

// ============================================================================
// vfp_fpscr.h — the FZ + DN initialisation for MDK's floating-point threads.
//
// AGENTS.md R1 (build-blocking): Flush-to-Zero (FPSCR bit 24) and
// Default-NaN (FPSCR bit 25) MUST be enabled for every floating-point thread
// before application float work.
//
// WHY: decay reaches subnormals. MDK's per-frame geometry build, the
// script VM's arithmetic and the audio mixer's envelope maths all multiply
// and accumulate small values; a decaying signal lands in the subnormal
// range, where VFP11 in full-compliance mode can require support-code
// handling. FZ and DN are the CHOSEN numerical mode: they trade
// subnormal-range precision and NaN payloads for predictable speed and
// defined NaN behaviour. They are deliberate numerical choices, not
// transparent optimisations.
//
// Each thread's state must be established INDIVIDUALLY. Modern libctru's
// initThreadVars() already sets both bits, and its main startup calls that
// initialiser -- but a WORKER thread's state is not the main thread's state,
// and the version and entry path actually linked by this project are what
// matter, not what a release is documented to do. So we set them ourselves,
// defensively, and read them back.
//
// DO NOT toggle FZ/DN within gameplay: they change numerical semantics, and
// toggling them mid-frame would make a frame's arithmetic depend on when it
// ran. DO NOT assume they preserve subnormal results or NaN payloads.
//
// The helpers are small and inline so the read-back costs nothing.
// ============================================================================

#include <stdint.h>

namespace mdk {

// FPSCR bit positions, from the ARM11 Architecture Reference Manual.
static constexpr uint32_t kFpscrFlushToZero = 1u << 24;   // FZ
static constexpr uint32_t kFpscrDefaultNaN = 1u << 25;     // DN

inline uint32_t vfpReadFpscr() {
    uint32_t fpscr;
    __asm__ volatile("vmrs %0, FPSCR" : "=r"(fpscr));
    return fpscr;
}

inline void vfpWriteFpscr(uint32_t fpscr) {
    __asm__ volatile("vmsr FPSCR, %0" : : "r"(fpscr));
}

// Enable FZ and DN and READ THEM BACK, so the caller can prove the bits
// stuck. Returns true when both are set.
//
// The read-back is the point: a helper that writes and hopes is not a
// verification, and R1 asks for a VERIFICATION. On a thread whose entry path
// already set the bits, this is a no-op that costs one vmrs.
inline bool vfpSetFzDn() {
    const uint32_t want = kFpscrFlushToZero | kFpscrDefaultNaN;
    const uint32_t current = vfpReadFpscr();
    if ((current & want) != want) {
        vfpWriteFpscr(current | want);
    }
    return (vfpReadFpscr() & want) == want;
}

// The main thread's entry. Call it BEFORE gfxInitDefault and before any
// application float work. On failure, return 1 from main IMMEDIATELY: never
// goto a cleanup path, because the cleanup path's own arithmetic would then
// run with an unverified numerical mode.
inline bool vfpSetFzDnMain() {
    return vfpSetFzDn();
}

// A worker thread's entry. The FIRST line of the thread function, before
// any float work. A worker that does float maths without this inherits
// whatever the thread's entry path left in FPSCR, which is not evidence of
// anything.
inline bool vfpThreadFzDn() {
    return vfpSetFzDn();
}

} // namespace mdk

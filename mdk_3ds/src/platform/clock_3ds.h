#pragma once

// ============================================================================
// clock_3ds.h -- the frontend's wall clock, in seconds.
// ============================================================================

#include <3ds.h>

namespace mdk {

// The game's frame timer. MDK's simulation runs at a fixed 60 Hz step, and
// the frontend accumulates real elapsed time into it.
//
// The 3DS's clock is read from the system tick counter, which is the
// platform's monotonic timer.
inline double nowSeconds() {
    // svcGetSystemTick() is the ARM11's cycle counter and SYSCLOCK_ARM11 is
    // its clock, so the quotient is seconds. osGetTime() is NOT used: it
    // divides by a tick rate that is only valid once the OS layer has been
    // initialised, and it returned 0 here -- a frame timer stuck at zero
    // would silently freeze the game's fixed-step accumulator.
    return (double)svcGetSystemTick() / (double)SYSCLOCK_ARM11;
}

} // namespace mdk

// ============================================================================
// run_end_3ds.cpp -- see run_end_3ds.h.
// ============================================================================

#include "platform/run_end_3ds.h"

namespace mdk {

namespace {

uint32_t g_frameBudget = 0;   // main thread only: set at boot, read by the frame loops

} // namespace

void runEndSetFrameBudget(uint32_t frames) {
    g_frameBudget = frames;
}

bool runEnded3ds(uint32_t frame, const char** reason) {
    // A harness branch puts its deadline HERE, first (harnessAutoquitDue()
    // with "harness autoquit (MDK_AUTOQUIT)"), and nowhere else: this is the
    // one check every frame loop makes.
    if (g_frameBudget != 0 && frame >= g_frameBudget) {
        *reason = "frame budget reached";
        return true;
    }
    return false;
}

} // namespace mdk

// ============================================================================
// harness_3ds.cpp -- see harness_3ds.h. The whole file is the harness
// build's: without MDK_AUTOQUIT or MDK_HW_PROFILE it compiles to the no-op
// answers, so the release binary carries no trace of the cycle (no deadline,
// no chain-load, no hb:ldr traffic).
// ============================================================================

#include "platform/harness_3ds.h"

// The harness compiles its real answers for either feature: the autoquit
// harness (MDK_AUTOQUIT) and the free-play profile (MDK_HW_PROFILE without
// MDK_AUTOQUIT, added 2026-11-22 to measure the owner's pauses on the owner's
// own route). With neither, the no-op stubs at the bottom of this file answer.

namespace mdk {

bool harnessAutoquitDue() {
    return false;
}

const char* harnessCapturePath(uint32_t) {
    return nullptr;
}

uint64_t harnessProfileTick() {
    return 0;
}

void harnessProfileStepSpans(uint64_t, uint64_t, uint64_t, uint64_t) {}

void harnessProfileSpans(uint64_t, uint32_t, uint64_t, uint64_t, uint64_t, uint64_t) {}

void harnessProfileDrawSub(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                           uint64_t, uint64_t, uint64_t, uint64_t) {}

void harnessProfileFrame(double) {}

void harnessProfileFrontFrame(double) {}

void harnessProfileReport() {}

void harnessArenaFrame(uint32_t, const HarnessArenaFrame&) {}

bool harnessWalkInit() {
    return true;
}

void harnessWalkApply(uint32_t, Input*, float, float, float, float, const char*) {}

void harnessChainloadStub() {}

} // namespace mdk


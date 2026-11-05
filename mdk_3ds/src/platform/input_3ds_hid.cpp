// ============================================================================
// input_3ds_hid.cpp — the libctru HID service, and nothing else. See input_3ds.h.
//
// This is the half of the input that cannot be compiled on a host, so it holds
// the least logic it can: scan, read, hand the frame to the mapping, and report
// the mask back. The mapping — the part that can be wrong in a way a host can
// check — is in input_3ds.cpp and pinned in tests/scripts/input_3ds_tests.cpp.
//
// ONE hidScanInput PER FRAME, HERE ONLY. libctru recomputes its down/up edges on
// every scan, so a second scan in the same frame discards the first one's edges
// (the "double-swap" class of bug in ../forsaken/3DS_PORT_NOTES.md, where a
// double buffer swap made every release undetectable and the player could not
// respawn). A frontend that needs the mask takes it from the return value.
//
// WHAT IS NOT HERE, with its cause: the C-Stick. Its analog read is not in
// hid.h at all — libctru's `hidCstickRead` is a compatibility macro for
// `irrstCstickRead` in irrst.h, so a C-Stick costs an extra service
// (irrstInit/irrstExit) on a path that only exists on a New 3DS, while the
// original 3DS is this port's target. The circle pad carries the look on both
// models, so nothing is missing on either; the C-Stick is a New 3DS comfort,
// deferred with that cause recorded in docs/CONTINUATION.md.
// ============================================================================

#include "platform/input_3ds.h"

#include <3ds.h>

// The mirror is checked, not assumed. If libctru ever moves a bit, the build
// fails here instead of silently rebinding the controls on hardware, where a
// wrong binding is discovered by playing and not by a gate.
static_assert(mdk::kHidA == (uint32_t)KEY_A, "kHidA drifted from libctru's KEY_A");
static_assert(mdk::kHidB == (uint32_t)KEY_B, "kHidB drifted from libctru's KEY_B");
static_assert(mdk::kHidSelect == (uint32_t)KEY_SELECT,
              "kHidSelect drifted from libctru's KEY_SELECT");
static_assert(mdk::kHidStart == (uint32_t)KEY_START,
              "kHidStart drifted from libctru's KEY_START");
static_assert(mdk::kHidDRight == (uint32_t)KEY_DRIGHT,
              "kHidDRight drifted from libctru's KEY_DRIGHT");
static_assert(mdk::kHidDLeft == (uint32_t)KEY_DLEFT, "kHidDLeft drifted from libctru's KEY_DLEFT");
static_assert(mdk::kHidDUp == (uint32_t)KEY_DUP, "kHidDUp drifted from libctru's KEY_DUP");
static_assert(mdk::kHidDDown == (uint32_t)KEY_DDOWN,
              "kHidDDown drifted from libctru's KEY_DDOWN");
static_assert(mdk::kHidR == (uint32_t)KEY_R, "kHidR drifted from libctru's KEY_R");
static_assert(mdk::kHidL == (uint32_t)KEY_L, "kHidL drifted from libctru's KEY_L");
static_assert(mdk::kHidX == (uint32_t)KEY_X, "kHidX drifted from libctru's KEY_X");
static_assert(mdk::kHidY == (uint32_t)KEY_Y, "kHidY drifted from libctru's KEY_Y");
static_assert(mdk::kHidZL == (uint32_t)KEY_ZL, "kHidZL drifted from libctru's KEY_ZL");
static_assert(mdk::kHidZR == (uint32_t)KEY_ZR, "kHidZR drifted from libctru's KEY_ZR");

namespace mdk {

uint32_t input3dsPoll(Input* input, float seconds) {
    hidScanInput();

    // The order matters: the reads return what the scan latched, so the pad is
    // read after the scan and the mask from the same scan, never from a later one.
    circlePosition pad;
    hidCircleRead(&pad);

    HidFrame frame;
    frame.held = hidKeysHeld();
    frame.padX = pad.dx;
    frame.padY = pad.dy;

    input3dsApply(frame, seconds, input);
    return frame.held;
}

} // namespace mdk

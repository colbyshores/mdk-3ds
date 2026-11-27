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
// THE C-STICK (New 3DS only). Its read is irrst.h's `irrstCstickRead` (libctru's
// `hidCstickRead` is a macro for it), served by IRRST, not HID. No irrstInit of
// ours: libctru 2.7.0's hidInit -- run by its default startup -- calls
// irrstInit itself when its weak hidShouldUseIrrst() says so (by default
// APT_CheckNew3DS), and hidScanInput calls irrstScanInput and ors in
// irrstKeysHeld, which is also where ZL and ZR come from. Checked against the
// linked libctru.a on 2026-11-27 (objdump of hid.o: hidInit tail-calls
// irrstInit, hidScanInput calls irrstScanInput), so ONE hidScanInput still
// latches both services. On an Old 3DS the stick is never read.
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

uint32_t input3dsPoll(Input* input, float seconds, const ControlBindings& bindings,
                      bool hasCStick, bool swapSticks) {
    hidScanInput();

    // The order matters: the reads return what the scan latched, so the pad is
    // read after the scan and the mask from the same scan, never from a later one.
    circlePosition pad;
    hidCircleRead(&pad);

    HidFrame frame;
    frame.held = hidKeysHeld();
    frame.padX = pad.dx;
    frame.padY = pad.dy;
    frame.hasCStick = hasCStick;
    frame.swapSticks = swapSticks;
    if (hasCStick) {
        circlePosition stick;
        hidCstickRead(&stick);
        frame.stickX = stick.dx;
        frame.stickY = stick.dy;
    }

    input3dsApply(frame, seconds, bindings, input);
    return frame.held;
}

} // namespace mdk

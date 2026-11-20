#pragma once

#include <stdint.h>

#include "game/input.h"
#include "platform/controls_3ds.h"

// ============================================================================
// input_3ds.h — libctru's HID, translated into the game's `Input`.
//
// WHY THIS FILE IS SPLIT IN TWO. `input_3ds.cpp` below is the MAPPING: raw
// controls in, `Input` out, and it names no libctru type, so a host test can
// pin it (`tests/scripts/input_3ds_tests.cpp`). `input_3ds_hid.cpp` is the
// SERVICE: `hidScanInput()`, the reads, and the mirror of libctru's `KEY_*`
// values, which cannot compile without <3ds.h> and therefore cannot be tested
// here. This is the same shape `render/pica_matrix.{h,cpp}` uses for the same
// reason: the part that can be pinned is separated from the part that needs the
// console.
//
// THE CONTROL SCHEME (the owner's defaults of 2026-11-19, in
// sdmc:/3ds/mdk/controls.ini; controls_3ds.h has the table and the file).
//
//   Circle Pad   look (yaw and pitch), as relative mouse counts
//   X / B        forward / back
//   Y / A        strafe left / strafe right
//   L            jump
//   R, ZR        fire
//   D-pad L / R  previous item / next item
//   D-pad up     sniper (bound; nothing reads it -- the sniper view is cut)
//   D-pad down   use item
//   ZL           turbo (New 3DS only; unassigned by the owner)
//   Start        the frontend's PAUSE menu (main_3ds.cpp, game/enhanced_menu.h:
//                Resume, the Video/Graphics settings of SPEC.md 7.9, Quit).
//                QUIT is a menu row confirmed with A -> flow::Event::Quit;
//                one press of Start no longer ends the run (owner,
//                2026-11-19). The mask still sets kKeyEscape, which nothing
//                on this target reads.
//   Select       opens the same menu; only when the menu's fonts did not
//                load (no menu) is Start+Select held together the way out
//
// The C# `GamepadMap` is the oracle for a controller, and it puts WALK on the
// left stick and LOOK on the right one. The original 3DS has ONE analog stick,
// so one of the two has to go digital, and the one that survives analog is the
// look: MDK aims by turning Kurt's view, the walk is four binary axes
// (`Input::axis` returns -1/0/+1 from two `down[]` bits), and a digital look
// is unplayable where a digital walk is what the keyboard already does. The
// walk was first on the D-pad (`../forsaken/input_3ds.c`'s call); the owner
// moved it to the face buttons (X/B/Y/A) and gave the D-pad the C# pad's
// items and sniper, which is what that pad's d-pad does.
//
// THE ORACLE for what is here. The 3DS has no C# counterpart — the C# reads SDL
// — so the NUMBERS cannot be measured against it. What is taken from the C#
// unchanged is the look model: `GamepadMap`'s dead zone, curve and degrees per
// second, and `Input`'s degrees-per-mouse-count, so a stick on the 3DS turns
// the view at the same rate, with the same curve, as a stick on the PC. The
// circle pad's reach is the number `../forsaken/input_3ds.c` measured on this
// hardware. Everything else is this target's, and the tests say so row by row.
//
// THE TRAP THIS SHAPE AVOIDS (the reason `input3dsPoll` exists at all). libctru
// latches its down/up edges inside `hidScanInput()`: a SECOND scan in the same
// frame erases the edges the first one produced, so a frontend that polls HID
// in two places loses every press-detection that the second site did not do.
// `../forsaken/3DS_PORT_NOTES.md` "Input Double-Swap" is the same class of bug
// one layer down (a double buffer swap there broke release detection and the
// player could never respawn). So: exactly one `input3dsPoll` per frame, and the
// frame's held mask is what it RETURNS, so a frontend never needs a second
// scan. And the poll does not consume the mouse: the game takes it
// (`Player::mouseLook`, `FollowCamera::mouseLook`), exactly as the C# does.
// ============================================================================

namespace mdk {

// libctru's `KEY_*` bit values, restated so the mapping compiles without
// <3ds.h>. They are NOT assumed: `input_3ds_hid.cpp` static_asserts every one of
// them against the real header, so a libctru that moves a bit fails the build
// instead of silently rebinding the controls. Only the bits this mapping reads
// are mirrored.
enum HidKey : uint32_t {
    kHidA = 1u << 0,
    kHidB = 1u << 1,
    kHidSelect = 1u << 2,
    kHidStart = 1u << 3,
    kHidDRight = 1u << 4,
    kHidDLeft = 1u << 5,
    kHidDUp = 1u << 6,
    kHidDDown = 1u << 7,
    kHidR = 1u << 8,
    kHidL = 1u << 9,
    kHidX = 1u << 10,
    kHidY = 1u << 11,
    kHidZL = 1u << 14,
    kHidZR = 1u << 15,
};

// One frame's raw controls: libctru's held mask and the circle pad, in
// libctru's own units (dx right, dy UP, both roughly -155..+155).
struct HidFrame {
    uint32_t held = 0;
    int32_t padX = 0;
    int32_t padY = 0;
};

// The circle pad's reach: the magnitude libctru reports at full tilt. Measured
// on this hardware by ../forsaken/input_3ds.c ("circle pad raw range is
// roughly -155..+155"), which is also what libctru's own CPAD direction bits
// threshold against.
constexpr float kPadReach = 155.0f;

// The look, in the C#'s numbers. GamepadMap: DeadZone 0.15 of the reach,
// Curve 2, LookRate (180, 120) degrees a second at full tilt; Input's
// LookDegrees is 0.15 degrees per mouse count, which is the same 0.15 the
// player and the follow camera multiply a count by (kMouseDegrees).
constexpr float kLookDeadZone = 0.15f;
constexpr float kLookRateX = 180.0f;
constexpr float kLookRateY = 120.0f;
constexpr float kLookDegreesPerCount = 0.15f;
// Gamepads.MaxStep: a long gap between frames (a pause, a load, a stall) must
// not buy an enormous turn.
constexpr float kMaxLookStep = 0.1f;

// Fill `input` from one frame's controls through `bindings`: every `down[]`
// action is written (the whole digital state comes from this one frame), and the
// circle pad ADDS to the accumulated mouse, which the game drains with
// takeMouseX/TakeMouseY. `seconds` is the time since the previous frame; it is
// clamped to kMaxLookStep and a zero, negative or NaN gap looks nowhere.
void input3dsApply(const HidFrame& frame, float seconds, const ControlBindings& bindings,
                   Input* input);

// The libctru service: ONE hidScanInput, the reads, and input3dsApply. Returns
// the frame's held mask so the caller can act on it without scanning HID a
// second time (see the trap above). `input` must be the frame's own — the mouse
// it accumulates belongs to this frame.
uint32_t input3dsPoll(Input* input, float seconds, const ControlBindings& bindings);

} // namespace mdk

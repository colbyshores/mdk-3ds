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
// THE CONTROL SCHEME (the owner's defaults of 2026-11-19, the run controls of
// 2026-11-27, in sdmc:/3ds/mdk/controls.ini; controls_3ds.h has the table and
// the file).
//
//                Old 3DS                       New 3DS (adds)
//   Circle Pad   look (yaw and pitch)          walk, when SWAP STICKS is on
//   C-Stick      --                            walk (up/down forward/back,
//                                              left/right strafe); look, when
//                                              SWAP STICKS is on
//   X / B        forward / back (the scope: zoom in / zoom out)
//   Y / A        strafe left / strafe right
//   L            jump
//   R            fire
//   D-pad L / R  previous item / next item
//   D-pad up     sniper (the scope's view and overlay: game/sniper_view.h)
//   D-pad down   use item
//   Select       RUN TOGGLE: each press flips Kurt's run (the game's turbo)
//   ZL           --                            RUN TOGGLE, as Select
//   ZR           --                            RUN BUTTON: held, it inverts the
//                                              toggle (toggle off: run; toggle
//                                              on: jog); released, the toggle
//   Start        the frontend's PAUSE menu (main_3ds.cpp, game/enhanced_menu.h:
//                Resume, the Video/Graphics settings of SPEC.md 7.9, Quit).
//                QUIT is a menu row confirmed with A -> flow::Event::Quit;
//                one press of Start never ends the run (owner, 2026-11-19).
//                Only when the menu's fonts did not load (no menu) is START
//                HELD FOR TWO SECONDS the way out.
//
// WHY SELECT LEFT THE MENU (owner, 2026-11-27): "the Original 3DS does not have
// enough buttons" -- it has no ZL, so Select is the Old 3DS's run toggle, and
// Start alone opens the menu. The run is a TOGGLE rather than a held button
// because Kurt runs most of the time and the thumbs are on the stick and the
// face buttons (input3dsRun below).
//
// THE C-STICK (New 3DS only; the one per-model difference, because the hardware
// differs). libctru 2.7.0's hidInit brings up IRRST itself on a New 3DS (its
// weak hidShouldUseIrrst() is APT_CheckNew3DS) and hidScanInput scans it, so
// hidCstickRead needs no irrstInit of ours -- checked against the linked
// libctru.a (hid.o calls irrstInit/irrstScanInput), not assumed; the vault's
// "3ds-cstick-requires-irrstinit" note predates that libctru. The walk it drives
// is DIGITAL (input3dsStickWalk): the game's walk is four binary axes.
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
// libctru's own units (dx right, dy UP, both roughly -155..+155); on a New 3DS
// the C-Stick too, in the same units. And the frame's STICK LAYOUT: which
// stick walks and which looks.
struct HidFrame {
    uint32_t held = 0;
    int32_t padX = 0;
    int32_t padY = 0;
    // The C-Stick (hidCstickRead): only when `hasCStick`, i.e. a New 3DS.
    bool hasCStick = false;
    int32_t stickX = 0;
    int32_t stickY = 0;
    // SWAP STICKS (config.ini swapSticks, the pause menu's row): the circle pad
    // walks and the C-Stick looks. IGNORED without a C-Stick, so an Old 3DS can
    // never lose its only look to a stale setting.
    bool swapSticks = false;
};

// The circle pad's reach: the magnitude libctru reports at full tilt. Measured
// on this hardware by ../forsaken/input_3ds.c ("circle pad raw range is
// roughly -155..+155"), which is also what libctru's own CPAD direction bits
// threshold against. The C-Stick is normalised by the same reach: its own full
// tilt is NOT measured here (no New 3DS run has logged it), so a C-Stick look
// at full tilt is at most the pad's rate, and the walk below is a direction
// test that does not depend on the reach past its dead zone.
constexpr float kPadReach = 155.0f;

// THE STICK WALK's dead zone, as a fraction of the reach: a stick inside it
// walks nowhere (the C-Stick does not rest at exactly zero). Past it the stick's
// DIRECTION picks one of eight sectors, 45 degrees each, centred on the four
// axes and the four diagonals -- an axis is held when the stick is within 67.5
// degrees of it, i.e. its component is at least sin(22.5 deg) of the length.
constexpr float kWalkDeadZone = 0.25f;
constexpr float kWalkSector = 0.38268343f;   // sin(22.5 degrees)

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
// action is written (the whole digital state comes from this one frame), the
// WALK stick (the C-Stick, or the circle pad when swapped) adds its digital walk
// to the walk buttons', and the LOOK stick (the circle pad, or the C-Stick when
// swapped) ADDS to the accumulated mouse, which the game drains with
// takeMouseX/TakeMouseY. `seconds` is the time since the previous frame; it is
// clamped to kMaxLookStep and a zero, negative or NaN gap looks nowhere.
// The run (turbo) here is only turbo's own buttons; input3dsRun adds the toggle.
void input3dsApply(const HidFrame& frame, float seconds, const ControlBindings& bindings,
                   Input* input);

// One stick's digital walk (x right, y UP, libctru's units): sets forward /
// back / strafe left / strafe right in `input` when the stick is past
// kWalkDeadZone in their sector, and clears none (the buttons' walk stays).
void input3dsStickWalk(int32_t x, int32_t y, Input* input);

// THE RUN (owner, 2026-11-27). `on` is the toggle; `lastHeld` the previous
// frame's mask, for the toggle's press edges.
struct RunState {
    bool on = false;
    uint32_t lastHeld = 0;
};

// The run, after input3dsApply, with the same held mask: a PRESS of any
// run_toggle button (ZL, SELECT) flips `run->on` -- only while `playing`, so the
// menus and the strike's skip never flip it -- and kKeyTurbo is then held when
// turbo's own buttons are, or when the toggle and the run_hold button (ZR)
// disagree: toggle off + ZR held runs, toggle on + ZR held jogs, ZR released is
// the toggle. The edges are tracked every frame, playing or not, so a button
// held across the menu's close is not a new press.
void input3dsRun(uint32_t held, const ControlBindings& bindings, bool playing, RunState* run,
                 Input* input);

// THE SCOPE'S BUTTONS (owner 2026-11-21). In sniper mode the C# reads forward/back
// as the scope's tilt (Kurt.SniperControls); on the 3DS the circle pad already
// tilts it, and the zoom keys default to forward's and back's buttons (X in, B
// out). So while the scope is up, forward and back read only their buttons that
// are NOT also a zoom key's: X zooms and does not tilt as well. A binding with no
// overlap (controls.ini) is untouched. The caller runs it after input3dsApply
// with the same held mask, only on frames Kurt is sniping.
// Forward and back are rebuilt from the BUTTONS alone, so a walk stick's
// forward/back (input3dsStickWalk) does nothing in the scope, as the circle pad
// already moves it; the stick's strafe is left as it was.
void input3dsScopeButtons(uint32_t held, const ControlBindings& bindings, Input* input);

// The libctru service: ONE hidScanInput, the reads, and input3dsApply. Returns
// the frame's held mask so the caller can act on it without scanning HID a
// second time (see the trap above). `input` must be the frame's own — the mouse
// it accumulates belongs to this frame. `hasCStick` is APT_CheckNew3DS's answer
// (the C-Stick is read only then); `swapSticks` the setting, ignored without it.
uint32_t input3dsPoll(Input* input, float seconds, const ControlBindings& bindings,
                      bool hasCStick, bool swapSticks);

} // namespace mdk

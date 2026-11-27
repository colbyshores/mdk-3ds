// ============================================================================
// input_3ds.cpp — the HID -> Input mapping, without libctru. See input_3ds.h.
//
// This file is compiled by the 3DS build AND by tests/scripts, which is the only
// way any of it can be checked at all: the console cannot be run here, and the
// C# has no 3DS device to measure against. The arithmetic therefore follows the
// C#'s own expression, operation for operation (GamepadMap.Look and
// Input.AddLook), so that the one claim this file can make — a stick turns the
// view as it does on the PC — is the one thing that is actually true.
//
// AGENTS.md: this is not a hot path (§2: once per frame, no profile, never 256
// times a frame), and it uses no libm transcendental: the only libm call is
// sqrtf, which VFPv2 does in hardware (R7). No float->int conversion, no float
// comparison in a loop condition, no runtime integer divide (R2, R3, T2, R11).
// ============================================================================

#include "platform/input_3ds.h"

#include <math.h>

#include "platform/controls_3ds.h"

namespace mdk {

void input3dsScopeButtons(uint32_t held, const ControlBindings& bindings, Input* input) {
    if (input == nullptr) {
        return;
    }
    const uint32_t zoom = bindings.buttons[kKeyZoomIn] | bindings.buttons[kKeyZoomOut];
    input->down[kKeyForward] = (held & bindings.buttons[kKeyForward] & ~zoom) != 0;
    input->down[kKeyBack] = (held & bindings.buttons[kKeyBack] & ~zoom) != 0;
}

namespace {

// One stick's look: the circle pad's arithmetic, unchanged, for whichever stick
// looks this frame. `x` right and `y` UP, libctru's units.
void addLook(int32_t x, int32_t y, float seconds, Input* input) {
    // Gamepads.Update clamps its frame gap the same way, and a NaN has to land
    // here too: a NaN step would poison the accumulated mouse for every frame
    // after it, and the player would spin with no way back.
    float step = seconds;
    if (!(step > 0.0f)) {
        step = 0.0f;
    }
    if (step > kMaxLookStep) {
        step = kMaxLookStep;
    }

    // The stick, normalised to its reach. libctru's dy is UP; the look's y is
    // down (the C#'s stick is SDL's, whose y is down, and FollowCamera adds
    // MouseY to a pitch that grows downward), so dy is negated once, here.
    const float nx = (float)x / kPadReach;
    const float ny = -(float)y / kPadReach;
    const float length = sqrtf(nx * nx + ny * ny);
    // GamepadMap: `if (tilt < DeadZone) return Vector2.Zero`. Written as a
    // negated >= so a NaN length takes the same early return instead of
    // reaching the arithmetic below.
    if (!(length >= kLookDeadZone)) {
        return;
    }
    // Past the dead zone, from 0 to 1 along the curve. The C# raises the ramp to
    // GamepadMap.Curve, which is 2; squaring it is that curve, and this does not
    // claim to be bit-identical to MathF.Pow(x, 2f) — there is no C# 3DS row to
    // be identical TO, and the curve is a feel, not a measurement.
    const float tilt = length > 1.0f ? 1.0f : length;
    const float ramp = (tilt - kLookDeadZone) / (1.0f - kLookDeadZone);
    const float strength = ramp * ramp;
    // Normalize(stick) * LookRate * seconds, in the C#'s order, and divided by
    // the degrees per count as Input.AddLook does. `length` is the UNCLAMPED
    // length, as Vector2.Normalize uses: at full tilt on a diagonal the
    // direction is what must be unit, not the clamped magnitude.
    const float dirX = nx / length;
    const float dirY = ny / length;
    // A float divide (R7, not R11): the constant lives in input_3ds.h, out of
    // the R11 screen's sight, so it is named here as the float it is.
    const float degreesPerCount = kLookDegreesPerCount;
    input->mouseX += ((dirX * strength) * kLookRateX * step) / degreesPerCount;
    input->mouseY += ((dirY * strength) * kLookRateY * step) / degreesPerCount;
}

} // namespace

void input3dsStickWalk(int32_t x, int32_t y, Input* input) {
    if (input == nullptr) {
        return;
    }
    // Normalised, y UP (forward is up on either stick). Inside the dead zone,
    // or a NaN, walks nowhere.
    const float nx = (float)x / kPadReach;
    const float ny = (float)y / kPadReach;
    const float length = sqrtf(nx * nx + ny * ny);
    if (!(length >= kWalkDeadZone)) {
        return;
    }
    // Eight sectors: an axis is held when the stick's component along it is at
    // least sin(22.5 deg) of the length, so a stick pushed straight up holds
    // forward alone, and one pushed between up and right holds both. Ored into
    // the buttons' walk: a stick never releases a held button.
    const float reach = kWalkSector * length;
    if (ny >= reach) {
        input->down[kKeyForward] = true;
    }
    if (-ny >= reach) {
        input->down[kKeyBack] = true;
    }
    if (-nx >= reach) {
        input->down[kKeyStrafeLeft] = true;
    }
    if (nx >= reach) {
        input->down[kKeyStrafeRight] = true;
    }
}

void input3dsRun(uint32_t held, const ControlBindings& bindings, bool playing, RunState* run,
                 Input* input) {
    if (run == nullptr || input == nullptr) {
        return;
    }
    const uint32_t toggle = bindings.buttons[kControlRunToggle];
    const uint32_t pressed = held & ~run->lastHeld;
    run->lastHeld = held;
    if (playing && (pressed & toggle) != 0) {
        run->on = !run->on;
    }
    const bool holding = (held & bindings.buttons[kControlRunHold]) != 0;
    // Turbo's own buttons (none by default) still hold it outright.
    input->down[kKeyTurbo] = input->down[kKeyTurbo] || (run->on != holding);
}

void input3dsApply(const HidFrame& frame, float seconds, const ControlBindings& bindings,
                   Input* input) {
    if (input == nullptr) {
        return;
    }

    // ---- the digital controls ------------------------------------------------
    // THE BINDINGS (controls_3ds.h: the owner's defaults, or controls.ini): an
    // action is held when any of its buttons is. Every action is written: the
    // frame's mask IS the state, so a key released since the last frame has to
    // go false here or the player walks forever. The turn axes and Escape have
    // no buttons on this target (the look stick turns, below; START is the
    // frontend's pause menu), so their masks are 0 and they read false -- which
    // also means a caller that reused an Input cannot keep a stale turn.
    const uint32_t held = frame.held;
    for (int key = 0; key < kKeyCount; key++) {
        input->down[key] = (held & bindings.buttons[key]) != 0;
    }

    // ---- the sticks: which walks and which looks ------------------------------
    // The Old 3DS has the circle pad alone, and it looks. The New 3DS's C-Stick
    // walks; swapped, the circle pad walks and the C-Stick looks. The swap needs
    // a C-Stick: without one the setting is ignored.
    const bool swapped = frame.hasCStick && frame.swapSticks;
    if (frame.hasCStick) {
        if (swapped) {
            input3dsStickWalk(frame.padX, frame.padY, input);
        } else {
            input3dsStickWalk(frame.stickX, frame.stickY, input);
        }
    }
    if (swapped) {
        addLook(frame.stickX, frame.stickY, seconds, input);
    } else {
        addLook(frame.padX, frame.padY, seconds, input);
    }
}

} // namespace mdk

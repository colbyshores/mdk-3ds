// ============================================================================
// input_3ds_tests.cpp — see input_3ds_tests.h.
//
// WHAT IS PINNED HERE, and where each number came from:
//
//   1. THE DIGITAL MAP. One row per button. TRANSLITERATED from the C#
//      `GamepadMap.Game` for the buttons a 3DS has an equivalent of, and from
//      the scheme documented in `input_3ds.h` for the two places the hardware
//      forces a difference (the D-pad walks, because there is no left stick;
//      ZL joins L, because the sniper the C# trigger binds is not in this port).
//   2. THE LOOK'S EXACT BITS. Hand-derived float arithmetic, one operation at a
//      time, with the rounding written out for the rows a reader is asked to
//      believe. Each pinned pattern was then produced a second time by an
//      exact-rational model of the same operation chain (see the progress log
//      for the derivation script) — two independent roundings agreeing is what
//      makes a pinned bit pattern evidence rather than a transcription of the
//      implementation.
//   3. THE DEAD ZONE, THE FRAME-GAP CLAMP, THE SIGN of each axis, symmetry, and
//      monotonicity. PROPERTIES of the C# formula, which do not depend on any
//      particular rounding.
//   4. THE MOUSE IS THE GAME'S to drain, not the platform's.
//
// The constants are the C#'s: GamepadMap.DeadZone 0.15, Curve 2, LookRate
// (180, 120) deg/s, Input.LookDegrees 0.15 deg per count, Gamepads.MaxStep 0.1 s.
// ============================================================================

#include "input_3ds_tests.h"

#include <stdio.h>
#include <string.h>

#include <limits>

#include "platform/input_3ds.h"

using namespace mdk;

namespace {

int failures = 0;

void expect(const char* what, long long got, long long want) {
    if (got != want) {
        printf("  FAIL: %s was %lld, expected %lld\n", what, got, want);
        failures++;
    }
}

unsigned bitsOf(float value) {
    unsigned bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

void expectBits(const char* what, float got, unsigned want) {
    if (bitsOf(got) != want) {
        printf("  FAIL: %s was %08X, the derivation has %08X\n", what, bitsOf(got), want);
        failures++;
    }
}

// A frame-gap of one 60 Hz step, which is the step the level runs on
// (flow::kViewerStep) and the number the 3DS frontend's self-test quotes.
const float kStep = 1.0f / 60.0f;

Input applied(const HidFrame& frame, float seconds) {
    Input input;
    input3dsApply(frame, seconds, &input);
    return input;
}

HidFrame stick(int32_t x, int32_t y) {
    HidFrame frame;
    frame.padX = x;
    frame.padY = y;
    return frame;
}

HidFrame buttons(uint32_t held) {
    HidFrame frame;
    frame.held = held;
    return frame;
}

// ---------------------------------------------------------------------------
// 1. The digital map. For each button, exactly the action it binds and nothing
//    else is down.
// ---------------------------------------------------------------------------
void testDigitalMap() {
    struct Row {
        uint32_t held;
        int key; // -1: no action at all
        const char* what;
    };
    const Row rows[] = {
        {kHidDUp, kKeyForward, "D-pad up walks forward"},
        {kHidDDown, kKeyBack, "D-pad down walks back"},
        {kHidDLeft, kKeyStrafeLeft, "D-pad left strafes left"},
        {kHidDRight, kKeyStrafeRight, "D-pad right strafes right"},
        {kHidA, kKeyJump, "A jumps (the C# pad's South)"},
        {kHidB, kKeyTurbo, "B is turbo (the C# pad's East)"},
        {kHidX, kKeyUseItem, "X uses the item (the C# pad's West)"},
        {kHidY, kKeyItemNext, "Y is the next item (the C# pad's North)"},
        {kHidR, kKeyFire, "R fires (the C# pad's right trigger)"},
        {kHidZR, kKeyFire, "ZR fires too"},
        {kHidL, kKeyItemPrevious, "L is the previous item (the C# pad's LB)"},
        {kHidZL, kKeyItemPrevious, "ZL takes the shoulder's action; the sniper is not ported"},
        {kHidStart, kKeyEscape, "Start is escape (the C# pad's Start)"},
        {kHidSelect, -1, "Select binds nothing yet"},
    };

    for (const Row& row : rows) {
        const Input input = applied(buttons(row.held), kStep);
        for (int key = 0; key < kKeyCount; key++) {
            const bool want = (key == row.key);
            if (input.down[key] != want) {
                printf("  FAIL: %s -- action %d was %d, expected %d\n", row.what, key,
                       (int)input.down[key], (int)want);
                failures++;
            }
        }
    }

    // The turn axes are the keyboard's on this target: the view turns with the
    // circle pad, so no button may hold them.
    const Input all = applied(buttons(kHidDUp | kHidDDown | kHidDLeft | kHidDRight | kHidA |
                                      kHidB | kHidX | kHidY | kHidR | kHidL | kHidZL | kHidZR |
                                      kHidStart | kHidSelect),
                              kStep);
    expect("every button at once still leaves the turn axes alone", (int)all.down[kKeyTurnLeft], 0);
    expect("and the right turn axis", (int)all.down[kKeyTurnRight], 0);
    expect("forward and back can both be held, as on a keyboard",
           (int)(all.down[kKeyForward] && all.down[kKeyBack]), 1);
    expect("and the axis is then zero, as the C#'s Kurt.Axis", (int)(all.axis(kKeyForward, kKeyBack) == 0.0f), 1);

    // The whole digital state comes from THIS frame: a release has to clear, or
    // the player walks forever. This is the row the double-swap bug in
    // ../forsaken/3DS_PORT_NOTES.md would have passed.
    const Input released = applied(buttons(0), kStep);
    int anyDown = 0;
    for (int key = 0; key < kKeyCount; key++) {
        anyDown += released.down[key] ? 1 : 0;
    }
    expect("an empty mask clears every action", anyDown, 0);
}

// ---------------------------------------------------------------------------
// 2. The look's exact bits.
//
// The chain, in the C#'s order (GamepadMap.Look then Input.AddLook):
//   n = pad / reach ; length = sqrt(nx^2 + ny^2) ; tilt = min(length, 1)
//   ramp = (tilt - 0.15f) / (1.0f - 0.15f) ; strength = ramp * ramp
//   dir = n / length ; degrees = ((dir * strength) * rate) * step
//   counts = degrees / 0.15f
//
// DERIVATION of the 20.0 row, by hand, so the reader can check it:
//   full tilt right: nx = 155/155 = 1, length = 1, tilt = 1.
//   1.0f - 0.15f: 0.15f is 0.1500000059604644775390625, so the difference is
//   0.8499999940395355224609375, exactly halfway between the two nearest floats
//   -- ties to even picks 0x3F59999A = 0.85000002384185791015625, which is also
//   what 0.85f rounds to, so ramp is 1.0 EXACTLY and so is strength.
//   degrees = 180.0f * (1/60f): 1/60f = 0.01666666753590106964111328125, the
//   exact product is 3.0000001564621925354003906250, which is 0.65625 ulp above
//   3 (ulp(3) = 2^-22), so it rounds UP to 3.0000002384185791015625.
//   counts = 3.0000002384185791015625 / 0.1500000059604644775390625 =
//   20.000000794728564, which is 0.4167 ulp above 20 (ulp(20) = 2^-19), so it
//   rounds back to EXACTLY 20.0 -- the number the 3DS frontend logs.
//
// DERIVATION of the 119.99999237060547 row (the clamp boundary, step = 0.1f):
//   180.0f * 0.1f: 0.1f = 0.100000001490116119384765625, the product is
//   18.0000002682209014892578125 = 0.1406 ulp above 18 -> exactly 18.0.
//   18.0 / 0.15f = 119.99999523162842 = 0.625 ulp BELOW 120 (ulp(120) = 2^-17),
//   so it rounds down one ulp to 119.99999237060546875 = 0x42EFFFFF.
//
// The Y rows are the same chain at 120 deg/s: 120.0f * (1/60f) is
// 2.00000010430812835693359375, which is 0.4375 ulp above 2 and rounds DOWN to
// exactly 2.0 -- a different rounding than the X row's, because the rate
// differs -- and 2.0 / 0.15f is 67108864/5033165 = 13.33333280351429, which is
// 0.2222 ulp BELOW 0x41555555 = 13.333333015441895 (ulp = 2^-20), so it rounds
// UP to that same 0x41555555 (review-7 F3 corrected this paragraph: it said
// 13.333332807028579 and called it above the pin, and the pin is what the
// arithmetic actually reaches).
// ---------------------------------------------------------------------------
void testLookBits() {
    const Input right = applied(stick(155, 0), kStep);
    expectBits("full right for one 60 Hz step", right.mouseX, 0x41A00000); // 20.0
    expectBits("and no pitch at all", right.mouseY, 0x00000000);

    const Input left = applied(stick(-155, 0), kStep);
    expectBits("full left is the same turn, negated", left.mouseX, 0xC1A00000);

    const Input up = applied(stick(0, 155), kStep);
    expectBits("libctru's dy is UP, and the look's y is down", up.mouseY, 0xC1555555);
    expectBits("and looking up turns nothing", up.mouseX, 0x00000000);

    const Input down = applied(stick(0, -155), kStep);
    expectBits("looking down is the same pitch, negated", down.mouseY, 0x41555555);

    // The clamp boundary: 0.1 s is Gamepads.MaxStep, and a longer gap buys the
    // same look and no more.
    const Input clamped = applied(stick(155, 0), 0.1f);
    expectBits("one tenth of a second at full tilt", clamped.mouseX, 0x42EFFFFF);
    const Input stalled = applied(stick(155, 0), 100.0f);
    expectBits("a stalled frame buys no more look than the clamp", stalled.mouseX,
               bitsOf(clamped.mouseX));

    // A full tilt on a diagonal: the tilt clamps to 1, so strength is 1 and the
    // DIRECTION is what is unit -- 1/sqrt(2) on each axis (0x3F3504F3), which is
    // why the two components are the X and Y rates times that, and why the ratio
    // of the two is the ratio of the two rates.
    const Input diagonal = applied(stick(155, 155), 0.1f);
    expectBits("a full diagonal", diagonal.mouseX, 0x42A9B4A4);
    expectBits("turns and pitches together", diagonal.mouseY, 0xC2624630);

    // A gap that is zero, negative or NaN looks nowhere. The NaN row is the one
    // that matters: a poisoned accumulator would spin the view for every frame
    // after it, and there is no way back short of a restart.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    expectBits("a zero gap does not look", applied(stick(155, 0), 0.0f).mouseX, 0x00000000);
    expectBits("a negative gap does not look", applied(stick(155, 0), -1.0f).mouseX, 0x00000000);
    expectBits("a NaN gap does not look", applied(stick(155, 0), nan).mouseX, 0x00000000);
    expectBits("and leaves no poison behind", applied(stick(155, 0), nan).mouseY, 0x00000000);
}

// ---------------------------------------------------------------------------
// 3. The dead zone, the curve's shape, and symmetry: properties of the C#
//    formula, independent of any particular rounding.
// ---------------------------------------------------------------------------
void testLookProperties() {
    // GamepadMap.DeadZone is 0.15 OF THE REACH: 0.15 * 155 = 23.25, so 23 is
    // inside the dead zone and 24 is outside it. There is no raw value between
    // them, so this is the whole boundary.
    expectBits("23 of 155 is inside the dead zone", applied(stick(23, 0), 0.1f).mouseX, 0x00000000);
    expect("24 of 155 is outside it", applied(stick(24, 0), 0.1f).mouseX > 0.0f, 1);

    // Monotone in the tilt, and the curve is convex: half tilt is well under half
    // the look of full tilt (Curve 2 squares the ramp past the dead zone).
    const float quarter = applied(stick(39, 0), 0.1f).mouseX;
    const float half = applied(stick(100, 0), 0.1f).mouseX;
    const float full = applied(stick(155, 0), 0.1f).mouseX;
    expect("the look grows with the tilt", half > quarter, 1);
    expect("and keeps growing to full tilt", full > half, 1);
    expect("the curve is convex, not linear", half < full * 0.5f, 1);

    // Symmetry: the same tilt the other way is the same look negated, on both
    // axes. A sign error in the normalisation breaks this and the bits rows
    // above would not catch it, because they are all full tilt.
    expectBits("left is right negated", applied(stick(-100, 0), 0.1f).mouseX,
               bitsOf(-applied(stick(100, 0), 0.1f).mouseX));
    expectBits("down is up negated", applied(stick(0, -100), 0.1f).mouseY,
               bitsOf(-applied(stick(0, 100), 0.1f).mouseY));

    // Past the reach (the pad can report a diagonal whose raw length exceeds it)
    // the tilt clamps, so the look must NOT keep growing.
    expect("a raw length over the reach does not look faster than full tilt",
           applied(stick(200, 0), 0.1f).mouseX <= full, 1);

    // The two axes are independent: pushing right must not pitch, pushing up must
    // not turn, at any tilt inside the dead zone or out of it.
    expectBits("a pure horizontal stick never pitches", applied(stick(120, 0), 0.1f).mouseY, 0);
    expectBits("a pure vertical stick never turns", applied(stick(0, 120), 0.1f).mouseX, 0);
}

// ---------------------------------------------------------------------------
// 4. The mouse belongs to the game. The platform ADDS; the consumer takes. If
//    the platform drained it, Player::mouseLook would see nothing and Kurt would
//    never turn; if neither drained it, the view would run away.
// ---------------------------------------------------------------------------
void testTheMouseIsDrainedByTheGame() {
    Input input;
    input3dsApply(stick(155, 0), kStep, &input);
    const float first = input.mouseX;
    expect("one frame accumulates", first > 0.0f, 1);

    input3dsApply(stick(155, 0), kStep, &input);
    expectBits("two frames accumulate", input.mouseX, bitsOf(first + first));

    expectBits("and taking it hands over exactly what was there", input.takeMouseX(),
               bitsOf(first + first));
    expectBits("leaving nothing for the next frame", input.takeMouseX(), 0x00000000);
    expectBits("the pitch is taken separately", input.takeMouseY(), 0x00000000);
}

} // namespace

namespace mdk {
namespace test {

int runInput3dsTests() {
    failures = 0;
    testDigitalMap();
    testLookBits();
    testLookProperties();
    testTheMouseIsDrainedByTheGame();
    return failures;
}

} // namespace test
} // namespace mdk

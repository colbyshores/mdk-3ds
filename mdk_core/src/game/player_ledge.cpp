// ============================================================================
// player_ledge.cpp — ledges, the C#'s partial class Kurt.Ledge.cs
// (damp_ledge_grab 0x469868, kurt.gd _grab_ledge, _update_climb; godot-mdk
// docs/gameplay.md "Vertical"): falling while running forward, Kurt grabs a flat
// top his hands cross and climbs onto it.
//
//        hands' path (4.6 above the feet, 1-3 ahead)
//   ─ ─ ─ ─ ─ ─●─ ─ ─►   crosses the top ──► its edge towards Kurt, faced within 30°,
//          ┌────┴──────   room above it ──► HANG: 1 before the edge, 4.6 below it
//   Kurt ► │  wall          K_HANG, 2 ticks a frame, climbs by the tables (≈ 4.3 up, 1.35 on)
//
// One file per C# partial (package #12), as player_sniper.cpp is: the grab
// (GrabLedge, HangFrom) runs at the end of the walk step, after the fall and the
// touches (Kurt.cs:237), and the climb (UpdateClimb) owns the step through
// UpdateSpecial's Hang arm (Kurt.cs:190-195, 598-601). The probes they stand on
// are ArenaSpace::floorCrossing / edge / free (ArenaSpace.Probes.cs) and
// Bsp::edge. They are BSP-only in the oracle: no solid object stops a grab.
//
// Kurt's facing is Player::facing(), the trig LUT the walk uses, as the C#'s
// Facing is the walk's. The goldens (golden/L*_ledge_*.txt) pin it at a yaw of
// 0, where the LUT and MathF agree (docs/OPEN_ITEMS.md: "`Player::facing()` takes its
// trig from the LUT", an owner decision, and "Ledges: what the goldens cannot see").
// ============================================================================

#include <cmath>

#include "game/player.h"

namespace mdk {

namespace {

// The hands' height above the feet (0x49798c), and how far ahead they reach
// (Kurt.Ledge.cs:20-21).
const float kLedgeHeight = 4.6041665f;
const int kLedgeReach = 3;
// Kurt grabs while falling at least this fast (u/s).
const float kLedgeSpeed = -0.25f;
// Only flat tops (|nz| >= 0.85), their edge faced within 30 degrees.
const float kLedgeFlat = 0.85f;
const float kLedgeAngle = 30.0f;
// The yaw is the edge's direction - 90 degrees (0x49799c).
const float kEdgeToYaw = -90.0f;
// Room above the edge: a box of half size (0.5, 0.5, 2), 2.5 above it, swept from 1
// before it to 0.5 past it.
const Vec kLedgeRoom = {0.5f, 0.5f, 2.0f};
const float kLedgeRoomLift = 2.5f;
const float kLedgeRoomPast = 0.5f;
// Kurt hangs this far before the edge.
const float kHangBack = 1.0f;

// Climbing (K_HANG, 2 ticks a frame): the height and the backward offset of each
// frame pair (0x491f34, 0x491f74), scaled by 0.708333 x 0.5 per tick
// (Kurt.Ledge.cs:37-44).
const float kClimbHeights[] = {0.374f, 0.326f, 0.292f, 0.311f, 0.677f, 1.263f,
                               2.754f, 4.172f, 4.903f, 5.343f, 5.711f, 6.001f,
                               6.212f, 6.345f, 6.406f, 6.417f};
const float kClimbBack[] = {0.685f,  0.383f,  0.167f,  0.254f,  0.635f,  1.053f,
                            0.847f,  0.514f,  0.17f,   -0.125f, -0.501f, -0.825f,
                            -1.066f, -1.221f, -1.293f, -1.305f};
const float kClimbScale = 0.708333f * 0.5f;
const int kClimbSteps = 15;
const int kTicksPerClimbFrame = 2;
static_assert(sizeof(kClimbHeights) / sizeof(kClimbHeights[0]) == kClimbSteps + 1,
              "the climb reads k-1 and k for k < kClimbSteps");
static_assert(sizeof(kClimbBack) / sizeof(kClimbBack[0]) == kClimbSteps + 1,
              "the climb reads k-1 and k for k < kClimbSteps");

const char kChuteOnSound[] = "CHUTEON";  // C# Kurt.ChuteOnSound

inline Vec toVec(const Vec3& v) { return Vec{v.x, v.y, v.z}; }
inline Vec3 fromVec(const Vec& v) { return Vec3{v.x, v.y, v.z}; }

// C# Kurt.Wrap180 (Kurt.Ledge.cs:131): a closed form, one floor.
float wrap180(float degrees) {
    return degrees - 360.0f * floorf((degrees + 180.0f) / 360.0f);
}

} // namespace

bool Player::grabLedge(const Vec3& before, float forward) {
    // C# Kurt.GrabLedge (Kurt.Ledge.cs:52-74), line for line. The vector sums keep the
    // oracle's left-to-right order: (before + hands) + ahead.
    if (forward <= 0.0f || verticalSpeed > kLedgeSpeed || health() == 0 ||
        state() == Kurt::Dead || state() == Kurt::Knocked || state() == Kurt::GetUp) {
        return false;
    }
    if (_space == nullptr) {
        return false;
    }

    const Vec facingNow = toVec(facing());
    const Vec hands = {0.0f, 0.0f, kLedgeHeight};
    for (int i = 1; i <= kLedgeReach; i++) {
        const Vec ahead = vScale(facingNow, (float)i);
        ArenaSpace::Floor top;
        if (!_space->floorCrossing(vAdd(vAdd(toVec(before), hands), ahead),
                                   vAdd(vAdd(toVec(feet), hands), ahead), &top)) {
            continue;
        }
        // The first top crossed decides: flat and hung from, or no grab at all.
        return std::fabs(top.normal.z) >= kLedgeFlat && hangFrom(top, fromVec(ahead));
    }
    return false;
}

/* @vfp-exempt R6 — the hang's yaw from the edge's direction: atan2f is the oracle's
 * MathF.Atan2 and the conversion .NET's left-associative RadiansToDegrees, as the
 * slide's steer; a LUT angle would turn Kurt off the edge the goldens pin. measured n/a
 * (corpus empty). Verified 2026-11-20. */
bool Player::hangFrom(const ArenaSpace::Floor& top, const Vec3& ahead) {
    // C# Kurt.HangFrom (Kurt.Ledge.cs:77-103), line for line.
    Vec edge;
    Vec direction;
    if (!ArenaSpace::edge(top, vSub(top.point, toVec(ahead)), &edge, &direction)) {
        return false;
    }

    const float newYaw = atan2f(direction.y, direction.x) * 180.0f / 3.14159265358979323846f +
                         kEdgeToYaw;
    const float turn = std::fabs(wrap180(newYaw - yaw));
    // The facing BEFORE the turn: the room and the hang point are measured along the
    // way Kurt ran, not the way the edge turns him.
    const Vec facingNow = toVec(facing());
    const Vec room = vAdd(edge, Vec{0.0f, 0.0f, kLedgeRoomLift});
    if (turn > kLedgeAngle ||
        !_space->free(vSub(room, vScale(facingNow, kHangBack)),
                      vAdd(room, vScale(facingNow, kLedgeRoomPast)), kLedgeRoom)) {
        return false;
    }

    yaw = newYaw;
    feet = fromVec(vSub(vSub(edge, vScale(facingNow, kHangBack)), Vec{0.0f, 0.0f, kLedgeHeight}));
    forwardSpeed = 0.0f;
    strafeSpeed = 0.0f;
    verticalSpeed = 0.0f;
    onFloor = false;
    stopFiring();
    stopSound(kChuteOnSound);
    chuteOpen = false;
    _climbTicks = 0;
    setState(Kurt::Hang);
    return true;
}

void Player::updateClimb(float delta) {
    // C# Kurt.UpdateClimb (Kurt.Ledge.cs:106-129), line for line: the state time in
    // ticks says how many climb ticks are due; each moves the feet by the tables' step
    // (up, then back along the facing), no gravity. The climb ends on the last tick of
    // K_HANG's frames and the walk takes over (Still, in the air until the fall lands).
    _stateTime += delta;
    const int32_t ticks = (int32_t)(_stateTime * Kurt::kTicks) - _climbTicks;
    const Vec3 f = facing();
    for (int32_t i = 0; i < ticks; i++) {
        _climbTicks++;
        const int32_t k = (_climbTicks + 1) >> 1;
        if (k < kClimbSteps) {
            feet.z += (kClimbHeights[k] - kClimbHeights[k - 1]) * kClimbScale;
            const float back = (kClimbBack[k] - kClimbBack[k - 1]) * kClimbScale;
            feet.x -= f.x * back;
            feet.y -= f.y * back;
            feet.z -= f.z * back;
        }
    }

    verticalSpeed = 0.0f;
    const int32_t frames = frameCount(Kurt::Hang);
    if (_climbTicks >= frames * kTicksPerClimbFrame - 1) {
        setState(Kurt::Still);
        return;
    }
    const float frame = (float)(_climbTicks / kTicksPerClimbFrame);
    const float last = (float)(frames - 1);
    _animationFrame = frame < last ? frame : last;
}

} // namespace mdk

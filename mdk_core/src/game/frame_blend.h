#pragma once

// ============================================================================
// frame_blend.h — the oracle's FrameBlend, the half this port draws with.
//
// Mdk.Game/FrameBlend.cs draws a frame BETWEEN the fixed steps: the scripts tick
// at 1/30 s and the screen presents faster, so most frames show an object part
// of the way from its last tick to its current one. The C# class does two jobs:
//
//   * the three statics below — the lerp, the shortest-way-around angle lerp and
//     the jump test — which MdkObject uses for its own tick-to-tick interpolation
//     (position_at/yaw_at/transform_at);
//   * Kurt's blended feet and yaw (Record/Update/Turn/KurtAt), over the carrier
//     and the ride (scripts/rides.h, package #9) and Kurt.Teleports -- not ported.
//
// Only the first half is here, because only the first half has a caller: the
// object pass. The second is deferred with its subsystem (docs/OPEN_ITEMS.md),
// which is also why the frontend still draws Kurt at Player::feet and not at
// FrameBlend::Feet. The scalar Lerp(float,float,float) the C# also has is here
// for the follow camera's blended pitch, pivot and roll; the cutscene camera is
// a later caller.
//
// WHY THE JUMP TEST IS NOT AN OPTIMIZATION. A teleport is a move no frame should
// be drawn on the way of, and it lands OUTSIDE the tick that snapshotted the pose,
// so an interpolator that does not test for it draws the object back where it was
// before the teleport for the rest of the tick. That is the render-only-frame bug
// class (Obsidian 30-architecture/patterns/render-only-frame-interpolation-trap.md):
// state changed outside the tick-and-snapshot flow must be seen by the
// interpolator. The C# sees it as a speed — more than MaxSpeed in one tick — and
// so must this, or the port invents a teleport rule the oracle does not have.
// ============================================================================

#include "render/render_types.h"

namespace mdk {
namespace frame_blend {

// FrameBlend.MaxSpeed, in units per second: the LEVEL6 lift moves 750, so a
// console `tp` (a teleport) is the thing this catches, not a fast ride.
constexpr float kMaxSpeed = 1500.0f;

// FrameBlend.Lerp(Vector3, Vector3, float): from -> to by t, and EXACTLY `to`
// once t reaches 1. That early return is not a shortcut: Vector3.Lerp's own
// arithmetic (from + (to - from) * t) does not land on `to` for a large `to`,
// and the frame at the end of a tick must be the tick's own pose, not a rounding
// of it.
Vec3 lerp(Vec3 from, Vec3 to, float t);

// FrameBlend.Lerp(float,float,float): from -> to by t, and EXACTLY `to` once
// t reaches 1. This is float.Lerp (Single.Lerp)'s own arithmetic, which is
// `fmaf(from, 1-t, to*t)` -- measured, see the .cpp -- not the twice-rounded form
// the vector overload above ships, and not the shortest-way angle lerp below.
float lerp(float from, float to, float t);

// FrameBlend.LerpAngle: degrees, the short way round, so 350 -> 10 goes through
// 0 and not through 180.
float lerp_angle(float from, float to, float t);

// FrameBlend.Jumped: whether a move of `seconds` is too fast to be drawn on the
// way (a teleport, a cutscene cut).
bool jumped(Vec3 from, Vec3 to, float seconds);

} // namespace frame_blend
} // namespace mdk

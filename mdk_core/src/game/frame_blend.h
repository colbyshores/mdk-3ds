#pragma once

// ============================================================================
// frame_blend.h — the oracle's FrameBlend, the half this port draws with.
//
// Mdk.Game/FrameBlend.cs draws a frame BETWEEN the fixed steps: the scripts tick
// at 1/30 s, Kurt steps at 1/60 s and the screen presents at its own rate, so most
// frames show an object part of the way from its last tick to its current one and
// Kurt part of the way from his last step to his current one. It has two halves:
//
//   * the three statics below -- the lerp, the shortest-way-around angle lerp and
//     the jump test -- which MdkObject uses for its own tick-to-tick interpolation
//     (position_at/yaw_at/transform_at);
//   * class FrameBlend below: Kurt's drawn feet and yaw (Record/Update/Turn/KurtAt)
//     and the transform each object is drawn with (TransformOf/PositionOf).
//
// WHY BOTH HALVES OR NEITHER (the owner's 2026-11-27 platform jiggle). Until
// 2026-11-27 only the first half was ported: the objects were drawn blended
// between ticks while Kurt -- and the follow camera, which follows his feet --
// were drawn at Player::feet, the last step's place. On a moving platform the
// platform is then drawn up to a whole tick BEHIND the Kurt standing on it, by an
// amount that changes every frame with the frame's place between two ticks: at
// the 3DS's uneven 20-35 frames a second the platform jiggles under a steady Kurt
// and camera, and while it moves down or towards the camera his feet are drawn
// inside it. FrameBlend draws Kurt ON his carrier at the carrier's blended place
// (KurtAt), so the two can never disagree.
// WHY THE JUMP TEST IS NOT AN OPTIMIZATION. A teleport is a move no frame should
// be drawn on the way of, and it lands OUTSIDE the tick that snapshotted the pose,
// so an interpolator that does not test for it draws the object back where it was
// before the teleport for the rest of the tick. That is the render-only-frame bug
// class (Obsidian 30-architecture/patterns/render-only-frame-interpolation-trap.md):
// state changed outside the tick-and-snapshot flow must be seen by the
// interpolator. The C# sees it as a speed — more than MaxSpeed in one tick — and
// so must this, or the port invents a teleport rule the oracle does not have.
// ============================================================================

#include <stdint.h>

#include "render/render_types.h"
#include "util/matrix.h"

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

class Player;
namespace scripts {
struct MdkObject;
class ScriptRuntime;
} // namespace scripts

// C# FrameBlend (Mdk.Game/FrameBlend.cs, upstream C++ src/game/FrameBlend.cpp): Kurt
// drawn between his last two steps -- on a carrier (the platform he stands on, the XE
// he flies), his place on it blended and added to the carrier's blended place -- and
// the objects drawn between their last two ticks, except what follows him each step
// (the XD2, the board: the C#'s Driven), which is drawn with his lag between steps.
//
//   steps   |-----1/60-----|-----1/60-----|   Kurt:    previous --a--> current, a = left over / step
//   ticks   |------------1/30-------------|   objects: last tick --a--> now, a = since the tick / tick
//                                   ^ frame
//
// The frontends call record() after every step (Viewer.RunSteps, after the camera),
// turn() with the mouse's per-frame turn (Viewer.MouseLook), update() once a frame
// after the steps (Viewer.BlendFrame), then draw Kurt at feet(), the follow camera
// with FollowCamera::blend(feet(), yaw()) and each object at transformOf(). With
// blending off (a fixed-step test run, the C#'s Blending.Off) every answer is the
// step's own: Kurt's feet, each object's transform.
//
// The carrier pointers are into ScriptRuntime's object deque, which never frees
// an object while its level runs; reset() drops them when a level goes.
// Cold code: a handful of lerps once a frame, nothing per object but transformOf.
class FrameBlend {
public:
    explicit FrameBlend(bool blending = true) : blending_(blending) {}

    void setBlending(bool on) { blending_ = on; }
    bool blending() const { return blending_; }
    // A new level or a reload: no pose to blend from (the C#'s _previous == null),
    // and Kurt drawn where he stands until the first update().
    void reset(const Player& player);

    // C# Record(kurt, Carrier(), Driven(), TickCount): Kurt after a step. The carrier
    // is Viewer.Carrier (the XE in the bomber, else his platform while he stands);
    // the driven ride is Viewer.Driven (the XD2 or the board, not the XE). A null
    // runtime (no scripts) has neither.
    void record(const Player& player, const scripts::ScriptRuntime* runtime);
    // C# Turn: the mouse turned Kurt by `degrees` between steps -- drawn at once.
    void turn(float degrees);
    // C# Update(kurt, step, tick): this frame, `step` of the way from Kurt's last
    // step to the next and `tick` of the way from the scripts' last tick (0-1).
    void update(const Player& player, float step, float tick);

    // Kurt as drawn.
    const Vec3& feet() const { return feet_; }
    float yaw() const { return yaw_; }
    // C# Tick: the tick share update() was given; 1 (each tick as it is) with
    // blending off.
    float tick() const { return blending_ ? tick_ : 1.0f; }
    // Whether `obj` is what Kurt drives (the XD2, the board), drawn at its own
    // place moved by lag() rather than blended between ticks.
    bool drives(const scripts::MdkObject& obj) const { return blending_ && &obj == driven_; }
    // Kurt as drawn minus Kurt after the last step.
    const Vec3& lag() const { return lag_; }
    // C# TransformOf / PositionOf: an object as drawn.
    Mat4 transformOf(const scripts::MdkObject& obj) const;
    Vec3 positionOf(const scripts::MdkObject& obj) const;

private:
    // C# FrameBlend.Pose: Kurt after a step, and what carries him (its place and
    // yaw now, and a step before).
    struct Pose {
        Vec3 feet = Vec3{0.0f, 0.0f, 0.0f};
        float yaw = 0.0f;
        int32_t teleports = 0;
        const scripts::MdkObject* carrier = nullptr;
        Vec3 carrierAt = Vec3{0.0f, 0.0f, 0.0f};
        float carrierYaw = 0.0f;
        Vec3 carrierWas = Vec3{0.0f, 0.0f, 0.0f};
        float carrierYawWas = 0.0f;
    };

    void kurtAt(const Pose& from, const Pose& to, float step, float tick);

    bool blending_ = true;
    bool havePrevious_ = false;
    Pose previous_;
    Pose current_;
    float tick_ = 1.0f;
    int32_t ticks_ = 0;
    const scripts::MdkObject* driven_ = nullptr;
    Vec3 lag_ = Vec3{0.0f, 0.0f, 0.0f};
    Vec3 feet_ = Vec3{0.0f, 0.0f, 0.0f};
    float yaw_ = 0.0f;
};

} // namespace mdk

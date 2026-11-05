#pragma once

// ============================================================================
// follow_camera.h — the follow camera's placement (C# FollowCamera).
//
// The third-person camera behind Kurt. The full C# class is coupled to Kurt's
// physics and the BSP camera-clear (ArenaSpace.Sight); those land with the
// collision port. What lands here is the placement math that does not depend
// on them: the camera's place for a pair of feet, a facing and a pitch
// (Point), the view axes for that facing and pitch (Orient), and a helper
// that turns them into the look-at Camera the renderer consumes.
//
// Pitch is in RADIANS here and POSITIVE LOOKS DOWN, exactly as the C#'s
// Orient: forward = facing*cos(pitch) - Z*sin(pitch).
// ============================================================================

#include "game/input.h"
#include "render/render_types.h"

namespace mdk {

class ArenaSpace;
class Player;

// The follow camera's constants (C# FollowCamera).
namespace follow_camera {

constexpr float kFieldOfView = 71.5f;
constexpr float kNear = 0.5f;
constexpr float kFar = 20000.0f;
constexpr float kDistance = 8.0f;
constexpr float kDownBack = 5.0f;
constexpr float kCloseUpPitch = -20.0f;   // degrees
constexpr float kCloseUpRange = 80.0f;
constexpr float kCloseUpBias = 100.0f;
constexpr float kDefaultPitch = 4.0f;     // degrees
// The C#'s FollowCamera reads its pivot off Kurt (Kurt.CameraPivot, default
// Kurt.DefaultPivot), so the live value is Player::cameraPivot; this is the same
// 4.5 the C# ships as Kurt.DefaultPivot, kept for the camera's own default Aim
// and for callers that place a view without a Player.
constexpr float kDefaultPivot = 4.5f;
// The view's pitch stays short of straight up or down: past it the view turns
// over (C# FollowCamera.ViewPitchLimit). The orient's up-vector degenerates at
// exactly 90 degrees.
constexpr float kViewPitchLimit = 89.0f;  // degrees

// The pitch smoothing (C# FollowCamera). The look offset is clamped to this
// range once the arena pitch and the air tilt are taken off it.
constexpr float kMouseDegrees = 0.15f;    // degrees per mouse count
constexpr float kMinPitch = -60.0f;
constexpr float kMaxPitch = 90.0f;
constexpr float kTicks = 30.0f;           // the original's ticks per second
// The arena pitch eases in by 0.85·old + 0.15·new per tick.
constexpr float kPitchEase = 0.85f;
// Air time starts below this fall speed; the tilt grows by 0.667·Ticks per
// second up to 40, and decays by 40 per second back on the ground.
constexpr float kFallStartSpeed = -16.0f;
constexpr float kAirPitchRate = 0.667f * 30.0f;
constexpr float kAirPitchMax = 40.0f;
constexpr float kAirPitchDecay = 40.0f;
// The climb: the smoothed rise of the feet per tick, clamped to ±0.5, eased by
// 0.97·old + 0.03·new, returning by 0.02 per tick; 40 degrees per unit.
constexpr float kClimbClamp = 0.5f;
constexpr float kClimbKeep = 0.97f;
constexpr float kClimbReturn = 0.02f;
constexpr float kClimbPitch = 40.0f;

// C# float.DegreesToRadians (Single.DegreesToRadians): multiply by the float
// pi, then divide by 180 -- TWO roundings, left-associative. This is the one
// spelling for the follow camera and the Player's facing (the debris and script
// trees keep their own separately pinned copies); the pre-folded
// `degrees * (pi / 180)` form differs from the oracle by 1 ULP on many inputs.
inline float degreesToRadians(float degrees) {
    constexpr float kPi = 3.14159265358979323846f;
    return degrees * kPi / 180.0f;
}

} // namespace follow_camera

// The camera's place for the feet, the horizontal facing (unit) and the pitch
// in radians (C# FollowCamera.Point).
Vec3 followCameraPoint(const Vec3& feet, const Vec3& facing, float pitch, float pivot);

// The view axes for the facing and pitch in radians (C# FollowCamera.Orient).
void followCameraOrient(const Vec3& facing, float pitch, Vec3* forward, Vec3* up);

// The roll half of C# FollowCamera.Turn (FollowCamera.cs:196-212): swing the up
// vector about the forward by rollDegrees (positive banks the view to the right;
// Kurt's moves write it). Forward is untouched, so the picture banks without
// turning, and a roll of exactly zero leaves the axes alone exactly as the C#'s
// early return does. The shake half of Turn is not ported (see the note on
// FollowCamera below): the oracle draws its shake from a time-seeded
// System.Random, so there is no reproducible oracle value to pin a shake against.
void followCameraRoll(const Vec3& forward, Vec3* up, float rollDegrees);

// The camera's clearance (C# FollowCamera.Clear, camera_clearance 0x417ee8): a
// wall between Kurt's head (feet + 5.5) and the camera pushes Kurt d·n away
// from it (with a floor there, else half of it to a side), and the camera moves
// as far as Kurt did. May move the player. `space` may be null (no clearance).
// `camera` is the un-cleared place from followCameraPoint; returns the cleared
// place. The solid-object ClipView pass is not ported (no objects yet).
Vec3 followCameraClear(ArenaSpace* space, Player& player, const Vec3& camera);

// Fills camera with the follow camera's view for the feet, the horizontal
// facing (unit), the pitch in DEGREES (positive looks down) and the pivot, at
// the game's FOV/near/far. `rollDegrees` is Kurt's CameraRoll (0 for a view that
// has not banked). Returns false if the look-at is degenerate.
bool buildFollowCamera(Camera* camera, const Vec3& feet, const Vec3& facing,
                       float pitchDegrees, float pivot, float aspect,
                       float rollDegrees);

// As buildFollowCamera, but placed from the player and run through the camera
// clearance (C# FollowCamera.Update: Point then Clear). The clearance may move
// the player (Kurt is shoved off a wall) and the camera with him.
bool buildFollowCameraCleared(Camera* camera, ArenaSpace* space, Player& player,
                              float pitchDegrees, float pivot, float aspect,
                              float rollDegrees);

// The follow camera's smoothing state (C# FollowCamera). It owns the eased arena
// pitch, the mouse's look offset, the air tilt and the climb, and places the
// view each step. Kurt's roll is applied here too, read off the Player the way the
// C# reads it off Kurt. The between-step blend is ported for the camera's own
// Aim triple; wiring it to a frontend still needs FrameBlend's Kurt pose. The
// shake, the sniper scope and the top-down view are not ported (they need the
// scripts and the moves); where the C# branches on them this takes the false
// side.
class FollowCamera {
public:
    // The per-frame mouse look up/down (C# FollowCamera.MouseLook): the look
    // offset, clamped against the eased arena pitch and the air tilt.
    void mouseLook(Input& input);

    // A frame between two steps (C# FollowCamera.Blend): place the camera for
    // Kurt as drawn (`feet`, `yaw`) and blend the camera's pitch, pivot and roll
    // from the last two steps by `alpha`. The feet and yaw are supplied by the
    // caller because the oracle gets them from FrameBlend's Kurt pose. Returns
    // false before the first step (there is no Aim to blend) or when the
    // look-at is degenerate; the camera is left alone in that case.
    bool blend(Camera* camera, const Vec3& feet, float yaw, float alpha, float aspect);

    // The eased arena pitch, in degrees. This is the camera's own state, exposed
    // read-only so the host harness can pin the oracle's exact rounding; no
    // gameplay path needs to read it back.
    float arenaPitch() const { return _arenaPitch; }

    // One step (C# FollowCamera.Update): ease the arena pitch toward its target,
    // update the air time and the climb, compute the pitch, then place the camera
    // and run the clearance (buildFollowCameraCleared). `arenaPitchTarget` is the
    // pitch of the arena Kurt is in (the C# PitchGoal).
    void update(Camera* camera, ArenaSpace* space, Player& player, float arenaPitchTarget,
                float delta, float aspect);

private:
    struct Aim {
        float pitch = 0.0f;
        float pivot = follow_camera::kDefaultPivot;
        float roll = 0.0f;
    };

    void updateAirTime(const Player& player, float delta);
    void updateClimb(float feetZ, float delta);
    static float climbStep(float climb, float rise, float ticks);

    float _arenaPitch = follow_camera::kDefaultPitch;
    float _lookOffset = 0.0f;
    float _airPitch = 0.0f;
    float _airTime = 0.0f;
    float _climb = 0.0f;
    bool _haveLastFeetZ = false;
    float _lastFeetZ = 0.0f;
    float _climbTime = 0.0f;
    Aim _previousAim;
    Aim _aim;
    bool _haveAim = false;
};

} // namespace mdk

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
constexpr float kDefaultPivot = 4.5f;
// The view's pitch stays short of straight up or down: past it the view turns
// over (C# FollowCamera.ViewPitchLimit). The orient's up-vector degenerates at
// exactly 90 degrees.
constexpr float kViewPitchLimit = 89.0f;  // degrees

} // namespace follow_camera

// The camera's place for the feet, the horizontal facing (unit) and the pitch
// in radians (C# FollowCamera.Point).
Vec3 followCameraPoint(const Vec3& feet, const Vec3& facing, float pitch, float pivot);

// The view axes for the facing and pitch in radians (C# FollowCamera.Orient).
void followCameraOrient(const Vec3& facing, float pitch, Vec3* forward, Vec3* up);

// The camera's clearance (C# FollowCamera.Clear, camera_clearance 0x417ee8): a
// wall between Kurt's head (feet + 5.5) and the camera pushes Kurt d·n away
// from it (with a floor there, else half of it to a side), and the camera moves
// as far as Kurt did. May move the player. `space` may be null (no clearance).
// `camera` is the un-cleared place from followCameraPoint; returns the cleared
// place. The solid-object ClipView pass is not ported (no objects yet).
Vec3 followCameraClear(ArenaSpace* space, Player& player, const Vec3& camera);

// Fills camera with the follow camera's view for the feet, the horizontal
// facing (unit), the pitch in DEGREES (positive looks down) and the pivot, at
// the game's FOV/near/far. Returns false if the look-at is degenerate.
bool buildFollowCamera(Camera* camera, const Vec3& feet, const Vec3& facing,
                       float pitchDegrees, float pivot, float aspect);

// As buildFollowCamera, but placed from the player and run through the camera
// clearance (C# FollowCamera.Update: Point then Clear). The clearance may move
// the player (Kurt is shoved off the wall) and the camera with him.
bool buildFollowCameraCleared(Camera* camera, ArenaSpace* space, Player& player,
                              float pitchDegrees, float pivot, float aspect);

} // namespace mdk

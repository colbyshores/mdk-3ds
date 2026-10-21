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

} // namespace follow_camera

// The camera's place for the feet, the horizontal facing (unit) and the pitch
// in radians (C# FollowCamera.Point).
Vec3 followCameraPoint(const Vec3& feet, const Vec3& facing, float pitch, float pivot);

// The view axes for the facing and pitch in radians (C# FollowCamera.Orient).
void followCameraOrient(const Vec3& facing, float pitch, Vec3* forward, Vec3* up);

// Fills camera with the follow camera's view for the feet, the horizontal
// facing (unit), the pitch in DEGREES (positive looks down) and the pivot, at
// the game's FOV/near/far. Returns false if the look-at is degenerate.
bool buildFollowCamera(Camera* camera, const Vec3& feet, const Vec3& facing,
                       float pitchDegrees, float pivot, float aspect);

} // namespace mdk

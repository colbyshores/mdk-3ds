#pragma once

// ============================================================================
// camera_builder.h — building the game's camera.
//
// THE MATRIX CONVENTION IS THE ENGINE'S, and it is the Direct3D one:
//
//   * ROW-MAJOR, with a ROW-VECTOR convention: the point is a row and the
//     transform is `p_clip = p_world * M`.
//   * LEFT-HANDED: the camera looks down +Z, and the depth runs 0 (near) to
//     1 (far).
//
// The 3DS's vertex shader is column-vector, and its depth range is [-1, 0],
// so the 3DS backend TRANSPOSES and REMAPS. That is the backend's job
// (SPEC.md 7.2); the engine's matrices are the engine's.
//
// MDK's COORDINATES ARE Z-UP, and the level's angles are in DEGREES with 90
// facing +Y. So "up" is +Z and the level's angle is a heading in the XY
// plane.
// ============================================================================

#include "render/render_types.h"

namespace mdk {

// A camera looking from `eye` towards `target`.
//
// `fovDegrees` is the vertical field of view. `aspect` is the width over
// the height. Returns false when the direction is degenerate.
bool buildLookAtCamera(Camera* camera, const Vec3& eye, const Vec3& target, const Vec3& up,
                       float fovDegrees, float aspect, float nearPlane, float farPlane);

// A camera at `eye` looking along a heading in the XY plane.
//
// `headingDegrees` is the level's angle convention: 90 faces +Y, and 0
// faces +X. `pitchDegrees` tilts the view up (positive) or down.
bool buildHeadingCamera(Camera* camera, const Vec3& eye, float headingDegrees, float pitchDegrees,
                        float fovDegrees, float aspect, float nearPlane, float farPlane);

} // namespace mdk

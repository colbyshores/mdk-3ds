#pragma once

// ============================================================================
// camera_builder.h — building the game's camera.
//
// THE MATRIX CONVENTION IS THE ENGINE'S, and it is the C# ORACLE'S
// (System.Numerics.Matrix4x4.CreateLookAt / CreatePerspectiveFieldOfView):
//
//   * ROW-MAJOR, with a ROW-VECTOR convention: the point is a row and the
//     transform is `p_clip = p_world * M`.
//   * RIGHT-HANDED: the camera looks down -Z in view space, and the depth
//     runs 0 (near) to 1 (far) with clip.w = -z_view.
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

// The view frustum as six planes (a,b,c,d per plane, a point inside has
// a*x + b*y + c*z + d >= 0 on all six), extracted from a Camera's
// view-projection in the engine's row-vector convention -- the same
// Gribb-Hartmann read the 3DS backend does per eye, so a game-side cull
// and the backend's per-eye cull agree about the volume.
struct Frustum {
    float planes[6][4] = {{0}};
};

bool buildFrustum(const Camera& camera, Frustum* out);

// Whether a world sphere is wholly outside the frustum: true only when
// it is on the outside of some plane by more than its radius, so an
// inside or intersecting sphere is never outside. A caller covering
// both stereo eyes inflates the radius by half the eye separation
// (the eyes are this frustum translated along the camera's right axis
// by at most that much, eyePlan's clamped symmetric split).
bool frustumSphereOutside(const Frustum& frustum, const Vec3& center, float radius);

// THE PROJECTED PIXEL SIZE: how many screen pixels `worldSize` world units
// cover at view depth `zNear` on a `width` x `height` picture,
//
//     px = worldSize * max(P00 * width / 2, P11 * height / 2) / zNear
//
// (P00, P11 the projection's x and y scales, so the sniper scope's zoom is
// in them). The larger axis is taken, so the answer never undersells a
// thin object. A `zNear` at or in front of the near plane -- the size
// reaching the camera -- answers the largest float: never "small". Callers
// that want a sphere's FULL diameter pass 2 * radius and its nearest
// depth (sphereNearDepth). A draw-side measure only: nothing that moves
// gameplay may read it.
float projectedPixelSize(const Camera& camera, float worldSize, float zNear, int32_t width,
                         int32_t height);

// A world sphere's nearest view depth: its centre's depth along the view
// (the view matrix's -z) less its radius. Negative behind the camera.
float sphereNearDepth(const Camera& camera, const Vec3& center, float radius);

// A camera at `eye` looking along a heading in the XY plane.
//
// `headingDegrees` is the level's angle convention: 90 faces +Y, and 0
// faces +X. `pitchDegrees` tilts the view up (positive) or down.
bool buildHeadingCamera(Camera* camera, const Vec3& eye, float headingDegrees, float pitchDegrees,
                        float fovDegrees, float aspect, float nearPlane, float farPlane);

} // namespace mdk

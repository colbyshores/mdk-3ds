// ============================================================================
// camera_builder.cpp — see camera_builder.h.
// ============================================================================

#include "render/camera_builder.h"

#include <cmath>

#include <string.h>

#include "util/math_lut.h"

namespace mdk {

namespace {

const float kPi = 3.14159265358979323846f;

Vec3 subtract(const Vec3& a, const Vec3& b) {
    return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 cross(const Vec3& a, const Vec3& b) {
    return Vec3{a.y * b.z - a.z * b.y,
                a.z * b.x - a.x * b.z,
                a.x * b.y - a.y * b.x};
}

float dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 normalize(const Vec3& v) {
    const float length = dot(v, v);
    if (length <= 0.0f) {
        return Vec3{0.0f, 0.0f, 0.0f};
    }
    const float inverse = 1.0f / sqrtf(length);
    return Vec3{v.x * inverse, v.y * inverse, v.z * inverse};
}

} // namespace

bool buildLookAtCamera(Camera* camera, const Vec3& eye, const Vec3& target, const Vec3& up,
                       float fovDegrees, float aspect, float nearPlane, float farPlane) {
    if (!camera) {
        return false;
    }
    Vec3 forward = normalize(subtract(target, eye));
    if (forward.x == 0.0f && forward.y == 0.0f && forward.z == 0.0f) {
        return false;
    }
    // LEFT-HANDED: the right vector is cross(up, forward), not
    // cross(forward, up). The other way round mirrors the picture.
    Vec3 right = normalize(cross(up, forward));
    if (right.x == 0.0f && right.y == 0.0f && right.z == 0.0f) {
        return false;
    }
    const Vec3 trueUp = cross(forward, right);

    // The view matrix, in the row-vector convention (p' = p * M): the
    // camera's axes are the COLUMNS of M -- x' = dot(right, p) needs
    // right in column 0 -- and the translation is row 3, the negated
    // projection of the eye onto each axis. This is exactly what
    // System.Numerics.Matrix4x4.CreateLookAt produces, which is what the
    // C# oracle uses.
    float* view = camera->view;
    view[0] = right.x;   view[1] = trueUp.x;  view[2] = forward.x;  view[3] = 0.0f;
    view[4] = right.y;   view[5] = trueUp.y;  view[6] = forward.y;  view[7] = 0.0f;
    view[8] = right.z;   view[9] = trueUp.z;  view[10] = forward.z; view[11] = 0.0f;
    view[12] = -dot(right, eye);
    view[13] = -dot(trueUp, eye);
    view[14] = -dot(forward, eye);
    view[15] = 1.0f;

    // The projection: Direct3D's LEFT-HANDED perspective, with the depth in
    // [0, 1], in the row-vector convention: z' = z*zScale + zOffset comes
    // from column 2 (zScale at [2][2], zOffset at [3][2]), and w' = z from
    // the 1 at [2][3].
    // R6: tan goes through the LUT's sin/cos (hardware vdiv, R7) rather than
    // libm tanf. The camera is rebuilt every frame, so this is a per-frame path.
    const float radians = fovDegrees * (kPi / 180.0f);
    const float halfFov = radians * 0.5f;
    float yScale = fastCos(halfFov) / fastSin(halfFov);
    float xScale = yScale / (aspect > 0.0f ? aspect : 1.0f);
    const float range = farPlane - nearPlane;
    if (range <= 0.0f) {
        return false;
    }
    const float zScale = farPlane / range;
    const float zOffset = -nearPlane * farPlane / range;

    float* projection = camera->projection;
    memset(projection, 0, sizeof(camera->projection));
    projection[0] = xScale;
    projection[5] = yScale;
    projection[10] = zScale;
    projection[11] = 1.0f;
    projection[14] = zOffset;

    camera->position = eye;
    camera->forward = forward;
    camera->up = trueUp;
    camera->near_plane = nearPlane;
    camera->far_plane = farPlane;
    return true;
}

bool buildHeadingCamera(Camera* camera, const Vec3& eye, float headingDegrees, float pitchDegrees,
                        float fovDegrees, float aspect, float nearPlane, float farPlane) {
    // The level's heading: 90 faces +Y, 0 faces +X. The heading is a
    // rotation about the Z axis, and the pitch tilts the view.
    const float heading = headingDegrees * (kPi / 180.0f);
    const float pitch = pitchDegrees * (kPi / 180.0f);
    const float cosHeading = fastCos(heading);
    const float sinHeading = fastSin(heading);
    const float cosPitch = fastCos(pitch);
    const float sinPitch = fastSin(pitch);

    // The forward direction: the heading in the XY plane, tilted by the
    // pitch.
    const Vec3 forward{cosHeading * cosPitch, sinHeading * cosPitch, -sinPitch};

    // MDK's up is +Z.
    return buildLookAtCamera(camera, eye, Vec3{eye.x + forward.x, eye.y + forward.y, eye.z + forward.z},
                              Vec3{0.0f, 0.0f, 1.0f}, fovDegrees, aspect, nearPlane, farPlane);
}

} // namespace mdk

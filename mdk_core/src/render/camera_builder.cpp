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
    // RIGHT-HANDED, exactly as the C# oracle's
    // System.Numerics.Matrix4x4.CreateLookAt builds it:
    //
    //   zaxis = normalize(eye - target)   = -forward
    //   xaxis = normalize(cross(up, zaxis))
    //   yaxis = cross(zaxis, xaxis)
    //
    // The old left-handed build (right = cross(up, forward), forward as the
    // third axis) put the camera 180 degrees round in yaw AND mirrored the
    // world: the picture came out inside-out and the solid geometry's front
    // faces were culled away. The oracle's convention is the reference.
    const Vec3 zaxis{-forward.x, -forward.y, -forward.z};
    Vec3 right = normalize(cross(up, zaxis));
    if (right.x == 0.0f && right.y == 0.0f && right.z == 0.0f) {
        return false;
    }
    const Vec3 trueUp = cross(zaxis, right);

    // The view matrix, in the row-vector convention (p' = p * M): the
    // camera's axes are the COLUMNS of M -- x' = dot(right, p) needs
    // right in column 0 -- and the translation is row 3, the negated
    // projection of the eye onto each axis. This is exactly what
    // System.Numerics.Matrix4x4.CreateLookAt produces, which is what the
    // C# oracle uses.
    float* view = camera->view;
    view[0] = right.x;   view[1] = trueUp.x;  view[2] = zaxis.x;  view[3] = 0.0f;
    view[4] = right.y;   view[5] = trueUp.y;  view[6] = zaxis.y;  view[7] = 0.0f;
    view[8] = right.z;   view[9] = trueUp.z;  view[10] = zaxis.z; view[11] = 0.0f;
    view[12] = -dot(right, eye);
    view[13] = -dot(trueUp, eye);
    view[14] = -dot(zaxis, eye);
    view[15] = 1.0f;

    // The projection: System.Numerics.Matrix4x4.CreatePerspectiveFieldOfView,
    // the right-handed one that goes with the view above. Its depth is still
    // [0, 1] (0 at the near plane, 1 at the far plane), but the clip's w is
    // -z_view, so a point in front of the camera has w > 0.
    // R6: tan goes through the LUT's sin/cos (hardware vdiv, R7) rather than
    // libm tanf. The camera is rebuilt every frame, so this is a per-frame path.
    const float radians = fovDegrees * kPi / 180.0f;
    const float halfFov = radians * 0.5f;
    float yScale = fastCos(halfFov) / fastSin(halfFov);
    float xScale = yScale / (aspect > 0.0f ? aspect : 1.0f);
    const float range = farPlane - nearPlane;
    if (range <= 0.0f) {
        return false;
    }
    const float zScale = -farPlane / range;
    const float zOffset = -nearPlane * farPlane / range;

    float* projection = camera->projection;
    memset(projection, 0, sizeof(camera->projection));
    projection[0] = xScale;
    projection[5] = yScale;
    projection[10] = zScale;
    projection[11] = -1.0f;
    projection[14] = zOffset;

    camera->position = eye;
    camera->forward = forward;
    camera->up = trueUp;
    camera->near_plane = nearPlane;
    camera->far_plane = farPlane;
    return true;
}

bool buildFrustum(const Camera& camera, Frustum* out) {
    if (out == nullptr) {
        return false;
    }
    // view * projection, row-vector (the multiply the renderer composes).
    float vp[16];
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += camera.view[row * 4 + k] * camera.projection[k * 4 + col];
            }
            vp[row * 4 + col] = sum;
        }
    }
    // Gribb-Hartmann in the row-vector convention (the backend's
    // pica::frustumPlanes, element for element): the planes are column
    // sums/differences of vp, near is column 2 (z >= 0 in [0,1] depth).
    for (int j = 0; j < 4; j++) {
        const float c0 = vp[j * 4 + 0], c1 = vp[j * 4 + 1];
        const float c2 = vp[j * 4 + 2], c3 = vp[j * 4 + 3];
        out->planes[0][j] = c3 + c0;
        out->planes[1][j] = c3 - c0;
        out->planes[2][j] = c3 + c1;
        out->planes[3][j] = c3 - c1;
        out->planes[4][j] = c2;
        out->planes[5][j] = c3 - c2;
    }
    // Unit normals, so the sphere test is a distance, not a ratio.
    for (int p = 0; p < 6; p++) {
        float* plane = out->planes[p];
        const float length = sqrtf(plane[0] * plane[0] + plane[1] * plane[1] +
                                   plane[2] * plane[2]);
        if (length > 0.0f) {
            for (int j = 0; j < 4; j++) {
                plane[j] /= length;
            }
        }
    }
    return true;
}

bool frustumSphereOutside(const Frustum& frustum, const Vec3& center, float radius) {
    for (int p = 0; p < 6; p++) {
        const float* plane = frustum.planes[p];
        const float distance =
            plane[0] * center.x + plane[1] * center.y + plane[2] * center.z + plane[3];
        if (distance < -radius) {
            return true;
        }
    }
    return false;
}

bool buildHeadingCamera(Camera* camera, const Vec3& eye, float headingDegrees, float pitchDegrees,
                        float fovDegrees, float aspect, float nearPlane, float farPlane) {
    // The level's heading: 90 faces +Y, 0 faces +X. The heading is a
    // rotation about the Z axis, and the pitch tilts the view.
    const float heading = headingDegrees * kPi / 180.0f;
    const float pitch = pitchDegrees * kPi / 180.0f;
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

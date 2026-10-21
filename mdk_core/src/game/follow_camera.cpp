// ============================================================================
// follow_camera.cpp — see follow_camera.h.
// ============================================================================

#include "game/follow_camera.h"

#include <math.h>

#include "render/camera_builder.h"
#include "util/math_lut.h"

namespace mdk {

namespace {

const float kPi = 3.14159265358979323846f;

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

Vec3 followCameraPoint(const Vec3& feet, const Vec3& facing, float pitch, float pivot) {
    float distance = follow_camera::kDistance;
    const float cosPitch = fastCos(pitch);
    float back = -distance * cosPitch;
    if (pitch > 0.0f) {
        back += follow_camera::kDownBack * (1.0f - cosPitch);
    } else if (pitch < follow_camera::kCloseUpPitch * (kPi / 180.0f)) {
        distance = follow_camera::kDistance *
                   (pitch * (180.0f / kPi) + follow_camera::kCloseUpBias) / follow_camera::kCloseUpRange;
        back = -distance * cosPitch;
    }
    const float sinPitch = fastSin(pitch);
    return Vec3{feet.x + facing.x * back,
                feet.y + facing.y * back,
                feet.z + pivot + distance * sinPitch};
}

void followCameraOrient(const Vec3& facing, float pitch, Vec3* forward, Vec3* up) {
    const float cosPitch = fastCos(pitch);
    const float sinPitch = fastSin(pitch);
    const Vec3 z{0.0f, 0.0f, 1.0f};
    const Vec3 f = normalize(Vec3{facing.x * cosPitch, facing.y * cosPitch, -sinPitch});
    *forward = f;
    *up = normalize(cross(cross(f, z), f));
}

bool buildFollowCamera(Camera* camera, const Vec3& feet, const Vec3& facing,
                       float pitchDegrees, float pivot, float aspect) {
    const float pitch = pitchDegrees * (kPi / 180.0f);
    const Vec3 position = followCameraPoint(feet, facing, pitch, pivot);
    Vec3 forward;
    Vec3 up;
    followCameraOrient(facing, pitch, &forward, &up);
    return buildLookAtCamera(camera, position,
                             Vec3{position.x + forward.x, position.y + forward.y,
                                  position.z + forward.z},
                             up, follow_camera::kFieldOfView, aspect, follow_camera::kNear,
                             follow_camera::kFar);
}

} // namespace mdk

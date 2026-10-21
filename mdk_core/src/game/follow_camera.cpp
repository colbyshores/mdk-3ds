// ============================================================================
// follow_camera.cpp — see follow_camera.h.
// ============================================================================

#include "game/follow_camera.h"

#include <math.h>

#include "formats/arena.h"
#include "game/arena_space.h"
#include "game/player.h"
#include "game/vec_util.h"
#include "render/camera_builder.h"
#include "util/math_lut.h"

namespace mdk {

namespace {

const float kPi = 3.14159265358979323846f;

// C# FollowCamera.Clear's constants (camera_clearance 0x417ee8).
const float kHeadHeight = 5.5f;
const float kSightBox = 0.1f;
const float kPushFloorRange = 4.0f;
const float kPushSide = 0.5f;

// The seam between the renderer's Vec3 and the collision's formats::Vec3f.
inline Vec toVec(const Vec3& v) { return Vec{v.x, v.y, v.z}; }
inline Vec3 fromVec(const Vec& v) { return Vec3{v.x, v.y, v.z}; }

// Keep the view's pitch short of straight up or down (C# FollowCamera clamps the
// degrees to ±ViewPitchLimit before Point/Orient; at exactly 90 degrees the
// orient's up-vector is a cross of parallel vectors and degenerates).
float clampViewPitch(float degrees) {
    if (degrees > follow_camera::kViewPitchLimit) {
        return follow_camera::kViewPitchLimit;
    }
    if (degrees < -follow_camera::kViewPitchLimit) {
        return -follow_camera::kViewPitchLimit;
    }
    return degrees;
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

// C# FollowCamera.Push: the push, or a push to a side with a floor under it (in
// the air: the push), or false when none of the three is over a floor.
bool clearancePush(ArenaSpace* space, const Player& player, const Vec& push, const Vec& side,
                   Vec* out) {
    if (!player.onFloor) {
        *out = push;
        return true;
    }
    const Vec offsets[3] = {push, vAdd(push, side), vSub(push, side)};
    for (const Vec& offset : offsets) {
        const Vec at = vAdd(toVec(player.feet), Vec{offset.x, offset.y, 0.0f});
        const Vec up = vAdd(at, Vec{0.0f, 0.0f, kPushFloorRange});
        const Vec down = vSub(at, Vec{0.0f, 0.0f, kPushFloorRange});
        if (space->crosses(up, down)) {
            *out = offset;
            return true;
        }
    }
    return false;
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
    const float pitch = clampViewPitch(pitchDegrees) * (kPi / 180.0f);
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

Vec3 followCameraClear(ArenaSpace* space, Player& player, const Vec3& camera) {
    // C# calls Clear only when space != null && !Sniping && !Frozen &&
    // !Dead; those states are not ported, so the guard is space != null here.
    if (space == nullptr) {
        return camera;
    }
    const Vec head = vAdd(toVec(player.feet), Vec{0.0f, 0.0f, kHeadHeight});
    Vec cam = toVec(camera);
    const Vec sightBox{kSightBox, kSightBox, kSightBox};
    Vec point;
    const formats::BspNode* plane = nullptr;
    if (space->sight(head, cam, sightBox, &point, &plane)) {
        // The horizontal normal towards Kurt: the plane's, flipped if the head
        // is on its negative side (C# plane.Distance(head) < 0).
        float nx = plane->normal[0];
        float ny = plane->normal[1];
        const float headDistance = plane->normal[0] * head.x + plane->normal[1] * head.y +
                                   plane->normal[2] * head.z + plane->d;
        if (headDistance < 0.0f) {
            nx = -nx;
            ny = -ny;
        }
        // d: how far the camera is past the hit, measured horizontally.
        const float dx = cam.x - point.x;
        const float dy = cam.y - point.y;
        const float d = sqrtf(dx * dx + dy * dy);
        const Vec push{nx * d, ny * d, 0.0f};
        const Vec side{ny * (d * kPushSide), -nx * (d * kPushSide), 0.0f};
        Vec chosen;
        if (clearancePush(space, player, push, side, &chosen)) {
            const Vec3 moved = player.shove(fromVec(chosen));
            cam = vAdd(cam, toVec(moved));
        }
    }
    // The solid-object ClipView pass is not ported (no objects yet): the camera
    // has moved as far as Kurt did.
    return fromVec(cam);
}

bool buildFollowCameraCleared(Camera* camera, ArenaSpace* space, Player& player,
                              float pitchDegrees, float pivot, float aspect) {
    const float pitch = clampViewPitch(pitchDegrees) * (kPi / 180.0f);
    const Vec3 facing = player.facing();
    Vec3 position = followCameraPoint(player.feet, facing, pitch, pivot);
    position = followCameraClear(space, player, position);
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

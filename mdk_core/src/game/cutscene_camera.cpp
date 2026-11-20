#include "game/cutscene_camera.h"

#include <math.h>

#include "game/follow_camera.h"
#include "game/frame_blend.h"
#include "render/camera_builder.h"
#include "scripts/script_runtime.h"
#include "util/math_lut.h"

namespace mdk {

namespace {

Vec3 crossOf(const Vec3& a, const Vec3& b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

Vec3 normalized(const Vec3& v) {
    const float length = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    if (length == 0.0f) {
        return v;
    }
    return Vec3{v.x / length, v.y / length, v.z / length};
}

} // namespace

bool CutsceneCamera::active(const scripts::ScriptRuntime& scripts) {
    // scripts.Cutscene != 0 && scripts.CameraPoint != Vector3.Zero (any component).
    const Vec3& point = scripts.cameraPoint;
    return scripts.cutscene != 0 && (point.x != 0.0f || point.y != 0.0f || point.z != 0.0f);
}

Vec3 CutsceneCamera::forwardOf(float yaw, float pitch) {
    // R6: camera trig through the shared table, as followCameraOrient does.
    const float y = follow_camera::degreesToRadians(yaw);
    const float p = follow_camera::degreesToRadians(pitch);
    const float cosPitch = fastCos(p);
    return Vec3{fastSin(y) * cosPitch, fastCos(y) * cosPitch, -fastSin(p)};
}

Vec3 CutsceneCamera::upOf(const Vec3& forward) {
    const Vec3 z{0.0f, 0.0f, 1.0f};
    return normalized(crossOf(crossOf(forward, z), forward));
}

void CutsceneCamera::place(const Vec3& point, float yaw, float pitch) {
    position = point;
    forward = forwardOf(yaw, pitch);
    up = upOf(forward);
    right = normalized(crossOf(forward, Vec3{0.0f, 0.0f, 1.0f}));
}

void CutsceneCamera::update(const scripts::ScriptRuntime& scripts) {
    place(scripts.cameraPoint, scripts.cameraYaw, scripts.cameraPitch);

    // A new tick: its shot follows the last one (none after ticks without the camera).
    const int32_t tick = scripts.tickCount();
    if (tick == tick_) {
        return;
    }
    Shot shot;
    shot.point = scripts.cameraPoint;
    shot.yaw = scripts.cameraYaw;
    shot.pitch = scripts.cameraPitch;
    previous_ = tick == tick_ + 1 ? shot_ : shot;
    havePrevious_ = true;
    shot_ = shot;
    tick_ = tick;
}

bool CutsceneCamera::isCut(const Shot& from, const Shot& to) {
    // FrameBlend.Jumped over one tick, or a turn of more than CutTurn (IEEERemainder is
    // remainderf: the signed short way round).
    return frame_blend::jumped(from.point, to.point, scripts::ScriptRuntime::kTick) ||
           fabsf(remainderf(to.yaw - from.yaw, 360.0f)) > kCutTurn ||
           fabsf(remainderf(to.pitch - from.pitch, 360.0f)) > kCutTurn;
}

void CutsceneCamera::blend(float alpha) {
    if (!havePrevious_ || isCut(previous_, shot_)) {
        return;
    }
    place(frame_blend::lerp(previous_.point, shot_.point, alpha),
          frame_blend::lerp_angle(previous_.yaw, shot_.yaw, alpha),
          frame_blend::lerp_angle(previous_.pitch, shot_.pitch, alpha));
}

bool CutsceneCamera::build(Camera* camera, float aspect) const {
    if (!buildLookAtCamera(camera, position,
                           Vec3{position.x + forward.x, position.y + forward.y,
                                position.z + forward.z},
                           up, follow_camera::kFieldOfView, aspect, kNear, kFar)) {
        return false;
    }
    camera->forward = forward;
    camera->up = up;
    return true;
}

} // namespace mdk

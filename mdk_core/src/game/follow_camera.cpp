// ============================================================================
// follow_camera.cpp — see follow_camera.h.
// ============================================================================

#include "game/follow_camera.h"

#include <math.h>

#include "formats/arena.h"
#include "game/arena_space.h"
#include "game/frame_blend.h"
#include "game/player.h"
#include "game/vec_util.h"
#include "render/camera_builder.h"
#include "util/math_lut.h"
#include "util/math_minmax.h"
#include "game/scope.h"

namespace mdk {

namespace {

const float kPi = 3.14159265358979323846f;

// C# FollowCamera.Clear's constants (camera_clearance 0x417ee8).
const float kHeadHeight = 5.5f;
const float kSightBox = 0.1f;
const float kPushFloorRange = 4.0f;
const float kPushSide = 0.5f;
// Upstream c81dafc1 (C# 1.2.0 FollowCamera.MaxPushSpeed): the camera's push on Kurt, and
// a solid object's shove, move him at most this fast (u/s), the rest over the next steps.
// All at once, a door behind him "fast-forwarded" him through it.
const float kMaxPushSpeed = 40.0f;

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

float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
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
    } else if (pitch < follow_camera::degreesToRadians(follow_camera::kCloseUpPitch)) {
        distance = follow_camera::kDistance *
                   (pitch * 180.0f / kPi + follow_camera::kCloseUpBias) / follow_camera::kCloseUpRange;
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

void followCameraRoll(const Vec3& forward, Vec3* up, float rollDegrees) {
    // C# FollowCamera.Turn's roll half (FollowCamera.cs:207-212). The early return on
    // an exact zero is the C#'s own, so a level view is bit-for-bit what Orient just
    // produced rather than a rotation by 0 that could round.
    if (rollDegrees == 0.0f) {
        return;
    }
    // The C# uses MathF.Cos/MathF.Sin; this is camera trig, so it goes through the
    // project's one trig path (AGENTS.md R6), the same one followCameraOrient uses.
    // The roll never exceeds 10 degrees (0.175 rad), where that table's own measured
    // error is 4.5e-06 on a positive argument and 1.2e-05 on a negative one (the
    // negative side is worse than math_lut.h's documented bound because its index
    // truncates toward zero -- measured, and recorded in vm_tests' camera-roll pin).
    // That is why golden/L3_kurt_camroll.txt pins the DEGREES exactly and vm_tests pins
    // the rotated axis to 2e-5 rather than bit-for-bit.
    const float radians = follow_camera::degreesToRadians(rollDegrees);
    const float cosine = fastCos(radians);
    const float sine = fastSin(radians);
    // The C#'s Right, from Orient (FollowCamera.cs:151): Cross(Forward, UnitZ).
    const Vec3 right = normalize(cross(forward, Vec3{0.0f, 0.0f, 1.0f}));
    const Vec3 before = *up;
    *up = Vec3{before.x * cosine + right.x * sine,
               before.y * cosine + right.y * sine,
               before.z * cosine + right.z * sine};
}

void followCameraTurn(Vec3* forward, Vec3* up, float shakeX, float shakeY, float rollDegrees) {
    // C# FollowCamera.Turn (FollowCamera.cs:194-213). No shake: the roll alone, which is
    // what this function was before package #15 (the C#'s `shake != Vector2.Zero`).
    if (shakeX == 0.0f && shakeY == 0.0f) {
        followCameraRoll(*forward, up, rollDegrees);
        return;
    }
    // Forward = Normalize(Forward + Right·tan(x) + Up·tan(y)), Right being Orient's
    // Normalize(Cross(Forward, UnitZ)); then Right = Normalize(Cross(Forward, Up)) and
    // Up = Normalize(Cross(Right, Forward)). The shake is at most 59 px of 71.5° over 360,
    // 0.2 rad, so the tangent is the table's sine over its cosine (R6).
    const Vec3 right0 = normalize(cross(*forward, Vec3{0.0f, 0.0f, 1.0f}));
    const float tanX = fastSin(shakeX) / fastCos(shakeX);
    const float tanY = fastSin(shakeY) / fastCos(shakeY);
    const Vec3 f = normalize(Vec3{forward->x + right0.x * tanX + up->x * tanY,
                                  forward->y + right0.y * tanX + up->y * tanY,
                                  forward->z + right0.z * tanX + up->z * tanY});
    const Vec3 right = normalize(cross(f, *up));
    const Vec3 u = normalize(cross(right, f));
    *forward = f;
    *up = u;
    if (rollDegrees == 0.0f) {
        return;
    }
    // The roll about the turned axes: Up = up·cos + right·sin (FollowCamera.cs:210).
    const float radians = follow_camera::degreesToRadians(rollDegrees);
    const float cosine = fastCos(radians);
    const float sine = fastSin(radians);
    *up = Vec3{u.x * cosine + right.x * sine, u.y * cosine + right.y * sine,
               u.z * cosine + right.z * sine};
}

bool buildFollowCamera(Camera* camera, const Vec3& feet, const Vec3& facing,
                       float pitchDegrees, float pivot, float aspect,
                       float rollDegrees) {
    const float pitch = follow_camera::degreesToRadians(clampViewPitch(pitchDegrees));
    const Vec3 position = followCameraPoint(feet, facing, pitch, pivot);
    Vec3 forward;
    Vec3 up;
    followCameraOrient(facing, pitch, &forward, &up);
    followCameraRoll(forward, &up, rollDegrees);
    if (!buildLookAtCamera(camera, position,
                           Vec3{position.x + forward.x, position.y + forward.y,
                                position.z + forward.z},
                           up, follow_camera::kFieldOfView, aspect, follow_camera::kNear,
                           follow_camera::kFar)) {
        return false;
    }
    camera->forward = forward;
    camera->up = up;
    return true;
}

Vec3 followCameraClear(ArenaSpace* space, Player& player, const Vec3& camera, float delta,
                       const ClipViewHook* clip) {
    // C# calls Clear only when space != null && !Sniping && !Frozen && !Dead
    // (FollowCamera.cs:132). Frozen is ported (the end of a level: no clearance, so
    // the camera does not shove a lifted Kurt), and so is Sniping (a sniping Kurt is
    // not shoved off a wall by a camera nobody is looking through); the Dead arm is
    // still open (docs/OPEN_ITEMS.md), so the guard is space, Frozen and Sniping here.
    if (space == nullptr || player.frozen || player.sniping()) {
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
            // Eased in (upstream c81dafc1): at most kMaxPushSpeed, the rest on the next steps.
            const float limit = kMaxPushSpeed * delta;
            const float length = sqrtf(chosen.x * chosen.x + chosen.y * chosen.y);
            if (length > limit) {
                chosen.x = chosen.x / length * limit;
                chosen.y = chosen.y / length * limit;
            }
            const Vec3 moved = player.shove(fromVec(chosen));
            cam = vAdd(cam, toVec(moved));
        }
    }
    // C# FollowCamera.Clear's ClipView pass (FollowCamera.cs:268-274): the line of
    // sight from the head against the objects that block the view; Kurt is shoved
    // toward where it ends and the camera goes with him.
    if (clip == nullptr || clip->clip == nullptr) {
        return fromVec(cam);
    }
    const Vec3 camera3 = fromVec(cam);
    const Vec3 end = clip->clip(clip->context,
                                Vec3{player.feet.x, player.feet.y, player.feet.z + kHeadHeight},
                                camera3);
    // A solid object (a door) behind Kurt shoves him on by up to ~8 units in one step:
    // eased in too (upstream c81dafc1).
    Vec3 offset{end.x - camera3.x, end.y - camera3.y, end.z - camera3.z};
    const float limit = kMaxPushSpeed * delta;
    const float length = sqrtf(offset.x * offset.x + offset.y * offset.y + offset.z * offset.z);
    if (length > limit) {
        offset = Vec3{offset.x / length * limit, offset.y / length * limit, offset.z / length * limit};
    }
    const Vec3 moved = player.shove(offset);
    return Vec3{camera3.x + moved.x, camera3.y + moved.y, camera3.z + moved.z};
}

bool buildFollowCameraCleared(Camera* camera, ArenaSpace* space, Player& player,
                              float pitchDegrees, float pivot, float aspect,
                              float rollDegrees, float delta, const ClipViewHook* clip,
                              float shakeX, float shakeY) {
    const float pitch = follow_camera::degreesToRadians(clampViewPitch(pitchDegrees));
    const Vec3 facing = player.facing();
    Vec3 position = followCameraPoint(player.feet, facing, pitch, pivot);
    position = followCameraClear(space, player, position, delta, clip);
    Vec3 forward;
    Vec3 up;
    followCameraOrient(facing, pitch, &forward, &up);
    followCameraTurn(&forward, &up, shakeX, shakeY, rollDegrees);
    if (!buildLookAtCamera(camera, position,
                           Vec3{position.x + forward.x, position.y + forward.y,
                                position.z + forward.z},
                           up, follow_camera::kFieldOfView, aspect, follow_camera::kNear,
                           follow_camera::kFar)) {
        return false;
    }
    camera->forward = forward;
    camera->up = up;
    return true;
}

Vec3 followCameraUpOf(const Vec3& forward) {
    // C# FollowCamera.UpOf (FollowCamera.cs:386-387):
    // Normalize(Cross(Cross(forward, Z), forward)).
    const Vec3 z{0.0f, 0.0f, 1.0f};
    return numerics::normalize(numerics::cross(numerics::cross(forward, z), forward));
}

void scopeProjection(float zoom, float aspect, float nearPlane, float farPlane, float out[16]) {
    // C# CameraMath.ScopeProjection (FollowCamera.cs:423-430), then .NET 10's
    // Matrix4x4.CreatePerspectiveOffCenter(left, right, bottom, top, near, far), which
    // multiplies by the RECIPROCALS of the width and height rather than dividing:
    // M11 = 2n · 1/(r - l), M22 = 2n · 1/(t - b), M31 = (l + r) · 1/(r - l),
    // M32 = (t + b) · 1/(t - b), M33 = range = f / (n - f), M34 = -1, M43 = range · n.
    // Measured: the divided M32 is 0x3E266666 where the oracle's is 0x3E266667 at every
    // zoom (golden/L*_sniper.txt's proj= lines).
    const float unit = nearPlane * zoom / Scope::kFocal;
    const float halfWidth = Scope::kScreenHeight / 2.0f * aspect * unit;
    const float top = Scope::kCentreY * unit;
    const float bottom = -(Scope::kScreenHeight - Scope::kCentreY) * unit;
    const float left = -halfWidth;
    const float right = halfWidth;
    const float range = farPlane / (nearPlane - farPlane);
    for (int i = 0; i < 16; i++) {
        out[i] = 0.0f;
    }
    const float reciprocalWidth = 1.0f / (right - left);
    const float reciprocalHeight = 1.0f / (top - bottom);
    out[0] = 2.0f * nearPlane * reciprocalWidth;
    out[5] = 2.0f * nearPlane * reciprocalHeight;
    out[8] = (left + right) * reciprocalWidth;
    out[9] = (top + bottom) * reciprocalHeight;
    out[10] = range;
    out[11] = -1.0f;
    out[14] = range * nearPlane;
}

bool buildScopeCamera(Camera* camera, const Vec3& eye, const Vec3& forward, float zoom,
                      float aspect, float nearPlane, float farPlane) {
    // C# CameraMath.ScopeView (FollowCamera.cs:412-420). The look-at is .NET 10's
    // Matrix4x4.CreateLookAt(eye, eye + forward, up) = CreateLookTo(eye, target - eye,
    // up): z = Normalize(-direction), x = Normalize(Cross(up, z)), y = Cross(z, x), the
    // axes in the columns and Dot(axis, -eye) in the last row. The direction is the
    // ROUNDED (eye + forward) - eye, as the oracle's is, not `forward` itself.
    if (camera == nullptr) {
        return false;
    }
    const Vec3 up = followCameraUpOf(forward);
    const Vec3 target{eye.x + forward.x, eye.y + forward.y, eye.z + forward.z};
    const Vec3 back{-(target.x - eye.x), -(target.y - eye.y), -(target.z - eye.z)};
    const Vec3 zaxis = numerics::normalize(back);
    const Vec3 xaxis = numerics::normalize(numerics::cross(up, zaxis));
    const Vec3 yaxis = numerics::cross(zaxis, xaxis);
    if (!(xaxis.x == xaxis.x) || !(zaxis.x == zaxis.x)) {
        return false;  // a NaN axis: a zero line of sight, or one straight up or down
    }
    const Vec3 negEye{-eye.x, -eye.y, -eye.z};
    float* view = camera->view;
    view[0] = xaxis.x;  view[1] = yaxis.x;  view[2] = zaxis.x;   view[3] = 0.0f;
    view[4] = xaxis.y;  view[5] = yaxis.y;  view[6] = zaxis.y;   view[7] = 0.0f;
    view[8] = xaxis.z;  view[9] = yaxis.z;  view[10] = zaxis.z;  view[11] = 0.0f;
    view[12] = numerics::dot(xaxis, negEye);
    view[13] = numerics::dot(yaxis, negEye);
    view[14] = numerics::dot(zaxis, negEye);
    view[15] = 1.0f;
    scopeProjection(zoom, aspect, nearPlane, farPlane, camera->projection);
    camera->position = eye;
    camera->forward = forward;
    camera->up = up;
    camera->near_plane = nearPlane;
    camera->far_plane = farPlane;
    return true;
}

bool buildSniperCamera(Camera* camera, const Player& player, float aspect) {
    // C# FollowCamera.SniperView (FollowCamera.cs:382-383).
    return buildScopeCamera(camera, player.sniperEye(), player.sniperForward(), player.scope().zoom,
                            aspect, follow_camera::kNear, follow_camera::kFar);
}

// C# FollowCamera.LookDown (FollowCamera.cs:297-305, 0x4183f0): the bomber's view --
// straight down from `height` above the feet, the heading at the top (Up is the facing,
// Right Kurt's right). `facing` is (cos yaw, sin yaw, 0).
static bool lookDown(Camera* camera, const Vec3& feet, const Vec3& facing, float height,
                     float aspect) {
    const Vec3 position{feet.x + 0.0f * height, feet.y + 0.0f * height, feet.z + height};
    const Vec3 forward{-0.0f, -0.0f, -1.0f};
    if (!buildLookAtCamera(camera, position,
                           Vec3{position.x + forward.x, position.y + forward.y,
                                position.z + forward.z},
                           facing, follow_camera::kFieldOfView, aspect, follow_camera::kNear,
                           follow_camera::kFar)) {
        return false;
    }
    camera->forward = forward;
    camera->up = facing;
    return true;
}

void FollowCamera::mouseLook(const Player& player, Input& input) {
    // C# FollowCamera.MouseLook (FollowCamera.cs:156-164): nothing in the bomber's
    // top-down view or through the scope.
    if (player.hasTopDownHeight || player.sniping()) {
        return;
    }
    mouseLook(input);
}

void FollowCamera::mouseLook(Input& input) {
    // C# FollowCamera.MouseLook's clamp, for a caller with no Player (the guards are
    // the overload above's).
    _lookOffset = clampf(_lookOffset + input.takeMouseY() * follow_camera::kMouseDegrees,
                         follow_camera::kMinPitch - _arenaPitch - _airPitch,
                         follow_camera::kMaxPitch - _arenaPitch - _airPitch);
}

void FollowCamera::updateAirTime(const Player& player, float delta) {
    // C# UpdateAirTime: starts below FallStartSpeed, ends on rising or landing.
    const float vz = player.verticalSpeed;
    if (_airTime == 0.0f) {
        _airTime = vz < follow_camera::kFallStartSpeed ? delta : 0.0f;
        return;
    }
    const bool landed = vz == 0.0f && player.onFloor;
    _airTime = (vz > 0.0f || landed) ? 0.0f : _airTime + delta;
}

float FollowCamera::climbStep(float climb, float rise, float ticks) {
    // C# FollowCamera.Climb: eased toward the rise; coming back from the other
    // sign it also returns by ClimbReturn per tick, without passing the rise.
    const float keep = powf(follow_camera::kClimbKeep, ticks);
    climb = rise * (1.0f - keep) + climb * keep;
    if (climb < 0.0f && rise >= 0.0f) {
        return mathMin(climb + follow_camera::kClimbReturn * ticks, rise);
    }
    if (climb > 0.0f && rise <= 0.0f) {
        return mathMax(climb - follow_camera::kClimbReturn * ticks, rise);
    }
    return climb;
}

void FollowCamera::updateClimb(float feetZ, float delta) {
    // C# UpdateClimb: the rise is measured once a tick (the original's 30 fps),
    // so Kurt's 60 steps a second down a slope do not shake the view.
    if (!_haveLastFeetZ) {
        _lastFeetZ = feetZ;
        _haveLastFeetZ = true;
    }
    _climbTime += delta * follow_camera::kTicks;
    if (_climbTime < 1.0f) {
        return;
    }
    const float ticks = floorf(_climbTime);
    _climbTime -= ticks;
    const float rise = clampf((feetZ - _lastFeetZ) / ticks, -follow_camera::kClimbClamp,
                              follow_camera::kClimbClamp);
    _lastFeetZ = feetZ;
    _climb = climbStep(_climb, rise, ticks);
}

void FollowCamera::raiseShake(float amount) {
    // C# RaiseShake: `_shake = MathF.Max(_shake, amount)`.
    _shake = mathMax(_shake, amount);
}

void FollowCamera::raiseShakeHook(void* camera, float amount) {
    static_cast<FollowCamera*>(camera)->raiseShake(amount);
}

float FollowCamera::shakeTurnX() const {
    // C# `ShakeOffset * float.DegreesToRadians(FieldOfView) / ViewHeight`, per component.
    return _shakeOffsetX * follow_camera::degreesToRadians(follow_camera::kFieldOfView) /
           follow_camera::kViewHeight;
}

float FollowCamera::shakeTurnY() const {
    return _shakeOffsetY * follow_camera::degreesToRadians(follow_camera::kFieldOfView) /
           follow_camera::kViewHeight;
}

void FollowCamera::updateShake(float delta) {
    // C# UpdateShake (FollowCamera.cs:216-242): a new random offset each tick (the
    // original's 30 a second) while the shake is above 1; it drains by 0.25 a tick and
    // stops below that.
    if (_shake <= 0.0f) {
        _shakeOffsetX = 0.0f;
        _shakeOffsetY = 0.0f;
        return;
    }
    _shakeTime += delta * follow_camera::kTicks;
    // The C#'s `while (_shakeTime >= 1f) _shakeTime -= 1f;` as a whole tick count (T2:
    // no float loop condition). Taking 1 off a float of at least 1 is exact (and the
    // shipped step adds exactly 0.5), so subtracting the floor once leaves the same
    // remainder the repeated subtraction does.
    const float whole = floorf(_shakeTime);
    _shakeTime -= whole;
    const int32_t ticks = (int32_t)whole;
    for (int32_t i = 0; i < ticks; i++) {
        _shakeOffsetX = 0.0f;
        _shakeOffsetY = 0.0f;
        if (_shake > follow_camera::kShakeMin) {
            // `new Vector2(Random(amplitude), Random(amplitude))`: X's draw first (C#
            // arguments run left to right); Random(a) = (NextSingle() * 2 - 1) * a. Then
            // Vector2.Round(Vector2.Clamp(offset, -limit, limit)), rounding half to even.
            const float amplitude = _shake * follow_camera::kShakeScale;
            const float x = (_random.nextSingle() * 2.0f - 1.0f) * amplitude;
            const float y = (_random.nextSingle() * 2.0f - 1.0f) * amplitude;
            _shakeOffsetX = nearbyintf(clampf(x, -follow_camera::kShakeLimitX,
                                              follow_camera::kShakeLimitX));
            _shakeOffsetY = nearbyintf(clampf(y, -follow_camera::kShakeLimitY,
                                              follow_camera::kShakeLimitY));
        }
        _shake -= follow_camera::kShakeDrain;
        if (_shake < follow_camera::kShakeDrain) {
            _shake = 0.0f;
        }
    }
}

void FollowCamera::update(Camera* camera, ArenaSpace* space, Player& player, float arenaPitchTarget,
                          float delta, float aspect) {
    // C# FollowCamera.cs:98-102: the bomber's view, and nothing else this step.
    if (player.hasTopDownHeight) {
        lookDown(camera, player.feet, player.facing(), player.topDownHeight, aspect);
        return;
    }
    // Ease the arena pitch toward its target: C# `float.Lerp(target, old, Pow(ease,
    // dt))`, and Single.Lerp's arithmetic is the FUSED `from*(1-t) + to*t` -- see the
    // measurement on frame_blend::lerp, whose scalar twin this now matches. The
    // twice-rounded `target + (old-target)*t` this used sits 1 ULP off the oracle on
    // most frames (87 of the probe's 200 random triples). On 256 fresh triples, the
    // OTHER twice-rounded form `from*(1-t) + to*t` is wrong on 53 and the fused form on
    // 0). The LEAF is measured too: .NET 10 `MathF.Pow(0.85f, 0.5f)` is 0x3F6C0535
    // and host `powf` gives
    // the same bits, and 0.5 is the only exponent the shipped fixed step can produce
    // (delta 1/60 x kTicks 30, and mdk_linux/src/main.cpp:523 is the only shipped caller).
    // What stays unmeasured is newlib's powf on the target -- the same libm-leaf gap as
    // sinf/cosf, docs/OPEN_ITEMS.md -- and the blend pin below covers the result either way.
    const float ease = powf(follow_camera::kPitchEase, delta * follow_camera::kTicks);
    _arenaPitch = fmaf(arenaPitchTarget, 1.0f - ease, _arenaPitch * ease);
    // The per-step input.MouseY (C# line 108) is always zero: the mouse look is
    // applied per frame in mouseLook, which consumes the accumulated mouse.
    updateAirTime(player, delta);
    updateClimb(player.feet.z, delta);
    // C# FollowCamera.cs:113-117: the end of a level lifts Kurt (Kurt.Rising), and
    // then there is no tilt in the air (end_level.gd); otherwise the air pitch decays
    // on the ground or grows in the air.
    if (player.rising) {
        _airPitch = 0.0f;
    } else if (_airTime == 0.0f) {
        _airPitch = mathMax(_airPitch - follow_camera::kAirPitchDecay * delta, 0.0f);
    } else {
        _airPitch = mathMax(_airPitch,
                            mathMin(_airTime * follow_camera::kAirPitchRate, follow_camera::kAirPitchMax));
    }
    _lookOffset = clampf(_lookOffset, follow_camera::kMinPitch - _arenaPitch - _airPitch,
                         follow_camera::kMaxPitch - _arenaPitch - _airPitch);
    // CameraTilt and CameraPivot live on the Player (C# Kurt.Ride), and CameraRoll
    // is read off it here exactly as the C# reads it off Kurt (FollowCamera.cs:139).
    const float degrees =
        _arenaPitch + _lookOffset + _airPitch - follow_camera::kClimbPitch * _climb + player.cameraTilt;
    // C# FollowCamera.cs:137-139: Orient, UpdateShake, then Turn by the shake and the roll.
    // The shake's update reads nothing the placement writes, so it runs before the build
    // that does Point, Clear, Orient and Turn together.
    updateShake(delta);
    buildFollowCameraCleared(camera, space, player, degrees, player.cameraPivot,
                             aspect, player.cameraRoll().roll(), delta, &clipView, shakeTurnX(),
                             shakeTurnY());

    // The Aim the next Blend interpolates (C# FollowCamera.cs:141-143): the
    // pitch WITHOUT the mouse look (Blend adds the current look on top), the
    // pivot and the roll. The first step makes previous and current the same;
    // after that the previous one is the one before this step.
    const Aim aim{
        _arenaPitch + _airPitch - follow_camera::kClimbPitch * _climb + player.cameraTilt,
        player.cameraPivot,
        player.cameraRoll().roll()};
    if (!_haveAim) {
        _previousAim = aim;
        _haveAim = true;
    } else {
        _previousAim = _aim;
    }
    _aim = aim;
}

bool FollowCamera::blend(Camera* camera, const Player& player, const Vec3& feet, float yaw,
                         float alpha, float aspect) {
    // C# FollowCamera.Blend (FollowCamera.cs:170-181): the facing of the drawn yaw, and
    // in the bomber's view straight down from its height over the drawn feet.
    if (player.hasTopDownHeight) {
        if (camera == nullptr) {
            return false;
        }
        const float yawRadians = follow_camera::degreesToRadians(yaw);
        const Vec3 facing{fastCos(yawRadians), fastSin(yawRadians), 0.0f};
        return lookDown(camera, feet, facing, player.topDownHeight, aspect);
    }
    return blend(camera, feet, yaw, alpha, aspect);
}

bool FollowCamera::blend(Camera* camera, const Vec3& feet, float yaw, float alpha, float aspect) {
    // C# Blend returns immediately before the first step; after one step the
    // previous Aim and the current Aim are the same, which makes that frame
    // hold the step's view.
    if (!_haveAim || camera == nullptr) {
        return false;
    }

    const float yawRadians = follow_camera::degreesToRadians(yaw);
    const Vec3 facing{fastCos(yawRadians), fastSin(yawRadians), 0.0f};
    const float pitchDegrees = frame_blend::lerp(_previousAim.pitch, _aim.pitch, alpha) + _lookOffset;
    const float pitch = follow_camera::degreesToRadians(pitchDegrees);
    const float pivot = frame_blend::lerp(_previousAim.pivot, _aim.pivot, alpha);
    const Vec3 position = followCameraPoint(feet, facing, pitch, pivot);
    Vec3 forward;
    Vec3 up;
    followCameraOrient(facing, pitch, &forward, &up);
    // C# Blend's Turn: the last step's shake offset, the blended roll.
    followCameraTurn(&forward, &up, shakeTurnX(), shakeTurnY(),
                     frame_blend::lerp(_previousAim.roll, _aim.roll, alpha));
    if (!buildLookAtCamera(camera, position,
                           Vec3{position.x + forward.x, position.y + forward.y,
                                position.z + forward.z},
                           up, follow_camera::kFieldOfView, aspect, follow_camera::kNear,
                           follow_camera::kFar)) {
        return false;
    }
    // The C#'s Blend leaves Position, Forward and Up set (Orient and Turn write them,
    // FollowCamera.cs:180-185), so the blended view is readable from the camera the
    // same way the step's is. Without these two the frame would draw on the blended
    // matrix while anything reading the view still saw the last step's.
    camera->forward = forward;
    camera->up = up;
    return true;
}

} // namespace mdk

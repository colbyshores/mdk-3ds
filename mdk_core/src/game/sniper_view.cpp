// ============================================================================
// sniper_view.cpp — the port of src/Mdk.Game/SniperView.cs. See sniper_view.h.
// ============================================================================

#include "game/sniper_view.h"

#include <cmath>
#include <cstring>

#include "game/follow_camera.h"
#include "game/player.h"
#include "game/scope.h"
#include "scripts/script_runtime.h"
#include "scripts/sniper_rounds.h"

namespace mdk {

namespace {

// float.Pi, and .NET's float.DegreesToRadians / RadiansToDegrees: the product
// first, then the quotient ((x * Pi) / 180, (x * 180) / Pi).
const float kPi = 3.14159265358979323846f;
float toRadians(float degrees) { return degrees * kPi / 180.0f; }
float toDegrees(float radians) { return radians * 180.0f / kPi; }

// The loaded rounds (0x41eb10): round i at time clip timer + i (shown between 0
// and 3, not at 0), between keys of position, angles (z, y, x: Rz(a) Ry(-b)
// Rx(c)) and scale (0x490e4c), seen from the origin along -Y (right +X, down
// +Z) with a focal length of 250 on the view.
struct ClipKey {
    Vec3 position;
    Vec3 angles;
    float scale;
};
const ClipKey kClipKeys[4] = {
    {{-225.0f, -242.0f, 65.0f}, {-30.0f, 180.0f, 90.0f}, 4.0f},
    {{-203.0f, -242.0f, 107.0f}, {0.0f, 180.0f, 0.0f}, 9.0f},
    {{-176.0f, -242.0f, 145.0f}, {0.0f, 180.0f, 0.0f}, 9.0f},
    {{-154.0f, -242.0f, 174.0f}, {0.0f, 180.0f, 0.0f}, 9.0f},
};
const int32_t kClipKeyCount = 4;
const char* const kRoundModels[6] = {"SW_SHOT",  "SW_HOME",  "SW_SGREN",
                                     "SW_HGREN", "SW_LGREN", "SW_BONES"};

// Vector3.Lerp on the oracle's host is MultiplyAddEstimate (fused, as the
// rounds' and the tube basis's); float.Lerp is fused too (frame_blend.cpp).
Vec3 lerp(const Vec3& a, const Vec3& b, float t) {
    const float keep = 1.0f - t;
    return Vec3{std::fma(a.x, keep, b.x * t), std::fma(a.y, keep, b.y * t),
                std::fma(a.z, keep, b.z * t)};
}
float lerp(float a, float b, float t) { return std::fma(a, 1.0f - t, b * t); }

// Matrix4x4.CreateRotationX / Y / Z (System.Numerics' row-vector forms).
/* @vfp-exempt R6 — the clip's loaded rounds turn as .NET's CreateRotationX/Y/Z do, with
 * MathF.Cos/Sin; glibc cosf/sinf match them bit for bit on the oracle's host
 * (golden/L8_sniper_hud_ship.txt, rot=) and the fastSin/fastCos LUT would not. Cold: three
 * rounds a frame in sniper mode. measured n/a (corpus empty). Verified 2026-11-20. */
Mat4 rotationX(float radians) {
    const float c = cosf(radians);
    const float s = sinf(radians);
    Mat4 m = Mat4::identity();
    m.m[5] = c;
    m.m[6] = s;
    m.m[9] = -s;
    m.m[10] = c;
    return m;
}
Mat4 rotationY(float radians) {
    const float c = cosf(radians);
    const float s = sinf(radians);
    Mat4 m = Mat4::identity();
    m.m[0] = c;
    m.m[2] = -s;
    m.m[8] = s;
    m.m[10] = c;
    return m;
}
/* @vfp-exempt R6 — CreateRotationZ's MathF.Cos/Sin, as rotationX's above. measured n/a
 * (corpus empty). Verified 2026-11-20. */
Mat4 rotationZ(float radians) {
    const float c = cosf(radians);
    const float s = sinf(radians);
    Mat4 m = Mat4::identity();
    m.m[0] = c;
    m.m[1] = s;
    m.m[4] = -s;
    m.m[5] = c;
    return m;
}

} // namespace

bool buildViewCamera(Camera* camera, const Vec3& position, const Vec3& forward, const Vec3& up,
                     float fieldOfView, float aspect, float nearPlane, float farPlane) {
    // C# CameraMath.View: CreateLookAt(position, position + forward, up) -- the
    // ROUNDED target's direction, as buildScopeCamera's -- and
    // CreatePerspectiveFieldOfView(DegreesToRadians(fieldOfView), aspect, near, far):
    // height = 1 / tan(fov / 2), width = height / aspect, range = far / (near - far),
    // M33 = range, M34 = -1, M43 = range * near.
    if (camera == nullptr) {
        return false;
    }
    const Vec3 target{position.x + forward.x, position.y + forward.y, position.z + forward.z};
    const Vec3 back{-(target.x - position.x), -(target.y - position.y), -(target.z - position.z)};
    const Vec3 zaxis = numerics::normalize(back);
    const Vec3 xaxis = numerics::normalize(numerics::cross(up, zaxis));
    const Vec3 yaxis = numerics::cross(zaxis, xaxis);
    if (!(xaxis.x == xaxis.x) || !(zaxis.x == zaxis.x)) {
        return false;
    }
    const Vec3 negEye{-position.x, -position.y, -position.z};
    float* view = camera->view;
    view[0] = xaxis.x;  view[1] = yaxis.x;  view[2] = zaxis.x;   view[3] = 0.0f;
    view[4] = xaxis.y;  view[5] = yaxis.y;  view[6] = zaxis.y;   view[7] = 0.0f;
    view[8] = xaxis.z;  view[9] = yaxis.z;  view[10] = zaxis.z;  view[11] = 0.0f;
    view[12] = numerics::dot(xaxis, negEye);
    view[13] = numerics::dot(yaxis, negEye);
    view[14] = numerics::dot(zaxis, negEye);
    view[15] = 1.0f;

    /* @vfp-exempt R6 — CreatePerspectiveFieldOfView's MathF.Tan, matched by glibc tanf
     * (golden/L8_sniper_hud_*.txt, view=/proj=). Cold: four views a frame in sniper mode.
     * measured n/a (corpus empty). Verified 2026-11-20. */
    const float height = 1.0f / tanf(toRadians(fieldOfView) * 0.5f);
    const float width = height / aspect;
    const float range = farPlane / (nearPlane - farPlane);
    float* projection = camera->projection;
    std::memset(projection, 0, sizeof(camera->projection));
    projection[0] = width;
    projection[5] = height;
    projection[10] = range;
    projection[11] = -1.0f;
    projection[14] = range * nearPlane;

    camera->position = position;
    camera->forward = forward;
    camera->up = up;
    camera->near_plane = nearPlane;
    camera->far_plane = farPlane;
    return true;
}

void SniperView::clipPlace(float t, Mat4* rotation, Vec3* position, float* scale) {
    // C# ClipPlace (SniperView.cs:151-162).
    const int32_t k = (int32_t)t;
    const float f = t - (float)k;
    const int32_t a = k < 0 ? 0 : (k > kClipKeyCount - 2 ? kClipKeyCount - 2 : k);
    const ClipKey& from = kClipKeys[a];
    const ClipKey& to = kClipKeys[a + 1];
    const Vec3 angles = lerp(from.angles, to.angles, f);
    *rotation = multiply(multiply(rotationX(toRadians(angles.z)), rotationY(-toRadians(angles.y))),
                         rotationZ(toRadians(angles.x)));
    *position = lerp(from.position, to.position, f);
    *scale = lerp(from.scale, to.scale, f);
}

SniperOverlay::Rect SniperView::onCanvas(const SniperOverlay::Rect& inView, float scale,
                                         float left) {
    // C# OnCanvas: (left + (View.X + x) * scale, (View.Y + y) * scale, w * scale, h * scale).
    SniperOverlay::Rect r;
    r.x = left + (SniperOverlay::kViewX + inView.x) * scale;
    r.y = (SniperOverlay::kViewY + inView.y) * scale;
    r.w = inView.w * scale;
    r.h = inView.h * scale;
    return r;
}

/* @vfp-exempt R6 — DrawRoundCamera's and DrawClip's fields of view, MathF.Atan/Tan in the
 * C#, matched by glibc atanf/tanf (golden/L8_sniper_hud_*.txt). Cold: once per view per
 * frame in sniper mode. measured n/a (corpus empty). Verified 2026-11-20. */
bool SniperView::roundCameraView(Camera* camera, const scripts::SniperRounds::RoundCamera& round,
                                 const SniperOverlay::Rect& area) {
    // C# DrawRoundCamera's view (SniperView.cs:100-104): 90 degrees wide.
    const float aspect = area.w / area.h;
    const float fieldOfView =
        toDegrees(2.0f * atanf(tanf(toRadians(kRoundFieldWidth) / 2.0f) / aspect));
    return buildViewCamera(camera, round.position, round.forward, followCameraUpOf(round.forward),
                           fieldOfView, aspect, kRoundNear, kFar);
}

bool SniperView::clipView(Camera* camera, const SniperOverlay::Rect& area) {
    // C# DrawClip's view (SniperView.cs:118-119): from the origin along -Y, up -Z.
    const float fieldOfView = toDegrees(2.0f * atanf(SniperOverlay::kViewHeight / 2.0f / kClipFocal));
    return buildViewCamera(camera, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f},
                           Vec3{0.0f, 0.0f, -1.0f}, fieldOfView, area.w / area.h, kClipNear,
                           kClipFar);
}

StrikeAim SniperView::aim(const Player& player, const scripts::ScriptRuntime* scripts) {
    // C# Aim (SniperView.cs:79-87): with the strike selected and the clip ready,
    // AirStrike.FindTarget from the sniper eye along its forward (package #14's,
    // scripts/air_strike.h). Without a runtime there is nothing to find.
    if (player.combat().selectedAmmo() != Scope::kStrike || player.scope().clipTime > 0.0f) {
        return StrikeAim::NotChecked;
    }
    Vec3 target;
    if (scripts != nullptr &&
        scripts->airStrike().findTarget(player.sniperEye(), player.sniperForward(), &target)) {
        return StrikeAim::Target;
    }
    return StrikeAim::None;
}

SniperHud SniperView::hudOf(const Player& player, const scripts::ScriptRuntime* scripts) {
    // C# SniperView.Draw (SniperView.cs:59-67): new SniperHud(kurt.Scope.Zoom,
    // kurt.Inventory.Ammo, kurt.Inventory.SelectedAmmo, cameras, Aim(kurt, scripts)).
    SniperHud hud;
    hud.zoom = player.scope().zoom;
    for (int32_t i = 0; i < 5; i++) {
        hud.ammo[i] = player.combat().ammo(i + 1);
    }
    hud.selectedAmmo = player.combat().selectedAmmo();
    if (scripts != nullptr) {
        for (int32_t i = 0; i < scripts::SniperRounds::kSlots; i++) {
            hud.cameras[i] = scripts->sniperRounds().camera(i);
        }
        hud.cameraCount = scripts::SniperRounds::kSlots;
    }
    hud.aim = aim(player, scripts);
    return hud;
}

const char* SniperView::roundModel(int32_t type) {
    return kRoundModels[type < 0 ? 0 : (type > 5 ? 5 : type)];
}

} // namespace mdk

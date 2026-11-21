// strike_scene.cpp — the full-screen strike: a port of src/Mdk.Game/Scripts/StrikeScene.cs.
// See strike_scene.h.

#include "scripts/strike_scene.h"

#include <math.h>

#include <cmath>
#include <string>

#include "formats/ascii_case.h"
#include "formats/model.h"
#include "formats/model_animation.h"
#include "game/kurt.h"
#include "scripts/script_runtime.h"

namespace mdk {
namespace scripts {

namespace {

// C# StrikeScene's constants (StrikeScene.cs:24-32).
const float kTurnRate = 45.0f;
const float kViewHeight = 360.0f;
const int32_t kCameraPoint = 1;
const int32_t kLookPoint = 0;
const int32_t kNeededPoints = 2;
// When Bones dives alone, only the plane's parts show (0x491ddc).
const char* const kPlaneParts[] = {"AWING", "CANOPY", "LEVER", "LEVER01", "LEVER02", "LEVER03",
                                   "OBJECT"};

// C# float.DegreesToRadians: multiply by pi before dividing by 180 (as object.cpp's).
float degreesToRadians(float degrees) {
    const float pi = 3.14159265358979323846f;
    return degrees * pi / 180.0f;
}

// Vector3.Transform(v, Matrix4x4.CreateRotationZ(radians)) as the .NET 10 host computes
// it: CreateRotationZ's M11 = cos, M12 = sin, M21 = -sin, M22 = cos (MathF: libm), and
// Transform's row X times x, then Y and Z fused in, then the translation row (zero) --
// sniper_rounds.cpp's transformPoint with this matrix written out.
Vec3 turnedZ(const Vec3& p, float radians) {
    /* @vfp-exempt R6 — the strike scene's camera is the oracle's MathF rotation to the
     * bit (golden/L5_strike_*.txt), once a frame while the scene plays: cold, not hot.
     * measured n/a. Verified 2026-11-20. */
    const float c = cosf(radians);
    const float s = sinf(radians);
    const float m[16] = {c, s, 0.0f, 0.0f, -s, c, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                         0.0f, 0.0f, 0.0f, 1.0f};
    const float x = std::fma(p.z, m[8], std::fma(p.y, m[4], p.x * m[0])) + m[12];
    const float y = std::fma(p.z, m[9], std::fma(p.y, m[5], p.x * m[1])) + m[13];
    const float z = std::fma(p.z, m[10], std::fma(p.y, m[6], p.x * m[2])) + m[14];
    return Vec3{x, y, z};
}

} // namespace

float StrikeScene::fieldOfView() {
    // C# FieldOfView = RadiansToDegrees(2f * MathF.Atan(ViewHeight / 2f / Focal)), with
    // Focal = 600f / 0.35265395f.
    const float focal = 600.0f / 0.35265395f;
    const float pi = 3.14159265358979323846f;
    return 2.0f * atanf(kViewHeight / 2.0f / focal) * 180.0f / pi;
}

void StrikeScene::camera(Vec3* eye, Vec3* target) const {
    // C# Camera (StrikeScene.cs:50-57): both reference points turned by the yaw.
    const float radians = degreesToRadians(yaw_);
    *eye = turnedZ(referencePoint(kCameraPoint), radians);
    *target = turnedZ(referencePoint(kLookPoint), radians);
}

void StrikeScene::play(ScriptRuntime& runtime, Kind kind, Plane plane) {
    // C# Play (StrikeScene.cs:60-79).
    const std::string arena = runtime.currentArena();
    const char* name = kind == Bones ? "X_STRIKB" : "X_STRIKD";
    const formats::Model* model = runtime.findModel(arena, name);
    animation_ = kind == Bones ? runtime.itemsAnimation(name)
                               : runtime.findArenaAnimation(arena, name);
    if (model == nullptr || animation_ == nullptr || animation_->frameCount == 0 ||
        (int32_t)model->referencePoints.size() < kNeededPoints) {
        // Finished?.Invoke(): the runtime's StrikeSceneChanged(Ended), which nothing reads.
        return;
    }

    subject_.type_name = name;
    subject_.model = model;
    subject_.arena = arena;
    subject_.animation = animation_;
    subject_.hidden_parts = plane == Only ? hiddenParts(*model) : 0;
    active_ = true;
    show();
}

int32_t StrikeScene::hiddenParts(const formats::Model& model) {
    // C# HiddenParts (StrikeScene.cs:81-93): every part not of the plane.
    int32_t hidden = 0;
    for (size_t i = 0; i < model.partList.size(); i++) {
        const std::string upper = formats::toUpperAscii(model.partList[i].nameString());
        bool plane = false;
        for (size_t p = 0; p < sizeof(kPlaneParts) / sizeof(kPlaneParts[0]); p++) {
            if (upper == kPlaneParts[p]) {
                plane = true;
            }
        }
        if (!plane) {
            hidden |= 1 << i;
        }
    }
    return hidden;
}

void StrikeScene::update(float delta) {
    // C# Update (StrikeScene.cs:96-111): delta * Kurt.Ticks * Speed, left to right.
    if (!active_) {
        return;
    }
    frame_ += delta * ::mdk::Kurt::kTicks * animation_->speed;
    yaw_ += kTurnRate * delta;
    if (frame_ >= (float)animation_->frameCount) {
        skip();
        return;
    }
    show();
}

void StrikeScene::skip() {
    // C# Skip (StrikeScene.cs:114-123); Finished is the runtime's unread event.
    if (!active_) {
        return;
    }
    active_ = false;
}

void StrikeScene::show() {
    // C# Show (StrikeScene.cs:125-129): the frame truncated, as (int)_frame.
    subject_.animation_frame = (int32_t)frame_;
    subject_.yaw = yaw_;
}

Vec3 StrikeScene::referencePoint(int32_t index) const {
    // C# ReferencePoint (StrikeScene.cs:132-139): the animation moves the points.
    if (animation_ != nullptr && index < (int32_t)animation_->referencePoints.size()) {
        int32_t frame = (int32_t)frame_;
        if (frame > animation_->frameCount - 1) {
            frame = animation_->frameCount - 1;
        }
        const formats::Vec3f& p = animation_->referencePoints[(size_t)index][(size_t)frame];
        return Vec3{p.x, p.y, p.z};
    }
    if (subject_.model != nullptr) {
        const formats::Vec3f& p = subject_.model->referencePoints[(size_t)index];
        return Vec3{p.x, p.y, p.z};
    }
    return Vec3{};
}

// C# ScriptRuntime.PlayStrikeScene (ScriptRuntime.cs:1697-1704): a new scene each time, then
// Play. Defined here, beside the scene, to keep package 14 out of script_runtime.cpp. The C#'s
// StrikeSceneChanged(Started / Ended) has no subscriber in src/, so it is not ported.
void ScriptRuntime::playStrikeScene(StrikeScene::Kind kind, StrikeScene::Plane plane) {
    strikeScene_ = StrikeScene();
    strikePlayed_ = true;
    strikeScene_.play(*this, kind, plane);
}

} // namespace scripts
} // namespace mdk

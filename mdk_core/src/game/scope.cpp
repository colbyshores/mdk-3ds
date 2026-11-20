// ============================================================================
// scope.cpp — sniper mode's scope (src/Mdk.Game/Kurt/Scope.cs). See scope.h.
// ============================================================================

#include "game/scope.h"

#include <cmath>

#include "game/follow_camera.h"
#include "game/kurt.h"
#include "util/math_minmax.h"

namespace mdk {

namespace numerics {

float dot(const Vec3& a, const Vec3& b) {
    // Vector3.Dot as the .NET 10 runtime computes it on the oracle's host: each product
    // rounded, summed left to right, NO fused multiply-add. Measured against the oracle:
    // the fmaf chain render/post_params.cpp uses for its normalize puts the scope view's
    // translation (Dot(axis, -eye)) one ULP off on L3 step 60 (0xC3354519 for the
    // oracle's 0xC3354518), and this sum matches every matrix of golden/L*_sniper.txt.
    // (The multiply of two matrices is a different runtime path and IS fused: tick1's
    // sniperMultiply.) Neither target contracts this: x86-64 without -mfma has no FMA,
    // and the ARM11's VFPv2 multiply-accumulate rounds the product.
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 cross(const Vec3& a, const Vec3& b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

Vec3 normalize(const Vec3& v) {
    // Vector3.Normalize: the vector divided by its Length (a true divide, not a
    // reciprocal multiply: the two differ in the last bit).
    const float length = std::sqrt(dot(v, v));
    return Vec3{v.x / length, v.y / length, v.z / length};
}

} // namespace numerics

namespace {

// Kurt.Ticks (the original's 30 ticks a second), the one constant both halves share.
const float kTicks = Kurt::kTicks;

// Looking around (Scope.cs:43-50): degrees per second (0.4/0.6 per tick², at most 4/6
// per tick), scaled by zoom x 0.416667.
const float kLookAcceleration = 0.4f * kTicks * kTicks;
const float kLookAccelerationTurbo = 0.6f * kTicks * kTicks;
const float kLookSpeed = 4.0f * kTicks;
const float kLookSpeedTurbo = 6.0f * kTicks;
const float kLookFrictionSlow = 1.0667f * kTicks * kTicks;
const float kLookFrictionFast = 1.6f * kTicks * kTicks;
const float kLookScale = 0.416667f;

// The zoom speed grows by 0.01 per tick up to 0.15 while a zoom key is held and
// decays by 0.015; a wheel notch holds the zoom key for 6 ticks (Scope.cs:52-57).
const float kZoomAcceleration = 0.01f;
const float kZoomMaxSpeed = 0.15f;
const float kZoomDecay = 0.015f;
const float kWheelTicks = 6.0f;

// The clip timer (0x5743eb) drops by 4 per second; a shot adds 1, an empty clip sets
// 3; a shot needs 5 ticks since the last one; normal bullets never run out
// (Scope.cs:59-66).
const float kClipDecay = 4.0f;
const float kShotTime = 1.0f;
const float kReloadTime = 3.0f;
const float kShotGap = 5.0f / kTicks;
const int32_t kEndlessStock = 999;

// The target lock's square around the crosshair (0x43b65c), pixels; the zoom limit on
// a locked target is 0.375 x its height (at least 10) / distance (0x4678b0).
const float kLockHalf = 32.0f;
const float kLockZoomFactor = 0.375f;
const float kLockMinHeight = 10.0f;

// The eye: 4 above the feet, and forward by 5 (1 - cos pitch) looking down.
const float kEyeHeight = 4.0f;
const float kEyeForward = 5.0f;

float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

float signf(float v) {
    return (float)((v > 0.0f) - (v < 0.0f));
}

// Scope.MoveToward.
float moveToward(float from, float to, float step) {
    return std::fabs(to - from) <= step ? to : from + signf(to - from) * step;
}

} // namespace

void Scope::reset() {
    // C# Scope.Reset (Scope.cs:95-105). The wheel's direction is not reset there either.
    pitch = 0.0f;
    zoom = 1.0f;
    clipRounds = 0;
    clipTime = 0.0f;
    lookSpeedX_ = 0.0f;
    lookSpeedY_ = 0.0f;
    zoomSpeed_ = 0.0f;
    wheelTicks_ = 0.0f;
    sinceShot_ = 3.40282347e+38f;
}

float Scope::look(float turn, float tilt, float mouseX, float mouseY, Turbo turbo, float delta) {
    // C# Scope.Look (Scope.cs:109-116), in its order: both speeds, then the pitch
    // (clamped to ±50), and the yaw change returned.
    lookSpeedX_ = accelerate(lookSpeedX_, turn, turbo, delta);
    lookSpeedY_ = accelerate(lookSpeedY_, tilt, turbo, delta);
    const float scale = zoom * kLookScale * delta;
    pitch = clampf(pitch + lookSpeedY_ * scale + mouseY * zoom, -kPitchLimit, kPitchLimit);
    return -(lookSpeedX_ * scale + mouseX * zoom);
}

float Scope::accelerate(float speed, float input, Turbo turbo, float delta) {
    // C# Scope.Accelerate (Scope.cs:118-133): friction without input, the opposite
    // direction reverses at once.
    if (input == 0.0f) {
        const float friction = std::fabs(speed) > kLookSpeed ? kLookFrictionFast : kLookFrictionSlow;
        return moveToward(speed, 0.0f, friction * delta);
    }
    const float max = turbo == TurboOn ? kLookSpeedTurbo : kLookSpeed;
    if (speed * input < 0.0f) {
        speed = 0.0f;
    }
    const float acceleration = turbo == TurboOn ? kLookAccelerationTurbo : kLookAcceleration;
    return clampf(speed + input * acceleration * delta, -max, max);
}

void Scope::wheel(float notches) {
    // C# Scope.Wheel (Scope.cs:136-147): a notch away from the user zooms in (-1).
    if (notches == 0.0f) {
        return;
    }
    const float direction = notches > 0.0f ? -1.0f : 1.0f;
    const float ticks = kWheelTicks * std::fabs(notches);
    wheelTicks_ = direction != wheelDirection_ ? ticks : wheelTicks_ + ticks;
    wheelDirection_ = direction;
}

Scope::Zooming Scope::updateZoom(float direction, float delta) {
    // C# Scope.UpdateZoom (Scope.cs:151-184). Zooming out multiplies the zoom by
    // (1 + v)^ticks, zooming in divides it by (1 - v)^ticks, between 1 and the limit.
    // powf's exponent is the step in ticks: 0.5 at the fixed 1/60 step, where host
    // powf and .NET's MathF.Pow agree (golden/L*_sniper.txt; the same leaf the follow
    // camera's PitchEase uses). newlib's powf on the 3DS is the open libm-leaf gap
    // docs/OPEN_ITEMS.md records for that one.
    const float ticks = delta * kTicks;
    if (wheelTicks_ > 0.0f) {
        wheelTicks_ -= ticks;
        if (direction == 0.0f) {
            direction = wheelDirection_;
        }
    }

    if (direction != 0.0f) {
        zoomSpeed_ = clampf(zoomSpeed_ + direction * kZoomAcceleration * ticks, -kZoomMaxSpeed,
                            kZoomMaxSpeed);
    } else {
        zoomSpeed_ = moveToward(zoomSpeed_, 0.0f, kZoomDecay * ticks);
    }

    if (zoomSpeed_ > 0.0f) {
        zoom = mathMin(zoom * powf(1.0f + zoomSpeed_, ticks), 1.0f);
    } else if (zoomSpeed_ < 0.0f) {
        // Past a lost lock's limit, the zoom stays: snapping back to it each time the
        // lock flickers jerks the view (the C#'s own note).
        zoom = mathMax(zoom / powf(1.0f - zoomSpeed_, ticks), mathMin(zoomLimit, zoom));
    }

    const bool moving = direction != 0.0f && (zoom < 1.0f || direction < 0.0f) &&
                        (zoom > zoomLimit || direction > 0.0f);
    return moving ? Moving : Still;
}

int32_t Scope::updateClip(Kurt& inventory, Trigger trigger, const FireHook& fire, float delta) {
    // C# Scope.UpdateClip (Scope.cs:189-226), in its order: a round loads per step
    // while the timer runs (only as many as there's ammo for, but normal bullets
    // never run out); an empty clip with the timer out reloads -- falling back to
    // bullets when the selected type ran out; the timer drops; then a shot, which
    // needs the trigger, the timer at 0, 5 ticks since the last shot, a round in the
    // clip and a free round slot (the hook; null is the C#'s `_ => true`). The hook
    // is asked LAST, as the C#'s || chain does, so a refused shot changes nothing.
    int32_t result = ClipNone;
    sinceShot_ += delta;
    const int32_t type = inventory.selectedAmmo();
    const int32_t stock = type == kBullets ? kEndlessStock : inventory.ammo(type);
    const int32_t loadable = stock < kClipSize ? stock : kClipSize;
    if (clipTime > 0.0f && clipRounds < loadable) {
        clipRounds++;
        result |= ClipLoaded;
    }

    if (clipRounds == 0 && clipTime <= 0.0f) {
        if (type != kBullets && stock <= 0) {
            inventory.selectAmmo(kBullets);
        }
        clipTime = kReloadTime;
    }

    clipTime = mathMax(clipTime - kClipDecay * delta, 0.0f);
    if (trigger == Released || clipTime > 0.0f || sinceShot_ < kShotGap || clipRounds <= 0) {
        return result;
    }
    if (fire.fire != nullptr && !fire.fire(fire.context, type)) {
        return result;
    }

    sinceShot_ = 0.0f;
    clipRounds--;
    inventory.useAmmo(type);
    clipTime += kShotTime;
    if (clipRounds == 0) {
        clipTime = kReloadTime;
    }
    return result | ClipFired;
}

void Scope::selectAmmo(Kurt& inventory, int32_t step) {
    // C# Scope.SelectAmmo (Scope.cs:230-241): a new type empties the clip and reloads.
    const int32_t type = inventory.nextAmmo(step);
    if (type == inventory.selectedAmmo()) {
        return;
    }
    inventory.selectAmmo(type);
    clipRounds = 0;
    clipTime = kReloadTime;
}

Vec3 Scope::eye(const Vec3& feet, const Vec3& facing) const {
    // C# Scope.Eye (Scope.cs:244-253): feet + Z x 4, then (looking down) forward by
    // facing x 5 x (1 - cos pitch), multiplied left to right as the C#'s
    // `facing * EyeForward * (...)` is. `facing` is Player::facing(), the LUT's (the
    // owner-gated docs/OPEN_ITEMS.md item); it enters only while looking down.
    Vec3 eye{feet.x, feet.y, feet.z + kEyeHeight};
    if (pitch > 0.0f) {
        /* @vfp-exempt R6 — the sniper eye is gameplay (#6 fires the rounds from it and
         * the target lock projects from it), and .NET takes MathF.Cos of the pitch; glibc
         * cosf matches it bit for bit on this host (golden/L*_sniper.txt) while the
         * fastSin/fastCos LUT would move the eye. Once a step, not per object.
         * measured n/a (corpus empty). Verified 2026-11-20. */
        const float shift = 1.0f - cosf(follow_camera::degreesToRadians(pitch));
        eye.x += facing.x * kEyeForward * shift;
        eye.y += facing.y * kEyeForward * shift;
        eye.z += facing.z * kEyeForward * shift;
    }
    return eye;
}

Vec3 Scope::direction(float yaw, float pitch) {
    // C# Scope.Direction (Scope.cs:259-264): (cos y cos p, sin y cos p, -sin p), with
    // .NET's two-rounding DegreesToRadians.
    /* @vfp-exempt R6 — the line of sight is gameplay (#6's rounds fly along it and the
     * target lock reads it), and .NET takes MathF.Cos/Sin of the yaw and pitch; glibc
     * cosf/sinf match them bit for bit on this host (golden/L*_sniper.txt) while the
     * fastSin/fastCos LUT would bend the line of sight. A few calls a step, not per
     * object. measured n/a (corpus empty). Verified 2026-11-20. */
    const float y = follow_camera::degreesToRadians(yaw);
    const float p = follow_camera::degreesToRadians(pitch);
    return Vec3{cosf(y) * cosf(p), sinf(y) * cosf(p), -sinf(p)};
}

bool Scope::toScreen(const Vec3& eye, const Vec3& forward, float zoom, const Vec3& point,
                     Vec2* screen) {
    // C# Scope.ToScreen (Scope.cs:268-282): the scope's right and up for the line of
    // sight, the point's depth along it, and the 384/zoom focal length about the
    // centre (319, 279), y down. The C#'s Vector2 `(dx, -dy) * focal / depth` is
    // per component: (d * focal) / depth.
    const Vec3 right = numerics::normalize(numerics::cross(forward, Vec3{0.0f, 0.0f, 1.0f}));
    const Vec3 up = numerics::cross(right, forward);
    const Vec3 offset{point.x - eye.x, point.y - eye.y, point.z - eye.z};
    const float depth = numerics::dot(offset, forward);
    if (depth <= 0.0f) {
        return false;
    }
    const float focal = kFocal / zoom;
    screen->x = kCentreX + numerics::dot(offset, right) * focal / depth;
    screen->y = kCentreY + -numerics::dot(offset, up) * focal / depth;
    return true;
}

bool Scope::locks(const Vec2& min, const Vec2& max) {
    // C# Scope.Locks (Scope.cs:285-286).
    return min.x <= kCentreX + kLockHalf && max.x >= kCentreX - kLockHalf &&
           min.y <= kCentreY + kLockHalf && max.y >= kCentreY - kLockHalf;
}

float Scope::lockLimit(float height, float distance) {
    // C# Scope.LockLimit (Scope.cs:289-290).
    return mathMin(kZoomMin, kLockZoomFactor * mathMax(height, kLockMinHeight) /
                                 mathMax(distance, 1.0f));
}

} // namespace mdk

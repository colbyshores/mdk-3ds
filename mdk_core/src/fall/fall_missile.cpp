#include "fall/fall_missile.h"

#include <math.h>

#include "util/math_minmax.h"

namespace mdk {
namespace fall {

namespace {

// FallMissile.cs constants.
constexpr float kSpeed = 250.0f;
constexpr int32_t kFreeTicks = 60;
constexpr float kPassHeight = 5.0f;
constexpr int32_t kGoneTicks = 60;
// Homing: v = 0.8 v + 0.2 × 250 × unit(aim − position). The C# writes
// `(1f - Homing) * Velocity + Homing * Speed * towards`, and both factors are
// constant expressions the compiler folds, so they are folded here too.
constexpr float kHoming = 0.2f;
constexpr float kKeep = 1.0f - kHoming;
constexpr float kPull = kHoming * kSpeed;
// Below 0.75 × Kurt's height it climbs 3 times as fast.
constexpr float kBoostHeight = 0.75f;
constexpr float kBoost = 3.0f;
// The aim is below Kurt by min(depth / 225, 10) × 66.67.
constexpr float kAimDepth = 225.0f;
constexpr float kAimMax = 10.0f;
constexpr float kAimSpeed = 2000.0f / 30.0f;
// MathF.Tau.
constexpr float kTau = 6.28318548f;

// FallMissile.Spread: uniform in ±spread.
float spreadOf(Random* random, float spread) {
    return (random->nextSingle() * 2.0f - 1.0f) * spread;
}

} // namespace

FallMissile::FallMissile(float spread, Random* random) {
    // C# FallMissile(float spread, Random random). The C# evaluates the
    // Vector2's arguments left to right; C++ does not promise an order, so the
    // two draws are sequenced explicitly.
    const float a = random->nextSingle() * kTau;
    /* @vfp-exempt R6 — missile launch direction, cold: once per launched missile (a few
     * per radar detection), never per frame; MathF.Sin/MathF.Cos are what the C# calls,
     * glibc matches them on the host (golden/L*_skydive_*.txt), and a LUT would move the
     * launch and so every later hit. newlib on the 3DS is unmeasured (docs/OPEN_ITEMS.md).
     * measured n/a (corpus empty). Verified 2026-11-20. */
    velocity =formats::Vec3f{sinf(a) * kSpeed, cosf(a) * kSpeed, 1.0f * kSpeed};
    offsetX_ = spreadOf(random, spread);
    offsetY_ = spreadOf(random, spread);
    ticks_ = (float)kFreeTicks;
}

FallMissile::Event FallMissile::update(float dt, int32_t ticks, const formats::Vec3f& kurt) {
    // C# FallMissile.Update.
    position.x += velocity.x * dt;
    position.y += velocity.y * dt;
    position.z += velocity.z * dt;
    if (position.z < kBoostHeight * kurt.z) {
        position.z += kBoost * velocity.z * dt;
    }

    // _trail.Add per tick, then RemoveRange down to the last kTrailPoints.
    for (int32_t i = 0; i < ticks; i++) {
        if (trailCount_ == kTrailPoints) {
            for (int32_t j = 1; j < kTrailPoints; j++) {
                trail_[j - 1] = trail_[j];
            }
            trailCount_--;
        }
        trail_[trailCount_++] = position;
    }

    switch (phase_) {
        case Free:
            ticks_ -= (float)ticks;
            if (ticks_ <= 0.0f) {
                phase_ = Homing;
            }
            return EventNone;
        case Homing:
            return home(kurt);
        default:
            ticks_ += (float)ticks;
            return ticks_ > (float)kGoneTicks ? EventGone : EventNone;
    }
}

FallMissile::Event FallMissile::home(const formats::Vec3f& kurt) {
    // C# FallMissile.Home.
    const float dz = kurt.z - position.z;
    if (dz <= -kPassHeight) {
        phase_ = Passed;
        ticks_ = 0.0f;
        return EventPassed;
    }

    const formats::Vec3f aim{kurt.x + offsetX_, kurt.y + offsetY_,
                             kurt.z + -mathMin(dz / kAimDepth, kAimMax) * kAimSpeed};
    formats::Vec3f towards{aim.x - position.x, aim.y - position.y, aim.z - position.z};
    if (towards.x != 0.0f || towards.y != 0.0f || towards.z != 0.0f) {
        // Vector3.Normalize: divided by Vector3.Length (sqrt of the plain dot).
        const float length = sqrtf(towards.x * towards.x + towards.y * towards.y + towards.z * towards.z);
        towards = formats::Vec3f{towards.x / length, towards.y / length, towards.z / length};
    }

    velocity = formats::Vec3f{kKeep * velocity.x + kPull * towards.x, kKeep * velocity.y + kPull * towards.y,
                              kKeep * velocity.z + kPull * towards.z};
    return EventNone;
}

} // namespace fall
} // namespace mdk

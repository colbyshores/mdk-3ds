// air_strike.cpp — Bones' air strike: a port of src/Mdk.Game/Scripts/AirStrike.cs.
// See air_strike.h.

#include "scripts/air_strike.h"

#include <math.h>

#include <cmath>
#include <string>

#include "formats/arena.h"
#include "game/kurt.h"
#include "game/level_flow.h"
#include "scripts/script_runtime.h"

namespace mdk {
namespace scripts {

namespace {

// C# AirStrike's constants (AirStrike.cs:22-55).
const float kSpeed = 150.0f;
const float kRange = 5000.0f;
const float kSky = 1000.0f;
const float kHeight = 30.0f;
const float kOverTarget = 32.0f;
const float kBehind = 50.0f;
const int32_t kTeeth = 9;
const float kToothSpacing = 15.0f;
// The middle tooth falls over the target.
const float kToothBias = 5.0f;
const int32_t kToothTicks = 900;
const float kToothFall = -5.0f;
const int32_t kDiveDamage = 450;
const int32_t kDiveKurtDamage = 67;
const float kDiveRadius = 80.0f;
const int32_t kHitDive = -6;
const float kExplosionScale = 2.0f;
const int32_t kTargetsAll = 6;
const int32_t kTargetKurt = 1;
const int32_t kStrikeHealth = 65000;
const int32_t kStrikeFlags = 0x85e20;
// Thrown items' flags (Items.ThrownFlags): the teeth fall as hand grenades.
const int32_t kThrownFlags = 0x818a6;
// Levels after the fourth in the order of play (LEVEL8, LEVEL5) have the dive.
const int32_t kLastStrikeLevel = 3;
// Points towards the target are tried in steps of 9% of the way.
const float kClearStep = 0.09f;
const int32_t kClearSteps = 10;
const float kClearFallback = 0.9f;
// The target sees the path from 2 units above it.
const float kTargetEye = 2.0f;
// SpawnExplosion's default lift (ScriptRuntime.cs:106, DefaultLift).
const float kDefaultLift = 3.0f;
const char kExplodeSound[] = "EXPLODE";

// BezierPath's constants (AirStrike.cs:246-247).
const float kHandleFactor = 0.25f;
const int32_t kSamplesPerSegment = 64;

// System.Numerics as the .NET 10 runtime computes it on the oracle's host (see
// sniper_rounds.cpp): Dot sums the rounded products left to right, Lerp is fused.
Vec3 add(const Vec3& a, const Vec3& b) { return Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 sub(const Vec3& a, const Vec3& b) { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 scaled(const Vec3& v, float s) { return Vec3{v.x * s, v.y * s, v.z * s}; }
// The C#'s `s * v` (scalar on the left): the same products.
Vec3 scaledBy(float s, const Vec3& v) { return Vec3{s * v.x, s * v.y, s * v.z}; }
float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float distanceSquared(const Vec3& a, const Vec3& b) {
    const Vec3 d = sub(a, b);
    return dot(d, d);
}
float distance(const Vec3& a, const Vec3& b) { return sqrtf(distanceSquared(a, b)); }
Vec3 lerp(const Vec3& a, const Vec3& b, float t) {
    const float keep = 1.0f - t;
    return Vec3{std::fma(a.x, keep, b.x * t), std::fma(a.y, keep, b.y * t),
                std::fma(a.z, keep, b.z * t)};
}
bool isZero(const Vec3& v) { return v.x == 0.0f && v.y == 0.0f && v.z == 0.0f; }

// Math.Clamp(float).
float clamp(float value, float min, float max) {
    if (value < min) {
        return min;
    }
    if (value > max) {
        return max;
    }
    return value;
}

// float.CompareTo, as List<float>.BinarySearch's default comparer: NaN sorts first.
int32_t compareFloat(float a, float b) {
    if (a < b) {
        return -1;
    }
    if (a > b) {
        return 1;
    }
    if (a == b) {
        return 0;
    }
    if (isnan(a)) {
        return isnan(b) ? 0 : -1;
    }
    return 1;
}

// List<float>.BinarySearch (Array.BinarySearch's loop): an index of a match, or the
// complement of the insertion point.
int32_t binarySearch(const std::vector<float>& values, float value) {
    int32_t lo = 0;
    int32_t hi = (int32_t)values.size() - 1;
    while (lo <= hi) {
        const int32_t i = lo + ((hi - lo) >> 1);
        const int32_t c = compareFloat(values[(size_t)i], value);
        if (c == 0) {
            return i;
        }
        if (c < 0) {
            lo = i + 1;
        } else {
            hi = i - 1;
        }
    }
    return ~lo;
}

} // namespace

// ---------------------------------------------------------------------------
// BezierPath (AirStrike.cs:244-315)
// ---------------------------------------------------------------------------

BezierPath::BezierPath(const std::vector<Vec3>& points) {
    // C# BezierPath(points): 64 samples a segment, each length the running sum.
    points_.push_back(points[0]);
    lengths_.push_back(0.0f);
    const int32_t count = (int32_t)points.size();
    for (int32_t i = 0; i + 1 < count; i++) {
        const Vec3& a = points[(size_t)i];
        const Vec3& b = points[(size_t)(i + 1)];
        const int32_t before = i - 1 > 0 ? i - 1 : 0;
        const int32_t after = i + 2 < count - 1 ? i + 2 : count - 1;
        const Vec3 outHandle = scaled(sub(b, points[(size_t)before]), kHandleFactor);
        // -(points[after] - a) * HandleFactor: the negation first, then the product.
        const Vec3 toAfter = sub(points[(size_t)after], a);
        const Vec3 inHandle = scaled(Vec3{-toAfter.x, -toAfter.y, -toAfter.z}, kHandleFactor);
        for (int32_t s = 1; s <= kSamplesPerSegment; s++) {
            const Vec3 point = cubic(a, add(a, outHandle), add(b, inHandle), b,
                                     (float)s / (float)kSamplesPerSegment);
            lengths_.push_back(lengths_.back() + distance(points_.back(), point));
            points_.push_back(point);
        }
    }
}

Vec3 BezierPath::cubic(const Vec3& p0, const Vec3& p1, const Vec3& p2, const Vec3& p3, float t) {
    // C# Cubic: u*u*u*p0 + 3f*u*u*t*p1 + 3f*u*t*t*p2 + t*t*t*p3, the scalars left to
    // right, then the vectors summed left to right.
    const float u = 1.0f - t;
    const Vec3 a = scaledBy(u * u * u, p0);
    const Vec3 b = scaledBy(3.0f * u * u * t, p1);
    const Vec3 c = scaledBy(3.0f * u * t * t, p2);
    const Vec3 d = scaledBy(t * t * t, p3);
    return add(add(add(a, b), c), d);
}

Vec3 BezierPath::sample(float offset) const {
    // C# Sample (AirStrike.cs:274-288).
    const int32_t index = binarySearch(lengths_, clamp(offset, 0.0f, length()));
    if (index >= 0) {
        return points_[(size_t)index];
    }
    const int32_t last = (int32_t)points_.size() - 1;
    const int32_t next = ~index < last ? ~index : last;
    const int32_t previous = next - 1 > 0 ? next - 1 : 0;
    const float span = lengths_[(size_t)next] - lengths_[(size_t)previous];
    const float t = span > 0.0f ? (offset - lengths_[(size_t)previous]) / span : 0.0f;
    return lerp(points_[(size_t)previous], points_[(size_t)next], t);
}

float BezierPath::closestOffset(const Vec3& point) const {
    // C# ClosestOffset (AirStrike.cs:291-313).
    float best = 0.0f;
    float bestDistance = 3.40282347e+38f;  // float.MaxValue
    for (size_t i = 0; i + 1 < points_.size(); i++) {
        const Vec3& a = points_[i];
        const Vec3 segment = sub(points_[i + 1], a);
        const float squared = dot(segment, segment);
        const float t = squared > 0.0f ? clamp(dot(sub(point, a), segment) / squared, 0.0f, 1.0f)
                                       : 0.0f;
        const float d = distanceSquared(point, add(a, scaled(segment, t)));
        if (d < bestDistance) {
            bestDistance = d;
            best = lengths_[i] + (lengths_[i + 1] - lengths_[i]) * t;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// AirStrike (AirStrike.cs:16-238)
// ---------------------------------------------------------------------------

bool AirStrike::isActive() const {
    // C# IsActive: `_strike is { Dead: false }`.
    return strike_ != nullptr && !strike_->dead;
}

bool AirStrike::findTarget(const Vec3& eye, const Vec3& forward, Vec3* target) const {
    // C# FindTarget (AirStrike.cs:68-77): the view's hit, with open sky above it.
    Vec3 hit;
    if (!runtime_.raycastPoint(eye, add(eye, scaled(forward, kRange)), &hit)) {
        return false;
    }
    const Vec3 above = add(hit, Vec3{0.0f, 0.0f, 1.0f});
    const Vec3 sky = add(hit, Vec3{0.0f, 0.0f, kSky});
    if (!runtime_.raycastClear(above, sky)) {
        return false;
    }
    *target = hit;
    return true;
}

bool AirStrike::call(const Vec3& eye, const Vec3& forward) {
    // C# Call (AirStrike.cs:81-100).
    if (isActive()) {
        return false;
    }
    Vec3 target;
    if (!findTarget(eye, forward, &target) || usedUp) {
        runtime_.mixer().play("RASPBER", audio::SoundMixer::Restart);
        return false;
    }
    const Flight flight =
        flow::indexOf(runtime_.level_->number()) > kLastStrikeLevel ? Dive : Over;
    usedUp |= flight == Dive;

    // Bones in his plane, full screen (without him when he dives).
    runtime_.playStrikeScene(StrikeScene::Bones,
                             flight == Dive ? StrikeScene::Only : StrikeScene::WithPilot);
    start(target, flight);
    return true;
}

void AirStrike::start(const Vec3& target, Flight flight) {
    // C# Start (AirStrike.cs:102-137).
    const bool dive = flight == Dive;
    const formats::Arena* arena = runtime_.level_->arenaNamed(runtime_.currentArena());
    float top = target.z;
    if (arena != nullptr && !arena->vertices().empty()) {
        // Vertices.Max(v => v.Z): the first of equal maxima, as Enumerable.Max's `>`.
        const std::vector<formats::Vec3f>& vertices = arena->vertices();
        top = vertices[0].z;
        for (size_t i = 1; i < vertices.size(); i++) {
            if (vertices[i].z > top) {
                top = vertices[i].z;
            }
        }
    }
    const float cruise = top + kHeight;
    const Vec3 kurt = runtime_.kurtPosition();
    // Vector2.Normalize divides by its length (sqrt of the left-to-right dot), then * Behind.
    const float awayX = kurt.x - target.x;
    const float awayY = kurt.y - target.y;
    const float awayLength = sqrtf(awayX * awayX + awayY * awayY);
    float away[2] = {awayX / awayLength * kBehind, awayY / awayLength * kBehind};
    if (isnan(away[0])) {
        away[0] = 0.0f;
        away[1] = 0.0f;
    }

    const Vec3 startPoint{kurt.x + away[0], kurt.y + away[1], cruise};
    const Vec3 over = add(target, Vec3{0.0f, 0.0f, kOverTarget});
    Vec3 end = sub(scaled(over, 2.0f), startPoint);
    if (!dive) {
        end.z = cruise;
    }

    const Vec3 before = clearPoint(startPoint, over, target);
    const Vec3 after = dive ? scaled(add(over, end), 0.5f) : clearPoint(end, over, target);
    std::vector<Vec3> points;
    points.push_back(startPoint);
    points.push_back(before);
    points.push_back(over);
    points.push_back(after);
    points.push_back(end);
    curve_ = BezierPath(points);
    hasCurve_ = true;
    targetOffset_ = curve_.closestOffset(over);
    diveOffset_ = dive ? (curve_.closestOffset(before) + targetOffset_) * 0.5f : INFINITY;
    offset_ = 0.0f;
    teeth_ = 0;
    diving_ = false;
    MdkObject* controller = &runtime_.getArenaState(runtime_.currentArena()).controller;
    strike_ = runtime_.spawn(controller, "X_STRIKE", startPoint, 0.0f, -1, 0, false);
    if (strike_ == nullptr) {
        return;
    }
    strike_->health = kStrikeHealth;
    strike_->flags = kStrikeFlags;
}

Vec3 AirStrike::clearPoint(const Vec3& from, const Vec3& over, const Vec3& target) const {
    // C# ClearPoint (AirStrike.cs:140-152): the first point towards `over` the target sees.
    const Vec3 eye = add(target, Vec3{0.0f, 0.0f, kTargetEye});
    for (int32_t i = 1; i <= kClearSteps; i++) {
        const Vec3 point = lerp(from, over, kClearStep * (float)i);
        if (runtime_.raycastClear(eye, point)) {
            return point;
        }
    }
    return lerp(from, over, kClearFallback);
}

void AirStrike::update(float ticks) {
    // C# Update (AirStrike.cs:155-197).
    if (!isActive() || !hasCurve_) {
        strike_ = nullptr;
        return;
    }

    MdkObject* strike = strike_;
    if (diving_) {
        crash(strike);
        return;
    }

    const Vec3 previous = strike->position;
    offset_ += kSpeed / Kurt::kTicks * ticks;
    if (offset_ >= curve_.length()) {
        runtime_.remove(strike);
        return;
    }

    strike->position = curve_.sample(offset_);
    const Vec3 motion = sub(strike->position, previous);
    if (!isZero(motion)) {
        /* @vfp-exempt R6 — the strike's heading is gameplay (the teeth spawn facing it)
         * and the oracle's MathF.Atan2 to the bit; one call a tick, cold. measured n/a.
         * Verified 2026-11-20. */
        const float pi = 3.14159265358979323846f;
        // (RadiansToDegrees(Atan2(y, x)) + 360f) % 360f; RadiansToDegrees is * 180 / pi.
        strike->yaw = fmodf(atan2f(motion.y, motion.x) * 180.0f / pi + 360.0f, 360.0f);
    }

    if (offset_ >= diveOffset_) {
        // It leaves the path with its velocity, falling, until it hits something.
        diving_ = true;
        strike->velocity = Vec3{motion.x * Kurt::kTicks / ticks, motion.y * Kurt::kTicks / ticks,
                                motion.z * Kurt::kTicks / ticks};
        strike->flags |= MdkObject::FlagGravity | MdkObject::FlagCollides;
        return;
    }

    // (int)MathF.Round(...): round half to even, as rintf in the default rounding mode.
    const int32_t tooth =
        (int32_t)rintf((offset_ - targetOffset_) / kToothSpacing + kToothBias);
    while (teeth_ < tooth && teeth_ < kTeeth && isinf(diveOffset_) && diveOffset_ > 0.0f) {
        teeth_++;
        dropTooth(strike);
    }
}

void AirStrike::crash(MdkObject* strike) {
    // C# Crash (AirStrike.cs:200-214): the diving strike blows up when it hits something.
    if ((strike->contact_flags & (MdkObject::ContactCollided | MdkObject::ContactFloor)) == 0) {
        return;
    }
    const Vec3 center = strike->position;
    runtime_.blast(center, kDiveDamage, kDiveRadius, kTargetsAll, kHitDive, strike);
    runtime_.blast(center, kDiveKurtDamage, kDiveRadius, kTargetKurt, kHitDive, strike);
    runtime_.spawnExplosion(strike->arena, center, kExplosionScale, 0.0f, kDefaultLift);
    runtime_.playSoundAt(kExplodeSound, center);
    runtime_.remove(strike);
}

void AirStrike::dropTooth(MdkObject* strike) {
    // C# DropTooth (AirStrike.cs:217-238): a tooth falls as a hand grenade.
    MdkObject* controller = &runtime_.getArenaState(runtime_.currentArena()).controller;
    MdkObject* tooth =
        runtime_.spawn(controller, "X_TOOTH", strike->position, strike->yaw, -1, 0, false);
    if (tooth == nullptr) {
        return;
    }
    tooth->flags |= kThrownFlags;
    tooth->thrown_kind = (int32_t)Kurt::Grenade;
    tooth->item_ticks = kToothTicks;
    tooth->friction = 0.0f;
    tooth->velocity = Vec3{0.0f, 0.0f, kToothFall};
    // Mixer.PlayOn("DROP", () => tooth.Position): a voice following the tooth. The deque
    // never moves an object, so its position (and yaw: no offset, no turn) stay valid.
    tooth->loop_sound = runtime_.mixer().playOn("DROP", &tooth->position, &tooth->yaw, Vec3{});
}

} // namespace scripts
} // namespace mdk

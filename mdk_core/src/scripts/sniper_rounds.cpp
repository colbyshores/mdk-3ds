// ============================================================================
// sniper_rounds.cpp — Kurt's sniper rounds (src/Mdk.Game/Scripts/SniperRounds.cs).
// See sniper_rounds.h.
// ============================================================================

#include "scripts/sniper_rounds.h"

#include <float.h>
#include <math.h>

#include <cmath>
#include <deque>
#include <string>
#include <vector>

#include "formats/ascii_case.h"
#include "game/bsp.h"
#include "game/kurt.h"
#include "game/player.h"
#include "game/scope.h"
#include "scripts/script_runtime.h"
#include "util/math_minmax.h"
#include "util/matrix.h"
#include "util/system_numerics_inverse.h"

namespace mdk {
namespace scripts {

namespace {

// SniperRounds.cs:35-38: each type's life (ticks), speed (u/s) and model; a bullet
// takes 8 hit points.
const int32_t kLife[] = {75, 240, 75, 240, 450};
const float kSpeed[] = {1100.0f, 400.0f, 1100.0f, 400.0f, 150.0f};
const char* const kModels[] = {"SW_SHOT", "SW_HOME", "SW_SGREN", "SW_HGREN", "SW_LGREN"};
const int32_t kDamage = 8;
// Objects this healthy aren't hurt by bullets.
const int32_t kUnhurt = 65000;
// Bullets and homing rounds spin 720 degrees per second (not the mortar).
const float kSpin = 720.0f;

// Homing (0x463174, SniperRounds.cs:44-55): steering starts after 7 ticks; the yaw rate
// accelerates by 540/s^2 up to 270/s, the pitch turns by at most 120/s; the speed aims
// for 250 within 35 degrees of the target, else 100, rising by 200/s and falling by 500/s.
const float kHomingStart = 233.0f;
const float kHomingYawAcceleration = 540.0f;
const float kHomingYawRate = 270.0f;
const float kHomingPitchRate = 120.0f;
const float kHomingAim = 35.0f;
const float kHomingFast = 250.0f;
const float kHomingSlow = 100.0f;
const float kHomingSpeedUp = 200.0f;
const float kHomingSlowDown = 500.0f;

// The mortar (0x46360c, SniperRounds.cs:57-70): drag of 60 u/s^2 shared between the
// horizontal and vertical speeds, gravity 32 u/s^2, bounces with v -= 1.75 (v.n) n; it
// settles after 15 ticks once it stops.
const float kMortarDrag = 60.0f;
const float kMortarGravity = 32.0f;
const float kMortarMaxFall = 220.0f;
const float kMortarBounce = 1.75f;
const float kMortarSettleLift = 4.0f;
const float kMortarSettleTicks = 15.0f;
const float kMortarStillSpeed = 0.5f;
const float kMortarStillRise = 1.0f;
// The half extents of the box the mortar sweeps through the arena (0x491f24).
const float kMortarBox = 0.5f;
// A guided mortar round stays alive until its path's last key.
const float kGuidedLife = 99.0f;

// Explosions (0x4638cc, SniperRounds.cs:72-81): 150 damage to objects and triangle
// groups, 75 to Kurt, hit type -7, within 25 (50 for the mortar), the effect at scale 2.
const int32_t kExplosionDamage = 150;
const int32_t kExplosionKurtDamage = 75;
const int32_t kHitExplosion = -7;
const float kBlastRadius = 25.0f;
const float kMortarBlastRadius = 50.0f;
const float kExplosionScale = 2.0f;
const int32_t kTargetsAll = 6;
const int32_t kTargetKurt = 1;
// ScriptRuntime.ExplodeSound and SpawnExplosion's default lift (ScriptRuntime.cs:111, 1770).
const char kExplodeSound[] = "EXPLODE";
const float kExplosionLift = 3.0f;
// ScriptRuntime.HitShot (ScriptRuntime.cs:115): the kind a round's group hit has.
const int32_t kHitShot = 1;

// Ticks a slot stays busy after its round ended, and the camera's watch after a hit.
const float kMissLinger = 30.0f;
const float kHitLinger = 45.0f;
const float kWatchTicks = 30.0f;
// The round camera backs off by 10 u/s, up to 10 behind the round (0x461d80).
const float kCameraBackSpeed = 10.0f;
const float kCameraBack = 10.0f;
// Below the arena's lowest point by this much a round is lost (ObjectMotion's).
const float kFallOutDepth = 200.0f;
// Objects flagged so are neither hit nor targets.
const int32_t kUntouchable = MdkObject::FlagNotSolid | MdkObject::FlagNotTarget;
const char kHeadPart[] = "HEAD";
// HitObject's event for the whole box (no part).
const int32_t kWholeObject = -2;

// MathF.PI and .NET's left-associative DegreesToRadians / RadiansToDegrees (see
// script_runtime.cpp's pair: the constant-folded forms differ by an ULP).
const float kPi = 3.14159265358979323846f;
float degreesToRadians(float degrees) { return degrees * kPi / 180.0f; }
float radiansToDegrees(float radians) { return radians * 180.0f / kPi; }

// SniperRounds.WrapAngle / Wrap360 (SniperRounds.cs:655-657): the C#'s float `%` is
// fmodf, and these are the REMAINDER forms, not ScriptMath's floor forms (they differ
// in the last bit), so they are spelled here, not borrowed.
float wrapAngle(float degrees) {
    return fmodf(fmodf(degrees + 180.0f, 360.0f) + 360.0f, 360.0f) - 180.0f;
}
float wrap360(float degrees) { return fmodf(fmodf(degrees, 360.0f) + 360.0f, 360.0f); }

// MathF.Sign as the float the C#'s int * float promotes it to. (A NaN throws in the
// C#; a NaN never reaches here from a finite aim.)
float sign(float value) { return value > 0.0f ? 1.0f : value < 0.0f ? -1.0f : 0.0f; }

// Math.Clamp(float): below min is min, above max is max.
float clamp(float value, float min, float max) {
    if (value < min) {
        return min;
    }
    if (value > max) {
        return max;
    }
    return value;
}

// System.Numerics as the .NET 10 runtime computes it on the oracle's host: a
// Vector3/Vector2 dot sums the rounded products left to right (scope.h's
// numerics::dot, measured), Lerp and Transform are MultiplyAddEstimate (fused: the
// tube basis's nLerp, the matrix multiply).
float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(const Vec3& a, const Vec3& b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vec3 sub(const Vec3& a, const Vec3& b) { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 add(const Vec3& a, const Vec3& b) { return Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 scaled(const Vec3& v, float s) { return Vec3{v.x * s, v.y * s, v.z * s}; }
float distance(const Vec3& a, const Vec3& b) {
    const Vec3 d = sub(a, b);
    return sqrtf(dot(d, d));
}
float length2(float x, float y) { return sqrtf(x * x + y * y); }
Vec3 lerp(const Vec3& a, const Vec3& b, float t) {
    const float keep = 1.0f - t;
    return Vec3{std::fma(a.x, keep, b.x * t), std::fma(a.y, keep, b.y * t),
                std::fma(a.z, keep, b.z * t)};
}
// Vector3.Transform(position, matrix): row X times x, then Y and Z fused in, then W.
Vec3 transformPoint(const Vec3& p, const float m[16]) {
    const float x = std::fma(p.z, m[8], std::fma(p.y, m[4], p.x * m[0])) + m[12];
    const float y = std::fma(p.z, m[9], std::fma(p.y, m[5], p.x * m[1])) + m[13];
    const float z = std::fma(p.z, m[10], std::fma(p.y, m[6], p.x * m[2])) + m[14];
    return Vec3{x, y, z};
}
Vec3 boxCentre(const formats::Box& box) {
    return Vec3{(box.min.x + box.max.x) * 0.5f, (box.min.y + box.max.y) * 0.5f,
                (box.min.z + box.max.z) * 0.5f};
}
Vec3 toVec3(const formats::Vec3f& v) { return Vec3{v.x, v.y, v.z}; }

// ScriptMath.SegmentEntry (ScriptMath.cs:89): where the segment enters the box (its start
// when inside), false when it does not. MathF.Max/Min (mathMax/mathMin), so a NaN keeps
// the C#'s answer.
bool segmentEntry(const formats::Box& box, const Vec3& from, const Vec3& to, Vec3* entry) {
    const float start[3] = {from.x, from.y, from.z};
    const float d[3] = {to.x - from.x, to.y - from.y, to.z - from.z};
    const float lo[3] = {box.min.x, box.min.y, box.min.z};
    const float hi[3] = {box.max.x, box.max.y, box.max.z};
    float enter = 0.0f;
    float leave = 1.0f;
    for (int axis = 0; axis < 3; axis++) {
        if (d[axis] == 0.0f) {
            if (start[axis] < lo[axis] || start[axis] > hi[axis]) {
                return false;
            }
            continue;
        }
        const float t0 = (lo[axis] - start[axis]) / d[axis];
        const float t1 = (hi[axis] - start[axis]) / d[axis];
        enter = mathMax(enter, mathMin(t0, t1));
        leave = mathMin(leave, mathMax(t0, t1));
        if (enter > leave) {
            return false;
        }
    }
    *entry = Vec3{from.x + d[0] * enter, from.y + d[1] * enter, from.z + d[2] * enter};
    return true;
}

// Where the segment crosses the triangle, as a fraction of it (0x4144c0, either side;
// SniperRounds.SegmentFace, Moller-Trumbore); infinity when it doesn't.
float segmentFace(const Vec3& from, const Vec3& to, const Vec3& a, const Vec3& b, const Vec3& c) {
    const float kParallel = 1e-9f;
    const Vec3 along = sub(to, from);
    const Vec3 ab = sub(b, a);
    const Vec3 ac = sub(c, a);
    const Vec3 p = cross(along, ac);
    const float det = dot(ab, p);
    if (fabsf(det) < kParallel) {
        return INFINITY;
    }

    const float inverse = 1.0f / det;
    const Vec3 s = sub(from, a);
    const float u = dot(s, p) * inverse;
    if (u < 0.0f || u > 1.0f) {
        return INFINITY;
    }

    const Vec3 q = cross(s, ab);
    const float v = dot(along, q) * inverse;
    if (v < 0.0f || u + v > 1.0f) {
        return INFINITY;
    }

    const float t = dot(ac, q) * inverse;
    return t >= 0.0f && t <= 1.0f ? t : INFINITY;
}

// A ray hit's normal, turned towards where the ray came from (SniperRounds.Facing).
Vec3 facing(const Vec3& normal, const Vec3& motion) {
    return dot(normal, motion) > 0.0f ? Vec3{-normal.x, -normal.y, -normal.z} : normal;
}

Vec3 direction(const SniperRounds::Round& round) {
    return Scope::direction(round.yaw, round.pitch);
}

// Matrix4x4.CreateRotationZ / CreateRotationY (row vectors; the angle in radians).
/* @vfp-exempt R6 — the round model's basis, per flying round per tick (at most 3 rounds:
 * cold); libm matches the oracle's Matrix4x4 builders (MathF) bit for bit and the goldens
 * hash the basis (`vis=`). measured n/a (corpus empty). Verified 2026-11-20. */
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
/* @vfp-exempt R6 — the round model's basis, per flying round per tick (at most 3 rounds:
 * cold); libm matches the oracle's Matrix4x4 builders (MathF) bit for bit and the goldens
 * hash the basis (`vis=`). measured n/a (corpus empty). Verified 2026-11-20. */
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

// SniperRounds.VisualBasis: the round models stand upright (their length along +Z): the
// nose is turned from +Z to the flight direction, and the spin is around the length.
Mat4 visualBasis(const SniperRounds::Round& round) {
    return multiply(multiply(rotationZ(degreesToRadians(round.roll)),
                             rotationY(kPi / 2.0f + degreesToRadians(round.pitch))),
                    rotationZ(degreesToRadians(round.yaw)));
}

} // namespace

bool SniperRounds::fire(int32_t type, const Vec3& eye, float yaw, float pitch, MdkObject* target) {
    // C# Fire (SniperRounds.cs:149-184): the first free slot (Array.Find).
    Round* round = nullptr;
    for (int32_t i = 0; i < kSlots; i++) {
        if (rounds_[i].state == Free) {
            round = &rounds_[i];
            break;
        }
    }
    if (round == nullptr) {
        return false;
    }

    const Type kind = (Type)type;
    const bool homing = kind == Homing || kind == HomingGrenade;
    round->state = Flying;
    round->type = kind;
    round->position = eye;
    round->yaw = yaw;
    round->pitch = pitch;
    round->roll = 0.0f;
    round->life = (float)kLife[type];
    round->linger = 0.0f;
    round->speed = kSpeed[type];
    round->vertical = 0.0f;
    round->path = 0;
    // PathTime is NOT reset here (the C# leaves it): FollowPath reads it only after
    // GuideLastMortar has zeroed it.
    round->yawRate = 0.0f;
    round->target = homing ? target : nullptr;
    round->targetPart = round->target != nullptr ? headOf(round->target) : -1;
    round->cameraDistance = 0.0f;
    round->cameraPosition = eye;
    round->watch = 0.0f;
    if (kind == Mortar) {
        /* @vfp-exempt R6 — the mortar's launch, once per shot (cold); libm cos/sin match
         * the oracle's MathF bit for bit, and the LUT would move the throw the
         * golden/L7_rounds_mortar.txt pins. measured n/a (corpus empty). Verified
         * 2026-11-20. */
        round->speed = kSpeed[type] * cosf(degreesToRadians(pitch));
        round->vertical = -kSpeed[type] * sinf(degreesToRadians(pitch));
    }

    makeVisual(round);
    return true;
}

SniperRounds::RoundCamera SniperRounds::camera(int32_t index) const {
    // C# Camera (SniperRounds.cs:187-207).
    const Round& round = rounds_[index];
    RoundCamera result;
    result.shot = ShotEmpty;
    result.position = Vec3{};
    result.forward = Vec3{};
    result.time = 0.0f;
    if (round.state == Free) {
        return result;
    }

    if (round.state == Flying || round.watch > 0.0f) {
        result.shot = ShotWatching;
        result.position = round.cameraPosition;
        result.forward = direction(round);
        return result;
    }

    result.shot = round.state == Hit ? ShotHit : round.state == Dead ? ShotMiss : ShotKill;
    result.time = kMissLinger - round.linger;
    return result;
}

void SniperRounds::guideLastMortar(int32_t path) {
    // C# GuideLastMortar (SniperRounds.cs:210-219): only a flying, unguided mortar.
    Round* round = lastMortar_;
    if (round == nullptr || round->state != Flying || round->type != Mortar || round->path != 0) {
        return;
    }

    round->path = path;
    round->pathTime = 0.0f;
}

int32_t SniperRounds::headOf(const MdkObject* obj) {
    // C# HeadOf (SniperRounds.cs:222-223): the first part whose name contains HEAD
    // (ignoring case), else the whole object.
    if (obj->model == nullptr) {
        return -1;
    }
    const std::vector<formats::Model::Part>& parts = obj->model->partList;
    for (size_t i = 0; i < parts.size(); i++) {
        if (formats::toUpperAscii(parts[i].nameString()).find(kHeadPart) != std::string::npos) {
            return (int32_t)i;
        }
    }
    return -1;
}

void SniperRounds::makeVisual(Round* round) {
    // C# MakeVisual (SniperRounds.cs:225-236): the type's model from Kurt's arena, a new
    // object outside the list, rolling (its orientation is set as a whole each tick).
    const std::string name = kModels[(int)round->type];
    const formats::Model* model = runtime_.findModel(runtime_.currentArena(), name);
    round->visual = MdkObject();
    round->hasVisual = model != nullptr;
    if (model == nullptr) {
        return;
    }
    round->visual.type_name = name;
    round->visual.model = model;
    round->visual.arena = runtime_.currentArena();
    round->visual.flags = MdkObject::FlagRolling;
}

void SniperRounds::update(float ticks) {
    // C# Update (SniperRounds.cs:239-263).
    const float dt = ticks / Kurt::kTicks;
    for (int32_t i = 0; i < kSlots; i++) {
        Round* round = &rounds_[i];
        if (round->state == Free) {
            continue;
        }

        if (round->state != Flying) {
            round->linger -= ticks;
            round->watch -= ticks;
            if (round->linger <= 0.0f) {
                round->state = Free;
            }
            continue;
        }

        fly(round, ticks, dt);
    }
}

void SniperRounds::fly(Round* round, float ticks, float dt) {
    // C# Fly (SniperRounds.cs:265-315).
    round->life -= ticks;
    if (round->type != Mortar) {
        round->roll = fmodf(round->roll + kSpin * dt, 360.0f);
    }

    if (round->path != 0) {
        followPath(round, ticks);
    } else if (round->type == Mortar) {
        moveMortar(round, dt);
    } else {
        if (round->target != nullptr) {
            steer(round, dt);
        }
        moveStraight(round, dt);
    }

    if (round->state == Flying && round->life <= 0.0f) {
        if (round->type >= Grenade) {
            explode(round, round->type == Mortar ? kMortarBlastRadius : kBlastRadius, nullptr);
        } else {
            end(round, Dead, kMissLinger);
        }
    }

    if (round->state != Flying) {
        return;
    }

    round->cameraDistance = mathMin(round->cameraDistance + kCameraBackSpeed * dt, kCameraBack);
    round->cameraPosition = sub(round->position, scaled(direction(*round), round->cameraDistance));
    if (round->hasVisual) {
        round->visual.position = round->position;
        round->visual.rolling_basis = visualBasis(*round);
    }
}

void SniperRounds::moveStraight(Round* round, float dt) {
    // C# MoveStraight (0x462f24, SniperRounds.cs:330-364): tested against the objects
    // and the arena; the nearer one stops the round.
    const Vec3 start = round->position;
    const Vec3 end = add(start, scaled(scaled(direction(*round), round->speed), dt));
    ObjectHit hit;
    const bool hitAny = testObjects(start, end, &hit);
    DebrisRayHit wall;
    const bool wallAny = runtime_.raycastHit(start, end, &wall);
    if (hitAny && (!wallAny || distance(start, hit.point) < distance(start, wall.point))) {
        round->position = hit.point;
        hitObject(round, hit.object, hit.part, hit.point);
        return;
    }

    if (wallAny) {
        // Taken back a unit off the wall; a reacting group gets one orange spark, a wall
        // 3 grey ones. (A bullet hole on the wall's texture is package #16's: this is
        // where the round stops on the arena, at round->position.)
        round->position = add(wall.point, facing(wall.normal, sub(end, start)));
        const bool reacted =
            (runtime_.hitGroup(wall.arena, wall.group, round->type < Grenade ? kDamage : 0,
                               kHitShot, (int32_t)round->type) &
             1) != 0;
        runtime_.sparkAt(round->position, reacted ? 1 : 3, std::string(),
                         reacted ? ScriptRuntime::Spark::Group : ScriptRuntime::Spark::Hard);
        if (round->type >= Grenade) {
            explode(round, kBlastRadius, nullptr);
            return;
        }

        this->end(round, Dead, kMissLinger);
        return;
    }

    round->position = end;
    if (round->position.z < runtime_.getArenaFloor(runtime_.currentArena()) - kFallOutDepth) {
        this->end(round, Dead, kMissLinger);
    }
}

void SniperRounds::steer(Round* round, float dt) {
    // C# Steer (0x463028, 0x463174; SniperRounds.cs:368-411): the yaw rate accelerates
    // towards the error (from 0 when it has the wrong sign), never overshooting; the
    // pitch turns at a limited rate.
    MdkObject* target = round->target;
    if (target->dead) {
        round->target = nullptr;
        return;
    }

    if (round->life > kHomingStart) {
        return;
    }

    formats::Box bounds = runtime_.worldBounds(target);
    const std::vector<ScriptRuntime::PartBox> parts = runtime_.partBounds(target);
    if (round->targetPart >= 0 && round->targetPart < (int32_t)parts.size() &&
        (target->hidden_parts & (1 << (round->targetPart & 31))) == 0 &&
        parts[(size_t)round->targetPart].valid) {
        bounds = runtime_.worldBounds(target, parts[(size_t)round->targetPart].box);
    }

    /* @vfp-exempt R6 — the homing round's aim, once per flying homing round per tick
     * (at most 3: cold); libm atan2 matches the oracle's MathF.Atan2 and the LUT would
     * move the round golden/L8_rounds_shots.txt pins. measured n/a (corpus empty).
     * Verified 2026-11-20. */
    const Vec3 aim = sub(boxCentre(bounds), round->position);
    const float yawError = wrapAngle(radiansToDegrees(atan2f(aim.y, aim.x)) - round->yaw);
    if (round->yawRate * yawError < 0.0f) {
        round->yawRate = 0.0f;
    }

    round->yawRate = clamp(round->yawRate + sign(yawError) * kHomingYawAcceleration * dt,
                           -kHomingYawRate, kHomingYawRate);
    float yawStep = round->yawRate * dt;
    if (fabsf(yawStep) > fabsf(yawError)) {
        yawStep = yawError;
    }

    round->yaw = wrap360(round->yaw + yawStep);
    const float pitchGoal = -radiansToDegrees(atan2f(aim.z, length2(aim.x, aim.y)));
    const float pitchError = wrapAngle(pitchGoal - round->pitch);
    round->pitch += clamp(pitchError, -kHomingPitchRate * dt, kHomingPitchRate * dt);
    const float goal = fabsf(yawError) + fabsf(pitchError) <= kHomingAim ? kHomingFast : kHomingSlow;
    const float rate = goal > round->speed ? kHomingSpeedUp : kHomingSlowDown;
    round->speed = fabsf(goal - round->speed) <= rate * dt
                       ? goal
                       : round->speed + sign(goal - round->speed) * rate * dt;
}

void SniperRounds::moveMortar(Round* round, float dt) {
    // C# MoveMortar (0x46360c, SniperRounds.cs:415-469): drag shared between the
    // horizontal and the vertical speed (only while rising), gravity, bounces off the
    // arena.
    const float fraction =
        round->speed > 0.0f ? round->speed / (fabsf(round->vertical) + round->speed) : 0.0f;
    round->speed = mathMax(round->speed - fraction * kMortarDrag * dt, 0.0f);
    if (round->vertical > 0.0f) {
        round->vertical = mathMax(round->vertical - (1.0f - fraction) * kMortarDrag * dt, 0.0f);
    }

    round->vertical = mathMax(round->vertical - kMortarGravity * dt, -kMortarMaxFall);
    /* @vfp-exempt R6 — the mortar's flight, once per flying mortar per tick (at most 3:
     * cold); libm matches the oracle's MathF and the LUT would move the puzzle's shot
     * golden/L7_rounds_mortar.txt pins. measured n/a (corpus empty). Verified 2026-11-20. */
    const float yaw = degreesToRadians(round->yaw);
    Vec3 velocity{cosf(yaw) * round->speed, sinf(yaw) * round->speed, round->vertical};
    const Vec3 start = round->position;
    const Vec3 end = add(start, scaled(velocity, dt));
    ObjectHit hit;
    if (testObjects(start, end, &hit)) {
        round->position = hit.point;
        explode(round, kMortarBlastRadius, hit.object);
        return;
    }

    // A box swept as the original's, not a ray: faces seen from their back don't stop it.
    DebrisRayHit wall;
    if (!runtime_.sweep(start, end, Vec3{kMortarBox, kMortarBox, kMortarBox}, &wall)) {
        round->position = end;
        return;
    }

    const Vec3 normal = wall.normal;
    round->position = wall.point;
    lastMortar_ = round;
    // The group's hit script may guide this round (bomb_follow_path): it then stops here.
    runtime_.hitGroup(wall.arena, wall.group, 0, kHitShot, (int32_t)Mortar);
    if (round->path != 0) {
        return;
    }

    velocity = sub(velocity, scaled(scaled(normal, dot(velocity, normal)), kMortarBounce));
    if (velocity.z > 0.0f && velocity.z < kMortarSettleLift) {
        velocity.z = 0.0f;
    }

    round->speed = length2(velocity.x, velocity.y);
    round->vertical = velocity.z;
    if (round->speed > 0.0f) {
        /* @vfp-exempt R6 — the bounced mortar's heading, once per bounce (cold); libm
         * atan2 matches the oracle's MathF.Atan2 and golden/L7_rounds_mortar.txt pins the
         * bounces. measured n/a (corpus empty). Verified 2026-11-20. */
        round->yaw = radiansToDegrees(atan2f(velocity.y, velocity.x));
    }

    if (round->life > kMortarSettleTicks && round->speed < kMortarStillSpeed &&
        round->vertical < kMortarStillRise) {
        round->life = kMortarSettleTicks;
    }
}

void SniperRounds::followPath(Round* round, float ticks) {
    // C# FollowPath (0x4634ac, SniperRounds.cs:473-489): along the path (absolute
    // positions) without collisions until its last key, then it goes off.
    const float last =
        (float)runtime_.pathKeyFrame(round->path, runtime_.pathKeyCount(round->path) - 1) - 1.0f;
    round->pathTime += ticks;
    if (round->pathTime < last) {
        round->life = kGuidedLife;
    } else {
        round->pathTime = last;
        round->life = 0.0f;
    }

    round->position = runtime_.pathPosition(round->path, round->pathTime);
}

bool SniperRounds::testObjects(const Vec3& start, const Vec3& end, ObjectHit* best) {
    // C# TestObjects (SniperRounds.cs:495-526): the first object the segment crosses
    // (part -1 for the whole box): objects of Kurt's arena, alive, not flagged 0x30. The
    // deque keeps removed objects in their slots (`dead`), which the C#'s list drops.
    bool found = false;
    float bestDistance = FLT_MAX;
    std::deque<MdkObject>& objects = runtime_.objects();
    for (std::deque<MdkObject>::iterator it = objects.begin(); it != objects.end(); ++it) {
        MdkObject* obj = &(*it);
        if (obj->dead || obj->arena != runtime_.currentArena() || obj->health == 0 ||
            (obj->flags & kUntouchable) != 0) {
            continue;
        }

        Vec3 entry;
        if (!segmentEntry(runtime_.worldBounds(obj), start, end, &entry)) {
            continue;
        }

        Vec3 point = entry;
        int32_t part = -1;
        if (obj->model != nullptr && !hitPart(obj, start, end, &point, &part)) {
            continue;
        }

        const float d = distance(start, point);
        if (d < bestDistance) {
            bestDistance = d;
            best->object = obj;
            best->point = point;
            best->part = part;
            found = true;
        }
    }
    return found;
}

bool SniperRounds::hitPart(const MdkObject* obj, const Vec3& start, const Vec3& end, Vec3* point,
                           int32_t* part) {
    // C# HitPart (0x414668, SniperRounds.cs:531-565): the nearest face of a visible part
    // the segment crosses: the segment in model space against the pose's triangles, from
    // either side. Faces, not part boxes: LEVEL8's XBSHIP hull box holds its turrets.
    const Mat4 world = obj->transform();
    float toModel[16];
    if (!invertSystemNumericsMatrix4(world.m, toModel)) {
        return false;
    }

    // An affine map keeps the fraction along the segment.
    const Vec3 from = transformPoint(start, toModel);
    const Vec3 to = transformPoint(end, toModel);
    float nearest = 1.0f;
    int32_t found = -1;
    const std::vector<std::vector<formats::Vec3f> >& pose = obj->pose_parts();
    for (size_t i = 0; i < pose.size(); i++) {
        // C#'s `1 << i` takes the shift count modulo 32.
        if ((obj->hidden_parts & (1 << (i & 31))) != 0) {
            continue;
        }

        const std::vector<formats::Vec3f>& vertices = pose[i];
        const std::vector<int32_t>& indices = obj->model->partList[i].triangleIndices;
        for (size_t t = 0; t + 2 < indices.size(); t += 3) {
            const float hit = segmentFace(from, to, toVec3(vertices[(size_t)indices[t]]),
                                          toVec3(vertices[(size_t)indices[t + 1]]),
                                          toVec3(vertices[(size_t)indices[t + 2]]));
            if (hit < nearest) {
                nearest = hit;
                found = (int32_t)i;
            }
        }
    }

    if (found < 0) {
        return false;
    }
    *point = lerp(start, end, nearest);
    *part = found;
    return true;
}

void SniperRounds::hitObject(Round* round, MdkObject* obj, int32_t part, const Vec3& point) {
    // C# HitObject (0x462708, SniperRounds.cs:605-636): grenades explode; bullets take 8
    // hit points (not from objects with 65000 or more) and kill it at 0, else make
    // sparks. The hit event is the part + 1 (if_hit_part).
    obj->hit_event = part >= 0 ? part + 1 : kWholeObject;
    obj->hit_type = (int32_t)round->type;
    obj->hit_direction = round->yaw;
    obj->shot_part = part + 1;
    obj->shot_point = point;
    runtime_.stats.sniperHits++;
    if (round->type >= Grenade) {
        explode(round, kBlastRadius, obj);
        return;
    }

    if (obj->health < kUnhurt) {
        obj->health -= runtime_.kurtDamage(obj, kDamage);
    }

    if (obj->health > 0) {
        runtime_.sparkAt(point, 3, obj->labels[1],
                         obj->indestructible ? ScriptRuntime::Spark::Hard
                                             : ScriptRuntime::Spark::Flesh);
        end(round, Hit, kHitLinger);
        return;
    }

    obj->health = 0;
    runtime_.stats.countEnemy(obj->type_name, GameStats::Killed);
    runtime_.kill(obj, round->yaw + 180.0f);
    end(round, Killed, kHitLinger);
}

void SniperRounds::explode(Round* round, float radius, MdkObject* source) {
    // C# Explode (0x4638cc, SniperRounds.cs:639-646).
    runtime_.blast(round->position, kExplosionDamage, radius, kTargetsAll, kHitExplosion, source);
    runtime_.blast(round->position, kExplosionKurtDamage, radius, kTargetKurt, kHitExplosion,
                   source);
    runtime_.spawnExplosion(runtime_.currentArena(), round->position, kExplosionScale, 0.0f,
                            kExplosionLift);
    runtime_.playSoundAt(kExplodeSound, round->position);
    end(round, Exploded, kMissLinger);
}

void SniperRounds::end(Round* round, State state, float linger) {
    // C# End (SniperRounds.cs:648-653).
    round->state = state;
    round->linger = linger;
    round->watch = state == Killed || state == Hit ? kWatchTicks : 0.0f;
}

// ---------------------------------------------------------------------------
// The runtime's half (ScriptRuntime.cs:1222-1293, :2928): the target lock, the fire
// hook, and the mortar's box sweep. They are ScriptRuntime members, kept here beside
// the rounds they serve so script_runtime.cpp carries only the wiring.
// ---------------------------------------------------------------------------

namespace {

// ScriptMath.Wrap360 (ScriptMath.cs:14), the floor form ScriptRuntime uses.
float scriptWrap360(float degrees) { return degrees - 360.0f * floorf(degrees / 360.0f); }

} // namespace

void ScriptRuntime::updateSniperTarget() {
    // C# UpdateSniperTarget (ScriptRuntime.cs:1227-1279): the nearest object whose
    // screen box overlaps a 64-pixel square around the crosshair (640x480 pixels), not
    // flagged 0x30. Homing rounds chase it, and the zoom goes down to 0.375 x its height
    // / distance (0x4678b0) instead of 0.25.
    const int kCorners = 8;
    sniperTarget = nullptr;
    if (player_ == nullptr) {
        // A harness with no Kurt entity: no scope to lock or limit (the C# always has one).
        return;
    }
    Scope& scope = player_->scope();
    scope.zoomLimit = Scope::kZoomMin;
    if (!player_->sniping()) {
        return;
    }

    const Vec3 eye = player_->sniperEye();
    const Vec3 forward = player_->sniperForward();
    const float zoom = scope.zoom;
    float bestDepth = FLT_MAX;
    for (size_t index = 0; index < objects_.size(); index++) {
        MdkObject* obj = &objects_[index];
        if (obj->dead || obj->arena != currentArena_ || obj->model == nullptr ||
            (obj->flags & kUntouchable) != 0) {
            continue;
        }

        const formats::Box bounds = worldBounds(obj);
        const Vec3 centre3 = boxCentre(bounds);
        Vec2 centre;
        if (!Scope::toScreen(eye, forward, zoom, centre3, &centre)) {
            continue;
        }

        Vec2 min = centre;
        Vec2 max = centre;
        for (int i = 0; i < kCorners; i++) {
            const Vec3 corner{(i & 1) != 0 ? bounds.max.x : bounds.min.x,
                              (i & 2) != 0 ? bounds.max.y : bounds.min.y,
                              (i & 4) != 0 ? bounds.max.z : bounds.min.z};
            Vec2 p;
            if (Scope::toScreen(eye, forward, zoom, corner, &p)) {
                // Vector2.Min / Vector2.Max, per component.
                min = Vec2{mathMin(min.x, p.x), mathMin(min.y, p.y)};
                max = Vec2{mathMax(max.x, p.x), mathMax(max.y, p.y)};
            }
        }

        const float depth = distance(eye, centre3);
        if (Scope::locks(min, max) && depth < bestDepth) {
            bestDepth = depth;
            sniperTarget = obj;
        }
    }

    if (sniperTarget != nullptr) {
        const formats::Box bounds = worldBounds(sniperTarget);
        scope.zoomLimit = Scope::lockLimit(bounds.max.z - bounds.min.z,
                                           distance(kurtPosition_, boxCentre(bounds)));
    }
}

bool ScriptRuntime::fireSniper(int32_t type) {
    // C# FireSniper (ScriptRuntime.cs:1282-1293): a round of `type`, or Bones' air
    // strike. The strike (AirStrike.Call) is package #14 and NOT ported: no strike goes,
    // so the clip keeps Bones' round (the C#'s answer when there is no target), until
    // #14 replaces this arm with its Call(Kurt.SniperEye, Kurt.SniperForward).
    if (player_ == nullptr) {
        return false;
    }
    const bool fired =
        type == Scope::kStrike
            ? false
            : sniperRounds_.fire(type, player_->sniperEye(), scriptWrap360(player_->yaw),
                                 player_->scope().pitch, sniperTarget);
    if (fired) {
        stats.sniperShots++;
    }
    return fired;
}

bool ScriptRuntime::fireSniperTrampoline(void* ctx, int32_t type) {
    return static_cast<ScriptRuntime*>(ctx)->fireSniper(type);
}

// One arena of Sweep's loop: the box swept against that arena's BSP.
static bool sweepArena(Bsp* bsp, const std::string& name, const Vec& a, const Vec& b, const Vec& h,
                       DebrisRayHit* out) {
    if (bsp == nullptr) {
        return false;
    }
    Vec point{};
    int node = 0;
    const int triangle = bsp->sweepBox(a, b, h, 0, 0.0f, &point, &node);
    if (triangle == Bsp::kNone) {
        return false;
    }
    const float* normal = bsp->arena()->nodes()[(size_t)node].normal;
    out->point = Vec3{point.x, point.y, point.z};
    out->normal = Vec3{normal[0], normal[1], normal[2]};
    out->arena = name;
    // TriangleGroup (ScriptRuntime.cs:2958): the top byte of the triangle's flags.
    out->group = (int32_t)(bsp->arena()->triangleFlags()[(size_t)triangle] >> 24);
    return true;
}

bool ScriptRuntime::sweep(const Vec3& from, const Vec3& to, const Vec3& half, DebrisRayHit* out) {
    // C# Sweep (ScriptRuntime.cs:2928-2946): RayArenas (Kurt's arena, then the second;
    // every arena before there is a current one, as raycastHit), and the FIRST arena
    // that stops the box answers -- not the nearest, unlike Raycast.
    const Vec a{from.x, from.y, from.z};
    const Vec b{to.x, to.y, to.z};
    const Vec h{half.x, half.y, half.z};
    if (!currentArena_.empty()) {
        return sweepArena(bspOf(currentArena_), currentArena_, a, b, h, out) ||
               sweepArena(bspOf(secondArena_), secondArena_, a, b, h, out);
    }
    const std::vector<const formats::Arena*>& arenas = level_->arenas();
    for (size_t i = 0; i < arenas.size(); i++) {
        if (sweepArena(bspOf(arenas[i]->name()), arenas[i]->name(), a, b, h, out)) {
            return true;
        }
    }
    return false;
}

} // namespace scripts
} // namespace mdk

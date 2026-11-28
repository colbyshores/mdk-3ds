#include "game/frame_blend.h"

#include <cmath>

#include "game/player.h"
#include "scripts/script_runtime.h"

namespace mdk {
namespace frame_blend {

// `from + (to - from) * t` per component; the t >= 1 early return is FrameBlend
// .Lerp's own, and it is what makes the last frame of a tick land on the tick's
// pose exactly instead of a rounding of it.
//
// THIS IS NOT .NET'S Vector3.Lerp FOR EVERY INPUT, and an earlier note here claimed
// it was. MEASURED against the oracle (a probe over 400 pseudo-random
// (from, to, t) triples, printing Vector3.Lerp's raw bits): `from + (to-from)*t`
// differs from the oracle on 187 of the 400, `from*(1-t) + to*t` on 96, and
// `fmaf(from, 1-t, to*t)` on none -- that is the oracle's arithmetic. It bites in
// the game: at level 3 tick 120 one object's interpolated x is 1 ULP off
// (C2C34BB6 wanted, C2C34BB5 produced), which is why golden/L3_t120_blend.txt does
// not exist while t1, t2 and t1200 do. It is NOT fixed here because the fix is not
// one line: `fmaf` is what matches, and devkitARM lowers `fmaf` to a `bl fmaf`
// libm call at -march=armv6k -mfpu=vfp (three calls with stack spills, in a path
// that runs per object per frame), while this spelling lowers to one `vmla.f32`
// per component -- so the replacement is a codegen decision to measure
// on the target, not a substitution. Recorded with this evidence in
// docs/OPEN_ITEMS.md.
//
// The two targets DO compute identical bits for the spelling below -- for the
// flags both builds actually use: the 3DS build compiles the expression to three
// `vmla.f32` (one per component) and VFPv2's VMLA is NOT fused -- it rounds its
// product before accumulating -- so it lands on the same twice-rounded value the
// host's multiply-then-add does. Measured in mdk_3ds/Makefile and "Iteration 87":
// the shipped object is byte-identical under -ffp-contract default/=off/=fast, an
// explicitly twice-rounded intermediate also lowers to one vmla.f32, and only the
// VFPv4 VFMA form obeys that flag. TWO conditions on that sentence, both measured
// (review-29 F2, F5): (1) it holds because neither host build sets -march, so the
// x86-64 baseline has no FMA to contract into -- an -march=native host build on
// this machine emits 4 `vfmadd*` for this function and would stop describing the
// shipped object (the follow-up is to pin -ffp-contract=off host-side, named in
// docs/OPEN_ITEMS.md); (2) the 3DS runs with FPSCR.FZ (AGENTS.md R1) and the host
// does not, so a DENORMAL intermediate would flush on the target and not here --
// unreachable for coordinates of this magnitude, but not proven impossible. So the
// gap here is port-vs-oracle, not host-vs-target: `fmaf` would match the oracle on
// BOTH targets and would cost a libm call per component on this one. There is no
// flag to reach for; the expression is the only lever, and the host goldens pin
// the host build. Same caveat as every other float golden in this repository.
Vec3 lerp(Vec3 from, Vec3 to, float t)
{
    if (t >= 1.0f) {
        return to;
    }
    Vec3 result;
    result.x = from.x + (to.x - from.x) * t;
    result.y = from.y + (to.y - from.y) * t;
    result.z = from.z + (to.z - from.z) * t;
    return result;
}

// FrameBlend.Lerp(float,float,float) is float.Lerp (Single.Lerp), not the vector
// overload above and not the shortest-way angle lerp below. The `t >= 1` early
// return is the oracle's own and keeps the last frame of a tick exactly on the
// tick's value.
//
// WHY fmaf AND NOT THE TWICE-ROUNDED SPELLING THE VECTOR OVERLOAD USES. Measured
// on .NET 10 against the oracle probe (`lerp1`: 200 pseudo-random (from, to, t)
// triples plus 56 edge rows, raw bits), the oracle's arithmetic is
// `fmaf(from, 1-t, to*t)` -- float.Lerp is `(from * (1 - t)) + (to * t)`
// and the JIT contracts the first product into a fused multiply-add. It matches
// 256/256. The two spellings this could have been left as match far less:
// `from + (to-from)*t` on 169/256 and `from*(1-t) + to*t` (twice rounded, which
// is what the host build of the vector overload above emits) on 201/256 -- so the
// blended pitch, pivot and roll would have sat 1 ULP off the oracle on most
// frames, and the ±0 edge rows are not even close.
//
// The vector overload above does NOT use fmaf, and that is not an inconsistency:
// it runs once per component for every object every frame, and devkitARM lowers
// `fmaf` to a `bl fmaf` libm call at -march=armv6k -mfpu=vfp (the note above it
// records the measurement). This one runs three times a step, for the follow
// camera's own triple, which is not a hot path by AGENTS.md 2 and never will be:
// three libm calls a frame against hundreds is not the same trade, so here the
// oracle's arithmetic is affordable and it is what ships. If a caller ever moves
// this into a per-object loop, the vector overload's codegen question applies to
// it too -- do not answer it by editing this line, answer it with a measurement.
float lerp(float from, float to, float t)
{
    if (t >= 1.0f) {
        return to;
    }
    return std::fmaf(from, 1.0f - t, to * t);
}

// MathF.IEEERemainder is the IEEE 754 remainder (round-to-nearest, so the result
// is signed and its magnitude is at most half the divisor), which is exactly what
// C's remainderf is -- fmod would take the long way round on 350 -> 10.
float lerp_angle(float from, float to, float t)
{
    if (t >= 1.0f) {
        return to;
    }
    const float turn = std::remainder(to - from, 360.0f);
    return from + turn * t;
}

bool jumped(Vec3 from, Vec3 to, float seconds)
{
    const float dx = from.x - to.x;
    const float dy = from.y - to.y;
    const float dz = from.z - to.z;
    // Vector3.DistanceSquared is Dot(d, d), and Dot sums its three terms in that
    // order. At the tick rate the limit is 2500.0002, so a 50-unit move is NOT a
    // jump and a console teleport is; the last bit of that limit decides which.
    const float distance_squared = (dx * dx + dy * dy) + dz * dz;
    // The C# writes `MaxSpeed * seconds * MaxSpeed * seconds`, which groups as
    // ((MaxSpeed * seconds) * MaxSpeed) * seconds. Same grouping, same last bit.
    const float limit = ((kMaxSpeed * seconds) * kMaxSpeed) * seconds;
    return distance_squared > limit;
}

} // namespace frame_blend

// ---- class FrameBlend: C# FrameBlend.cs, Kurt's half and the objects' choice ----

namespace {

// C# Viewer.Step: Kurt's poses are a step apart (FrameBlend's Jumped tests at it).
const float kKurtStep = 1.0f / 60.0f;

Vec3 sub(const Vec3& a, const Vec3& b) { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 add(const Vec3& a, const Vec3& b) { return Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }

} // namespace

void FrameBlend::reset(const Player& player) {
    havePrevious_ = false;
    previous_ = Pose();
    current_ = Pose();
    tick_ = 1.0f;
    ticks_ = 0;
    driven_ = nullptr;
    lag_ = Vec3{0.0f, 0.0f, 0.0f};
    feet_ = player.feet;
    yaw_ = player.yaw;
}

void FrameBlend::record(const Player& player, const scripts::ScriptRuntime* runtime) {
    // Viewer.Carrier: the XE he flies; else the platform he stands on. Viewer.Driven:
    // what follows him each step (the XD2, the board), never the XE.
    const scripts::MdkObject* ridden = runtime != nullptr ? runtime->rides().ridden() : nullptr;
    const bool bomber = runtime != nullptr && runtime->rides().bomber();
    const scripts::MdkObject* platform =
        bomber ? ridden : (player.onFloor ? player.platform() : nullptr);
    const scripts::MdkObject* driven = bomber ? nullptr : ridden;
    const int32_t tick = runtime != nullptr ? runtime->tickCount() : 0;

    // What follows him (the board he stands on) doesn't carry him.
    const scripts::MdkObject* carrier = platform != driven ? platform : nullptr;
    // The carrier a step ago: before this step's tick if one ran (objects move at ticks).
    const float was = tick != ticks_ ? 0.0f : 1.0f;
    ticks_ = tick;
    Pose now;
    now.feet = player.feet;
    now.yaw = player.yaw;
    now.teleports = player.teleports;
    now.carrier = carrier;
    if (carrier != nullptr) {
        now.carrierAt = carrier->position;
        now.carrierYaw = carrier->yaw;
        now.carrierWas = carrier->position_at(was);
        now.carrierYawWas = carrier->yaw_at(was);
    }
    // A teleport is never blended across: the pose before it is forgotten.
    previous_ = !havePrevious_ || now.teleports != current_.teleports ? now : current_;
    havePrevious_ = true;
    current_ = now;
    driven_ = driven;
}

void FrameBlend::turn(float degrees) {
    if (!havePrevious_) {
        return;
    }
    previous_.yaw += degrees;
    current_.yaw += degrees;
}

void FrameBlend::update(const Player& player, float step, float tick) {
    tick_ = tick;
    if (!blending_ || !havePrevious_) {
        feet_ = player.feet;
        yaw_ = player.yaw;
    } else {
        kurtAt(previous_, current_, step, tick);
    }
    lag_ = sub(feet_, player.feet);
}

Mat4 FrameBlend::transformOf(const scripts::MdkObject& obj) const {
    if (!blending_) {
        return obj.transform();
    }
    if (&obj == driven_) {
        return multiply(obj.transform(), create_translation(lag_.x, lag_.y, lag_.z));
    }
    return obj.transform_at(tick_);
}

Vec3 FrameBlend::positionOf(const scripts::MdkObject& obj) const {
    if (!blending_) {
        return obj.position;
    }
    if (&obj == driven_) {
        return add(obj.position, lag_);
    }
    return obj.position_at(tick_);
}

void FrameBlend::kurtAt(const Pose& from, const Pose& to, float step, float tick) {
    // C# KurtAt: Kurt between two steps; on a carrier at either (landing, leaving, or
    // his fall missing it a step), his place and yaw on it -- it moves at ticks, he at
    // steps -- added to the carrier's blended place and yaw.
    const scripts::MdkObject* carrier = to.carrier != nullptr ? to.carrier : from.carrier;
    if (carrier == nullptr) {
        if (frame_blend::jumped(from.feet, to.feet, kKurtStep)) {
            feet_ = to.feet;
            yaw_ = to.yaw;
        } else {
            feet_ = frame_blend::lerp(from.feet, to.feet, step);
            yaw_ = frame_blend::lerp_angle(from.yaw, to.yaw, step);
        }
        return;
    }
    const bool fromOn = from.carrier == carrier;
    const Vec3 nearPoint = sub(from.feet, fromOn ? from.carrierAt : to.carrierWas);
    const float nearYaw = from.yaw - (fromOn ? from.carrierYaw : to.carrierYawWas);
    const bool toOn = to.carrier == carrier;
    const Vec3 farPoint = sub(to.feet, toOn ? to.carrierAt : carrier->position);
    const float farYaw = to.yaw - (toOn ? to.carrierYaw : carrier->yaw);
    const Vec3 on = frame_blend::jumped(nearPoint, farPoint, kKurtStep)
                        ? farPoint
                        : frame_blend::lerp(nearPoint, farPoint, step);
    const float turned = frame_blend::lerp_angle(nearYaw, farYaw, step);
    feet_ = add(carrier->position_at(tick), on);
    yaw_ = carrier->yaw_at(tick) + turned;
}

} // namespace mdk

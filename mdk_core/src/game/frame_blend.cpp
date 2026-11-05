#include "game/frame_blend.h"

#include <cmath>

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
} // namespace mdk

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
// that runs per object per frame), while this spelling lowers to a single
// `vmla.f32` -- so the replacement is a codegen decision to measure on the target,
// not a substitution. Recorded with this evidence in docs/OPEN_ITEMS.md.
//
// The two targets do not compute identical bits today either way: the 3DS build
// compiles the expression below to `vmla.f32` (GCC's -ffp-contract contraction),
// so the host goldens pin the host build. Same caveat as every other float golden
// in this repository.
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

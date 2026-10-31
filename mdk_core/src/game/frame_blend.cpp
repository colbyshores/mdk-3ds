#include "game/frame_blend.h"

#include <cmath>

namespace mdk {
namespace frame_blend {

// Vector3.Lerp is `from + (to - from) * t` per component; the t >= 1 early return
// is FrameBlend.Lerp's own, and it is what makes the last frame of a tick land on
// the tick's pose exactly instead of a rounding of it.
//
// MEASURED, not assumed: .NET 10's Vector3.Lerp agrees with this spelling bit for
// bit on the object places this game produces (positions of order 1e3 moving by
// 1e-2..1e1 a tick). It does NOT agree for a component of order 1e6, where the
// library lands 1 ULP off the hand-written expression; no MDK coordinate is near
// that, and the blend golden is printed at %.6f, which is where the agreement was
// checked.
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

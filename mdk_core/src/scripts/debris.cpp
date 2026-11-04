// debris.cpp — a 1:1 port of src/Mdk.Game/Scripts/Debris.cs. See debris.h.
//
// The constants keep the C#'s names in comments: gravity, bounce restitution,
// the spark tetrahedron, and the rand() ranges the original engine used
// (System.Random.Next(32768) here). All randomness comes from the runtime's
// seeded Random in the same call order as the C#, so the golden stream stays
// synchronized.

#include "scripts/debris.h"

#include <cmath>

#include "util/math_minmax.h"

namespace mdk {
namespace scripts {

namespace {

// Bounce: speed kept along the surface normal (restitution via 1.4 factor).
const float kBounce = 1.4f;
const int32_t kBounceTicks = 20;
const int32_t kTrailChance = 4;
const int32_t kLifeShift = 9;
const int32_t kRandRange = 32768;
const int32_t kRandHalf = 0x4000;
const float kRand14 = 1.0f / 16384.0f;
const float kRand15 = 1.0f / 32768.0f;
const int32_t kLaunchUp = 0x800;
const int32_t kSpinBias = 0x41c2;
const float kSpinDegrees = 28.0f / 32768.0f;
const float kShatterSpinDegrees = 0.000854492f;
const float kCornerJitter = 2e-5f;
const float kTicksPerSecond = 30.0f;
// A hit this close to the start is the surface the piece rests on.
const float kOnSurface = 1e-3f;

// A spark's tetrahedron corners and its 4 faces (Debris.SparkCorners/Faces).
const Vec3 kSparkCorners[4] = {{0.0f, 0.0f, 0.5f},
                               {0.5f, 0.0f, -0.5f},
                               {-0.5f, 0.5f, -0.5f},
                               {-0.5f, -0.5f, -0.5f}};
const int32_t kSparkFaces[12] = {0, 2, 1, 0, 3, 2, 0, 1, 3, 1, 2, 3};

Vec3 vadd(const Vec3& a, const Vec3& b) { return Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 vsub(const Vec3& a, const Vec3& b) { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 vscale(const Vec3& a, float s) { return Vec3{a.x * s, a.y * s, a.z * s}; }
// The sum order is the oracle's, and that is measured, not assumed: .NET's
// `Vector3.LengthSquared`/`Dot`/`DistanceSquared` all return the LEFT-assoc fold
// (((x*x + y*y) + z*z)) on triples whose two groupings differ -- probe mode `shatter`, row
// `ASSOC` (far 3E0D8004,3DE4D6F4,3E6DC3C0 -> lensq 3DAF1718, where the other fold is
// 3DAF1719). The `assoc` case of DebrisGroupBoxPropagatesNaNLikeTheOracle pins it at this
// site: swapping the two terms here fails that pin on one velocity row (review-27 F6).
float vdot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float vlenSq(const Vec3& a) { return vdot(a, a); }
Vec3 vcross(const Vec3& a, const Vec3& b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vec3 vnormalize(const Vec3& a) {
    float len = sqrtf(vlenSq(a));
    return Vec3{a.x / len, a.y / len, a.z / len};
}
float degreesToRadians(float degrees) {
    // .NET's `float.DegreesToRadians` is NOT `degrees * (float)(pi/180)`. It is
    // `degrees * (float)Math.PI / 180f`: TWO roundings, a multiply by the float pi
    // (0x40490FDB) and then a divide by 180f. Measured against the runtime on the
    // exact angles the shatter draws: the single-rounding factor spelling is 1 ULP
    // low in magnitude on 8 of the first 12 angles, and the two-rounding spelling
    // reproduces all 12 bit-for-bit. The old comment here claimed the double factor
    // had been proven; it proved the FACTOR's value, not the rounding, and
    // `(float)(pi/180.0) == 0.017453292f` is the same float either way -- the
    // difference is one multiply-and-round versus a multiply, a round and a divide.
    // Keep the divide: it is a constant divisor, so it is a real vdiv.f32 on VFPv2
    // (no -freciprocal-math in either build) and it is what .NET's divss does.
    const float kPi = (float)3.14159265358979323846;  // 0x40490FDB, .NET's (float)Math.PI
    return (degrees * kPi) / 180.0f;
}

// System.Numerics Quaternion.CreateFromYawPitchRoll, stored as (x, y, z, w).
void quatFromYawPitchRoll(float yaw, float pitch, float roll, float out[4]) {
    // Do not "fix" these four expressions because the shipped object lowers them to
    // vmla.f32/vnmls.f32. VFPv2's VMLA is NOT fused -- it rounds its product before
    // accumulating -- so those MACs compute the twice-rounded value the source
    // says, for normal-range intermediates (the target runs with FPSCR.FZ per
    // AGENTS.md R1 and the host does not, so a DENORMAL product would flush here and
    // not on the host; randomSpin's smallest non-zero half-angle is ~7.5e-6 rad and
    // its triple products ~4e-16, so a denormal would need the shatter falloff f
    // below ~3e-8 -- and f cannot be that small. f is `1.0f - r` with r a float
    // (debris.cpp split()), so for r in [0.5,2] the subtraction is exact and the
    // smallest non-zero |f| it can produce is 2^-24 = 5.96e-8, twice that bound;
    // f == 0 makes the angles exactly 0 and the products exact zeros, not
    // denormals). -ffp-contract cannot change any of that (measured four ways in
    // mdk_3ds/Makefile and "Iteration 87"): the shipped object is byte-identical
    // under the default, =off and =fast, an explicitly twice-rounded intermediate
    // also lowers to one vmla.f32, and the same source at -mfpu=vfpv4 becomes
    // vfma.f32 only under =fast. Replaying the shipped grouping with a rounded
    // product reproduces all 72 of the oracle's pinned spin components; replaying it
    // as a fused MAC gets 23 of 72 wrong -- that pair is a scratch model of the
    // disassembly's grouping, not a committed harness, and the recipe to re-derive
    // it is in "Iteration 88". What is NOT measured on the target is the libm leaf
    // itself: the R6 pin's "sinf/cosf match" claim was measured against glibc, and
    // the shipped link resolves them from newlib (docs/OPEN_ITEMS.md).
    /* @vfp-exempt R6 — piece-spawn spin construction, cold: once per created piece
     * and never in the per-frame update; these libm sinf/cosf are what the C#'s
     * System.Numerics calls and a LUT would change the spin orientation shown. The
     * pin sits on the three calls and not on the function because
     * audit_arm11_math.py honours a pin only within 25 lines above the site, and the
     * paragraph above is long enough to push a function-level pin out of that window
     * (review-30 Finding A). measured n/a (corpus empty). Verified 2026-11-04. */
    float sr = sinf(roll * 0.5f), cr = cosf(roll * 0.5f);
    float sp = sinf(pitch * 0.5f), cp = cosf(pitch * 0.5f);
    float sy = sinf(yaw * 0.5f), cy = cosf(yaw * 0.5f);
    out[0] = cy * sp * cr + sy * cp * sr;
    out[1] = sy * cp * cr - cy * sp * sr;
    out[2] = cy * cp * sr - sy * sp * cr;
    out[3] = cy * cp * cr + sy * sp * sr;
}

// System.Numerics Quaternion.Multiply(a, b) then Normalize (orientation step).
void quatMulNormalize(const float a[4], const float b[4], float out[4]) {
    float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    float y = a[3] * b[1] + a[1] * b[3] + a[2] * b[0] - a[0] * b[2];
    float z = a[3] * b[2] + a[2] * b[3] + a[0] * b[1] - a[1] * b[0];
    float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    float len = sqrtf(x * x + y * y + z * z + w * w);
    out[0] = x / len;
    out[1] = y / len;
    out[2] = z / len;
    out[3] = w / len;
}

} // namespace

void Debris::Piece::reset() {
    arena.clear();
    names = nullptr;
    materials.clear();
    corners.clear();
    uvs.clear();
    colourBase = 0;
    colourRange = 0;
    trailEvery = 0;
    trailTicks = 0;
    ticks = 0;
    center = Vec3{};
    velocity = Vec3{};
    orientation[0] = orientation[1] = orientation[2] = 0.0f;
    orientation[3] = 1.0f;
    spin[0] = spin[1] = spin[2] = 0.0f;
    spin[3] = 1.0f;
}

Debris::Debris(Random& rng) : rng_(rng) {
    pool_.resize(kMaxPieces);
    for (int32_t i = kMaxPieces - 1; i >= 0; i--) {
        free_.push_back(&pool_[i]);
    }
}

Debris::Piece* Debris::take() {
    if (free_.empty()) {
        // The C# allocates a fresh piece when the pool is empty.
        pool_.push_back(Piece());
        Piece* piece = &pool_.back();
        piece->reset();
        return piece;
    }
    Piece* piece = free_.back();
    free_.pop_back();
    piece->reset();
    return piece;
}

int32_t Debris::randomHalf() { return rng_.next(kRandRange) - kRandHalf; }

void Debris::randomSpin(float degrees, float out[4]) {
    // Three Angle() calls in argument order (yaw, pitch, roll), as the C#.
    float yaw = degreesToRadians((float)(rng_.next(kRandRange) - kSpinBias) * degrees);
    float pitch = degreesToRadians((float)(rng_.next(kRandRange) - kSpinBias) * degrees);
    float roll = degreesToRadians((float)(rng_.next(kRandRange) - kSpinBias) * degrees);
    quatFromYawPitchRoll(yaw, pitch, roll, out);
}

void Debris::shatter(const std::string& arena, int32_t group, float life, float size,
                     float speed, const Vec3& point, const Vec3& direction) {
    DebrisGroup shape;
    if (groupFacets_ == nullptr || !groupFacets_(ctx_, arena, group, &shape) ||
        shape.facets.empty()) {
        return;
    }
    const std::vector<DebrisFacet>& facets = shape.facets;

    // The squared distance to the farthest corner of the group's box.
    //
    // The oracle side is Debris.cs:160-161, a NESTED `Vector3.Min`/`Vector3.Max` per facet.
    // `System.Numerics` propagates a NaN the way `MathF.Min`/`MathF.Max` does and NOT the
    // way libm's NaN-silent `fminf`/`fmaxf` does, in BOTH argument orders (oracle probe
    // mode `shatter`, rows `V3MIN`/`V3MAX` and `V3MIN_REV`/`V3MAX_REV`: a NaN corner wins
    // whichever side it is on). So a NaN corner makes the box NaN, which makes `far` NaN,
    // which makes the `far > 0` test below FALSE and leaves `f` at exactly 1.0 -- every
    // piece flies at full speed. `fminf` dropped the NaN instead, kept the box finite, and
    // gave a different `f` for every piece in the group: measured on a two-facet group with
    // one NaN corner, the oracle's first piece velocity is 1.5 (bits 3FC00000) and the
    // fminf port's was 1.0810 (bits 3F8A5E98). Pinned by
    // DebrisGroupBoxPropagatesNaNLikeTheOracle in tests/scripts/vm_tests.cpp.
    //
    // The ±0 half of the helper is NOT observable here, and that is measured too: the only
    // consumer of the box is `Max(Abs(min - point), Abs(max - point)).LengthSquared()`, and
    // `Abs` erases the sign of a zero, so a group whose corners are ±0 shatters into
    // bit-identical pieces to the same group with plain 0.0 (probe rows `signed_zero` and
    // `signed_zero_plain`, both directions). The fold order is the C#'s own -- the nested
    // `Min(min, Min(A, Min(B, C)))` and this per-corner loop put the same four values
    // through the same three comparisons, and `mathMin` is associative because a NaN wins
    // and a tie returns the same bits either way.
    Vec3 min = facets[0].a;
    Vec3 max = min;
    for (size_t i = 0; i < facets.size(); i++) {
        const Vec3 corners[3] = {facets[i].a, facets[i].b, facets[i].c};
        for (int k = 0; k < 3; k++) {
            min.x = mathMin(min.x, corners[k].x);
            min.y = mathMin(min.y, corners[k].y);
            min.z = mathMin(min.z, corners[k].z);
            max.x = mathMax(max.x, corners[k].x);
            max.y = mathMax(max.y, corners[k].y);
            max.z = mathMax(max.z, corners[k].z);
        }
    }

    Shot shot;
    shot.arena = arena;
    shot.names = shape.names;
    shot.maxCross = size * size * 0.25f;
    shot.life = life;
    shot.speed = speed;
    shot.point = point;
    bool zeroDir = direction.x == 0.0f && direction.y == 0.0f && direction.z == 0.0f;
    shot.direction = zeroDir ? Vec3{} : vnormalize(direction);
    Vec3 dMin = vsub(min, point);
    Vec3 dMax = vsub(max, point);
    // The same pair again: the C# is `Vector3.Max(Vector3.Abs(min - point), Vector3.Abs(max
    // - point))`, so the max has to propagate the NaN the Abs passed through.
    Vec3 farVec{mathMax(fabsf(dMin.x), fabsf(dMax.x)), mathMax(fabsf(dMin.y), fabsf(dMax.y)),
                mathMax(fabsf(dMin.z), fabsf(dMax.z))};
    shot.far = vlenSq(farVec);
    for (size_t i = 0; i < facets.size(); i++) {
        const Vec3 c[3] = {facets[i].a, facets[i].b, facets[i].c};
        const Vec2 uv[3] = {facets[i].uvA, facets[i].uvB, facets[i].uvC};
        split(c, uv, facets[i].material, shot);
    }
}

void Debris::split(const Vec3 c[3], const Vec2 uv[3], int32_t material, const Shot& shot) {
    if ((int32_t)pieces_.size() >= kMaxPieces) {
        return;
    }
    if (vlenSq(vcross(vsub(c[1], c[0]), vsub(c[1], c[2]))) > shot.maxCross) {
        // Longest side: the corner starting it.
        int32_t longest = 0;
        for (int32_t k = 1; k < 3; k++) {
            if (vlenSq(vsub(c[(k + 1) % 3], c[k])) >
                vlenSq(vsub(c[(longest + 1) % 3], c[longest]))) {
                longest = k;
            }
        }
        int32_t a = longest;
        int32_t b = (a + 1) % 3;
        int32_t o = (a + 2) % 3;
        Vec3 middle = vscale(vadd(c[a], c[b]), 0.5f);
        Vec2 middleUv{(uv[a].x + uv[b].x) * 0.5f, (uv[a].y + uv[b].y) * 0.5f};
        const Vec3 left[3] = {c[a], middle, c[o]};
        const Vec2 leftUv[3] = {uv[a], middleUv, uv[o]};
        split(left, leftUv, material, shot);
        const Vec3 right[3] = {middle, c[b], c[o]};
        const Vec2 rightUv[3] = {middleUv, uv[b], uv[o]};
        split(right, rightUv, material, shot);
        return;
    }

    Piece* piece = take();
    piece->arena = shot.arena;
    piece->names = shot.names;
    Vec3 sum = vadd(vadd(c[0], c[1]), c[2]);
    piece->center = Vec3{sum.x / 3.0f, sum.y / 3.0f, sum.z / 3.0f};
    piece->materials.push_back(material);
    for (int k = 0; k < 3; k++) {
        piece->corners.push_back(vsub(c[k], piece->center));
        piece->uvs.push_back(uv[k]);
    }

    // Slower farther from the point.
    float f = shot.far > 0.0f ? 1.0f - vlenSq(vsub(piece->center, shot.point)) / shot.far : 1.0f;
    bool zeroDir = shot.direction.x == 0.0f && shot.direction.y == 0.0f && shot.direction.z == 0.0f;
    // The C# associates the speed LEFT: `shot.Direction * shot.Speed * f` is
    // `(Direction * Speed) * f`, two multiplies, and the radial line below is spelled the
    // same way. Folding it into `Direction * (Speed * f)` is one multiply and a different
    // float: measured 1 ULP off on every piece of a group whose direction has three real
    // components (probe rows `oblique`, pinned by DebrisGroupBoxPropagatesNaNLikeTheOracle).
    // An axis-aligned direction hides the difference, because multiplying by 1.0 is exact,
    // which is how it survived the first version of the pin. The `center == point` branch is
    // NOT part of this: the C# writes `new Vector3(0f, 0f, shot.Speed * f)` there, with the
    // parentheses in that position, and the port already matches it.
    if (!zeroDir) {
        piece->velocity = vscale(vscale(shot.direction, shot.speed), f);
    } else if (piece->center.x == shot.point.x && piece->center.y == shot.point.y &&
               piece->center.z == shot.point.z) {
        piece->velocity = Vec3{0.0f, 0.0f, shot.speed * f};
    } else {
        piece->velocity = vscale(vscale(vnormalize(vsub(piece->center, shot.point)), shot.speed), f);
    }
    randomSpin(kShatterSpinDegrees * f, piece->spin);
    piece->ticks = (int32_t)rintf(shot.life * kTicksPerSecond);
    pieces_.push_back(piece);
}

void Debris::spark(const std::string& arena, const Vec3& point, int32_t count, float size,
                   int32_t colour, int32_t range, float speed, Launch launch) {
    for (int32_t i = 0; i < count && (int32_t)pieces_.size() < kMaxPieces; i++) {
        Piece* piece = take();
        piece->colourBase = colour;
        piece->colourRange = range;
        float s = (1.0f + (float)randomHalf() * kRand15) * size;
        for (int c = 0; c < 4; c++) {
            Vec3 jitter{(float)randomHalf(), (float)randomHalf(), (float)randomHalf()};
            sparkCorners_[c] = vscale(vadd(kSparkCorners[c], vscale(jitter, kCornerJitter)), s);
        }
        for (int k = 0; k < 12; k++) {
            piece->corners.push_back(sparkCorners_[kSparkFaces[k]]);
        }
        Vec3 offset{(float)randomHalf() * kRand14, (float)randomHalf() * kRand14,
                    (float)randomHalf() * kRand15};
        piece->center = vadd(point, offset);
        start(piece, arena, speed);
        if (launch == Launch::Still) {
            piece->velocity = Vec3{};
            piece->center = point;
        }
        if (s < 1.0f) {
            piece->trailEvery = 0;
        }
        pieces_.push_back(piece);
    }
}

void Debris::start(Piece* piece, const std::string& arena, float speed) {
    piece->arena = arena;
    Vec3 velocity{(float)randomHalf() * kRand14, (float)randomHalf() * kRand14,
                  (float)(rng_.next(kRandRange) - kLaunchUp) * kRand14};
    piece->velocity = vscale(velocity, speed);
    randomSpin(kSpinDegrees, piece->spin);
    piece->ticks = (rng_.next(kRandRange) >> kLifeShift) + kLifeMin;
    if (rng_.next(kTrailChance) == 0) {
        piece->trailEvery = rng_.next(2) + 1;
    }
}

bool Debris::hasPieces(const std::string& arena) const {
    for (size_t i = 0; i < pieces_.size(); i++) {
        if (pieces_[i]->arena == arena) {
            return true;
        }
    }
    return false;
}

void Debris::update(float ticks) {
    for (int32_t i = (int32_t)pieces_.size() - 1; i >= 0; i--) {
        Piece* piece = pieces_[i];
        float next[4];
        quatMulNormalize(piece->orientation, piece->spin, next);
        for (int k = 0; k < 4; k++) {
            piece->orientation[k] = next[k];
        }
        move(piece, ticks);
        piece->ticks -= (int32_t)rintf(ticks);
        if (piece->ticks <= 0) {
            pieces_.erase(pieces_.begin() + i);
            free_.push_back(piece);
            continue;
        }
        if (piece->trailEvery == 0 || trail_ == nullptr) {
            continue;
        }
        piece->trailTicks += (int32_t)rintf(ticks);
        if (piece->trailTicks >= piece->trailEvery) {
            piece->trailTicks = 0;
            trail_(ctx_, piece->arena, piece->center);
        }
    }
}

void Debris::move(Piece* piece, float ticks) {
    Vec3 motion = vscale(piece->velocity, ticks);
    bool moved = motion.x != 0.0f || motion.y != 0.0f || motion.z != 0.0f;
    DebrisRayHit hit;
    bool hasHit = false;
    if (moved && ray_ != nullptr) {
        hasHit = ray_(ctx_, piece->arena, piece->center, vadd(piece->center, motion), &hit);
    }
    if (hasHit && vlenSq(vsub(hit.point, piece->center)) > kOnSurface * kOnSurface) {
        piece->center = hit.point;
        piece->velocity =
            vsub(piece->velocity, vscale(hit.normal, vdot(piece->velocity, hit.normal) * kBounce));
        piece->ticks -= kBounceTicks;
    } else {
        piece->center = vadd(piece->center, motion);
        piece->velocity.z -= kGravity * ticks;
    }
    float vz = NAN;
    if (updraft_ != nullptr) {
        vz = updraft_(ctx_, piece->arena, piece->center, piece->velocity.z * kTicksPerSecond,
                      ticks / kTicksPerSecond);
    }
    if (!std::isnan(vz)) {
        piece->velocity.z = vz / kTicksPerSecond;
    }
}

} // namespace scripts
} // namespace mdk


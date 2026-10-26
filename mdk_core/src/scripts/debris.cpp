// debris.cpp — a 1:1 port of src/Mdk.Game/Scripts/Debris.cs. See debris.h.
//
// The constants keep the C#'s names in comments: gravity, bounce restitution,
// the spark tetrahedron, and the rand() ranges the original engine used
// (System.Random.Next(32768) here). All randomness comes from the runtime's
// seeded Random in the same call order as the C#, so the golden stream stays
// synchronized.

#include "scripts/debris.h"

#include <cmath>

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
    // .NET's MathF.DegreesToRadians factor: pi/180 computed in double, then
    // cast to float. The float-division spelling differs by 1 ULP in about
    // half of draws (proven against the runtime by the adversarial review).
    return degrees * (float)(3.14159265358979323846 / 180.0);
}

// System.Numerics Quaternion.CreateFromYawPitchRoll, stored as (x, y, z, w).
/* @vfp-exempt R6 — piece-spawn spin construction (cold: once per created
 * piece, never in the per-frame update); libm sinf/cosf match the C#'s
 * System.Numerics values, and a LUT would change the spin orientation the
 * renderer shows. measured n/a (corpus empty). Verified 2026-10-25. */
void quatFromYawPitchRoll(float yaw, float pitch, float roll, float out[4]) {
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
    Vec3 min = facets[0].a;
    Vec3 max = min;
    for (size_t i = 0; i < facets.size(); i++) {
        const Vec3 corners[3] = {facets[i].a, facets[i].b, facets[i].c};
        for (int k = 0; k < 3; k++) {
            min.x = fminf(min.x, corners[k].x);
            min.y = fminf(min.y, corners[k].y);
            min.z = fminf(min.z, corners[k].z);
            max.x = fmaxf(max.x, corners[k].x);
            max.y = fmaxf(max.y, corners[k].y);
            max.z = fmaxf(max.z, corners[k].z);
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
    Vec3 farVec{fmaxf(fabsf(dMin.x), fabsf(dMax.x)), fmaxf(fabsf(dMin.y), fabsf(dMax.y)),
                fmaxf(fabsf(dMin.z), fabsf(dMax.z))};
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
    if (!zeroDir) {
        piece->velocity = vscale(shot.direction, shot.speed * f);
    } else if (piece->center.x == shot.point.x && piece->center.y == shot.point.y &&
               piece->center.z == shot.point.z) {
        piece->velocity = Vec3{0.0f, 0.0f, shot.speed * f};
    } else {
        piece->velocity = vscale(vnormalize(vsub(piece->center, shot.point)), shot.speed * f);
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


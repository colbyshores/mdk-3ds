// ============================================================================
// stream_tube.cpp — C# src/Mdk.Game/Stream/StreamTube.cs, operation for
// operation (see stream_tube.h). Each function names the C# member it ports.
// ============================================================================

#include "stream/stream_tube.h"

#include <cmath>

namespace mdk {
namespace stream {

namespace {

constexpr float kStartRadius = 10.0f;
// The Gunter tube straightens after this segment.
constexpr int32_t kStraightenSegment = 168;
// (rand() - 0x4000) * kWalkStep is +-1.
constexpr int32_t kRandHalf = 0x4000;
constexpr double kWalkStep = 6.10352e-5;
// Ring points are jittered by +-10 %: (rand() - 0x4000 + Jitter) / Jitter.
constexpr double kJitter = 163840.0;
constexpr float kTurnDamping = 0.8f;
constexpr double kShadeStep = 0.1;
// Vertex alpha (of 256) by distance from the tail (0x5744d8): levels 0-5 every 5 segments.
constexpr int32_t kAlpha[6] = {0x5A, 0x55, 0x50, 0x3C, 0x28, 0x0F};
constexpr int32_t kLevelSegments = 5;
constexpr int32_t kFarLevel = 5;
// The ramp (0x491ccc): a start colour, then 8 keys of 8 steps.
constexpr int32_t kRampStart[3] = {222, 206, 90};
constexpr int32_t kRampKeys[8][3] = {{140, 123, 33},  {123, 49, 8},    {198, 165, 132}, {132, 123, 140},
                                     {214, 198, 231}, {198, 165, 132}, {123, 49, 8},    {222, 206, 90}};
constexpr int32_t kRampSteps = 8;
// The difficulty's limits (0x433b50): max turn = index + base.
constexpr float kTurnBase[3] = {6.0f, 8.0f, 10.0f};
// Lights (0x434f64): in 3 segments of 4, up to +-4 units off the axis, 4-8 in
// size, flying back at 3-7 segments/s. The planet (0x4350dc) is 3 segments
// before the end, size 36.
constexpr int32_t kLightChance = 3;
constexpr float kLightSize = 4.0f;
constexpr float kLightSpeed = -3.0f;
constexpr double kSizeUnit = 8192.0;
constexpr double kOffsetUnit = 4096.0;
constexpr int32_t kPlanetBack = 3;
constexpr float kPlanetSize = 36.0f;
// Math.Tau.
constexpr double kTau = 6.283185307179586476925;

// StreamTube.MoveToward (double).
double moveToward(double from, double to, double step)
{
    if (std::fabs(to - from) <= step) {
        return to;
    }
    const double difference = to - from;
    const int32_t sign = difference > 0.0 ? 1 : (difference < 0.0 ? -1 : 0);
    return from + sign * step;
}

// Math.Clamp(double, double, double).
double clampDouble(double value, double low, double high)
{
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

// StreamTube.SetPlane.
void setPlane(Vec3* normals, float* distances, int32_t i, Vec3 a, Vec3 b, Vec3 c)
{
    const Vec3 normal = nNormalize(nCross(nSub(b, a), nSub(c, b)));
    normals[i] = normal;
    distances[i] = -nDot(normal, a);
}

} // namespace

// StreamTube(index, difficulty, kind, random).
StreamTube::StreamTube(int32_t index, Kurt::Difficulty difficulty, Kind kind, WatcomRandom* random)
    : kind_(kind), random_(random), radius_(kStartRadius)
{
    const int32_t half = index >> 1;
    maxTurn_ = index + kTurnBase[(int)difficulty];
    minRadius_ = kStartRadius - half;
    switch (difficulty) {
        case Kurt::Easy:
            maxRadius_ = 17.0f - half;
            break;
        case Kurt::Normal:
            maxRadius_ = 17.0f - index;
            break;
        default:
            maxRadius_ = 13.0f - half;
            break;
    }

    buildRamp();
    for (int32_t i = 0; i < kRings; i++) {
        bases_[i] = basisIdentity();
        origins_[i] = Vec3{};
        built_[i] = false;
        for (int32_t j = 0; j < kPoints; j++) {
            points_[i][j] = Vec3{};
            colours_[i][j] = 0;
        }
        for (int32_t p = 0; p < kPlanesPerSegment; p++) {
            normals_[i][p] = Vec3{};
            distances_[i][p] = 0.0f;
        }
    }

    // 31 rings ahead of Kurt.
    shadeFrom_ = random_->next() & (kRampSize - 1);
    shadeTo_ = random_->next() & (kRampSize - 1);
    for (int32_t i = 0; i < kRings - 1; i++) {
        generate();
    }
}

// StreamTube.BuildRamp: each key blended from the previous one in 8 steps.
void StreamTube::buildRamp()
{
    int32_t previous[3] = {kRampStart[0], kRampStart[1], kRampStart[2]};
    int32_t k = 0;
    for (int32_t key = 0; key < 8; key++) {
        for (int32_t i = 0; i < kRampSteps; i++) {
            // Blend(to, from) = (to * i + from * (8 - i)) / 8, all non-negative.
            ramp_[k].r = (uint8_t)((kRampKeys[key][0] * i + previous[0] * (kRampSteps - i)) / kRampSteps);
            ramp_[k].g = (uint8_t)((kRampKeys[key][1] * i + previous[1] * (kRampSteps - i)) / kRampSteps);
            ramp_[k].b = (uint8_t)((kRampKeys[key][2] * i + previous[2] * (kRampSteps - i)) / kRampSteps);
            k++;
        }
        previous[0] = kRampKeys[key][0];
        previous[1] = kRampKeys[key][1];
        previous[2] = kRampKeys[key][2];
    }
}

// StreamTube.Advance.
void StreamTube::advance()
{
    tail_++;
    generate();
}

// StreamTube.TakeSpawns.
void StreamTube::takeSpawns(std::vector<Spawn>* out)
{
    for (size_t i = 0; i < spawns_.size(); i++) {
        out->push_back(spawns_[i]);
    }
    spawns_.clear();
}

// StreamTube.Contains.
bool StreamTube::contains(float t) const
{
    const int32_t s = (int32_t)std::floor(t);
    return s >= tail_ && s < head_;
}

// StreamTube.Centre.
Vec3 StreamTube::centre(float t) const
{
    const int32_t s = (int32_t)std::floor(t);
    return nLerp(origins_[s & kSlotMask], origins_[(s + 1) & kSlotMask], t - (float)s);
}

// StreamTube.Frame.
TubeBasis StreamTube::frame(float t, Vec3 up) const
{
    const Vec3 y = nNormalize(nSub(centre(t + 1.0f), centre(t)));
    const Vec3 x = nNormalize(nCross(y, up));
    TubeBasis b;
    b.x = x;
    b.y = y;
    b.z = nCross(x, y);
    return b;
}

// StreamTube.Place.
Vec3 StreamTube::place(float t, float x, float z) const
{
    const int32_t s = (int32_t)std::floor(t);
    const int32_t slot = s & kSlotMask;
    return nAdd(origins_[slot], basisApply(bases_[slot], Vec3{x, kSegmentLength * (t - (float)s), z}));
}

// StreamTube.WallHit.
float StreamTube::wallHit(Vec3 position, float t, float margin) const
{
    const int32_t slot = (int32_t)std::floor(t) & kSlotMask;
    // The C#'s arrays are empty until the segment is built: no planes, no hit.
    const int32_t count = built_[slot] ? kPlanesPerSegment : 0;
    for (int32_t i = 0; i < count; i++) {
        const float d = nDot(normals_[slot][i], position) + distances_[slot][i] - margin;
        if (d > 0.0f) {
            continue;
        }

        const float towards = nDot(normals_[slot][i], nSub(position, centre(t)));
        return towards != 0.0f ? d / towards : 0.0f;
    }

    return -1.0f;
}

// StreamTube.AddTriangles.
void StreamTube::addTriangles(int32_t n, Vec3 eye, std::vector<TubeVertex>* into) const
{
    const int32_t slot = n & kSlotMask;
    const Vec3* older = points_[slot];
    const Vec3* newer = points_[(n + 1) & kSlotMask];
    const int32_t olderLevel = levelOf(n);
    int32_t newerLevel = levelOf(n + 1);

    // The newest ring has no colours yet: the first ramp colour at the farthest level.
    const bool newest = n + 2 >= head_;
    if (newest) {
        newerLevel = kFarLevel;
    }

    const Vec3* normals = normals_[slot];
    const float* distances = distances_[slot];
    for (int32_t j = 0; j < kPoints; j++) {
        const int32_t j1 = (j + 1) % kPoints;
        for (int32_t i = 0; i < 2; i++) {
            const int32_t plane = j * 2 + i;
            if (nDot(normals[plane], eye) + distances[plane] < 0.0f) {
                continue;
            }

            TubeVertex v;
            v.position = older[j];
            v.colour = colourOf(n, j);
            v.level = olderLevel;
            into->push_back(v);
            if (i == 0) {
                v.position = newer[j1];
                v.colour = newest ? 0 : colourOf(n + 1, j1);
                v.level = newerLevel;
                into->push_back(v);
                v.position = older[j1];
                v.colour = colourOf(n, j1);
                v.level = olderLevel;
                into->push_back(v);
                continue;
            }

            v.position = newer[j];
            v.colour = newest ? 0 : colourOf(n + 1, j);
            v.level = newerLevel;
            into->push_back(v);
            v.position = newer[j1];
            v.colour = newest ? 0 : colourOf(n + 1, j1);
            into->push_back(v);
        }
    }
}

// StreamTube.Rgba: the RGBA table entry 64 level + colour.
Rgba8 StreamTube::rgba(const TubeVertex& v) const
{
    const Rgb8 c = ramp_[v.colour];
    Rgba8 out;
    out.r = c.r;
    out.g = c.g;
    out.b = c.b;
    out.a = (uint8_t)kAlpha[v.level];
    return out;
}

// StreamTube.LevelOf: 0 at the tail, one more every 5 segments.
int32_t StreamTube::levelOf(int32_t n) const
{
    if (n <= tail_) {
        return 0;
    }
    const int32_t level = (n - tail_ - 1) / kLevelSegments;
    return level < kFarLevel ? level : kFarLevel;
}

// StreamTube.ColourOf.
int32_t StreamTube::colourOf(int32_t n, int32_t j) const
{
    const int32_t slot = n & kSlotMask;
    return built_[slot] ? colours_[slot][j] : 0;
}

// StreamTube.Generate: ring head's frame follows the previous one turned by the
// three angles, its points are jittered, and the segment before it gets its
// wall planes and colours.
void StreamTube::generate()
{
    const int32_t n = head_;
    if (kind_ == Gunter && n > kLastSegment) {
        spawnPlanet(n);
        return;
    }

    const int32_t slot = n & kSlotMask;
    if (n == tail_) {
        bases_[slot] = basisIdentity();
        origins_[slot] = Vec3{};
    } else {
        turnRing(n);
        spawnLight(n);
    }

    makePoints(slot);
    if (n != tail_) {
        buildSegment(n - 1);
    }

    head_++;
    walkAngles();
}

// StreamTube.WalkAngles: the angles walk randomly by +-1 degree per segment
// (the Gunter tube straightens at the end), in the order z, x, y; then the
// radius by +-1.
void StreamTube::walkAngles()
{
    static const int32_t kAxes[3] = {2, 0, 1};
    for (int32_t k = 0; k < 3; k++) {
        const int32_t axis = kAxes[k];
        double angle = (double)angles_[axis];
        if (kind_ == Gunter && head_ > kStraightenSegment) {
            angle = moveToward(angle, 0.0, 1.0);
        } else {
            angle += (random_->next() - kRandHalf) * kWalkStep;
        }

        // MaxTurn and TurnDamping are floats promoted to double, as in the C#.
        if (std::fabs(angle) > (double)maxTurn_) {
            angle *= (double)kTurnDamping;
        }

        angles_[axis] = (float)angle;
    }

    radius_ = clampDouble(radius_ + (random_->next() - kRandHalf) * kWalkStep, (double)minRadius_,
                          (double)maxRadius_);
}

// StreamTube.TurnRing: one segment along the previous ring's forward axis,
// turned by the angles, its columns normalised.
void StreamTube::turnRing(int32_t n)
{
    const int32_t slot = n & kSlotMask;
    const int32_t previous = (n - 1) & kSlotMask;
    const TubeBasis turn =
        basisMultiply(basisMultiply(basisRotation(Vec3{1.0f, 0.0f, 0.0f}, angles_[0]),
                                    basisRotation(Vec3{0.0f, 1.0f, 0.0f}, angles_[1])),
                      basisRotation(Vec3{0.0f, 0.0f, 1.0f}, angles_[2]));
    const TubeBasis b = basisMultiply(bases_[previous], turn);
    origins_[slot] = nAdd(origins_[previous], nScale(bases_[previous].y, kSegmentLength));
    bases_[slot].x = nNormalize(b.x);
    bases_[slot].y = nNormalize(b.y);
    bases_[slot].z = nNormalize(b.z);
}

// StreamTube.MakePoints: 16 points around the axis (point 0 on top), each
// coordinate jittered by +-10 %.
void StreamTube::makePoints(int32_t slot)
{
    /* @vfp-exempt R6 — a new ring's 16 points, once per segment (cold: ~6 rings a
     * second); the C#'s Math.Sin/Cos in double, which glibc sin/cos match bit for bit
     * on this machine (20000 cases, tube_basis.h), and every point is printed by the
     * stream goldens. measured n/a (corpus empty). Verified 2026-11-20. */
    for (int32_t j = 0; j < kPoints; j++) {
        const double angle = kTau * j / kPoints;
        const double jx = ((random_->next() - kRandHalf) + kJitter) / kJitter;
        const double jz = ((random_->next() - kRandHalf) + kJitter) / kJitter;
        const Vec3 local{(float)(radius_ * std::sin(angle) * jx), 0.0f, (float)(radius_ * std::cos(angle) * jz)};
        points_[slot][j] = nAdd(origins_[slot], basisApply(bases_[slot], local));
    }
}

// StreamTube.SpawnLight: a light in the segment before ring n, in 3 segments of 4.
void StreamTube::spawnLight(int32_t n)
{
    if ((random_->next() & kLightChance) == 0) {
        return;
    }

    const double size = kLightSize + random_->next() / kSizeUnit;
    const double x = (random_->next() - kRandHalf) / kOffsetUnit;
    const double z = (random_->next() - kRandHalf) / kOffsetUnit;
    const double speed = kLightSpeed - random_->next() / kSizeUnit;
    Spawn spawn;
    spawn.kind = Light;
    spawn.t = (float)n - 1.0f;
    spawn.x = (float)x;
    spawn.z = (float)z;
    spawn.size = (float)size;
    spawn.speed = (float)speed;
    spawns_.push_back(spawn);
}

// StreamTube.SpawnPlanet: the planet at the end of the Gunter tube, once.
void StreamTube::spawnPlanet(int32_t n)
{
    if (planetSpawned_) {
        return;
    }

    planetSpawned_ = true;
    Spawn spawn;
    spawn.kind = Planet;
    spawn.t = (float)(n - kPlanetBack);
    spawn.size = kPlanetSize;
    spawns_.push_back(spawn);
}

// StreamTube.BuildSegment: the wall planes (two per quad, facing in) and the
// colours of the segment from ring p to p + 1.
void StreamTube::buildSegment(int32_t p)
{
    const int32_t slot = p & kSlotMask;
    const Vec3* older = points_[slot];
    const Vec3* newer = points_[(p + 1) & kSlotMask];
    const int32_t shade = (int32_t)((1.0 - shade_) * shadeFrom_ + shade_ * shadeTo_);
    for (int32_t j = 0; j < kPoints; j++) {
        // A triangle wave around the ring, one step further each segment: spiral stripes.
        const int32_t x = (p + 1 + j) & kSlotMask;
        const int32_t wave = x < kPoints ? x : kSlotMask - x;
        colours_[slot][j] = (wave + shade) & (kRampSize - 1);

        const int32_t j1 = (j + 1) % kPoints;
        setPlane(normals_[slot], distances_[slot], j * 2, older[j], newer[j1], older[j1]);
        setPlane(normals_[slot], distances_[slot], j * 2 + 1, older[j], newer[j], newer[j1]);
    }
    built_[slot] = true;

    // The shade drifts to a new random target every 10 segments.
    shade_ += kShadeStep;
    if (shade_ > 1.0) {
        shade_ = 0.0;
        shadeFrom_ = shadeTo_;
        shadeTo_ = random_->next() & (kRampSize - 1);
    }
}

} // namespace stream
} // namespace mdk

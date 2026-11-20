// ============================================================================
// end_level.cpp -- see end_level.h. A port of src/Mdk.Game/Scripts/EndLevel.cs,
// method for method, in the C#'s order.
// ============================================================================

#include "scripts/end_level.h"

#include <math.h>

#include <algorithm>

#include "formats/arena.h"
#include "game/kurt.h"
#include "game/level_data.h"
#include "game/player.h"
#include "render/material_resolver.h"
#include "util/math_minmax.h"
#include "util/random.h"

namespace mdk {
namespace scripts {

namespace {

// EndLevel.cs's constants, under their C# names.
const float kRise = 0.025f;
const float kSpinGrowth = 0.15f;
const float kSpinMax = 2.5f;
const float kLimit = 500.0f;
const int32_t kPiecesPerTick = 8;
const float kFastRise = 3.0f;
const float kFastRiseFactor = 2.0f;
const float kTurnGrowth = 0.15f;
const float kTurnMax = 2.5f;
const float kTiltSpeed = 22.5f;
const float kTiltTarget = -60.0f;
const float kTiltShare = 0.5f;
const float kFlashStep = 8.0f;
const float kFlashEnd = 300.0f;
const float kFlashMax = 255.0f;
// Triangle flag of hidden triangles (group_set_state).
const uint32_t kHidden = formats::Arena::kGroupHidden;
// Kurt.Ticks (Kurt.cs:25): the tilt's per-second speed is per tick over this.
const float kKurtTicks = Kurt::kTicks;

// MathF.PI, and float.DegreesToRadians spelled as .NET spells it, left-associatively
// (degrees * Pi / 180f: two roundings; see script_runtime.cpp's twin).
const float kPi = 3.14159265358979323846f;
const float kTau = 6.28318530717958647692f;

float degreesToRadians(float degrees) {
    return degrees * kPi / 180.0f;
}

// The arena's vertex `k` of triangle `t`.
const formats::Vec3f& corner(const formats::Arena& arena, int32_t t, int32_t k) {
    return arena.vertices()[(size_t)arena.triangleIndices()[(size_t)t * 3 + (size_t)k]];
}

// EndLevel.Top: the highest corner's Z (Enumerable.Max over the three).
float top(const formats::Arena& arena, int32_t t) {
    float z = corner(arena, t, 0).z;
    for (int32_t k = 1; k < 3; k++) {
        const float c = corner(arena, t, k).z;
        if (c > z) {
            z = c;
        }
    }
    return z;
}

// EndLevel.Centre: the corners summed from Vector3.Zero in order, then divided
// by 3 (Vector3 / float divides each lane).
Vec3 centre(const formats::Arena& arena, int32_t t) {
    Vec3 sum{0.0f, 0.0f, 0.0f};
    for (int32_t k = 0; k < 3; k++) {
        const formats::Vec3f& v = corner(arena, t, k);
        sum.x = sum.x + v.x;
        sum.y = sum.y + v.y;
        sum.z = sum.z + v.z;
    }
    return Vec3{sum.x / 3.0f, sum.y / 3.0f, sum.z / 3.0f};
}

// Matrix3x2.CreateRotation(radians) then Vector2.Transform(v, m), as .NET 10 does
// them: the angle reduced by the IEEE remainder of Tau, the four exact quarter
// turns within 0.001 degree, else SinCos; the transform is X*v.x, then a FUSED
// multiply-add of Y*v.y (Vector2.MultiplyAddEstimate, an FMA on the oracle's
// x64), then + Z (zero). Pinned by golden/L*_end_*.txt (the pieces' sums).
/* @vfp-exempt R6 — the end of a level's pieces turn around Kurt by the C#'s own
 * Matrix3x2.CreateRotation; glibc sinf/cosf match the oracle's MathF.SinCos
 * bit-for-bit on this machine (fromAngle's probe), and the pieces' centres are
 * golden-pinned state that the fastSin/fastCos LUT would move. Once per piece per
 * 1/30 s tick, during the few seconds of a level's end. measured n/a (corpus
 * empty). Verified 2026-11-20. */
void rotate(float degrees, float x, float y, float* outX, float* outY) {
    float radians = remainderf(degreesToRadians(degrees), kTau);
    const float epsilon = 0.001f * kPi / 180.0f;
    float c;
    float s;
    if (radians > -epsilon && radians < epsilon) {
        c = 1.0f;
        s = 0.0f;
    } else if (radians > kPi / 2.0f - epsilon && radians < kPi / 2.0f + epsilon) {
        c = 0.0f;
        s = 1.0f;
    } else if (radians < -kPi + epsilon || radians > kPi - epsilon) {
        c = -1.0f;
        s = 0.0f;
    } else if (radians > -kPi / 2.0f - epsilon && radians < -kPi / 2.0f + epsilon) {
        c = 0.0f;
        s = -1.0f;
    } else {
        s = sinf(radians);
        c = cosf(radians);
    }
    // Rows X = (c, s), Y = (-s, c), Z = (0, 0).
    *outX = fmaf(-s, y, c * x) + 0.0f;
    *outY = fmaf(c, y, s * x) + 0.0f;
}

} // namespace

const char* const EndLevel::kTakeoff = "K_TAKEOF";
const char* const EndLevel::kFloating = "K_FLOATC";

void EndLevel::start(LevelData* level, const std::string& arena, Player* kurt, Random* rng) {
    // EndLevel.Start: _arena = Level.Arenas.Find(a => a.Name == CurrentArena), its
    // triangles queued highest first; then Kurt stops and rises.
    level_ = level;
    kurt_ = kurt;
    rng_ = rng;
    started_ = true;
    arena_ = level != nullptr ? level->arenaNamed(arena) : nullptr;
    arenaName_ = arena_ != nullptr ? arena_->name() : std::string();
    if (arena_ != nullptr) {
        highestFirst(*level, *arena_, &queue_);
        // Kept: no growth while it runs (AGENTS.md section 10). Every queued
        // triangle becomes one torn entry and at most one piece.
        torn_.reserve(queue_.size());
        pieces_.reserve(queue_.size());
    }
    if (kurt_ != nullptr) {
        kurt_->frozen = true;
        kurt_->rising = true;
    }
}

void EndLevel::highestFirst(const LevelData& level, const formats::Arena& arena,
                            std::vector<int32_t>* out) {
    out->clear();
    const std::vector<const formats::TextureArchive*> archives = level.archivesOf(&arena);
    const std::vector<uint32_t>& flags = arena.triangleFlags();
    const std::vector<int32_t>& materials = arena.triangleMaterials();
    for (int32_t t = 0; t < arena.triangleCount(); t++) {
        if ((flags[(size_t)t] & kHidden) != 0) {
            continue;
        }
        if (!MaterialResolver::isDrawn(materials[(size_t)t], arena.materials(), archives)) {
            continue;
        }
        out->push_back(t);
    }
    // OrderByDescending(Top): stable, so equal tops keep the triangles' order.
    std::vector<std::pair<float, int32_t> > keyed;
    keyed.reserve(out->size());
    for (size_t i = 0; i < out->size(); i++) {
        keyed.push_back(std::make_pair(top(arena, (*out)[i]), (*out)[i]));
    }
    std::stable_sort(keyed.begin(), keyed.end(),
                     [](const std::pair<float, int32_t>& a, const std::pair<float, int32_t>& b) {
                         return a.first > b.first;
                     });
    for (size_t i = 0; i < keyed.size(); i++) {
        (*out)[i] = keyed[i].second;
    }
}

bool EndLevel::update(float ticks) {
    if (done_ || !started_ || kurt_ == nullptr) {
        return false;
    }

    Player& kurt = *kurt_;
    tearOff(rng_->next(kPiecesPerTick));
    movePieces(kurt.feet, ticks);

    // Kurt takes off, turns and rises (0x40b558).
    turn_ = mathMin(turn_ + kTurnGrowth * ticks, kTurnMax);
    kurt.yaw -= turn_ * ticks;
    riseSpeed_ = riseSpeed(riseSpeed_, ticks);
    kurt.feet.z += riseSpeed_ * ticks;
    takeoff_ += ticks;
    kurt.hasPose = true;
    kurt.poseName = takeoff_ < (float)takeoffFrames_ ? kTakeoff : kFloating;
    kurt.poseFrame = (int32_t)takeoff_;
    if (riseSpeed_ <= kFastRise) {
        return false;
    }

    // The view tilts up, then the screen goes white.
    const float arenaPitch = level_->arenaPitch(arenaName_);
    const float tilt = (kTiltTarget - arenaPitch) * kTiltShare;
    if (pitchOffset_ > tilt) {
        pitchOffset_ = mathMax(pitchOffset_ - kTiltSpeed * ticks / kKurtTicks, tilt);
        kurt.cameraTilt = pitchOffset_;
        return false;
    }

    flash_ += kFlashStep * ticks;
    kurt.combat().setWhiteFlash(mathMax(kurt.combat().whiteFlash(), mathMin(flash_, kFlashMax)));
    if (flash_ <= kFlashEnd) {
        return false;
    }

    done_ = true;
    return true;
}

float EndLevel::riseSpeed(float speed, float ticks) {
    speed += kRise * ticks;
    return speed > kFastRise ? speed + kRise * ticks * kFastRiseFactor : speed;
}

void EndLevel::tearOff(int32_t count) {
    // The next `count` highest triangles become pieces.
    for (int32_t i = 0; i < count && queueHead_ < queue_.size(); i++) {
        const int32_t triangle = queue_[queueHead_++];
        torn_.push_back(triangle);
        Piece piece;
        piece.triangle = triangle;
        piece.centre = centre(*arena_, triangle);
        pieces_.push_back(piece);
    }
}

void EndLevel::movePieces(const Vec3& kurt, float ticks) {
    // The pieces rise and spin around Kurt; those 500 above him go.
    const float limit = kurt.z + kLimit;
    for (int32_t i = (int32_t)pieces_.size() - 1; i >= 0; i--) {
        Piece& piece = pieces_[(size_t)i];
        piece.riseSpeed += kRise * ticks;
        piece.spin = mathMin(piece.spin + kSpinGrowth * ticks, kSpinMax);
        float aroundX;
        float aroundY;
        rotate(piece.spin * ticks, piece.centre.x - kurt.x, piece.centre.y - kurt.y, &aroundX,
               &aroundY);
        piece.centre = Vec3{kurt.x + aroundX, kurt.y + aroundY,
                            piece.centre.z + piece.riseSpeed * ticks};
        piece.angle += piece.spin * ticks;
        if (piece.centre.z > limit) {
            pieces_.erase(pieces_.begin() + i);
        }
    }
}

} // namespace scripts
} // namespace mdk

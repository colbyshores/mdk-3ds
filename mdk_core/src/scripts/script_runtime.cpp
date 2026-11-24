// ============================================================================
// script_runtime.cpp — see script_runtime.h.
// ============================================================================

#include "scripts/script_runtime.h"

#include <float.h>
#include <math.h>
#include <string.h>

#include <algorithm>

#include "formats/ascii_case.h"
#include "formats/bin_reader.h"
#include "formats/bni.h"
#include "formats/dti.h"
#include "formats/model_animation.h"
#include "game/bsp.h"
#include "game/level_flow.h"
#include "game/player.h"
#include "util/math_lut.h"
#include "util/math_minmax.h"

namespace mdk {
namespace scripts {

using formats::Model;
using formats::ModelAnimation;
using formats::Vec3f;
using formats::Box;

const float ScriptRuntime::kTick = 1.0f / 30.0f;

namespace {

const uint32_t kDtiAlien = 2;
const uint32_t kDtiStatic = 4;
const int32_t kDtiStaticFlags = MdkObject::FlagPickup | MdkObject::FlagNotSolid2 |
                                MdkObject::FlagNoBanking | MdkObject::FlagNotTarget;

float wrap360(float degrees) {
    return degrees - 360.0f * floorf(degrees / 360.0f);
}

// MathF.PI, as a float.
const float kPi = 3.14159265358979323846f;

// float.DegreesToRadians / float.RadiansToDegrees. .NET spells BOTH left-
// associatively (x * MathF.PI / 180f, x * 180f / MathF.PI) — verified against
// the real runtime — so the constant-folded (pi/180f)/(180f/pi) forms are
// different by 1 ULP at many inputs, and the yaws the goldens pin would move
// in the 4th decimal. Keep the multiply-then-divide order exactly.
static float degreesToRadians(float degrees) {
    return degrees * kPi / 180.0f;
}

static float radiansToDegrees(float radians) {
    return radians * 180.0f / kPi;
}

// Case-insensitive string equality (the C#'s Equals(OrdinalIgnoreCase) / Is).
static bool iequalsType(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

// MathF.Round's single-argument form (MidpointRounding.ToEven): exact halves go
// to the EVEN neighbour. C roundf is halves-away-from-zero and disagrees at
// every N.5, which would move the animation's frame clock a frame off the
// oracle (ScriptMath.Round is a DIFFERENT helper: it passes AwayFromZero, and
// the plain MathF.Round here is a deliberate banker's round).
static int32_t roundToEven(float v) {
    if (v != v) {
        return 0;
    }
    const float f = floorf(v);
    const float d = v - f;
    if (d > 0.5f) {
        return (int32_t)f + 1;
    }
    if (d < 0.5f) {
        return (int32_t)f;
    }
    return ((int32_t)f & 1) != 0 ? (int32_t)f + 1 : (int32_t)f;
}

// ScriptMath.WrapAngle: an angle folded into [-180, 180).
float wrapAngle(float degrees) {
    return degrees - 360.0f * floorf((degrees + 180.0f) / 360.0f);
}

// ScriptMath.FromAngle: the unit heading (x, y) for a yaw in degrees. cosf/sinf
// match the oracle's MathF.SinCos bit-for-bit on this machine (probed); the
// conversion is .NET's left-associative DegreesToRadians (the LUT would move the step).
/* @vfp-exempt R6 — heading from yaw in the walk/fly step; glibc cosf/sinf match
 * the C# oracle's MathF.SinCos bit-for-bit on this machine (probed), and the
 * conversion is .NET's left-associative DegreesToRadians, while the fastSin/
 * fastCos LUT would move the walked position. measured n/a (corpus empty).
 * Verified 2026-10-23. */
Vec3 fromAngle(float degrees) {
    float radians = degreesToRadians(degrees);
    return Vec3{cosf(radians), sinf(radians), 0.0f};
}

float clampf(float value, float lo, float hi) {
    return value < lo ? lo : (value > hi ? hi : value);
}

// ScriptMath.Heading: the degrees from `from` to `to`, NOT folded into any range
// (Wrap360/WrapAngle are the caller's job). This is yawTo without that helper's
// wrap: the C# returns RadiansToDegrees(Atan2(...)) straight, and the runner's
// turn is `WrapAngle(Heading + 180 - Yaw)`, where adding a wrapped heading would
// add 360 to the argument of a floor-based wrap. glibc atan2f matches the oracle's
// MathF.Atan2 bit-for-bit on this machine (probed, see yawTo's pin) and the
// conversion is .NET's left-associative RadiansToDegrees; this is cold per-object
// behaviour, so it keeps the libm call rather than the fastAtan2 LUT, whose error
// would move the turned yaw.
float heading(const Vec3& from, const Vec3& to) {
    return radiansToDegrees(atan2f(to.y - from.y, to.x - from.x));
}

float signf(float value) {
    return value > 0.0f ? 1.0f : (value < 0.0f ? -1.0f : 0.0f);
}

// ScriptMath.MoveToward (ScriptMath.cs:19): step from `from` toward `to` by at most
// `step`, landing exactly on `to` rather than overshooting it. The C# applies it to
// raw angles as well as to distances, so this does NOT wrap an angle difference:
// FollowFormation's yaw chase relies on the unwrapped form exactly as the oracle
// does.
float moveToward(float from, float to, float step) {
    return fabsf(to - from) <= step ? to : from + signf(to - from) * step;
}

// TriangleNormal (ScriptRuntime.cs:2950): the normalized cross product of the
// hit triangle's second and third edges, or +Z when the face is degenerate.
Vec3 triangleNormal(const formats::Arena* arena, int32_t triangle) {
    Vec3 normal{0.0f, 0.0f, 1.0f};
    if (arena == nullptr || triangle < 0) {
        return normal;
    }
    const std::vector<int32_t>& indices = arena->triangleIndices();
    const std::vector<formats::Vec3f>& vertices = arena->vertices();
    if ((size_t)triangle >= indices.size() / 3) {
        return normal;
    }
    const size_t base = (size_t)triangle * 3;
    const int32_t ia = indices[base];
    const int32_t ib = indices[base + 1];
    const int32_t ic = indices[base + 2];
    if ((size_t)ia >= vertices.size() || (size_t)ib >= vertices.size() ||
        (size_t)ic >= vertices.size()) {
        return normal;
    }
    const formats::Vec3f a = vertices[(size_t)ia];
    const formats::Vec3f b = vertices[(size_t)ib];
    const formats::Vec3f c = vertices[(size_t)ic];
    const float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
    const float vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
    const float nx = uy * vz - uz * vy;
    const float ny = uz * vx - ux * vz;
    const float nz = ux * vy - uy * vx;
    if (nx != 0.0f || ny != 0.0f || nz != 0.0f) {
        const float length = sqrtf(nx * nx + ny * ny + nz * nz);
        normal = Vec3{nx / length, ny / length, nz / length};
    }
    return normal;
}

// mathMin/mathMax are the oracle's MathF.Min/MathF.Max and live in
// util/math_minmax.h, ONE definition for the whole tree. The oracle's speed
// clamps are MathF.Min/MathF.Max (ObjectMotion.cs:784,788,916,917), so a NaN
// speed stays NaN through them there; fminf would silently turn it into the
// finite bound.

// Vector3 equality: the planner sets Waypoint to a copy of MoveDestination, so exact
// component equality is the "already heading for the destination" test the C# uses.
bool vecEq(const Vec3& a, const Vec3& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

const float kTurnSpeed = 180.0f;
const float kSlowTurnAngle = 80.0f;
const float kPlanHeight = 8.0f;

// ScriptMath.RotatedZ: turn about Z by yaw degrees, keep Z (the yaw).
Vec3 rotatedZ(const Vec3& v, float degrees) {
    /* @vfp-exempt R6 — the footing sweep's yaw rotation of the collision box; the
     * C# oracle uses MathF.SinCos (exact), and libm sinf/cosf track it to within
     * this machine (probed), and the conversion is .NET's left-associative
     * DegreesToRadians; the fastSin/fastCos LUT's error is far larger and
     * error is far larger and would move the box and its landing. measured n/a
     * (corpus empty). Verified 2026-10-23. */
    float s = sinf(degreesToRadians(degrees));
    float c = cosf(degreesToRadians(degrees));
    return Vec3{v.x * c - v.y * s, v.x * s + v.y * c, v.z};
}

// Little-endian reads from the CMI image (memcpy: the ARM11 is unforgiving about
// aliasing/unaligned access, and these land on unaligned path keys).
float readF32(const uint8_t* b, int at) {
    float v;
    memcpy(&v, b + at, 4);
    return v;
}
int32_t readS32(const uint8_t* b, int at) {
    int32_t v;
    memcpy(&v, b + at, 4);
    return v;
}
uint32_t readU32(const uint8_t* b, int at) {
    uint32_t v;
    memcpy(&v, b + at, 4);
    return v;
}

// The CMI path layout: a U32 count, then keys of 40 bytes (an S32 frame then
// three 12-byte vectors: position, end tangent, start tangent).
const int kPathCountSize = 4;
const int kPathKeySize = 40;
const int kPathFrameSize = 4;
const int kPathVectorSize = 12;

// One Hermite component (ObjectMotion.PathPosition's per-axis math).
float hermite(float p0, float p1, float m0, float m1, float u) {
    float d = p1 - p0;
    float a = m0 + m1 - d * 2.0f;
    float b = d * 3.0f - m0 * 2.0f - m1;
    return ((a * u + b) * u + m0) * u + p0;
}

} // namespace

ScriptRuntime::ScriptRuntime(LevelData* level, const formats::Cmi* cmi, int32_t seed,
                             audio::SoundBank* bank, Player* player,
                             const formats::Bni* sprites)
    : player_(player), level_(level), cmi_(cmi),
      decoder_(cmi->bytes().data(), (int32_t)cmi->bytes().size()), rng_(seed),
      vm_(*this, decoder_), bank_(bank), mixer_(bank), debris_(rng_), sprites_(sprites) {
    // Debris delegates (ScriptRuntime.cs:340-346), all four as the C# wires them: the
    // arena ray, the smoke trail (Effects.SpawnTrail -- a break-up piece trails one time
    // in four, so an explosion's pieces leave puffs in the effects list), the fans'
    // updraft for effects (Fans.Query with MaskEffects), and the group facets.
    debris_.setCallbacks(this, &ScriptRuntime::debrisRayTrampoline,
                         &ScriptRuntime::debrisTrailTrampoline,
                         &ScriptRuntime::debrisUpdraftTrampoline,
                         &ScriptRuntime::debrisFacetsTrampoline);
    // AnimatedTextures' frame counts come from the level's archives
    // (ScriptRuntime.cs:336, TextureFrames via FindTexture).
    animatedTextures = AnimatedTextures(&ScriptRuntime::animatedFrameCount, this);
    // The fans stand on the DTI's records and share the runtime's RNG
    // (ScriptRuntime.cs:337: `new Fans(...) { Spark = FanSpark, IsLive = IsLiveArena }`).
    // The spark is the C#'s FanSpark: a still fire spark in the debris, which the fans'
    // updraft for effects (the Debris callback above) then lifts. It draws from the
    // runtime's RNG like every spark, so a fan in a live arena moves the stream exactly
    // as the oracle's does (golden/L*_wind_*.txt).
    fans = Fans(&level_->dti(), &rng_);
    fans.setSpark(&ScriptRuntime::fanSparkTrampoline, this);
    fans.setIsLive(&ScriptRuntime::fanIsLive, this);
    // The effects share the RNG, the archives' frame counts and the
    // arena ray (ScriptRuntime.cs:341's neighbours).
    effects = Effects(&rng_);
    effects.setFrameCount(&ScriptRuntime::animatedFrameCount, this);
    effects.setRay(&ScriptRuntime::debrisRayTrampoline, this);
    // The arenas the world is built from, as the driver does: the reachable
    // ones, so the arena under Kurt is found.
    const std::vector<const formats::Arena*>& arenas = level_->arenas();
    for (size_t i = 0; i < arenas.size(); i++) {
        if (level_->isReachable(arenas[i]->name())) {
            space_.add(arenas[i]);
        }
    }

    // The C# TriangleGroups registration (LevelView's constructor, before the
    // ScriptRuntime exists) converts every arena's flagged triangles once, when
    // the level is registered for play. It is NOT part of Arena::parse: the
    // 1996 demo sets its clip flag AFTER parsing, and its flagged triangles
    // mean something else. Retail arenas carry clip flag 0, so this is where
    // LEVEL8 GUNT_2's invisible diagonal wall becomes the non-solid wall the
    // original has.
    for (size_t i = 0; i < arenas.size(); i++) {
        formats::Arena* writable = level_->arenaNamedMutable(arenas[i]->name());
        if (writable != nullptr) {
            writable->applyDisabledTriangles();
        }
    }

    // Kurt starts at the level's start (the driver places him there; nothing
    // moves him on the first tick).
    const float* start = level_->dti().startPosition();
    kurtPosition_.x = start[0];
    kurtPosition_.y = start[1];
    kurtPosition_.z = start[2];
    kurtYaw_ = wrap360(level_->dti().startAngle());
    targetPosition_ = kurtPosition_;
    targetYaw_ = kurtYaw_;
    previousKurtPosition_ = kurtPosition_;

    // The arenas' doorways are paired once, when the runtime is built, exactly as
    // the C# does (ScriptRuntime.cs:364). Every crossing test in a tick reads this
    // table, so a runtime without it is a level with no doors.
    findConnections();

    // Kurt's item events (ScriptRuntime.cs:331-333): ItemUsed += Items.UseItem,
    // BombTriggered += Items.TriggerBomb, CanUseItem = Items.CanUse.
    if (player_ != nullptr) {
        Player::ItemHooks hooks;
        hooks.context = this;
        hooks.itemUsed = &ScriptRuntime::itemUsedTrampoline;
        hooks.bombTriggered = &ScriptRuntime::bombTriggeredTrampoline;
        hooks.canUseItem = &ScriptRuntime::canUseItemTrampoline;
        player_->setItemHooks(hooks);
        // Kurt's solids and keeper (ScriptRuntime.cs:334, 346): kurt.SolidsWithin =
        // SolidsWithin, kurt.Keeper = KeeperAt (package #3, scripts/platforms.cpp).
        Player::SolidHooks solids;
        solids.context = this;
        solids.solidsWithin = &ScriptRuntime::solidsWithinTrampoline;
        solids.keeperAt = &ScriptRuntime::keeperAtTrampoline;
        player_->setSolidHooks(solids);
        // Kurt's sniper fire (ScriptRuntime.cs:330, kurt.SniperFire = FireSniper): the
        // clip asks the rounds for a free slot (sniper_rounds.cpp).
        Scope::FireHook fire;
        fire.context = this;
        fire.fire = &ScriptRuntime::fireSniperTrampoline;
        player_->setSniperHooks(fire);
        // Kurt's updraft (ScriptRuntime.cs:345): kurt.Updraft = (vz, dt) =>
        // KurtUpdraft(kurt.Feet, vz, dt).
        Player::UpdraftHook updraft;
        updraft.context = this;
        updraft.updraft = &ScriptRuntime::kurtUpdraftTrampoline;
        player_->setUpdraftHook(updraft);
    }

    // The town's timer, by the difficulty, except on the last level (Gunter's) and
    // in the 1996 demo, which this runtime does not load (ScriptRuntime.cs:367-370;
    // TownTicksByDifficulty :38, LastLevelIndex :40). Package #19 carries it in a
    // save; FlattenTown at 0 is #15's (runTick).
    static const int32_t kTownTicksByDifficulty[3] = {81000, 54000, 36000};
    static const int32_t kLastLevelIndex = 5;
    if (flow::indexOf(level_->number()) != kLastLevelIndex) {
        const int32_t difficulty = player_ != nullptr ? (int32_t)player_->combat().difficulty()
                                                      : (int32_t)Kurt::Normal;
        townTicks = kTownTicksByDifficulty[difficulty];
    }
}

ScriptRuntime::~ScriptRuntime() {
    forgetBoxModels();
    if (player_ != nullptr && player_->itemHooks().context == this) {
        player_->setItemHooks(Player::ItemHooks());
        // The platform, the keeper and the touched list point into objects_ (package #3).
        player_->releaseSolids();
    }
    if (player_ != nullptr && player_->sniperHooks().context == this) {
        player_->setSniperHooks(Scope::FireHook());
    }
    if (player_ != nullptr && player_->updraftHook().context == this) {
        player_->setUpdraftHook(Player::UpdraftHook());
    }
}

Vec3 ScriptRuntime::kurtLiveVelocity() const {
    // Read the live Player at dispatch time. A snapshot would silently keep
    // the previous tick's velocity when the frontend forgot to push an update.
    return player_ != nullptr ? player_->velocity() : Vec3{};
}

void ScriptRuntime::update(float delta) {
    // ScriptRuntime.cs:378-390 accumulates the elapsed time and completes a
    // 1/30-second tick for each whole interval. The C# Viewer calls this
    // once per 1/60-second gameplay step, so its scripts run at 30 Hz while
    // Kurt's physics run at 60 Hz.
    tickAccumulator_ += delta;
    while (tickAccumulator_ >= kTick) {
        tickAccumulator_ -= kTick;
        // The C# snapshots every object's pose before the tick runs
        // (ScriptRuntime.cs:383-386), because the frames until the next tick are
        // drawn from here to there. It walks its live Objects list; the port keeps
        // removed objects in the deque for the pointers other objects hold, and
        // snapshotting one that is not drawn costs a pose copy and changes nothing
        // — the snapshot of a removed object is never read.
        for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
            it->remember();
        }
        runTick();
        tickCount_++;
    }
    // Rides.Follow (ScriptRuntime.cs:392, package #9): each step, after the ticks, the
    // XD2 is where Kurt is.
    rides_.follow();
}

float ScriptRuntime::tick_blend(float ahead) const {
    // Math.Clamp((time + ahead) / Tick, 0f, 1f) — the C#'s TickBlend, with the
    // runtime's own leftover time as `time`.
    const float alpha = (tickAccumulator_ + ahead) / kTick;
    return alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
}

namespace {

// The DTI record type of a link between two arenas (C# LevelData.Connection,
// LevelData.cs:10). Two arenas that carry the same connection id are joined, and
// each of them writes down the doorway it is reached through.
const uint32_t kRecordConnection = 6;

// The DTI record types of a trigger box (C# ScriptRuntime.TriggerShow / TriggerLoad,
// ScriptRuntime.cs:705-706, CheckTriggers 0x41bf1c): type 1 SHOWS the arena its id names and
// type 3 LOADS it ahead. Both are boxes written in Kurt's arena and both are tested every
// tick; nothing else in the DTI is a trigger.
const uint32_t kRecordTriggerShow = 1;
const uint32_t kRecordTriggerLoad = 3;

// C# ScriptRuntime.DoorwayDrop / HatchDrop (ScriptRuntime.cs:82-83). A horizontal
// doorway's box reaches DoorwayDrop below its own bottom, so a walk into it counts
// even when Kurt is a little below the sill; a hatch (a MinusZ/PlusZ doorway) is
// crossed at HatchDrop below its record, i.e. just about at the floor.
const float kDoorwayDrop = 5.0f;
const float kHatchDrop = 0.5f;

// C# ScriptRuntime.Doorway (ScriptRuntime.cs:775): the connection record's ANGLE
// field, read as an integer, says which way a move goes out through the doorway.
// The numbers are the enum's, in its order, and every arm of the C#'s switch is
// here — including the two hatches (MinusZ/PlusZ), which are NOT decoration: the
// shipped DTIs of levels 3-8 use 0,1,2,3 AND 6,7, and level 5's doorways are ALL
// hatches (measured: docs/evidence/2026-11-10-crossing.txt — dropping the two hatch
// arms seals every hatch on levels 3, 5, 6 and 8). Only Left/Right (4/5) are absent
// from every shipped level, exactly as the C#'s comment says of the diagonals.
enum Doorway {
    kDoorwayMinusX = 0,
    kDoorwayPlusX = 1,
    kDoorwayMinusY = 2,
    kDoorwayPlusY = 3,
    kDoorwayRight = 5,
    kDoorwayMinusZ = 6,
    kDoorwayPlusZ = 7
};

// The record's angle reinterpreted as the Doorway integer, exactly the C#'s
// BitConverter.SingleToInt32Bits(record.Angle). This is a BIT pattern, not a
// numeric conversion: nothing is rounded, and the same 4 bytes decide the doorway on
// both targets. memcpy because the aliasing rules are not a place to be clever.
int32_t doorwayOf(float angle) {
    int32_t bits = 0;
    memcpy(&bits, &angle, sizeof(bits));
    return bits;
}

// Whether a move from a to b on one axis is within [min, max] or crosses it
// (C# ScriptRuntime.Spans, :832). mathMin/mathMax, not fminf/fmaxf: the oracle
// writes MathF.Min/MathF.Max, which propagate a NaN instead of hiding it.
bool spans(float a, float b, float min, float max) {
    return mathMax(a, b) >= min && mathMin(a, b) <= max;
}

// C# ScriptRuntime.SegmentsCross (:756-770): whether the segment a-b crosses c-d, in XY.
// The parametric form, INCLUSIVE at both ends (t and u in [0,1], so a move that merely
// touches a corner counts), and FALSE for parallel segments -- including collinear ones,
// which the denominator cannot separate. The orientation-sign form a port might reach for
// disagrees at exactly those two places, so this is :758-769 line for line. u is (q x r)/
// denominator with q = c - a, exactly as :766 writes it: the second term carries r.X, and a
// slip to s.X makes a horizontal move cross a box it is nowhere near.
bool segmentsCross(const formats::Vec2f& a, const formats::Vec2f& b, const formats::Vec2f& c,
                   const formats::Vec2f& d) {
    const float rx = b.x - a.x;
    const float ry = b.y - a.y;
    const float sx = d.x - c.x;
    const float sy = d.y - c.y;
    const float denominator = rx * sy - ry * sx;
    if (denominator == 0.0f) {
        return false;
    }
    const float t = ((c.x - a.x) * sy - (c.y - a.y) * sx) / denominator;
    const float u = ((c.x - a.x) * ry - (c.y - a.y) * rx) / denominator;
    return t >= 0.0f && t <= 1.0f && u >= 0.0f && u <= 1.0f;
}

// C# ScriptRuntime.Crosses (:743-755): whether a move crosses one of the four edges of a box
// in XY. The corner order is the C#'s array literal and `(i + 1) % 4` closes the loop, so the
// four edges are the box's, and a move that starts inside and ends inside crosses none of
// them -- which is why CheckTriggers tests `inside` SEPARATELY and on the END of the move
// only.
bool crossesBox(const formats::Vec2f& min, const formats::Vec2f& max, const formats::Vec2f& from,
                const formats::Vec2f& to) {
    const formats::Vec2f corners[4] = {min, formats::Vec2f{max.x, min.y}, max,
                                       formats::Vec2f{min.x, max.y}};
    for (int i = 0; i < 4; i++) {
        if (segmentsCross(from, to, corners[i], corners[(i + 1) % 4])) {
            return true;
        }
    }
    return false;
}

// Whether a move goes out through a doorway: across its plane in its direction, and
// the move within (or across) the doorway on the other axes (C# CrossesDoorway,
// :808-830). A horizontal doorway's box reaches kDoorwayDrop below it; the diagonal
// ones only test where the move ENDS. The record's min is its Position and its max
// its BoxEnd -- the far corner the DTI reader keeps in the name's bytes.
bool crossesDoorway(const formats::Dti::Record& record, const Vec3& from, const Vec3& to) {
    const float minX = record.position[0];
    const float minY = record.position[1];
    const float minZ = record.position[2];
    const float maxX = record.boxEnd[0];
    const float maxY = record.boxEnd[1];
    const float maxZ = record.boxEnd[2];
    // A hatch (a MinusZ/PlusZ doorway) is crossed just below its record, at
    // HatchDrop: Kurt's feet are ON the floor he steps through, not 5 units under it.
    const float plane = minZ - kHatchDrop;
    const bool withinX = spans(from.x, to.x, minX, maxX);
    const bool withinY = spans(from.y, to.y, minY, maxY);
    const bool withinZ = spans(from.z, to.z, minZ - kDoorwayDrop, maxZ);
    // Which side of the min..max diagonal the move ends on, for the two diagonal
    // doorways. Written exactly as the C# spells it, including the operand order.
    const float side =
        (to.x - minX) * (maxY - minY) - (maxX - minX) * (to.y - minY);
    switch (doorwayOf(record.angle)) {
        case kDoorwayMinusX:
            return withinY && withinZ && to.x < minX && from.x >= minX;
        case kDoorwayPlusX:
            return withinY && withinZ && to.x > minX && from.x <= minX;
        case kDoorwayMinusY:
            return withinX && withinZ && to.y < minY && from.y >= minY;
        case kDoorwayPlusY:
            return withinX && withinZ && to.y > minY && from.y <= minY;
        case kDoorwayMinusZ:
            return withinX && withinY && to.z < plane && from.z >= plane;
        case kDoorwayPlusZ:
            return withinX && withinY && to.z > plane && from.z <= plane;
        case kDoorwayRight:
            return withinX && withinY && withinZ && side > 0.0f;
        default:
            // Doorway.Left, and every other value the field can hold: the C#'s `_`
            // arm, so an unknown angle reads as Left and not as "no doorway".
            return withinX && withinY && withinZ && side < 0.0f;
    }
}

}  // namespace

void ScriptRuntime::doorBubbleArenas(const Vec3& position, float radius,
                                     std::vector<std::string>* out) const {
    const std::string& arena = currentArena_;
    if (!out || radius <= 0.0f || arena.empty()) {
        return;
    }
    // arenaRecords is non-const (it builds the arena's record list on
    // first ask): the const_cast reaches that builder, which the tick
    // itself runs for the current arena -- the query changes no script
    // state beyond that lazy build.
    const std::vector<formats::Dti::Record>* records =
        const_cast<ScriptRuntime*>(this)->arenaRecords(arena);
    if (records == nullptr) {
        return;
    }
    const float radius2 = radius * radius;
    std::vector<std::pair<float, std::string>> byDistance;
    for (size_t i = 0; i < records->size(); i++) {
        const formats::Dti::Record& record = (*records)[i];
        if (record.type != kRecordConnection) {
            continue;
        }
        std::map<std::pair<std::string, int32_t>, std::string>::const_iterator other =
            connections_.find(std::make_pair(arena, record.id));
        if (other == connections_.end() || other->second.empty() ||
            other->second == arena) {
            continue;
        }
        // Distance from the position to the doorway box (clamped per
        // axis): inside the box is zero, which any radius admits.
        float d2 = 0.0f;
        const float p[3] = {position.x, position.y, position.z};
        for (int j = 0; j < 3; j++) {
            const float lo = record.position[j] < record.boxEnd[j] ? record.position[j]
                                                                   : record.boxEnd[j];
            const float hi = record.position[j] < record.boxEnd[j] ? record.boxEnd[j]
                                                                   : record.position[j];
            const float d = p[j] < lo ? lo - p[j] : (p[j] > hi ? p[j] - hi : 0.0f);
            d2 += d * d;
        }
        if (d2 > radius2) {
            continue;
        }
        byDistance.push_back(std::make_pair(d2, other->second));
    }
    // Nearest first, so a caller bounding the hint count keeps the
    // doorways Kurt reaches soonest.
    std::sort(byDistance.begin(), byDistance.end());
    for (size_t i = 0; i < byDistance.size(); i++) {
        bool seen = false;
        for (size_t k = 0; k < out->size(); k++) {
            if ((*out)[k] == byDistance[i].second) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            out->push_back(byDistance[i].second);
        }
    }
}

std::string ScriptRuntime::crossedArena(const std::string& arena, const Vec3& from,
                                        const Vec3& to) {
    // C# ScriptRuntime.CrossedArena (:791-803): the FIRST connection record of this
    // arena whose doorway the move crosses, and whose id is paired with another
    // arena, is the arena the move enters. The order is the DTI's record order, and
    // an unpaired record is skipped -- both of those decide an answer when two
    // doorways are close enough that one move crosses both.
    const std::vector<formats::Dti::Record>* records = arenaRecords(arena);
    if (records == nullptr) {
        return std::string();
    }
    for (size_t i = 0; i < records->size(); i++) {
        const formats::Dti::Record& record = (*records)[i];
        if (record.type != kRecordConnection || !crossesDoorway(record, from, to)) {
            continue;
        }
        std::map<std::pair<std::string, int32_t>, std::string>::const_iterator other =
            connections_.find(std::make_pair(arena, record.id));
        if (other != connections_.end()) {
            return other->second;
        }
    }
    return std::string();
}

void ScriptRuntime::followArenas(MdkObject* obj) {
    // C# FollowArenas (:780-787): the arena its OWN last move crossed, and only a
    // non-empty answer replaces the object's arena -- a move through no doorway
    // leaves it where it is.
    const std::string arena = crossedArena(obj->arena, obj->previous_position, obj->position);
    if (!arena.empty()) {
        obj->arena = arena;
    }
}

const std::vector<formats::Dti::Record>* ScriptRuntime::arenaRecords(
    const std::string& arena) {
    std::map<std::string, std::vector<formats::Dti::Record>>::iterator found = records_.find(arena);
    if (found != records_.end()) {
        return &found->second;
    }
    if (unknownArenas_.count(arena) != 0) {
        return nullptr;  // the C#'s empty list for a name no arena has, cached
    }
    const formats::Dti::ArenaEntry* entry = level_->entryNamed(arena);
    if (entry == nullptr) {
        unknownArenas_.insert(arena);
        return nullptr;
    }
    // Copy into runtime-owned storage: a borrowed &entry->records would dangle if
    // the DTI arenas vector were ever replaced (LevelData::mutableDti()). std::map
    // keeps this element's address stable for the runtime's life.
    std::pair<std::map<std::string, std::vector<formats::Dti::Record>>::iterator, bool> inserted =
        records_.emplace(arena, entry->records);
    return &inserted.first->second;
}

void ScriptRuntime::findConnections() {
    // C# FindConnections (:856-881), over EVERY arena in the DTI. Two arenas that
    // write down the same connection id are joined; the grouping is by id and the
    // pairing is every ordered pair inside a group, so with the usual two arenas per
    // id each of them gets the other. Should a level ever give one id three arenas,
    // the C#'s nested loops leave the LAST one of the group as the answer, in DTI
    // order -- reproduced, not "fixed", because the C# is the oracle.
    std::map<int32_t, std::vector<std::string> > byId;
    const std::vector<formats::Dti::ArenaEntry>& arenas = level_->dti().arenas();
    for (size_t i = 0; i < arenas.size(); i++) {
        const formats::Dti::ArenaEntry& entry = arenas[i];
        for (size_t j = 0; j < entry.records.size(); j++) {
            if (entry.records[j].type == kRecordConnection) {
                byId[entry.records[j].id].push_back(entry.name);
            }
        }
    }
    for (std::map<int32_t, std::vector<std::string> >::iterator it = byId.begin();
         it != byId.end(); ++it) {
        const std::vector<std::string>& names = it->second;
        for (size_t a = 0; a < names.size(); a++) {
            for (size_t b = 0; b < names.size(); b++) {
                if (names[a] != names[b]) {
                    connections_[std::make_pair(names[a], it->first)] = names[b];
                }
            }
        }
    }
}

void ScriptRuntime::runTick() {
    // The C# copies Kurt's live feet and yaw into the script runtime at the
    // start of every tick (ScriptRuntime.cs:886-889). A bound Player is the
    // live source; with no Player, the level-start state remains in place.
    if (player_ != nullptr) {
        kurtPosition_ = player_->feet;
        kurtYaw_ = wrap360(player_->yaw);
        targetPosition_ = kurtPosition_;
        targetYaw_ = kurtYaw_;
    }

    // A cutscene's tick replaces the game's (ScriptRuntime.cs:890-894): only some objects
    // run, no controller, no countdown, trigger, pickup, gun or effect -- everything below
    // waits until EndCutscene.
    if (cutscene != 0) {
        cutsceneTick();
        return;
    }

    // The alarm countdown decays once per tick, before any object runs
    // (ScriptRuntime.cs:896), after the cutscene's early return above.
    alarmTicks = alarmTicks > 0 ? alarmTicks - 1 : 0;

    // The camera-track request lasts for the current tick and the next one
    // (ScriptRuntime.cs:898).
    cameraTrackTicks = cameraTrackTicks > 0 ? cameraTrackTicks - 1 : 0;

    // The screen-shake request belongs to one tick (ScriptRuntime.cs:899).
    // Calls made later in this tick replace it only when they are stronger.
    shake = 0.0f;
    // The health bar's second runs out, or its object died (ScriptRuntime.cs:900).
    updateBar();
    // The town's timer (ScriptRuntime.cs:901-908, package #15): at 0 the minecrawler
    // has reached the town.
    if (townTicks > 0) {
        townTicks--;
        if (townTicks == 0) {
            flattenTown();
        }
    }

    // The first tick has no movement to observe (ScriptRuntime.cs:911-917,
    // after the decrements above, as in the oracle). A teleport can set the
    // previous position, and when it did BEFORE this first tick the C# keeps it
    // (`&& !_teleported`, :911): the soak tour teleported onto a doorway before the
    // first tick, the first tick overwrote the teleport's destination with the same
    // place, and Kurt stayed in the room while standing in the corridor until it went
    // (ArenaSwitchTests.FirstTickKeepsTeleport). The flag is never cleared, and cannot
    // need clearing: this rule only runs at tick 0.
    if (tickCount_ == 0 && !teleported_) {
        previousKurtPosition_ = kurtPosition_;
    }

    // Kurt changes arena only through a connection; on the first tick he is
    // placed and the arena under him is found (ScriptRuntime.cs:918-919).
    //
    // The C#'s first branch is `IsBeta ? BetaArena() : ...`. The 1996 demo is not
    // reachable from this runtime (see the IsBeta note by the animation lookup), so
    // the retail branch is the whole behaviour here; when the demo lands, BetaArena
    // (:29-34) has to be ported and NOT approximated by the geometric arena, which
    // is a different rule (smallest box around Kurt, only if the current arena
    // connects to it).
    std::string arena;
    if (currentArena_.empty()) {
        formats::Vec3f at;
        at.x = kurtPosition_.x;
        at.y = kurtPosition_.y;
        at.z = kurtPosition_.z;
        const std::string* found = space_.arenaAt(at);
        arena = found ? *found : std::string();
    } else {
        arena = crossedArena(currentArena_, previousKurtPosition_, kurtPosition_);
    }

    if (!arena.empty() && arena != currentArena_) {
        if (!currentArena_.empty()) {
            secondArena_ = currentArena_;
            // Kurt.EnterArena() (ScriptRuntime.cs:926): a slide that carries him through
            // a doorway ends 15 ticks later (Kurt.Slide.cs:97-105); it does nothing
            // unless he is sliding with no slide budget running.
            if (player_ != nullptr) {
                player_->enterArena();
            }
        }
        secondActive_ = true;
        currentArena_ = arena;
        showArena(arena);
    }

    // The collision world is snapshotted HERE, at the oracle's own position:
    // ScriptRuntime.cs:934-935 runs CheckTriggers() and then UpdateArenas(), and
    // UpdateArenas is the only writer of _drawnArenas (:694-695). Everything after it
    // in the C# -- KurtTouchesGroups, Rides.Update, CollectPickups, the object loop
    // (:962-987) and RunControllers (:991) -- can still move CurrentArena,
    // SecondArena and SecondActive (a door opening/closing, an arena_show opcode) and
    // the oracle's pin does NOT follow them until the next tick.
    //
    // CheckTriggers is the other writer, and it is the LAST one above the snapshot: the
    // oracle's own order is `CheckTriggers(); UpdateArenas();` (:934-935), so the arena a
    // trigger box shows or preloads belongs to THIS tick's pin, while a door or an arena_show
    // that runs in the object loop below belongs to the next one's.
    checkTriggers();
    updateDrawnArenas();
    // KurtTouchesGroups (ScriptRuntime.cs:936), after UpdateArenas and before
    // Rides.Update and CollectPickups (package #3).
    kurtTouchesGroups();
    // Rides.Update (ScriptRuntime.cs:937, package #9): getting on, riding the XD2,
    // getting off -- after Kurt moved, before the pickups and the objects.
    rides_.update();

    // ScriptRuntime.cs:945-946 records Kurt's observed feet delta before the
    // object scripts run. The C# computes it after CollectPickups; pickups and
    // the other movement writers that can precede it are later slices, so the
    // copied Player position is the complete source in this runtime.
    //
    // The cutscene early-return above (ScriptRuntime.cs:890-894) skips this write AND
    // the previous-position update during a cutscene, as the C# does. One oracle
    // detail this slice does not have yet: the C#'s save/load snapshot
    // resets the previous position (ScriptRuntime.Snapshot.cs:37-39), so the
    // first tick after a load observes no movement. The snapshot slice must
    // reset previousKurtPosition_ the same way.
    // CollectPickups (ScriptRuntime.cs:940-943): from the second tick on, after
    // KurtTouchesGroups and Rides.Update and BEFORE the velocity
    // write below, because it reads the segment from the previous position.
    if (tickCount_ > 0) {
        collectPickups();
    }
    kurtVelocity.x = (kurtPosition_.x - previousKurtPosition_.x) * kTicksPerSecond;
    kurtVelocity.y = (kurtPosition_.y - previousKurtPosition_.y) * kTicksPerSecond;
    kurtVelocity.z = (kurtPosition_.z - previousKurtPosition_.z) * kTicksPerSecond;
    previousKurtPosition_ = kurtPosition_;

    // Kurt moves and takes pickups, then fires, then the objects run (game_frame,
    // ScriptRuntime.cs:947-950): one tick of the chain gun while Kurt.Firing.
    if (kurt().firing()) {
        fireChainGun();
    }

    // Items.UpdateTwisters (ScriptRuntime.cs:952), then Effects.Update(1f),
    // Fans.Update(), Debris.Update(1f) (ScriptRuntime.cs:953-955): the twisters,
    // effects, fan sparks, then the pieces move, all before the object loop, as in
    // the C#.
    updateTwisters();
    effects.update(1.0f);
    fans.update();
    debris_.update(1.0f);
    // SniperRounds.Update(1f) (ScriptRuntime.cs:956): the rounds fly and hit, before the
    // objects run. AirStrike.Update(1f) next (ScriptRuntime.cs:957, package 14): the strike
    // flies its curve, drops its teeth or dives, and crashes. Then EndLevel?.Update(1f)
    // (ScriptRuntime.cs:958): the end of the level tears, lifts and turns, before the
    // objects run. Its Finished event is EndLevel.cs:1509's LevelEnded(GameOver.No),
    // latched here.
    sniperRounds_.update(1.0f);
    airStrike_.update(1.0f);
    if (endLevel_ && endLevel_->update(1.0f)) {
        levelEnded_ = true;
    }
    // UpdateSniperTarget (ScriptRuntime.cs:959): the scope's lock and zoom limit.
    updateSniperTarget();

    // Only Kurt's arena and the active second one update. On the first tick
    // nothing has spawned yet, so this loop is empty; the controller below is
    // what creates the level's objects.
    std::vector<MdkObject*> snapshot;
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        snapshot.push_back(&(*it));
    }
    for (size_t i = 0; i < snapshot.size(); i++) {
        MdkObject* obj = snapshot[i];
        if (obj->dead) {
            continue;
        }
        bool live = obj->arena == currentArena_ || (secondActive_ && obj->arena == secondArena_);
        if (!live) {
            continue;
        }
        // A door's behaviour runs BEFORE its script (ScriptRuntime.cs:970-973), so
        // the script's if_flag_clear sees the state the door engine just wrote.
        if ((obj->flags & MdkObject::FlagDoor) != 0) {
            updateDoor(obj);
        }
        // Where the object was before its script and motion (ScriptRuntime.cs:975): the
        // platform carry below moves Kurt by the difference (package #3).
        const Vec3 carriedFrom = obj->position;
        const float carriedYaw = obj->yaw;
        vm_.run(obj);
        // ObjectMotion.Update (ScriptRuntime.cs:976-979: `if (!obj.Dead) Motion.Update(obj)`),
        // shared with the cutscene tick (motionUpdate below).
        if (!obj->dead) {
            motionUpdate(obj);
        }
        // Kurt moves and turns with the platform he stands on (ScriptRuntime.cs:982-986,
        // after Motion.Update), for an effect too. Where motionUpdate returned early the
        // object is dead (a removed thrown item, a cow on its fuse), which the carry skips.
        carryKurt(obj, carriedFrom, carriedYaw);
    }

    runControllers();
}

// C# ObjectMotion.Update, as RunTick's object loop and CutsceneTick (ScriptRuntime.cs:1398-1402)
// both call it after Vm.Run: one copy, so the cutscene's objects move exactly as the game's.
void ScriptRuntime::motionUpdate(MdkObject* obj) {
    // ObjectMotion.Update's first branch (ObjectMotion.cs:54-68): an effect (an
    // explosion) only plays its texture animation and returns, so nothing below --
    // the path, the velocity, the arena crossing, the pickup, the animation, the
    // roll -- runs for it; every gate below tests the same `effect_frames <= 0`.
    if (!obj->dead && obj->effect_frames > 0) {
        motionEffect(obj);
        return;
    }
    // The path-following half of Motion.Update, gated on liveness exactly as
    // the C# (Vm.Run then `if (!Dead) Motion.Update`): a script that kills the
    // object this tick must not have it path-moved afterwards.
    if (!obj->dead) {
        motionPath(obj);
        motionVelocity(obj);
        // An object that changes arena follows its own move through a doorway
        // (ObjectMotion.cs:104-107, after ApplyVelocity and before the pickup
        // dispatch). motionVelocity returns early on a zero-length move, and so
        // does the C#'s ApplyVelocity -- the C#'s Update keeps going to
        // FollowArenas either way, so the hook is HERE, not inside
        // motionVelocity, even though a move that did not happen crosses nothing.
        //
        // The effect gate is NOT decoration: ObjectMotion.Update returns at
        // :54-68 for EffectFrames > 0, BEFORE ApplyVelocity and therefore before
        // FollowArenas at :106, so an effect object never evaluates a crossing in
        // the oracle. It matters here for the same reason it does there:
        // previous_position is written only for a non-effect object (below, with
        // Roll and Bank), so an effect's "move" would be read off a stale segment
        // from before the effect started and could relocate it across a doorway.
        //
        // The gate is the INVERSE of the oracle's predicate, so it is `<= 0` and
        // not `== 0`: the C# returns for a POSITIVE count and runs the normal path
        // for anything else, and the three gates in this loop must agree with
        // motionPath() and motionVelocity(), which test `> 0` to return.
        // No writer in either tree makes it negative today (the oracle's two are
        // ScriptRuntime.cs:1780 and :1784, both positive counts), so this is
        // exactness against the oracle rather than a live behaviour change, and no
        // golden can distinguish the two spellings until an effects port writes a
        // writer.
        if (!obj->dead && obj->effect_frames <= 0 &&
            (obj->flags & MdkObject::FlagChangesArena) != 0) {
            followArenas(obj);
        }
        // Kurt's items (ObjectMotion.cs:110-118): a thrown item (ThrownKind > 0) goes
        // to Items.UpdateThrown, and the C# returns when that removed it -- no
        // animation, roll, bank or previous position for it this tick; otherwise
        // UpdatePickup for a pickup. Both only if the object survived the velocity
        // step (the C# returns on Dead before them), and never for an effect object.
        if (!obj->dead && obj->effect_frames <= 0 && obj->thrown_kind > 0) {
            itemsUpdateThrown(obj);
            if (obj->dead) {
                return;
            }
        } else if (!obj->dead && obj->effect_frames <= 0 &&
                   (obj->flags & MdkObject::FlagPickup) != 0) {
            motionPickup(obj);
        } else if (!obj->dead && obj->effect_frames <= 0 &&
                   (obj->flags & MdkObject::FlagCow) != 0) {
            // SW_EWJ's holy cow (ObjectMotion.cs:120-127): it may be killed by its fuse.
            updateCow(obj);
            if (obj->dead) {
                return;
            }
        }
        // AdvanceAnimation (ObjectMotion.cs:132), after ApplyVelocity and the
        // pickup dispatch. An effect object returned from Update before this
        // point, so it never advances an animation here. Same inverse predicate
        // as the two gates above: `<= 0`, because the oracle returns on `> 0`.
        if (!obj->dead && obj->effect_frames <= 0) {
            float time = obj->animation_time;
            advanceAnimation(obj, kTick);
            // The frame sound (ObjectMotion.cs:133): fired once when the frame
            // clock crosses its frame, at the object's position.
            if (!obj->frame_sound.empty() && obj->animation != nullptr &&
                time < (float)obj->frame_sound_frame &&
                (float)obj->frame_sound_frame <= time + kTick * obj->animation_rate *
                                                       obj->animation->speed) {
                playSound(obj, obj->frame_sound, 0, formats::ScriptValue());
                obj->frame_sound.clear();
            }
            // Roll (ObjectMotion.cs:139-143), then PreviousPosition
            // (ObjectMotion.cs:146) -- IN THAT ORDER, and both here rather than
            // at the end of motionVelocity: Roll reads the PREVIOUS frame's
            // position, so writing it any earlier makes every roll read a zero
            // movement and the basis never turns.
            if ((obj->flags & MdkObject::FlagRolling) != 0) {
                obj->update_rolling();
            }
            // Bank (ObjectMotion.cs:145), between Roll and PreviousPosition.
            motionBank(obj);
            obj->previous_position = obj->position;
        }
    }
}

void ScriptRuntime::runControllers() {
    if (!currentArena_.empty()) {
        vm_.run(&getArenaState(currentArena_).controller);
    }
    if (secondActive_ && !secondArena_.empty()) {
        vm_.run(&getArenaState(secondArena_).controller);
    }
}

void ScriptRuntime::updateDrawnArenas() {
    // C# ScriptRuntime.UpdateArenas (:658-664, the drawn half). The current arena goes
    // in even when it is "", exactly as :660 does: the empty name is ArenaSpace's to
    // drop (its C# Named() finds no arena called ""), not this function's. The second
    // goes in only while it is BOTH active and named (:661-664). Both rooms being in
    // the list is the whole point of it: move() sweeps element 0 as Kurt's own arena
    // and the rest as the free pass (arena_space.cpp), so the room behind the door he
    // came through keeps its floor.
    //
    // First the loaded half (:650-692, 0x419cb0): the current arena (named or not) and
    // the second (named, active or not) are loaded; when that pair changes, an arena
    // that left it is put away -- Kurt's thrown items and effects there (flags 0x1000 or
    // 0x4000, so a collected pickup too) go, the other objects' loop sounds stop -- and
    // one that came back restarts its objects' loop sounds (arena_activate 0x43f8e0).
    // The comparison is the C#'s Same: ordered, element by element.
    std::vector<std::string> loaded;
    loaded.push_back(currentArena_);
    if (!secondArena_.empty()) {
        loaded.push_back(secondArena_);
    }
    if (loaded != loadedArenas_) {
        std::vector<MdkObject*> snapshot;
        for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
            if (!it->dead) {
                snapshot.push_back(&(*it));
            }
        }
        for (size_t i = 0; i < snapshot.size(); i++) {
            MdkObject* obj = snapshot[i];
            const bool wasLoaded = std::find(loadedArenas_.begin(), loadedArenas_.end(),
                                             obj->arena) != loadedArenas_.end();
            const bool isLoaded = std::find(loaded.begin(), loaded.end(), obj->arena) != loaded.end();
            if (wasLoaded && !isLoaded) {
                if ((obj->flags & (kItemFlagThrown | kItemFlagActive)) != 0) {
                    itemsForget(obj);
                    remove(obj);
                    continue;
                }
                mixer_.stopVoice(obj->loop_sound);
                obj->loop_sound = 0;
            } else if (isLoaded && !wasLoaded && !obj->loop_sound_name.empty()) {
                obj->loop_sound = playOn(obj, obj->loop_sound_name, audio::SoundMixer::New, Vec3());
            }
        }
        loadedArenas_ = loaded;
    }

    drawnArenas_.clear();
    drawnArenas_.push_back(currentArena_);
    if (secondActive_ && !secondArena_.empty()) {
        drawnArenas_.push_back(secondArena_);
    }
    // obj.Visible (:696-699): an object is drawn only while its arena is. This is
    // the consumer the drawn list was waiting for -- until it was written every
    // object of every arena the scripts had ever shown was drawn, wherever Kurt was
    // -- and it is what lets the frontends keep only the drawn arenas' textures
    // (game/arena_views.h). The C#'s other term, `!Rides.Hides(obj)` (the bomber's
    // XE once the view is inside it), is rides_.hides(), false until the bomber
    // (package #11) lands.
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        bool drawn = false;
        for (size_t i = 0; i < drawnArenas_.size() && !drawn; i++) {
            drawn = drawnArenas_[i] == it->arena;
        }
        it->visible = drawn && !rides_.hides(&(*it));
    }
}

const std::vector<std::string>& ScriptRuntime::solidArenas() const {
    // C# ScriptRuntime.SolidArenas (ScriptRuntime.cs:265-282). Off the board it is the
    // drawn list itself -- the snapshot updateDrawnArenas() took at the tick's own
    // UpdateArenas point, NOT a fresh read of currentArena_/secondArena_ (which is what
    // a door or an arena_show that ran later in the tick would have moved). On the
    // board it is drawn[0] alone (:274-280), and drawn[0] is always the current arena
    // because :660 adds it unconditionally -- so the board case is this list without the
    // second, which is what the `!rides_.onBoard()` half below encodes. The snowboard
    // (package #10, scripts/snowboard.cpp) reaches it: golden/L4_board_*.txt pin the one
    // name from the mount on, and both names again once Kurt is off.
    //
    // Before the first tick drawnArenas_ is empty and so is the C#'s _drawnArenas:
    // ArenaSpace::setSolid then clears its pin and Kurt collides with the arenas around
    // his feet, in both trees.
    if (!rides_.onBoard()) {
        return drawnArenas_;
    }
    solidArenas_.clear();
    if (!drawnArenas_.empty()) {
        // drawn[0] is the current arena, so on the board the room behind the door is
        // NOT swept: that is the oracle's rule, not an oversight (:274-280).
        solidArenas_.push_back(drawnArenas_[0]);
    }
    return solidArenas_;
}

void ScriptRuntime::showArena(const std::string& arena) {
    if (arena.empty() || arena == "NONE") {
        secondArena_ = "";
        secondActive_ = false;
        return;
    }

    if (arena == currentArena_) {
        // The C# loads Kurt's own arena too (:582-586): it is the one arena whose doors
        // PullDoors must NOT move, but its resident-arena bookkeeping still runs.
        loadArena(arena);
    } else if (arena != secondArena_) {
        secondArena_ = arena;
        loadArena(arena);
    }

    secondActive_ = true;
    ArenaState& state = getArenaState(arena);
    if (!state.started) {
        state.started = true;
        spawnDtiAliens(arena);
    }
}

// C# ScriptRuntime.PreloadArena (ScriptRuntime.cs:602-612, arena_set_neighbour 0x41a2d0, VM
// opcode 223): the arena takes the SECOND slot and is loaded, but the second stays INACTIVE,
// so the room is loaded ahead of Kurt -- its doors pulled, its MTO resident -- without being
// drawn or live with him. "NONE" is the C#'s NoArena (:85). The `arena == SecondArena` test
// runs BEFORE the assignment (:604), so preloading the arena that is already the second does
// nothing at all: not the deactivation, not LoadArena, so it pulls that arena's doors no
// second time. SecondArena is reassigned BEFORE the load and SecondActive is cleared AFTER it
// (:609-611), so during a preload the previous second is no longer SecondArena by the time
// PullDoors runs and the only exemption that can apply to its doors is Kurt's own arena. That
// is the oracle's ordering and HALF of it is observable: hoisting the assignment above the
// load leaves the previous second in `secondArena_` while PullDoors runs, and a door held by
// a neighbour that is that previous second is then exempt and does not move. Moving the clear
// above the load is NOT observable, and the reason is worth writing down because it is the
// reason a reader cannot see it: once `secondArena_` is the arena being loaded, the second
// exemption would need a connection whose far end is that same arena, and `findConnections()`
// never writes one -- its only writer pairs two DIFFERENT names (the `names[a] != names[b]`
// guard), so no arena is ever its own neighbour.
void ScriptRuntime::preloadArena(const std::string& arena) {
    if (arena.empty() || arena == "NONE" || arena == secondArena_) {
        return;
    }
    secondArena_ = arena;
    loadArena(arena);
    secondActive_ = false;
}

// C# ScriptRuntime.LoadArena (ScriptRuntime.cs:615-623, arena_load 0x419d00): "only one arena
// (not corridor) is loaded at a time" -- the MTO holds a ROOM's geometry and a corridor is not
// in it, so loading a corridor leaves the resident arena alone and only pulls its doors.
// Three oracle callers, all three ported: showArena, preloadArena, and TeleportSecond
// (:516) -- the last one landed with TeleportKurt, so a teleport that only names the
// neighbour does not leave it an inactive, unloaded second with its doors still parked
// in the room it came from.
void ScriptRuntime::loadArena(const std::string& arena) {
    if (level_->mto().has(arena)) {
        residentArena_ = arena;
    }
    pullDoors(arena);
}

// Defined below with the rest of the door engine (ObjectBehaviors.MoveDoor, 0x43ca00).
static void moveDoor(MdkObject* obj);

// C# ScriptRuntime.PullDoors (ScriptRuntime.cs:628-643): an arena being loaded takes the doors
// leading to it out of the neighbours that hold them and turns them around, so Kurt meets them
// in the room he is walking towards -- except from his own arena and from the active second,
// which keep theirs. Without it a door stays parked in the corridor it was spawned in, which
// is the artefact the oracle's DoorArenaTests class comment opens with ("the doorway showed the
// sky"): the door is not in the drawn list, so neither is its geometry.
//
// The pixel half of that artefact is NOT reachable here yet. What makes an undrawn door
// invisible is the drawn-list CONSUMERS -- the C#'s `obj.Visible` (ObjectBehaviors, unported;
// MdkObject::visible still defaults to true and has no writer) and the renderer's arena list --
// and neither is ported, so geometry is drawn regardless of the list. This restores the oracle's
// RULE ahead of its effect, the way residentArena_ is written ahead of its reader; the batch that
// ports obj.Visible still owes itself the capture that shows the doorway change.
//
// The C#'s Objects.Where(...) is a filter over the whole object list and the loop below is the
// same predicate per object; moveDoor changes only arena/connects/yaw, so nothing here
// disturbs the sequence the way a removal would.
void ScriptRuntime::pullDoors(const std::string& arena) {
    const std::vector<formats::Dti::Record>* records = arenaRecords(arena);
    if (records == nullptr) {
        return;  // the C#'s ArenaRecords() is an empty list for an arena the DTI does not know
    }
    for (size_t i = 0; i < records->size(); i++) {
        const formats::Dti::Record& record = (*records)[i];
        if (record.type != kRecordConnection) {
            continue;
        }
        std::map<std::pair<std::string, int32_t>, std::string>::const_iterator other =
            connections_.find(std::make_pair(arena, record.id));
        if (other == connections_.end()) {
            continue;
        }
        // The two exemptions, in the C#'s order (:633): the neighbour Kurt is standing in, and
        // the neighbour that is the ACTIVE second. Both keep their door, because a door pulled
        // out of a drawn arena would leave its doorway showing nothing.
        if (other->second == currentArena_ || (secondActive_ && other->second == secondArena_)) {
            continue;
        }
        for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
            MdkObject* door = &(*it);
            if (!door->dead && (door->flags & MdkObject::FlagDoor) != 0 &&
                door->arena == other->second && door->connects == arena) {
                moveDoor(door);
            }
        }
    }
}

// C# ScriptRuntime.EnterArena (ScriptRuntime.cs:449-456): the arena joins Kurt's
// collision space. This is how an arena no doorway leads to becomes solid -- until a
// script teleports Kurt into it, nothing puts it in the space (level.gd enter_arena),
// and ArenaSpace::add already refuses an arena with no vertices, as the C#'s Add does.
// The C#'s `ArenaEntered?.Invoke(arena)` runs first; its one subscriber is
// LevelView.Enter (Viewer.cs:255 -> Level/LevelView.cs:87), which sets `Reachable` on the
// arena's VIEW so the draw includes it. The level view is presentation and is deferred,
// so the event has no C++ twin and this is the half that has a consumer.
void ScriptRuntime::enterArena(const std::string& arena) {
    const formats::Arena* entered = level_->arenaNamed(arena);
    if (entered != nullptr) {
        space_.add(entered);
    }
}

// C# ScriptRuntime.IsCorridorLoaded (ScriptRuntime.cs:522-531): the corridors loaded with
// an arena are its own, Kurt's, and the second's -- so a teleport into one of them does
// not have to go looking for a room to load ahead. This is the ONLY reader of
// residentArena_ in the whole oracle, which is what batch 139 left "written and not yet
// read"; with TeleportKurt ported it is read.
bool ScriptRuntime::isCorridorLoaded(const std::string& corridor, const std::string& from) {
    if (corridor == secondArena_ || corridor == from) {
        return true;
    }
    const std::vector<formats::Dti::Record>* records = arenaRecords(residentArena_);
    if (records == nullptr) {
        return false;  // the C#'s `EntryNamed(...)?.Records ?? []` for a name no arena has
    }
    for (size_t i = 0; i < records->size(); i++) {
        const formats::Dti::Record& record = (*records)[i];
        if (record.type != kRecordConnection) {
            continue;
        }
        std::map<std::pair<std::string, int32_t>, std::string>::const_iterator other =
            connections_.find(std::make_pair(residentArena_, record.id));
        if (other != connections_.end() && other->second == corridor) {
            return true;
        }
    }
    return false;
}

// C# ScriptRuntime.TeleportSecond (ScriptRuntime.cs:496-519, 0x41bce4): what the SECOND
// slot does when a teleport moves Kurt, and it runs BEFORE ShowArena (:468 before :469),
// which is what makes the third arm observable -- ShowArena then activates whatever this
// left in the slot. Three arms, in the C#'s order:
//   * into a ROOM (the MTO has it): the second is emptied and deactivated. A room is
//     drawn on its own; a second would be a room behind a door that does not exist.
//   * into a corridor that is already loaded (IsCorridorLoaded): NOTHING. The second
//     keeps what it had, because the room it names is still the one on the other side.
//   * into a corridor not loaded: the LAST arena in DTI order that is a room and has a
//     connection into it (CDANT_1 -> DANT_2), named as the second, INACTIVE, and LOADED
//     ahead -- the LoadArena batch 139 recorded as owed here (:516) is here, so the
//     room's doors are pulled into it before Kurt arrives.
void ScriptRuntime::teleportSecond(const std::string& arena, const std::string& from) {
    if (level_->mto().has(arena)) {
        secondArena_ = std::string();
        secondActive_ = false;
        return;
    }

    if (isCorridorLoaded(arena, from)) {
        return;
    }

    // Level.Dti.Arenas.LastOrDefault(...): the scan runs to the end and keeps the LAST
    // match, so this cannot break out early -- two rooms reaching one corridor is rare in
    // shipped data but it is a DTI that answers differently, not a bug to fix.
    const std::vector<formats::Dti::ArenaEntry>& arenas = level_->dti().arenas();
    std::string neighbour;
    bool found = false;
    for (size_t i = 0; i < arenas.size(); i++) {
        const formats::Dti::ArenaEntry& entry = arenas[i];
        if (!level_->mto().has(entry.name)) {
            continue;
        }
        for (size_t j = 0; j < entry.records.size(); j++) {
            const formats::Dti::Record& record = entry.records[j];
            if (record.type != kRecordConnection) {
                continue;
            }
            std::map<std::pair<std::string, int32_t>, std::string>::const_iterator other =
                connections_.find(std::make_pair(entry.name, record.id));
            if (other != connections_.end() && other->second == arena) {
                neighbour = entry.name;
                found = true;
                break;  // this arena matches; keep looking for a LATER one
            }
        }
    }

    secondArena_ = found ? neighbour : std::string();
    secondActive_ = false;
    if (found) {
        loadArena(neighbour);
    }
}

// C# ScriptRuntime.TeleportKurt (ScriptRuntime.cs:460-481, teleport_player 0x41bce4,
// VM opcodes 112 and 173): "Moves Kurt (teleport_player): within the arena with a white
// flash when `arena` is empty, otherwise into that arena." The order is the C#'s and two
// parts of it are load-bearing. (1) CurrentArena is written BEFORE TeleportSecond, so
// TeleportSecond's `from` is the arena Kurt LEFT while ShowArena sees the one he ENTERED
// -- which is why a teleport into a corridor leaves ShowArena taking its `arena ==
// CurrentArena` branch instead of writing the second slot a second time. (2) The
// previous position is set to the DESTINATION and teleported_ is raised: no teleport is
// ever a move across a connection, and the first-tick rule above must not overwrite it.
void ScriptRuntime::teleportKurt(const std::string& arena, const Vec3& position, float yaw) {
    if (!arena.empty()) {
        enterArena(arena);

        const std::string from = currentArena_;
        currentArena_ = arena;
        teleportSecond(arena, from);
        showArena(arena);

        // No move crosses a connection (0x41bce4 sets 0x5739cc too).
        previousKurtPosition_ = position;
        teleported_ = true;
    }

    // The C# cannot reach here without a Kurt: its constructor requires one
    // (ScriptRuntime.cs:168). With no Player bound there is no entity to move, and
    // writing only the runtime's own snapshot would invent a Kurt whose feet the next
    // tick overwrites with the place he was NOT teleported to -- the same guard and the
    // same reason pushKurt records.
    if (player_ != nullptr) {
        player_->teleport(position, yaw);
    }

    if (arena.empty()) {
        // MathF.Max, so a flash already standing stronger than the teleport's is kept --
        // and mathMax propagates a NaN through it exactly as MathF does.
        kurt().setWhiteFlash(mathMax(kurt().whiteFlash(), kTeleportFlash));
    }
}

// C# ScriptRuntime.CheckTriggers (ScriptRuntime.cs:707-740, 0x41bf1c): the DTI trigger boxes
// of KURT'S arena. The move it tests is THIS tick's, and in XY ONLY: `from` is the previous
// position the tick has not published yet (the C# writes _previousKurtPosition at :946, below
// both this call and the crossing, and so does runTick) and `to` is Kurt's feet now. Z is not
// part of a trigger box at all -- the reader fills the record's third coordinate and :718
// ignores it -- so a box catches Kurt at any height above it, which is what the shipped boxes
// assume: level 7's CDANT_1 pair sits at z = -42..-36 under a tunnel whose floor is -3, and a
// Z test would switch both of them off.
void ScriptRuntime::checkTriggers() {
    const formats::Vec2f from{previousKurtPosition_.x, previousKurtPosition_.y};
    const formats::Vec2f to{kurtPosition_.x, kurtPosition_.y};
    const std::vector<formats::Dti::Record>* records = arenaRecords(currentArena_);
    if (records == nullptr) {
        return;  // the C#'s `EntryNamed(CurrentArena)?.Records ?? []` (:711)
    }
    const std::vector<formats::Dti::ArenaEntry>& arenas = level_->dti().arenas();
    for (size_t i = 0; i < records->size(); i++) {
        const formats::Dti::Record& record = (*records)[i];
        if (record.type != kRecordTriggerShow && record.type != kRecordTriggerLoad) {
            continue;
        }
        // mathMin/mathMax, not fminf/fmaxf: the C# writes MathF.Min/MathF.Max (:718-719),
        // which propagates a NaN corner instead of hiding it. Same reason as spans().
        const formats::Vec2f min{mathMin(record.position[0], record.boxEnd[0]),
                                 mathMin(record.position[1], record.boxEnd[1])};
        const formats::Vec2f max{mathMax(record.position[0], record.boxEnd[0]),
                                 mathMax(record.position[1], record.boxEnd[1])};
        // Closed near side, open far side, and on the END of the move only (:720). A move that
        // starts inside and walks out is caught by crossesBox, not by this.
        const bool inside = to.x >= min.x && to.y >= min.y && to.x < max.x && to.y < max.y;
        if (!inside && !crossesBox(min, max, from, to)) {
            continue;
        }
        // The id is an INDEX INTO THE ARENA LIST (:726) -- not a name, and not a connection id.
        // -1, the "clear it" box every shipped level ships, and an id past the end both name no
        // arena; the two types then split, and the LOAD of no arena goes through the
        // ShowArena("") arm rather than through PreloadArena (:731-738).
        const std::string arena =
            (record.id >= 0 && (size_t)record.id < arenas.size())
                ? arenas[(size_t)record.id].name
                : std::string();
        if (record.type == kRecordTriggerShow) {
            showArena(arena);
        } else if (arena.empty()) {
            showArena(std::string());
        } else {
            preloadArena(arena);
        }
    }
}

ArenaState& ScriptRuntime::getArenaState(const std::string& arena) {
    std::map<std::string, ArenaState>::iterator it = arenas_.find(arena);
    if (it != arenas_.end()) {
        return it->second;
    }
    int32_t script = 0;
    formats::findOffset(&cmi_->arenaScripts(), arena, &script);
    std::pair<std::map<std::string, ArenaState>::iterator, bool> inserted =
        arenas_.emplace(arena, ArenaState(arena, script));
    // The C#'s Dictionary keeps its insertion order, and a save lists the arenas
    // in it (package #19).
    arenaOrder_.push_back(arena);
    return inserted.first->second;
}

void ScriptRuntime::spawnDtiAliens(const std::string& arena) {
    const formats::Dti::ArenaEntry* entry = level_->entryNamed(arena);
    if (entry == nullptr) {
        return;
    }
    MdkObject& controller = getArenaState(arena).controller;
    for (size_t i = 0; i < entry->records.size(); i++) {
        const formats::Dti::Record& record = entry->records[i];
        if (record.type == kDtiAlien) {
            std::string key = arena + "$" + record.name + "_" + std::to_string(record.id);
            int32_t script = 0;
            formats::findOffset(&cmi_->alienScripts(), key, &script);
            Vec3 position;
            position.x = record.position[0];
            position.y = record.position[1];
            position.z = record.position[2];
            spawn(&controller, record.name, position, record.angle, record.id, script, false);
            continue;
        }
        if (record.type != kDtiStatic) {
            continue;
        }
        std::string key = arena + "$" + record.name;
        int32_t script = 0;
        formats::findOffset(&cmi_->alienScripts(), key, &script);
        Vec3 position;
        position.x = record.position[0];
        position.y = record.position[1];
        position.z = record.position[2];
        MdkObject* obj = spawn(&controller, record.name, position, record.angle, -1, script, false);
        if (obj != nullptr) {
            obj->flags |= kDtiStaticFlags;
            obj->health = 1;
        }
    }
}

MdkObject* ScriptRuntime::spawn(MdkObject* parent, const std::string& type, const Vec3& position,
                                float yaw, int32_t instance, int32_t script, bool flagged) {
    const Model* model = findModel(parent->arena, type);
    if (model == nullptr) {
        return nullptr;
    }

    MdkObject obj;
    obj.arena = parent->arena;
    obj.type_name = type;
    obj.model = model;
    obj.instance_id = instance >= 0 ? instance : nextInstance_;
    obj.position = position;
    obj.spawn_position = position;
    obj.previous_position = position;
    obj.yaw = wrap360(yaw);
    if (instance < 0) {
        nextInstance_++;
    }
    if (flagged) {
        obj.flags = kSpawnFlaggedFlags;
    }

    objects_.push_back(obj);
    MdkObject* created = &objects_.back();
    // The Score-o-matic's enemies (ScriptRuntime.cs:1576), counted at creation; the
    // chain gun's kills are the other half of the same pair (chainGunHit).
    stats.countEnemy(type, GameStats::Created);

    // The object type's init script runs once at creation.
    std::string key = parent->arena + "$" + type;
    int32_t initScript = 0;
    formats::findOffset(&cmi_->objectScripts(), key, &initScript);
    if (initScript != 0) {
        created->restart = initScript;
        vm_.run(created);
    }

    created->restart = script;
    return created;
}

void ScriptRuntime::spawnConnector(MdkObject* obj, const std::string& type, const Vec3& position,
                                   float yaw, int32_t instance, const std::string& otherArena,
                                   int32_t script) {
    static const int32_t kDoorFlags = 0x1108000;
    if (otherArena == "NONE") {
        return;
    }

    // A door already linking these two arenas is reused (moved here if out of sight),
    // not spawned again.
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        MdkObject& other = *it;
        if (other.dead || other.type_name != type ||
            !((other.arena == obj->arena && other.connects == otherArena) ||
              (other.arena == otherArena && other.connects == obj->arena))) {
            continue;
        }
        if (other.arena != currentArena_ && other.arena != secondArena_) {
            other.arena = obj->arena;
            other.connects = otherArena;
        }
        return;
    }

    MdkObject* door = spawn(obj, type, position, yaw, instance, script, false);
    if (door == nullptr) {
        return;
    }
    door->flags |= kDoorFlags;
    door->connects = otherArena;
    setupDoor(door);
}

int32_t ScriptRuntime::pathKeyCount(int32_t path) {
    if (path < 0 || (size_t)path + 4 > cmi_->bytes().size()) {
        return 0;
    }
    return (int32_t)readU32(cmi_->bytes().data(), path);
}

int32_t ScriptRuntime::pathKeyFrame(int32_t path, int32_t key) {
    // A corrupt count could make key huge; compute the offset in 64-bit so the
    // multiply cannot overflow, and read nothing past the CMI image.
    if (key < 0) {
        return 0;
    }
    long long off = (long long)path + kPathCountSize + (long long)key * kPathKeySize;
    if (off < 0 || (size_t)off + 4 > cmi_->bytes().size()) {
        return 0;
    }
    return readS32(cmi_->bytes().data(), (int)off);
}

Vec3 ScriptRuntime::pathVector(int32_t path, int32_t key, int32_t field) {
    Vec3 v;
    long long off = (long long)path + kPathCountSize + (long long)key * kPathKeySize +
                    kPathFrameSize + field * kPathVectorSize;
    if (key < 0 || off < 0 || (size_t)off + 12 > cmi_->bytes().size()) {
        return v;
    }
    const uint8_t* b = cmi_->bytes().data();
    v.x = readF32(b, (int)off);
    v.y = readF32(b, (int)off + 4);
    v.z = readF32(b, (int)off + 8);
    return v;
}

Vec3 ScriptRuntime::pathPosition(int32_t path, float time) {
    int32_t key = pathKeyCount(path) - 2;
    while (key > 0 && (float)pathKeyFrame(path, key) >= time) {
        key--;
    }
    if (key < 0) {
        key = 0;
    }
    float start = (float)pathKeyFrame(path, key);
    float length = (float)pathKeyFrame(path, key + 1) - start;
    float u = length != 0.0f ? (time - start) / length : 0.0f;
    Vec3 p0 = pathVector(path, key, 0);
    Vec3 p1 = pathVector(path, key + 1, 0);
    Vec3 m0 = pathVector(path, key, 2);
    Vec3 m1 = pathVector(path, key + 1, 1);
    // Hermite: a = m0 + m1 - 2d, b = 3d - 2m0 - m1, result = ((a*u + b)*u + m0)*u + p0,
    // where d = p1 - p0. Component-wise (Vec3 has no operators).
    Vec3 out;
    out.x = hermite(p0.x, p1.x, m0.x, m1.x, u);
    out.y = hermite(p0.y, p1.y, m0.y, m1.y, u);
    out.z = hermite(p0.z, p1.z, m0.z, m1.z, u);
    return out;
}

void ScriptRuntime::startPath(MdkObject* obj, int32_t path, float time, const Vec3& origin) {
    obj->path = path;
    obj->path_time = time;
    obj->path_origin = origin;
    obj->path_stop = -1;
    Vec3 p = pathPosition(path, time);
    obj->position.x = p.x + origin.x;
    obj->position.y = p.y + origin.y;
    obj->position.z = p.z + origin.z;
}

void ScriptRuntime::motionPath(MdkObject* obj) {
    // An effect plays only its texture animation and never path-moves (the C#
    // Update returns on EffectFrames before any path work).
    if (obj->effect_frames > 0) {
        return;
    }
    if (obj->path == 0) {
        return;
    }
    if ((obj->flags & MdkObject::FlagPathSpeedByKurt) != 0) {
        // ObjectMotion.UpdatePathSpeed (ObjectMotion.cs:189, 0x43c258; opcode 164): the
        // path speed goes towards the speed for Kurt being ahead of, around, or behind
        // the set distance (along the object's heading), by (near - far) * 0.5 per
        // second. It runs before UpdatePath, so before the path-stop test below.
        const float kAround = 5.0f;
        const Vec3 heading = fromAngle(obj->yaw);
        const float ahead = (obj->position.x - kurtPosition_.x) * heading.x +
                            (obj->position.y - kurtPosition_.y) * heading.y;
        const std::array<float, 4>& speeds = obj->path_speeds;
        const float distance = speeds[0];
        float target = speeds[1];
        if (ahead <= distance + kAround) {
            target = ahead >= distance - kAround ? speeds[2] : speeds[3];
        }
        const float step = (speeds[3] - speeds[1]) * kTick * 0.5f;
        obj->path_speed = obj->path_speed <= target ? mathMin(obj->path_speed + step, target)
                                                    : mathMax(obj->path_speed - step, target);
    }
    int32_t path = obj->path;
    if (obj->path_stop >= 0) {
        // Round halves away from zero (ObjectMotion.Round == MathF.Round's
        // AwayFromZero); roundf matches it exactly, the ±0.5f idiom does not.
        int32_t r = (int32_t)roundf(obj->path_time);
        if (r == obj->path_stop) {
            return;
        }
    }
    const float step = obj->path_speed;  // Ticks is 1 frame per tick
    int32_t last = pathKeyFrame(path, pathKeyCount(path) - 1);
    float stop = (float)obj->path_stop;
    bool once = (obj->flags & MdkObject::FlagPathOnce) != 0;
    if (step < 0.0f) {
        if (obj->path_stop < 0 || obj->path_time < stop || stop < obj->path_time + step) {
            obj->path_time += step;
        } else {
            obj->path_time = stop;
        }
        if (obj->path_time < 0.0f) {
            if (once) {
                obj->path = 0;
                obj->path_time = 0.0f;
            } else {
                obj->path_time += (float)(last - 1);
            }
        }
    } else {
        if (obj->path_stop < 0 || stop < obj->path_time || obj->path_time + step < stop) {
            obj->path_time += step;
        } else {
            obj->path_time = stop;
        }
        if (once) {
            if (obj->path_time >= (float)(last - 2)) {
                obj->path = 0;
                obj->path_time = (float)(last - 2);
            }
        } else if (obj->path_time >= (float)(last - 1)) {
            obj->path_time -= (float)(last - 1);
        }
    }

    Vec3 old = obj->position;
    Vec3 pp = pathPosition(path, obj->path_time);
    Vec3 p;
    p.x = pp.x + obj->path_origin.x;
    p.y = pp.y + obj->path_origin.y;
    p.z = pp.z + obj->path_origin.z;
    bool pushes = (obj->flags & MdkObject::FlagPathPushes) != 0;
    if (pushes) {
        p.z = old.z;
    }
    if ((obj->flags & MdkObject::FlagNoTurning) == 0 &&
        fabsf(p.x - old.x) + fabsf(p.y - old.y) > kTick * 0.5f) {
        /* @vfp-exempt R6 — path-following turn (per path-riding object per tick);
         * glibc atan2f matches the C# oracle's MathF.Atan2 bit-for-bit on this
         * machine (probed), and the conversion is .NET's left-associative
         * radiansToDegrees; the fastAtan2 LUT's error is far larger and would move
         * those values. measured n/a (corpus empty). Verified 2026-10-23. */
        float deg = radiansToDegrees(atan2f(p.y - old.y, p.x - old.x));
        obj->yaw = wrap360(deg + obj->path_yaw_offset);
    }
    if (!pushes) {
        obj->position = p;
        return;
    }
    // The path drives the horizontal velocity instead of the position
    // (ObjectMotion.cs:309-310): Push, which ApplyVelocity spends this tick. The
    // snowboarders of LEVEL4 (XGSNOW) ride their paths this way, at the speed
    // path_speed_by_kurt sets above.
    const float pathDt = kTick;  // ObjectMotion.Dt: a float divide, as the C#'s
    obj->push.x += (p.x - old.x) / pathDt;
    obj->push.y += (p.y - old.y) / pathDt;
}

void ScriptRuntime::motionVelocity(MdkObject* obj) {
    // An effect plays only its texture animation (the C# Update returns before any
    // gravity/velocity work), exactly as motionPath skips it.
    if (obj->effect_frames > 0) {
        return;
    }
    // UpdateCommand (ObjectMotion.cs:80): a commanded object walks or flies toward
    // its waypoint, adding to push BEFORE gravity/ApplyVelocity below. The command
    // was set this tick by the VM (vm_.run ran before motionVelocity in runTick),
    // so the first step happens on the command tick exactly as the C#.
    updateCommand(obj);
    // A just-spawned object is not moved on its spawn tick by the tick loop's
    // snapshot (the C# ListCopy at ScriptRuntime.cs:962): objects spawned during
    // the loop or by the controllers are appended after it and skipped. There is
    // deliberately NO spawn-tick gate here — the C# has none, and one would wrongly
    // skip the first real fall tick for a DTI-spawned gravity object (spawned
    // before the snapshot) once arena crossings land; footing holds those instead.
    bool gravity = (obj->flags & MdkObject::FlagGravity) != 0;
    if (gravity) {
        // ObjectMotion.cs:91-96, in the C#'s order: gravity, the fans' updraft, then the
        // terminal clamp (MathF.Max, so a NaN stays NaN as there).
        obj->velocity.z -= obj->gravity * kTick;
        motionUpdraft(obj);
        obj->velocity.z = mathMax(obj->velocity.z, -220.0f);  // TerminalVelocity
    }

    // ApplyVelocity (0x45e810). Friction acts on the horizontal only for a gravity
    // object (its vertical is gravity's, not friction's).
    obj->contact_flags &= ~(MdkObject::ContactCollided | MdkObject::ContactFloor | 0x10);
    float mx = obj->velocity.x;
    float my = obj->velocity.y;
    float mz = gravity ? 0.0f : obj->velocity.z;
    float length = sqrtf(mx * mx + my * my + mz * mz);
    if (length > 0.0f) {
        // MathF.Max (ObjectMotion.cs:1066), so a NaN friction makes the FACTOR NaN and
        // the velocity NaN; fmaxf would return the 0 and silently stop the object dead.
        // Measured on .NET 10: friction NaN leaves Velocity = NaN,NaN,NaN (probe mode
        // `nanclamp`, row FRICTION nan), while a finite 60 leaves 1.8,2.4,0.
        float factor = mathMax(length - obj->friction * kTick, 0.0f) / length;
        obj->velocity.x *= factor;
        obj->velocity.y *= factor;
        if (!gravity) {
            obj->velocity.z *= factor;
        }
    }

    Vec3 motion;
    motion.x = (obj->velocity.x + obj->push.x) * kTick;
    motion.y = (obj->velocity.y + obj->push.y) * kTick;
    motion.z = (obj->velocity.z + obj->push.z) * kTick;
    obj->push.x = 0.0f;
    obj->push.y = 0.0f;
    obj->push.z = 0.0f;
    if (motion.x == 0.0f && motion.y == 0.0f && motion.z == 0.0f) {
        // C# ApplyVelocity returns here. Update still runs the animation, the
        // roll and the PreviousPosition write after it (ObjectMotion.cs:132-146),
        // which the caller does; nothing is recorded here.
        return;
    }

    // A FlagCollides object sweeps its box against the arena BSP and stops on a
    // floor (velocity.Z zeroed, ContactFloor set); one without it moves freely.
    if ((obj->flags & MdkObject::FlagCollides) == 0) {
        obj->position.x += motion.x;
        obj->position.y += motion.y;
        obj->position.z += motion.z;
    } else {
        Vec3 normal{0.0f, 0.0f, 1.0f};
        if (sweepBox(obj, motion, &normal)) {
            obj->contact_flags |= MdkObject::ContactCollided;
            if (motion.z <= 0.0f) {
                obj->contact_flags |= MdkObject::ContactFloor;
            }
            if ((obj->flags & MdkObject::FlagBounces) != 0) {
                // C# velocity += normal * dot(velocity, normal) * Bounce (-1.8).
                float d = obj->velocity.x * normal.x + obj->velocity.y * normal.y +
                          obj->velocity.z * normal.z;
                obj->velocity.x += normal.x * d * -1.8f;
                obj->velocity.y += normal.y * d * -1.8f;
                obj->velocity.z += normal.z * d * -1.8f;
            } else {
                obj->velocity.z = 0.0f;
            }
        }
    }

    // FallOut: below the arena floor by 200 (FallOutDepth).
    if (obj->position.z < getArenaFloor(obj->arena) - 200.0f) {
        fallOut(obj);
    }
}

// UpdatePickup (0x43d8f0), run after ApplyVelocity for a FlagPickup object: a
// falling pickup lands on floor contact (Fall), opens its chute once it reaches
// terminal speed (a SW_CHUTE hung on it by attach command 74), shrinks that chute
// away after it lands, and — for the SW_H150, once the chute is gone — runs (the
// Runner, below). A landed or non-gravity pickup not in the Still list spins in
// place.
//
// What is NOT here is the Collected branch: an object the player has picked up
// shrinks over ShrinkTicks and is then removed (ObjectBehaviors.cs:206-212), and
// that waits on the inventory slice (ObjectBehaviors.cs:207-218). The guard below
// returns early for a FlagCollected object — nothing in this port sets that flag
// yet, and where the C# shrinks before returning, this returns without shrinking.

// ObjectMotion.Updraft (ObjectMotion.cs:162-181, 0x45e74c): the fans lift objects with
// gravity (not LEVEL6's rolling ones); Kurt's thrown items in flight -- flag 0x1000 without
// 0x4000 (active), 0x40000 (collected) -- but the mortar also lose most of their
// horizontal speed in them. Fans::query answers NaN where no fan of the arena holds it.
void ScriptRuntime::motionUpdraft(MdkObject* obj) {
    const int32_t kLevelSix = 6;
    const int32_t kThrownFlagsMask = 0x45000;
    const int32_t kMortarItem = 4;
    const float kUpdraftBrake = 0.1f;
    if (level_->number() == kLevelSix && (obj->flags & MdkObject::FlagRolling) != 0) {
        return;
    }
    const float vz = fans.query(obj->arena, obj->position, obj->velocity.z, Fans::kMaskObjects, kTick);
    if (vz != vz) {
        return;
    }
    obj->velocity.z = vz;
    if ((obj->flags & kThrownFlagsMask) == kItemFlagThrown && obj->thrown_kind != kMortarItem) {
        obj->velocity.x *= kUpdraftBrake;
        obj->velocity.y *= kUpdraftBrake;
    }
}

// ObjectMotion.Bank (0x43b65c): an object leans into its turn. The turn counted is
// the yaw change since the LAST call, at most 2 degrees of it per tick; the roll is
// the old roll minus that turn, damped by 0.95 and kept inside +/-10 degrees.
//
// Two details a rewrite loses:
//   * BankingYaw is written BEFORE the flag test, so an object that never banks
//     still tracks its own yaw. Writing it only for a banking object would make the
//     first tick after set_banking 1 count every degree turned while it was
//     flagged, and lean over by the whole of it.
//   * The roll is wrapped before the subtraction, so a roll that some other writer
//     left outside +/-180 comes back through the short way round.
void ScriptRuntime::motionBank(MdkObject* obj) {
    const float kMaxTurn = 2.0f;   // degrees per tick
    const float kMaxRoll = 10.0f;  // degrees
    const float kDamping = 0.95f;
    const int32_t kNoRoll = 1;     // the C#'s unnamed flag 1
    const float kTicks = 1.0f;     // ObjectMotion.Ticks: one tick, in ticks
    // The clamps are float predicates on a gameplay angle, not a loop bound and not
    // a lattice index: R2/R3/T2 have nothing to bite on here, and the same idiom
    // (clampf) is what the walk step and the turn step already use.
    const float turn = clampf(wrapAngle(obj->yaw - obj->banking_yaw) / kTicks, -kMaxTurn, kMaxTurn);
    obj->banking_yaw = obj->yaw;
    if ((obj->flags & (MdkObject::FlagNoBanking | kNoRoll | MdkObject::FlagRolling)) != 0 ||
        obj->thrown_kind > 0) {
        return;
    }
    obj->roll = clampf((wrapAngle(obj->roll) - turn) * kDamping, -kMaxRoll, kMaxRoll);
}

void ScriptRuntime::motionPickup(MdkObject* obj) {
    const float kPickupHover = 1.5f;
    const float kPickupTurnSpeed = 180.0f;
    if ((obj->flags & MdkObject::FlagCollected) != 0) {
        // A pickup Kurt took (ObjectBehaviors.cs:207-217): it shrinks over ShrinkTicks
        // (30), spinning 4 times as fast, and goes.
        const float kShrinkTicks = 30.0f;
        const float kCollectedSpin = 4.0f;
        obj->parameter_timer -= 1.0f;
        obj->scale = mathMax(obj->parameter_timer / kShrinkTicks, 0.0f);
        obj->yaw = wrap360(obj->yaw + kTick * kPickupTurnSpeed * kCollectedSpin);
        if (obj->parameter_timer <= 0.0f) {
            remove(obj);
        }
        return;
    }
    bool gravity = (obj->flags & MdkObject::FlagGravity) != 0;
    bool landed = (obj->flags & MdkObject::FlagLanded) != 0;
    if (gravity && !landed) {
        // Fall (0x43daf4): on floor contact the pickup lands. The Runner (SW_H150)
        // rests on the floor (hover 0); the others hover on their chute.
        if ((obj->contact_flags & MdkObject::ContactFloor) != 0) {
            float hover = obj->type_name == "SW_H150" ? 0.0f : kPickupHover;
            obj->height_offset = hover;
            obj->flags |= MdkObject::FlagLanded;
            obj->position.z += hover;
            return;
        }
        // Still falling: once it reaches terminal speed (PickupChuteSpeed, -15) it
        // clamps there and opens its chute — a SW_CHUTE hung on it by attach command
        // 74. Above that speed it is still accelerating, so nothing happens yet.
        const float kPickupChuteSpeed = -15.0f;
        if (obj->velocity.z >= kPickupChuteSpeed) {
            return;
        }
        obj->velocity.z = kPickupChuteSpeed;
        if (obj->attached != nullptr) {
            return;  // chute already open
        }
        MdkObject* chute = spawn(obj, "SW_CHUTE", obj->position, obj->yaw, -1, 0, false);
        if (chute == nullptr) {
            return;
        }
        chute->flags = 0x820;      // ChuteFlags
        chute->leader = obj;
        chute->attach_point_a = 0;
        chute->attach_point_b = 0;
        chute->move_command = 74;  // AttachCommand
        obj->attached = chute;
        return;
    }
    // The chute shrinks away once the pickup has landed (ObjectBehaviors.cs:226):
    // a notch per tick, and gone (removed) at ChuteGone.
    const float kChuteGone = 0.2f;
    MdkObject* chute = obj->attached;
    if (chute != nullptr && chute->scale > kChuteGone) {
        chute->scale -= kTick;
        if (chute->scale <= kChuteGone) {
            remove(chute);
            obj->attached = nullptr;
        }
    }
    // The Runner: SW_H150 runs away, and only once its chute is gone (the C#'s
    // `obj.Attached == null`, which is also the first tick it is on the floor).
    if (obj->type_name == "SW_H150" && obj->attached == nullptr) {
        motionRunner(obj);
    }
    // Landed or non-gravity: a pickup not in the Still list spins in place at
    // PickupTurnSpeed.
    if (obj->type_name != "SW_H150" && obj->type_name != "SW_SEAL" &&
        obj->type_name != "SW_SBONE") {
        obj->yaw = wrap360(obj->yaw + kTick * kPickupTurnSpeed);
    }
}

// The C#'s Items.GetAnimation, for the two animations the Runner plays. The
// archive is the level's TRAVSPRT.BNI; the C# parses each name once and keeps it,
// and ObjectBehaviors compares the RESULT BY IDENTITY (`obj.Animation != run`), so
// the cache must answer the same address for the same name — a std::map's nodes do.
//
// Null when there is no archive (a harness that did not load one) or no such entry.
// The C# has no such case: the entries are there in a real game, and the port does
// not add a guard the oracle does not have.
const ModelAnimation* ScriptRuntime::itemsAnimation(const char* name) {
    if (sprites_ == nullptr) {
        return nullptr;
    }
    std::map<std::string, ModelAnimation>::iterator found = itemsAnimations_.find(name);
    if (found != itemsAnimations_.end()) {
        return &found->second;
    }
    const formats::ArchiveIndex::Entry* entry = sprites_->index().find(name);
    if (entry == nullptr) {
        return nullptr;
    }
    ModelAnimation animation;
    const std::vector<uint8_t>& bytes = sprites_->bytes();
    if (!ModelAnimation::parse(&animation, name, bytes.data(), (int32_t)bytes.size(), entry->offset)) {
        return nullptr;
    }
    std::pair<std::map<std::string, ModelAnimation>::iterator, bool> inserted =
        itemsAnimations_.emplace(name, std::move(animation));
    return &inserted.first->second;
}

// MdkObject.IsAnimationDone, as the runtime reads it (the VM has its own copy).
static bool animationDone(const MdkObject* obj);

// ObjectBehaviors.UpdateRunner (0x43daf4's SW_H150 branch): the hamster.
//
//   IDLE ──(Kurt within 20)──► RUN ──(Kurt beyond sqrt(1000))──► IDLE
//
// Idle, it replays H150_I on a 72-in-32768 chance a tick; running, it pushes 40
// units/s along its yaw, turns away from Kurt at up to 270 degrees/s, and keeps
// FlagGravity set (it is on the floor, not hovering on a chute).
//
// THE RNG IS THE POINT. The chance is drawn ONLY when the object is idle or its
// animation has ended, from the same Random every scripted random decision takes.
// Drawing it unconditionally, or not at all, moves every later draw in the level —
// which is why this was not quietly approximated by a timer.
void ScriptRuntime::motionRunner(MdkObject* obj) {
    const float kRunnerStart = 20.0f;        // units: it starts when Kurt is this close
    const float kRunnerStopSquared = 1000.0f;  // units^2 (sqrt = 31.6): stops beyond this
    const float kRunnerSpeed = 40.0f;        // units/s
    const float kRunnerTurn = 270.0f;        // degrees/s
    const int32_t kRunnerIdleChance = 72;
    const int32_t kChanceRange = 32768;

    const ModelAnimation* run = itemsAnimation("H150_R");
    const Vec3 kurt = kurtPosition_;
    if (obj->animation != run) {
        if ((obj->animation == nullptr || animationDone(obj)) &&
            rng_.next(kChanceRange) < kRunnerIdleChance) {
            restartAnimation(obj, itemsAnimation("H150_I"), false);
            return;
        }
        // Vector3.DistanceSquared(Position, kurt) < (RunnerStart * RunnerStart).
        const float dx = obj->position.x - kurt.x;
        const float dy = obj->position.y - kurt.y;
        const float dz = obj->position.z - kurt.z;
        if (dx * dx + dy * dy + dz * dz < kRunnerStart * kRunnerStart) {
            restartAnimation(obj, run, true);
            mixer_.play("RUNNER", audio::SoundMixer::Once);
        }
        return;
    }

    // Running: ScriptMath.FromAngle(yaw) * RunnerSpeed joins the push, so the
    // ApplyVelocity step that follows moves it RunnerSpeed * Dt this tick.
    const Vec3 ahead = fromAngle(obj->yaw);
    obj->push.x += ahead.x * kRunnerSpeed;
    obj->push.y += ahead.y * kRunnerSpeed;
    // It faces away from Kurt: the heading to him, plus half a turn.
    const float away = heading(obj->position, kurt) + 180.0f;
    const float turn = clampf(wrapAngle(away - obj->yaw), -kRunnerTurn * kTick, kRunnerTurn * kTick);
    obj->yaw = wrap360(obj->yaw + turn);
    obj->flags |= MdkObject::FlagGravity;
    // The stop test is the plan-view distance only, and it is `>`, not `>=`.
    const float ex = kurt.x - obj->position.x;
    const float ey = kurt.y - obj->position.y;
    if (ex * ex + ey * ey > kRunnerStopSquared) {
        restartAnimation(obj, itemsAnimation("H150_I"), false);
    }
}

// Raycast (0x421680): whether any triangle of the active arenas crosses [from,to].
// Only Kurt's arena and the second one stop rays; the rest are passed through.
// Script predicates may request the nearest hit point and its geometric normal.
// CrossesFloor (ScriptRuntime.cs:2914): an up-facing face of the arena crosses
// the segment. No BSP for the arena → false, as the C#'s TryGetValue miss.
bool ScriptRuntime::crossesFloor(const std::string& arena, const Vec3& from, const Vec3& to) {
    Bsp* bsp = bspOf(arena);
    if (bsp == nullptr) {
        return false;
    }
    Vec a{from.x, from.y, from.z};
    Vec b{to.x, to.y, to.z};
    Vec point;
    return bsp->segment(a, b, Bsp::kSegmentFloor, &point, Bsp::kClipPassesThrough) != Bsp::kNone;
}

bool ScriptRuntime::raycastPoint(const Vec3& from, const Vec3& to, Vec3* out, Vec3* normal) {
    Vec a{from.x, from.y, from.z};
    Vec b{to.x, to.y, to.z};
    // RayArenas: the current + second arena when a current is set, else every arena
    // (the C# _bsps is filled from level.Arenas at construction, so "all" is the full
    // set, not just the arenas touched so far). bspOf builds each on demand.
    // The C# Raycast keeps the NEAREST hit across the arenas by distance-squared.
    bool hit = false;
    float bestDistance = FLT_MAX;
    const auto tryArena = [&](const std::string& name) {
        Bsp* bsp = bspOf(name);
        if (bsp == nullptr) {
            return;
        }
        Vec point{};
        const int32_t triangle = bsp->segmentWithGroup(
            a, b, Bsp::kSegmentAny, &point, Bsp::kClipPassesThrough, nullptr);
        if (triangle == Bsp::kNone) {
            return;
        }
        const float dx = point.x - a.x;
        const float dy = point.y - a.y;
        const float dz = point.z - a.z;
        const float distance = dx * dx + dy * dy + dz * dz;
        // The C#'s exact skip: `distance >= bestDistance` (bestDistance starts at
        // float.MaxValue), so a NaN distance is ADOPTED, as in the C#, and a first
        // hit at exactly MaxValue is skipped. A `<`-with-hit-flag form would differ.
        if (distance >= bestDistance) {
            return;
        }
        bestDistance = distance;
        *out = Vec3{point.x, point.y, point.z};
        if (normal != nullptr) {
            *normal = triangleNormal(bsp->arena(), triangle);
        }
        hit = true;
    };
    if (!currentArena_.empty()) {
        tryArena(currentArena_);
        tryArena(secondArena_);
    } else {
        const std::vector<const formats::Arena*>& arenas = level_->arenas();
        for (size_t i = 0; i < arenas.size(); i++) {
            tryArena(arenas[i]->name());
        }
    }
    return hit;
}

// Raycast (ScriptRuntime.cs:2886) answering with the whole RayHit: the same arena set,
// mode and nearest-hit rule as raycastPoint, plus the hit's arena and triangle group.
bool ScriptRuntime::raycastHit(const Vec3& from, const Vec3& to, DebrisRayHit* out) {
    Vec a{from.x, from.y, from.z};
    Vec b{to.x, to.y, to.z};
    bool hit = false;
    float bestDistance = FLT_MAX;
    const auto tryArena = [&](const std::string& name) {
        Bsp* bsp = bspOf(name);
        if (bsp == nullptr) {
            return;
        }
        Vec point{};
        int32_t triangleGroup = 0;
        const int32_t triangle = bsp->segmentWithGroup(a, b, Bsp::kSegmentAny, &point,
                                                      Bsp::kClipPassesThrough, &triangleGroup);
        if (triangle == Bsp::kNone) {
            return;
        }
        const float dx = point.x - a.x;
        const float dy = point.y - a.y;
        const float dz = point.z - a.z;
        const float distance = dx * dx + dy * dy + dz * dz;
        // The C#'s exact skip, as in raycastPoint.
        if (distance >= bestDistance) {
            return;
        }
        bestDistance = distance;
        out->point = Vec3{point.x, point.y, point.z};
        out->normal = triangleNormal(bsp->arena(), triangle);
        out->arena = name;
        out->group = triangleGroup;
        hit = true;
    };
    if (!currentArena_.empty()) {
        tryArena(currentArena_);
        tryArena(secondArena_);
    } else {
        const std::vector<const formats::Arena*>& arenas = level_->arenas();
        for (size_t i = 0; i < arenas.size(); i++) {
            tryArena(arenas[i]->name());
        }
    }
    return hit;
}

bool ScriptRuntime::raycastClear(const Vec3& from, const Vec3& to) {
    Vec3 point;
    return !raycastPoint(from, to, &point);
}

// Raycast (ScriptRuntime.cs:2886) answering with the hit triangle's group. The
// SAME arena set and mode as raycastPoint: SegmentMode.Any with PassesThrough
// over the current arena and the second — the second is included REGARDLESS of
// whether it is active — or every arena when Kurt has none yet.
bool ScriptRuntime::raycastGroup(const Vec3& from, const Vec3& to, int32_t* group) {
    Vec a{from.x, from.y, from.z};
    Vec b{to.x, to.y, to.z};
    bool hit = false;
    float bestDistance = FLT_MAX;
    int32_t bestGroup = 0;
    const auto tryArena = [&](const std::string& name) {
        Bsp* bsp = bspOf(name);
        if (bsp == nullptr) {
            return;
        }
        Vec point{};
        int32_t triangleGroup = 0;
        const int32_t triangle = bsp->segmentWithGroup(a, b, Bsp::kSegmentAny, &point,
                                                      Bsp::kClipPassesThrough, &triangleGroup);
        if (triangle == Bsp::kNone) {
            return;
        }
        const float dx = point.x - a.x;
        const float dy = point.y - a.y;
        const float dz = point.z - a.z;
        const float distance = dx * dx + dy * dy + dz * dz;
        // The C#'s exact skip: `distance >= bestDistance`, so a NaN distance is
        // ADOPTED, as in the C#. Same form as raycastPoint.
        if (distance >= bestDistance) {
            return;
        }
        bestDistance = distance;
        bestGroup = triangleGroup;
        hit = true;
    };
    if (!currentArena_.empty()) {
        tryArena(currentArena_);
        tryArena(secondArena_);
    } else {
        const std::vector<const formats::Arena*>& arenas = level_->arenas();
        for (size_t i = 0; i < arenas.size(); i++) {
            tryArena(arenas[i]->name());
        }
    }
    if (hit) {
        *group = bestGroup;
    }
    return hit;
}

// RayIn (ScriptRuntime.cs:2249): the arena's nearest segment hit with its
// triangle normal and group, for Debris (and later Effects).
bool ScriptRuntime::rayIn(const std::string& arena, const Vec3& from, const Vec3& to,
                          DebrisRayHit* out) {
    Bsp* bsp = bspOf(arena);
    if (bsp == nullptr) {
        return false;
    }
    Vec a{from.x, from.y, from.z};
    Vec b{to.x, to.y, to.z};
    Vec point{};
    int32_t triangleGroup = 0;
    const int32_t triangle = bsp->segmentWithGroup(a, b, Bsp::kSegmentAny, &point,
                                                   Bsp::kClipPassesThrough, &triangleGroup);
    if (triangle == Bsp::kNone) {
        return false;
    }
    out->point = Vec3{point.x, point.y, point.z};
    out->normal = triangleNormal(level_->arenaNamed(arena), triangle);
    out->arena = arena;
    out->group = triangleGroup;
    return true;
}

// GroupFacets (ScriptRuntime.cs:2261): the triangles of an arena's group
// with their current material, for shattering. Null group 0, as the C#.
bool ScriptRuntime::groupFacets(const std::string& arena, int32_t group, DebrisGroup* out) {
    const formats::Arena* mesh = level_->arenaNamed(arena);
    if (mesh == nullptr || group == 0 || !mesh->hasGroup(group)) {
        return false;
    }
    int32_t groupMaterial = 0;
    bool hasGroupMaterial = mesh->groupMaterial(group, &groupMaterial);
    const std::vector<uint32_t>& flags = mesh->triangleFlags();
    const std::vector<int32_t>& idx = mesh->triangleIndices();
    const std::vector<formats::Vec3f>& verts = mesh->vertices();
    const std::vector<formats::Vec2f>& uvs = mesh->triangleUvs();
    const std::vector<int32_t>& mats = mesh->triangleMaterials();
    out->names = &mesh->materials();
    out->facets.clear();
    for (size_t t = 0; t < flags.size(); t++) {
        if ((int32_t)(flags[t] >> 24) != group) {
            continue;
        }
        DebrisFacet facet;
        formats::Vec3f va = verts[(size_t)idx[t * 3]];
        formats::Vec3f vb = verts[(size_t)idx[t * 3 + 1]];
        formats::Vec3f vc = verts[(size_t)idx[t * 3 + 2]];
        facet.a = Vec3{va.x, va.y, va.z};
        facet.b = Vec3{vb.x, vb.y, vb.z};
        facet.c = Vec3{vc.x, vc.y, vc.z};
        formats::Vec2f ua = uvs[t * 3], ub = uvs[t * 3 + 1], uc = uvs[t * 3 + 2];
        facet.uvA = Vec2{ua.x, ua.y};
        facet.uvB = Vec2{ub.x, ub.y};
        facet.uvC = Vec2{uc.x, uc.y};
        facet.material = hasGroupMaterial ? groupMaterial : mats[t];
        out->facets.push_back(facet);
    }
    return !out->facets.empty();
}

bool ScriptRuntime::debrisRayTrampoline(void* ctx, const std::string& arena, const Vec3& from,
                                        const Vec3& to, DebrisRayHit* out) {
    return static_cast<ScriptRuntime*>(ctx)->rayIn(arena, from, to, out);
}

bool ScriptRuntime::debrisFacetsTrampoline(void* ctx, const std::string& arena, int32_t group,
                                           DebrisGroup* out) {
    return static_cast<ScriptRuntime*>(ctx)->groupFacets(arena, group, out);
}

void ScriptRuntime::debrisTrailTrampoline(void* ctx, const std::string& arena, const Vec3& point) {
    static_cast<ScriptRuntime*>(ctx)->effects.spawnTrail(arena, point);
}

float ScriptRuntime::debrisUpdraftTrampoline(void* ctx, const std::string& arena, const Vec3& point,
                                             float vz, float dt) {
    return static_cast<ScriptRuntime*>(ctx)->fans.query(arena, point, vz, Fans::kMaskEffects, dt);
}

bool ScriptRuntime::fanIsLive(void* ctx, const std::string& arena) {
    ScriptRuntime* self = static_cast<ScriptRuntime*>(ctx);
    return arena == self->currentArena_ ||
           (self->secondActive_ && arena == self->secondArena_);
}

int32_t ScriptRuntime::animatedFrameCount(void* ctx, const std::string& arena,
                                          const std::string& texture) {
    ScriptRuntime* self = static_cast<ScriptRuntime*>(ctx);
    if (!self->level_) {
        return 0;
    }
    const formats::Arena* owner = self->level_->arenaNamed(arena);
    if (!owner) {
        return 0;
    }
    const std::vector<const formats::TextureArchive*> archives = self->level_->archivesOf(owner);
    for (size_t i = 0; i < archives.size(); i++) {
        const formats::Texture* found = archives[i]->findTexture(texture);
        if (found) {
            return found->frameCount();
        }
    }
    return 0;
}

// CanSeeKurt (ScriptRuntime.cs:3236): within range, inside the view cone (a cone
// under HalfTurn is clamped to MinCone), and a clear ray from the object's eye to
// Kurt's chest. The ray uses the same active-arena raycast as the move planner.
// MdkObject.YawTo: the compass yaw (degrees, [0, 360)) from the object to a
// point — the fmod wrap, NOT ScriptMath.Wrap360's floor form, and the two
// round differently at the range edges.
/* @vfp-exempt R6 — yaw-to-point in the movement-command path; glibc atan2f
 * matches the C# oracle's MathF.Atan2 bit-for-bit on this machine (probed:
 * .NET's MathF.Atan2 is the float libm call, and both picked the same value at
 * every ULP-divergent input tested), and the conversion is .NET's left-assoc
 * radiansToDegrees; the fastAtan2 LUT's error is far larger and would move the
 * turned yaw. measured n/a (corpus empty). Verified 2026-10-23. */
float ScriptRuntime::yawTo(const MdkObject* obj, const Vec3& point) const {
    float degrees = radiansToDegrees(atan2f(point.y - obj->position.y, point.x - obj->position.x));
    return fmodf(fmodf(degrees, 360.0f) + 360.0f, 360.0f);
}

// C# KurtLooksAt (ScriptRuntime.cs:2701): range, distance-scaled cone, and line of sight.
/* @vfp-exempt R6 — kurtLooksAt's heading is a cold script predicate; keep the
 * C# MathF.Atan2 + radians-to-degrees spelling to preserve cone-edge behavior.
 * measured n/a (corpus empty). Verified 2026-10-24. */
bool ScriptRuntime::kurtLooksAt(const MdkObject* obj, float minRange, float maxRange, float cone) {
    const float kEyeHeight = 5.0f;
    const float kTargetHeight = 2.0f;
    const float kFullTurn = 360.0f;
    float dx = obj->position.x - kurtPosition_.x;
    float dy = obj->position.y - kurtPosition_.y;
    float dz = obj->position.z - kurtPosition_.z;
    float distance = sqrtf(dx * dx + dy * dy + dz * dz);
    if (distance < minRange || distance > maxRange) {
        return false;
    }
    const float heading = radiansToDegrees(atan2f(dy, dx));
    const float raw = fabsf(kurtYaw_ - heading);
    const float angle = wrap360(raw);
    const float allowed = (cone - 90.0f) / maxRange * distance + 90.0f;
    if (angle > allowed && angle < kFullTurn - allowed) {
        return false;
    }
    Vec3 from{kurtPosition_.x, kurtPosition_.y, kurtPosition_.z + kEyeHeight};
    Vec3 to{obj->position.x, obj->position.y, obj->position.z + kTargetHeight};
    return raycastClear(from, to);
}

bool ScriptRuntime::canSeeKurt(MdkObject* obj, float range, float cone) {
    const float kMinCone = 10.0f;
    const float kEyeHeight = 5.0f;
    const float kKurtChest = 4.0f;
    const float kHalfTurn = 180.0f;
    float dx = obj->position.x - kurtPosition_.x;
    float dy = obj->position.y - kurtPosition_.y;
    float dz = obj->position.z - kurtPosition_.z;
    if (sqrtf(dx * dx + dy * dy + dz * dz) > range) {
        return false;
    }
    // MathF.Max(cone, MinCone) (ScriptRuntime.cs:3244). This one CANNOT be reached with a
    // NaN: the `cone < kHalfTurn` guard in front of it is false for a NaN, so a NaN cone
    // skips the cone test entirely — measured on .NET 10 (probe `nanclamp`, rows CONE
    // cone45=False / coneNaN=True / cone200=True, i.e. the NaN row is the no-test row).
    // mathMax is here because the oracle writes MathF.Max, not because a pin can reach it.
    if (cone < kHalfTurn &&
        fabsf(wrapAngle(yawTo(obj, kurtPosition_) - obj->yaw)) > mathMax(cone, kMinCone)) {
        return false;
    }
    Vec3 from{obj->position.x, obj->position.y, obj->position.z + kEyeHeight};
    Vec3 to{kurtPosition_.x, kurtPosition_.y, kurtPosition_.z + kKurtChest};
    return raycastClear(from, to);
}

void ScriptRuntime::setGroupHitScript(const std::string& arena, int32_t group, int32_t mask, int32_t script) {
    const int32_t index = (group - 1) & (kGroupCount - 1);
    ArenaState& state = getArenaState(arena);
    state.group_hit_masks[index] = mask;
    state.group_hit_scripts[index] = script;
}

void ScriptRuntime::setGroupHitFlags(const std::string& arena, int32_t group, int32_t flags) {
    const int32_t index = (group - 1) & (kGroupCount - 1);
    getArenaState(arena).group_hit_flags[index] = flags;
    if ((flags & kGroupDestructible) != 0) {
        setGroupState(arena, group, kGroupHide);
    }
}

void ScriptRuntime::setGroupCounter(const std::string& arena, int32_t group, int32_t value) {
    const int32_t index = (group - 1) & (kGroupCount - 1);
    getArenaState(arena).group_counters[index] = value;
}

int32_t ScriptRuntime::groupCounter(const std::string& arena, int32_t group) {
    const int32_t index = (group - 1) & (kGroupCount - 1);
    return getArenaState(arena).group_counters[index];
}

// HitGroup (ScriptRuntime.cs:1909, 0x40d560): a hit on a triangle of an arena
// group. The group's hit flags (opcode 168) decide what the hit does — a
// destructible group shows its damaged version, an always-hit group takes at
// least the amount, a stopping group answers | 2 — and the group's hit script
// (opcode 99) runs in the arena's scratch object when its mask admits the kind
// (or the group is always-hit). Answers | 1 when the script ran.
int32_t ScriptRuntime::hitGroup(const std::string& arena, int32_t group, int32_t amount,
                                int32_t kind, int32_t weapon) {
    const int32_t kScriptRan = 1;
    const int32_t kStopped = 2;
    if (group < 1 || group > kGroupCount) {
        return 0;
    }
    ArenaState& state = getArenaState(arena);
    const int32_t index = group - 1;
    const int32_t flags = state.group_hit_flags[index];
    bool always = false;
    int32_t result = 0;
    if ((flags & kind) != 0) {
        if ((flags & kGroupDestructible) != 0) {
            // A destructible group: its damaged version appears.
            setGroupState(arena, group, kGroupShow);
        }
        if ((flags & kGroupAlwaysHit) != 0) {
            always = true;
            amount = amount > 1 ? amount : 1;
        }
        if ((flags & kGroupStopsHit) != 0) {
            result = kStopped;
        }
    }
    if (state.group_hit_scripts[index] == 0 ||
        ((state.group_hit_masks[index] & kind) == 0 && !always)) {
        return result;
    }
    state.group_counters[index] += amount;
    // The script runs at once, from its start, in the arena's scratch object (0x45c9a0).
    MdkObject& scratch = state.hit_scripts;
    scratch.hit_type = weapon;
    scratch.hit_event = 0;
    scratch.wait_time = 0.0f;
    scratch.gosub_returns.clear();
    scratch.gosub_restarts.clear();
    scratch.restart = state.group_hit_scripts[index];
    vm_.run(&scratch);
    return result | kScriptRan;
}

// GetKurtFloorGroup (ScriptRuntime.cs:410, 0x573c10): the group of the floor
// triangle under Kurt — a raycast from one above him to ten below, the group of
// the nearest hit, or 0.
int32_t ScriptRuntime::getKurtFloorGroup() {
    Vec3 from{kurtPosition_.x, kurtPosition_.y, kurtPosition_.z + 1.0f};
    Vec3 to{kurtPosition_.x, kurtPosition_.y, kurtPosition_.z - 10.0f};
    int32_t group = 0;
    if (!raycastGroup(from, to, &group)) {
        return 0;
    }
    return group;
}

// C# TriangleGroups.SetState (TriangleGroups.cs:90): the group's state is a
// STORED per-group value, not derived from the triangle bits. The group must
// exist (a triangle with that top byte) and not be group 0; group numbers are
// the full top byte, not limited to the 16 the hit arrays use. The operation
// edits the stored state, and the result is written to every triangle of the
// group, where the collisions read it.
void ScriptRuntime::setGroupState(const std::string& arena, int32_t group, int32_t operation) {
    formats::Arena* mesh = level_->arenaNamedMutable(arena);
    if (mesh == nullptr || group == 0 || !mesh->hasGroup(group)) {
        return;
    }
    const uint32_t kHidden = formats::Arena::kGroupHidden;
    const uint32_t kNotSolid = formats::Arena::kGroupNotSolid;
    uint32_t state = mesh->groupState(group);
    switch (operation) {
        case 0:  // HideAndRelease
            state |= kHidden | kNotSolid;
            break;
        case 2:  // Hide
            state |= kHidden;
            break;
        case 3:  // Show
            state &= ~kHidden;
            break;
        case 4:  // Release
            state |= kNotSolid;
            break;
        case 5:  // Solidify
            state &= ~kNotSolid;
            break;
        default:  // any other operation shows and solidifies
            state = 0;
            break;
    }
    mesh->setGroupState(group, state);
    const uint32_t stateBits = kHidden | kNotSolid;
    std::vector<uint32_t>& triangleFlags = mesh->mutableTriangleFlags();
    for (size_t i = 0; i < triangleFlags.size(); i++) {
        if ((int32_t)(triangleFlags[i] >> 24) != group) {
            continue;
        }
        triangleFlags[i] = (triangleFlags[i] & ~stateBits) | state;
    }
}

// C# SetGroupTexture (ScriptRuntime.cs:1983) -> TriangleGroups.SetMaterial: the
// group must exist and not be group 0; it stores the group's material. The
// scene reads it when it is built, and the C#'s Changed event rebuilds a built
// one (LevelView.Rebuild): here the arena is recorded for the frontend, which
// rebuilds its view (retexturedArenas, package #15).
void ScriptRuntime::setGroupTexture(const std::string& arena, int32_t group, int32_t value) {
    formats::Arena* mesh = level_->arenaNamedMutable(arena);
    if (mesh == nullptr || group == 0 || !mesh->hasGroup(group)) {
        return;
    }
    mesh->setGroupMaterial(group, value);
    // SetMaterial's Changed?.Invoke(arena, number): each arena once until taken.
    for (size_t i = 0; i < retextured_.size(); i++) {
        if (retextured_[i] == arena) {
            return;
        }
    }
    retextured_.push_back(arena);
}


// FindAdvanceSpot (ScriptRuntime.cs:2413, opcode 197): advance toward Kurt
// (command 197) via a type-5 spot of the object's arena, 5-400 units from the
// object, no farther from the object than Kurt is, and nearer to Kurt than the
// object is. The qualifying spots are collected in arena-record order and one is
// picked uniformly with a single RNG draw. Sets MoveCommand 197 on success and
// clears (zeroes) it otherwise, matching the C# one-for-one.
void ScriptRuntime::findAdvanceSpot(MdkObject* obj) {
    const int32_t kAdvanceCommand = 197;
    const uint32_t kDtiCover = 5;
    auto distance2d = [](const Vec3& a, const Vec3& b) {
        float dx = a.x - b.x;
        float dy = a.y - b.y;
        return sqrtf(dx * dx + dy * dy);
    };
    obj->move_command = 0;
    float toKurt = distance2d(obj->position, kurtPosition_);
    std::vector<Vec3> spots;
    const formats::Dti::ArenaEntry* entry = level_->entryNamed(obj->arena);
    if (entry != nullptr) {
        for (const formats::Dti::Record& record : entry->records) {
            if (record.type != kDtiCover) {
                continue;
            }
            Vec3 spot{record.position[0], record.position[1], record.position[2]};
            float dist = distance2d(obj->position, spot);
            if (dist < 5.0f || dist > 400.0f || dist > toKurt ||
                distance2d(spot, kurtPosition_) >= toKurt) {
                continue;
            }
            spots.push_back(spot);
        }
    }
    if (spots.empty()) {
        return;
    }
    const Vec3& destination = spots[rng_.next((int)spots.size())];
    obj->move_command = kAdvanceCommand;
    obj->move_destination = destination;
    obj->waypoint = destination;
    obj->contact_flags &= ~MdkObject::ContactStuck;
}

// PickWaypoint8 (ScriptRuntime.cs:2338, opcode 221): one of the arena's DTI
// type-8 waypoints, chosen by weighted random, becomes the destination (command
// 221). Every waypoint weighs at least 1 inside the mode's window and 0 outside
// it: 500 - distance (2D), plus 200 - distance-to-Kurt when Kurt is closer than
// 200, plus 250 when the object is nearer to Kurt than the waypoint is and the
// waypoint is farther from Kurt than from the object, less half the height
// difference. Mode 1 keeps the waypoints whose id is within 3 of the nearest one
// (nearest by 3D distance); any other mode keeps those 50-500 units away (2D).
// The single RNG draw (when any waypoint qualifies) matches the C# one-for-one.
// Command 221 is not a case in the oracle's UpdateCommand, so setting the
// destination is the whole effect -- there is no per-tick consumer to port.
void ScriptRuntime::pickWaypoint8(MdkObject* obj, int32_t mode) {
    const int32_t kPickCommand = 221;
    const uint32_t kDtiWaypoint = 8;
    auto distance2d = [](const Vec3& a, const Vec3& b) {
        float dx = a.x - b.x;
        float dy = a.y - b.y;
        return sqrtf(dx * dx + dy * dy);
    };
    auto distance3d = [](const Vec3& a, const Vec3& b) {
        float dx = a.x - b.x;
        float dy = a.y - b.y;
        float dz = a.z - b.z;
        return sqrtf(dx * dx + dy * dy + dz * dz);
    };
    obj->move_command = 0;

    // OfType(ArenaRecords(arena), DtiWaypoint): the arena's type-8 records in DTI
    // order. arenaRecords caches the list; a null list is the C#'s empty list.
    std::vector<const formats::Dti::Record*> records;
    const std::vector<formats::Dti::Record>* all = arenaRecords(obj->arena);
    if (all != nullptr) {
        for (size_t i = 0; i < all->size(); i++) {
            if ((*all)[i].type == kDtiWaypoint) {
                records.push_back(&(*all)[i]);
            }
        }
    }

    // Mode 1's nearest waypoint id, by 3D distance from the object.
    int32_t nearestId = 0;
    if (mode == 1) {
        float nearest = 999999.0f;
        for (size_t i = 0; i < records.size(); i++) {
            const formats::Dti::Record& r = *records[i];
            Vec3 spot{r.position[0], r.position[1], r.position[2]};
            float distance = distance3d(obj->position, spot);
            if (distance < nearest) {
                nearest = distance;
                nearestId = r.id;
            }
        }
    }

    // One weight per waypoint and their total. The distance from the object to
    // Kurt is loop-invariant (the C# recomputes it every iteration; same value).
    std::vector<int32_t> weights;
    weights.reserve(records.size());
    int32_t total = 0;
    const float objToKurt = distance2d(obj->position, kurtPosition_);
    for (size_t i = 0; i < records.size(); i++) {
        const formats::Dti::Record& r = *records[i];
        Vec3 spot{r.position[0], r.position[1], r.position[2]};
        float distance = distance2d(spot, obj->position);
        int32_t weight = 0;
        int32_t idDelta = r.id - nearestId;
        if (idDelta < 0) {
            idDelta = -idDelta;
        }
        const bool nearNearest = mode == 1 && idDelta <= 3;
        const bool inWindow = mode != 1 && distance >= 50.0f && distance <= 500.0f;
        if (nearNearest || inWindow) {
            float toKurt = distance2d(spot, kurtPosition_);
            // Round is ScriptMath.Round (MidpointRounding.AwayFromZero), which the
            // C# reaches unqualified via `using static ScriptMath`; C roundf matches
            // it exactly, MathF.Round's banker's form (roundToEven above) does not.
            weight = (int32_t)roundf(500.0f - distance);
            if (toKurt < 200.0f) {
                weight = (int32_t)roundf((float)weight + 200.0f - toKurt);
            }
            if (objToKurt < distance && toKurt > distance) {
                weight += 250;
            }
            weight = (int32_t)roundf((float)weight - fabsf(spot.z - obj->position.z) * 0.5f);
            weight = weight > 1 ? weight : 1;
        }
        weights.push_back(weight);
        total += weight;
    }

    if (total == 0) {
        return;
    }

    int32_t pick = rng_.next(total);
    for (size_t i = 0; i < records.size(); i++) {
        pick -= weights[i];
        if (pick >= 0) {
            continue;
        }
        const formats::Dti::Record& r = *records[i];
        obj->move_command = kPickCommand;
        obj->move_destination = Vec3{r.position[0], r.position[1], r.position[2]};
        obj->waypoint = obj->move_destination;
        obj->contact_flags &= ~MdkObject::ContactStuck;
        return;
    }
}

// FindCoverSpot (ScriptRuntime.cs:2447, opcode 46): a DTI cover spot (type 5) in
// the object's arena that Kurt cannot see (a blocked ray Kurt->spot) but the
// object can reach (a clear ray obj->spot), within [9,400] of the object, nearer
// to the object than Kurt is, and nearer to the object than Kurt. The nearest is
// chosen, biased nearer-first when three or more qualify. Sets MoveCommand 43 on
// success and clears it otherwise. The single RNG draw (when any spot qualifies)
// matches the C# one-for-one.
void ScriptRuntime::findCoverSpot(MdkObject* obj) {
    const int kCoverCommand = 43;
    const uint32_t kDtiCover = 5;
    const float kEyeHeight = 5.0f;
    static const int kNearerFirst[] = {0, 0, 0, 1, 1, 2};
    const int kNearerFirstCount = (int)(sizeof(kNearerFirst) / sizeof(kNearerFirst[0]));
    auto distance = [](const Vec3& a, const Vec3& b) {
        float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return sqrtf(dx * dx + dy * dy + dz * dz);
    };
    float toKurt = distance(obj->position, kurtPosition_);
    Vec3 kurtEye{kurtPosition_.x, kurtPosition_.y, kurtPosition_.z + kEyeHeight};
    Vec3 objEye{obj->position.x, obj->position.y, obj->position.z + kEyeHeight};
    std::vector<Vec3> spots;
    const formats::Dti::ArenaEntry* entry = level_->entryNamed(obj->arena);
    if (entry != nullptr) {
        for (const formats::Dti::Record& record : entry->records) {
            if (record.type != kDtiCover) {
                continue;
            }
            Vec3 spot{record.position[0], record.position[1], record.position[2]};
            float dist = distance(obj->position, spot);
            if (dist < 9.0f || dist > 400.0f || dist > toKurt ||
                distance(spot, kurtPosition_) >= toKurt) {
                continue;
            }
            Vec3 spotEye{spot.x, spot.y, spot.z + kEyeHeight};
            if (raycastClear(kurtEye, spotEye) || !raycastClear(objEye, spotEye)) {
                continue;
            }
            spots.push_back(spot);
        }
    }
    obj->move_command = 0;
    if (spots.empty()) {
        return;
    }
    std::sort(spots.begin(), spots.end(), [&](const Vec3& a, const Vec3& b) {
        return distance(obj->position, a) < distance(obj->position, b);
    });
    int choice = (int)spots.size() >= 3 ? kNearerFirst[rng_.next(kNearerFirstCount)]
                                        : rng_.next((int)spots.size());
    if (choice > (int)spots.size() - 1) {
        choice = (int)spots.size() - 1;
    }
    const Vec3& destination = spots[choice];
    obj->move_command = kCoverCommand;
    obj->move_destination = destination;
    obj->waypoint = destination;
    obj->path = 0;
}

// PlanMove (0x45a434): head straight for the destination when a ray clears it, else
// the nearest detour point around the block (both sides, widening from the midpoint).
void ScriptRuntime::planMove(MdkObject* obj) {
    const int kDetours = 11;
    obj->waypoint = obj->move_destination;
    obj->stuck_count = 0;
    obj->stuck_ticks = 0.0f;
    obj->stuck_moved = Vec3{0.0f, 0.0f, 0.0f};
    obj->contact_flags &= ~MdkObject::ContactStuck;
    Vec3 up{0.0f, 0.0f, kPlanHeight};
    Vec3 a{obj->position.x + up.x, obj->position.y + up.y, obj->position.z + up.z};
    Vec3 b{obj->move_destination.x + up.x, obj->move_destination.y + up.y,
           obj->move_destination.z + up.z};
    if (raycastClear(a, b)) {
        return;
    }
    Vec3 middle{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f};
    const float sides[2] = {-1.0f, 1.0f};
    for (int i = 0; i < kDetours; i++) {
        for (int s = 0; s < 2; s++) {
            // Detour(a,b,middle, 0.1*(i+1)*side): a point offset perpendicular to
            // the a->b line through the midpoint, by a fraction of the a->b distance:
            // raw radians, no degree conversion anywhere, as the C#'s
            // MathF.SinCos(MathF.Atan2(...)).
            /* @vfp-exempt R6 — the plan-move detour probe's perpendicular
             * direction; glibc atan2f/sinf/cosf match the C# oracle's MathF
             * calls bit-for-bit on this machine (probed); the LUT would move the
             * chosen waypoint. measured n/a (corpus empty). Verified 2026-10-26. */
            float rad = atan2f(b.y - a.y, b.x - a.x);
            float amount = 0.1f * (i + 1) * sides[s];
            float dist = sqrtf((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y) +
                               (b.z - a.z) * (b.z - a.z));
            Vec3 point{middle.x + sinf(rad) * amount * dist, middle.y + cosf(rad) * amount * dist,
                       middle.z};
            if (raycastClear(a, point) && raycastClear(point, b)) {
                obj->waypoint = point;
                return;
            }
        }
    }
}

// The replan on a stuck object (0x45a434): head for the destination again when the
// waypoint was a detour, otherwise probe detours from 3/4 of the way there, then
// from itself (0.1 to 1.9 of the distance, both sides).
static Vec3 detour(const Vec3& a, const Vec3& b, const Vec3& origin, float amount) {
    /* @vfp-exempt R6 — the stuck-replan detour's perpendicular direction; glibc
     * atan2f/sinf/cosf match the C# oracle's MathF.SinCos/Atan2 bit-for-bit on
     * this machine (probed), and the conversion is absent exactly as the C#'s;
     * the LUT would move the chosen waypoint. measured n/a (corpus empty).
     * Verified 2026-10-26. */
    const float rad = atan2f(b.y - a.y, b.x - a.x);
    const float distance = sqrtf((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y) +
                                 (b.z - a.z) * (b.z - a.z));
    Vec3 point;
    point.x = origin.x + sinf(rad) * amount * distance;
    point.y = origin.y + cosf(rad) * amount * distance;
    point.z = origin.z;
    return point;
}

// The replan on a stuck object (0x45a434): head for the destination again when the
// waypoint was a detour; otherwise probe detours from 3/4 of the way there, then
// from itself (0.1 to 1.9 of the distance, both sides).
void ScriptRuntime::replan(MdkObject* obj) {
    const int kDetours = 7;
    if (obj->waypoint.x != obj->move_destination.x || obj->waypoint.y != obj->move_destination.y ||
        obj->waypoint.z != obj->move_destination.z) {
        obj->waypoint = obj->move_destination;
        obj->stuck_ticks = 0.0f;
        obj->stuck_moved = Vec3{0.0f, 0.0f, 0.0f};
        obj->contact_flags &= ~MdkObject::ContactStuck;
        return;
    }
    static const float kPlanHeight = 8.0f;
    Vec3 up{0.0f, 0.0f, kPlanHeight};
    Vec3 a{obj->position.x + up.x, obj->position.y + up.y, obj->position.z + up.z};
    Vec3 b{obj->move_destination.x + up.x, obj->move_destination.y + up.y,
           obj->move_destination.z + up.z};
    // The C#'s two origins in order: 3/4 there, then itself.
    for (int originIndex = 0; originIndex < 2; originIndex++) {
        Vec3 origin;
        if (originIndex == 0) {
            origin.x = b.x * 0.75f + a.x * 0.25f;
            origin.y = b.y * 0.75f + a.y * 0.25f;
            origin.z = b.z * 0.75f + a.z * 0.25f;
        } else {
            origin = a;
        }
        const float sides[2] = {-1.0f, 1.0f};
        for (int i = 0; i < kDetours; i++) {
            for (int s = 0; s < 2; s++) {
                Vec3 point = detour(a, b, origin, (0.1f + 0.3f * i) * sides[s]);
                if (raycastClear(a, point) && raycastClear(point, b)) {
                    obj->waypoint = point;
                    return;
                }
            }
        }
    }
}

void ScriptRuntime::startMove(MdkObject* obj, int32_t command, const Vec3& destination) {
    obj->move_command = command;
    obj->move_destination = destination;
    obj->path = 0;
    planMove(obj);
}

// TurnAndJump (ObjectMotion.cs:426): turn towards the destination by at most
// rate degrees per second. Once facing it, enter movement command 229 and set
// the ballistic velocity. The arc clears the higher end by at least ten units
// and the lower end by at least thirty; its duration accounts for friction.
void ScriptRuntime::turnAndJump(MdkObject* obj, float rate) {
    const int32_t kJumpCommand = 229;
    const float kAboveHigher = 10.0f;
    const float kAboveLower = 30.0f;
    const float heading = yawTo(obj, obj->move_destination);
    const float diff = wrapAngle(heading - obj->yaw);
    if (fabsf(diff) > rate * kTick) {
        obj->yaw = wrap360(obj->yaw + signf(diff) * rate * kTick);
        return;
    }

    obj->yaw = heading;
    obj->move_command = kJumpCommand;
    const float z = obj->position.z;
    const float goal = obj->move_destination.z;
    const float top = goal < z ? mathMax(z + kAboveHigher, goal + kAboveLower)
                               : mathMax(goal + kAboveHigher, z + kAboveLower);
    const float gravity = obj->gravity;
    const float up = sqrtf((top - z) * 2.0f * gravity);
    const float flight = mathMax(up * up + (z - goal) * gravity * 2.0f, 0.0f);
    const float time = (up + sqrtf(flight)) / gravity;
    obj->parameter = time;
    obj->parameter_timer = 0.0f;

    const float dx = obj->move_destination.x - obj->position.x;
    const float dy = obj->move_destination.y - obj->position.y;
    const float distance = sqrtf(dx * dx + dy * dy);
    const float speed =
        (obj->friction * 0.5f * time * time + distance) / time;
    const float directionX = distance > 0.0f ? dx / distance : 0.0f;
    const float directionY = distance > 0.0f ? dy / distance : 0.0f;
    obj->velocity.x = directionX * speed;
    obj->velocity.y = directionY * speed;
    obj->velocity.z = up;
}

// Links (ObjectMotion.cs:503): the live followers of the leader, ordered by
// instance id. The C#'s insertion sort is stable, so equal ids keep the
// object-list order; this one does the same.
void ScriptRuntime::chainLinks(const MdkObject* leader) {
    chainLinks_.clear();
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        MdkObject& other = *it;
        if (other.dead || other.leader != leader) {
            continue;
        }
        size_t at = chainLinks_.size();
        while (at > 0 && chainLinks_[at - 1]->instance_id > other.instance_id) {
            at--;
        }
        chainLinks_.insert(chainLinks_.begin() + at, &other);
    }
}

// UpdateChain (ObjectMotion.cs:464, movement command 30): the chain links hang
// off their leader — each link's reference point 0 sits at the previous link's
// reference point 1, and the links turn 90 degrees apart. The link with instance
// id 0 drives the chain; a link whose chain is broken (a link before it is gone)
// lets go of the leader, exactly as the C#.
void ScriptRuntime::updateChain(MdkObject* link) {
    const float kLinkTurn = 90.0f;
    MdkObject* leader = link->leader;
    if (leader == nullptr || leader->dead) {
        link->leader = nullptr;
        link->move_command = 0;
        return;
    }
    chainLinks(leader);
    // Every link before this one must be there, with the instance id its place
    // in the chain says; a gap breaks the chain.
    for (int32_t i = 0; i < link->instance_id; i++) {
        if (i >= (int32_t)chainLinks_.size() ||
            chainLinks_[(size_t)i]->instance_id != i) {
            link->leader = nullptr;
            link->move_command = 0;
            return;
        }
    }
    // Only the first link moves the chain; the others keep their place.
    if (link->instance_id != 0) {
        return;
    }
    MdkObject* previous = leader;
    for (size_t i = 0; i < chainLinks_.size() &&
                        chainLinks_[i]->instance_id == (int32_t)i; i++) {
        MdkObject* current = chainLinks_[i];
        current->yaw = wrap360(leader->yaw + kLinkTurn * (float)i);
        // The C# sets the yaw first, so the link's own reference point 0 is the
        // one with the NEW yaw and the old position.
        Vec3 from = current->reference_point(0);
        Vec3 to = previous->reference_point(1);
        current->position.x += to.x - from.x;
        current->position.y += to.y - from.y;
        current->position.z += to.z - from.z;
        previous = current;
    }
}

// FollowAttachment (0x45b6c8, movement command 74): an attached object (a pickup's
// chute) hangs on its leader's reference point. With AttachPoints (0,0) the delta is
// the leader's reference point minus its own, so a chute with no reference points
// snaps to the leader's position and copies its yaw/pitch. The command clears when
// the leader is dead or gone (the C# also clears on Health==0).
void ScriptRuntime::followAttachment(MdkObject* obj) {
    MdkObject* other = obj->leader;
    if (other == nullptr || other->dead || other->health == 0) {
        obj->move_command = 0;
        return;
    }
    Vec3 a = obj->reference_point(obj->attach_point_a);
    Vec3 b = other->reference_point(obj->attach_point_b);
    obj->position.x += b.x - a.x;
    obj->position.y += b.y - a.y;
    obj->position.z += b.z - a.z;
    obj->yaw = other->yaw;
    obj->pitch = other->pitch;
}

// ObjectMotion.Accelerate (ObjectMotion.cs:780-790): the oracle's ONE speed step, and it
// has exactly three callers — MoveForward (:979), Walk (:819) and Fly (:840). The port had
// inlined it at all three, which is the shape review-20 F2 came from: a fidelity fix to the
// clamp lands in one copy and not the others and no gate sees it (batch 49 changed 2 of the
// 8 clamp sites that way, and review-20 caught it). One implementation per behaviour, as an
// ordinary file-scope free function — no template, no virtual, and the same shape as
// detectStuck below (§10 permits exactly this). NOTHING here is new arithmetic: the two
// comparisons, the two clamps and the multiply order are the lines that were already in all
// three arms, moved, so no hot-path rule changes state (R3's float comparison is the
// oracle's own, relocated rather than introduced). Pinned by
// AccelerateIsOneImplementationAcrossTheArms, which drives all three arms from identical
// state and requires each to land on the value .NET's own Accelerate prints (probe mode
// "accel"), and by the three per-arm NaN pins of batch 50.
static void accelerate(MdkObject* obj, float targetSpeed) {
    if (obj->speed > targetSpeed) {
        obj->speed = mathMax(obj->speed - obj->deceleration * ScriptRuntime::kTick,
                             targetSpeed);
    } else if (obj->speed < targetSpeed) {
        obj->speed = mathMin(obj->speed + obj->acceleration * ScriptRuntime::kTick,
                             targetSpeed);
    }
}

// ObjectMotion.MoveForward (command 88, set_move_x): accelerate to the max speed
// (or to a stop, parameter 0) and push along the yaw — and the pitch, when the
// object is not held on the ground by gravity.
void ScriptRuntime::moveForward(MdkObject* obj) {
    const float targetSpeed = obj->move_parameter != 0 ? obj->max_speed : 0.0f;
    accelerate(obj, targetSpeed);
    const Vec3 direction = fromAngle(obj->yaw);
    if ((obj->flags & MdkObject::FlagGravity) != 0) {
        obj->push.x += obj->speed * direction.x;
        obj->push.y += obj->speed * direction.y;
    } else {
        /* @vfp-exempt R6 — MoveForward's pitch push (per moving object per tick);
         * glibc sinf/cosf match the C# oracle's MathF.SinCos bit-for-bit on this
         * machine (probed), and the conversion is .NET's left-associative
         * DegreesToRadians; the fastSin/fastCos LUT's error is far larger and
         * would move those values. measured n/a (corpus empty).
         * Verified 2026-10-23. */
        const float s = sinf(degreesToRadians(obj->pitch));
        const float c = cosf(degreesToRadians(obj->pitch));
        obj->push.x += direction.x * c * obj->speed;
        obj->push.y += direction.y * c * obj->speed;
        obj->push.z += s * obj->speed;
    }
    if (obj->move_parameter == 0 && obj->speed == targetSpeed) {
        obj->move_command = 0;
    }
}

// ObjectMotion.DetectStuck (0x45a7d4, 0x45ae74): after Walk/Fly, collisions with
// enough speed and little motion over a 16-tick window (9 for a flying 197) mark
// the object stuck. A walking 43 counts ANY collision as stuck (the cover case).
static void detectStuck(MdkObject* obj, bool walking) {
    const float kStuckSpeed = 2.0f;
    const float kWindow = 16.0f;
    const float kAdvanceWindow = 9.0f;
    const float kCoverStuckTicks = 30.0f;
    const bool collided = (obj->contact_flags & MdkObject::ContactCollided) != 0;
    if (walking && obj->move_command == 43) {
        if (collided) {
            obj->contact_flags |= MdkObject::ContactStuck;
            obj->stuck_ticks = kCoverStuckTicks;
            return;
        }
        obj->stuck_count = 0;
        obj->stuck_ticks = 0.0f;
        obj->stuck_moved = Vec3{0.0f, 0.0f, 0.0f};
        return;
    }
    if (!collided || obj->speed <= kStuckSpeed) {
        obj->stuck_count = 0;
        obj->stuck_ticks = 0.0f;
        obj->stuck_moved = Vec3{0.0f, 0.0f, 0.0f};
        return;
    }
    const float window = walking && obj->move_command == 197 ? kAdvanceWindow : kWindow;
    if (obj->stuck_ticks < window) {
        // Ticks, not Dt: the window counts updates (ObjectMotion.Ticks = 1f).
        obj->stuck_moved.x += obj->position.x - obj->previous_position.x;
        obj->stuck_moved.y += obj->position.y - obj->previous_position.y;
        obj->stuck_moved.z += obj->position.z - obj->previous_position.z;
        obj->stuck_ticks += 1.0f;
        return;
    }
    if (fabsf(obj->stuck_moved.x) + fabsf(obj->stuck_moved.y) + fabsf(obj->stuck_moved.z) >=
        obj->speed * 0.5f) {
        obj->stuck_ticks = 0.0f;
        obj->stuck_moved = Vec3{0.0f, 0.0f, 0.0f};
        return;
    }
    obj->contact_flags |= MdkObject::ContactStuck;
}

// ObjectMotion.HandleStuck (0x45b6c8): a stuck object that isn't turning (under 3°
// this update) replans once, then move_to slides straight to the waypoint through
// everything and the others give up (if_move_idle_flag sees StuckCount). A 197
// gives up at once.
void ScriptRuntime::handleStuck(MdkObject* obj, float turned) {
    const float kTurning = 3.0f;
    const int32_t kReplans = 3;

    if ((obj->contact_flags & MdkObject::ContactStuck) == 0 || turned >= kTurning) {
        return;
    }
    obj->stuck_count++;
    if (obj->move_command == 197) {
        // ObjectMotion.Stop: an idled command keeps no stale state.
        obj->move_command = 0;
        obj->speed = 0.0f;
        obj->contact_flags &= ~MdkObject::ContactStuck;
    } else if (obj->stuck_count < kReplans) {
        replan(obj);
        obj->stuck_count++;
    } else if (obj->move_command == 78) {
        Vec3 toWaypoint;
        toWaypoint.x = obj->waypoint.x - obj->position.x;
        toWaypoint.y = obj->waypoint.y - obj->position.y;
        toWaypoint.z = obj->waypoint.z - obj->position.z;
        const float step = obj->speed * kTick;
        obj->position.x += clampf(toWaypoint.x, -step, step);
        obj->position.y += clampf(toWaypoint.y, -step, step);
        obj->position.z += clampf(toWaypoint.z, -step, step);
    } else {
        obj->move_command = 0;
        obj->speed = 0.0f;
    }
}

// FlyProjectile (ObjectMotion.cs:1016): straight along the yaw and pitch until the
// lifetime (parameter, set by the projectile's own script — opcode 106) runs out,
// the raycast hits the arena, or the grown Kurt box is entered. Touching Kurt (in
// his box grown by 1) records the contact; with damage, the C# hurts Kurt and
// kills the projectile — HurtKurt is Kurt's slice, so only the projectile's own
// death is observable here (the zero-damage touch records without removing).
void ScriptRuntime::flyProjectile(MdkObject* obj) {
    // ObjectMotion.FlyProjectile, in .NET evaluation order: from the old position.
    const Vec3 old = obj->position;
    // float.DegreesToRadians is left-associated (x * pi / 180f).
    /* @vfp-exempt R6 — the projectile's straight-line step (per projectile per
     * tick); glibc sinf/cosf match the C# oracle's MathF.SinCos bit-for-bit on
     * this machine (probed), and the conversions are .NET's left-associative
     * spellings; the LUT would move the step. measured n/a (corpus empty).
     * Verified 2026-10-24. */
    const float yawSin = sinf(obj->yaw * 3.14159265358979323846f / 180.0f);
    const float yawCos = cosf(obj->yaw * 3.14159265358979323846f / 180.0f);
    const float pitchSin = sinf(obj->pitch * 3.14159265358979323846f / 180.0f);
    const float pitchCos = cosf(obj->pitch * 3.14159265358979323846f / 180.0f);
    obj->position.x += yawCos * pitchCos * obj->speed * kTick;
    obj->position.y += yawSin * pitchCos * obj->speed * kTick;
    obj->position.z += pitchSin * obj->speed * kTick;
    obj->parameter -= kTick;
    if (obj->parameter <= 0.0f) {
        kill(obj);
        return;
    }
    Vec3 point;
    if (raycastPoint(old, obj->position, &point)) {
        obj->position = point;
        kill(obj);
        return;
    }
    const float kKurtMargin = 1.0f;
    formats::Box grown = kurtBox();
    grown.min.x -= kKurtMargin;
    grown.min.y -= kKurtMargin;
    grown.min.z -= kKurtMargin;
    grown.max.x += kKurtMargin;
    grown.max.y += kKurtMargin;
    grown.max.z += kKurtMargin;
    // SegmentEntry (ScriptMath.cs:89): where the segment enters the box (its start
    // when inside), or no entry. Like the C#, the hit writes the contact flag and
    // the entry point.
    float enter = 0.0f;
    float leave = 1.0f;
    bool miss = false;
    const float from[3] = {old.x, old.y, old.z};
    const float delta[3] = {obj->position.x - old.x, obj->position.y - old.y,
                            obj->position.z - old.z};
    const float boxMin[3] = {grown.min.x, grown.min.y, grown.min.z};
    const float boxMax[3] = {grown.max.x, grown.max.y, grown.max.z};
    for (int axis = 0; axis < 3 && !miss; axis++) {
        if (delta[axis] == 0.0f) {
            if (from[axis] < boxMin[axis] || from[axis] > boxMax[axis]) {
                miss = true;
            }
            continue;
        }
        const float t0 = (boxMin[axis] - from[axis]) / delta[axis];
        const float t1 = (boxMax[axis] - from[axis]) / delta[axis];
        // MathF.Max/MathF.Min (ScriptMath.cs:110-111), so a NaN box or a NaN step makes
        // BOTH enter and leave NaN — and NaN > NaN is false, so there is still no miss and
        // the entry point is from + d*NaN = NaN. fmaxf/fminf would return 0 and 1 and
        // report an entry at the segment's own start. Measured on .NET 10 (probe
        // `nanclamp`, rows SEGMENT nan_box and nan_delta: hit=True point=NaN,NaN,NaN).
        enter = mathMax(enter, mathMin(t0, t1));
        leave = mathMin(leave, mathMax(t0, t1));
        if (enter > leave) {
            miss = true;
        }
    }
    if (!miss) {
        obj->contact_flags |= MdkObject::ContactTouchedKurt;
        obj->position.x = old.x + delta[0] * enter;
        obj->position.y = old.y + delta[1] * enter;
        obj->position.z = old.z + delta[2] * enter;
        if (obj->touch_damage > 0) {
            // C#: runtime.HurtKurt(obj.TouchDamage) — Kurt's slice — then Kill.
            kill(obj);
        }
    } else {
        obj->contact_flags &= ~MdkObject::ContactTouchedKurt;
    }
}

// A numeric operand as an int (the C#'s ScriptMath.I), file-local because the
// runtime's only ScriptValue inputs are decoded lists (fire's origin).
static int32_t operandInt(const formats::ScriptValue& v) {
    if (v.kind == formats::ScriptValueKind::Int) {
        return v.intValue;
    }
    if (v.kind == formats::ScriptValueKind::Float) {
        return (int32_t)v.floatValue;
    }
    return 0;
}

// A string operand (the C#'s `origin[1] as string ?? ""`): "" for anything else.
static std::string operandString(const formats::ScriptValue& v) {
    if (v.kind == formats::ScriptValueKind::String) {
        return v.stringValue;
    }
    return std::string();
}

// Fire (ScriptRuntime.cs:2659): a projectile from a reference point of the object
// (origin [0, index]) or at the centre of one of its parts ([1, name]), flying
// along the object's yaw and pitch, under the projectile's script.
MdkObject* ScriptRuntime::fire(MdkObject* obj, const std::vector<formats::ScriptValue>& origin,
                               const std::string& bullet, int32_t script) {
    const int32_t kProjectileCommand = 61;
    // ProjectileFlags (ScriptRuntime.cs:25): FlagNotTarget | FlagNotSolid2 |
    // FlagChangesArena — the shot is not the chain gun's target, walks through,
    // and may change arena.
    static const int32_t kProjectileFlags = 0x80820;
    Vec3 start;
    if (!origin.empty() && operandInt(origin[0]) == 0) {
        const int32_t index = origin.size() > 1 ? operandInt(origin[1]) : 0;
        start = obj->reference_point(index);
    } else {
        const int32_t part =
            origin.size() > 1 ? obj->findPart(operandString(origin[1])) : -1;
        start = obj->partCenter(part);
    }
    MdkObject* shot = spawn(obj, bullet, start, obj->yaw, -1, 0, false);
    if (shot == nullptr) {
        return nullptr;
    }
    shot->move_command = kProjectileCommand;
    shot->flags |= kProjectileFlags;
    shot->pitch = obj->pitch;
    shot->restart = script;
    return shot;
}

// UpdateCommand (0x45a7d4): the movement-command dispatch for one object, run from
// motionVelocity before gravity and ApplyVelocity. The C#'s switch (ObjectMotion.cs:
// 331) has one arm per command and no default; every one of them is here now.
void ScriptRuntime::updateCommand(MdkObject* obj) {
    switch (obj->move_command) {
        case 1:  // FollowFormation: hold the slot command_objects gave us
            followFormation(obj);
            break;
        case 6: {  // Chase (move_to_target): fly at the runtime's target, 8 above it
            const float kChaseAbove = 8.0f;  // ObjectMotion.cs:328
            Vec3 point = targetPosition_;
            point.z += kChaseAbove;
            chase(obj, point);
            break;
        }
        case 74:
            followAttachment(obj);
            break;
        case 88:  // MoveForward (command of set_move_x): straight along the yaw
            moveForward(obj);
            break;
        case 30:  // UpdateChain: the chain links hang off their leader
            updateChain(obj);
            break;
        case 15: {  // alarm: ALERT every 32 ticks; if_alarm holds for ten ticks
            const int32_t kAlarmEvery = 31;
            const int32_t kAlarmTicks = 10;
            if (obj->arena != currentArena_) {
                break;
            }
            if ((tickCount_ & kAlarmEvery) == 0) {
                mixer_.playAt("ALERT", obj->position);
            }
            alarmTicks = kAlarmTicks;
            break;
        }
        case 61:  // FlyProjectile (fire): straight along the yaw and pitch
            flyProjectile(obj);
            break;
        case 229: {  // TurnAndJump's flight: stop after half the recorded flight time
            obj->parameter_timer += kTick;
            if (obj->parameter_timer >= obj->parameter * 0.5f &&
                (obj->contact_flags & MdkObject::ContactFloor) != 0) {
                obj->move_command = 0;
                obj->velocity = Vec3{};
            }
            break;
        }
        case 43:   // move to a destination (command_objects 43):
        case 197:  // the same walk/fly handling as move_to (ObjectMotion.cs:338)
        case 78: {
            const float yaw = obj->yaw;
            bool walking = (obj->flags & MdkObject::FlagGravity) != 0;
            if (walking) {
                // Walk (0x45a7d4): step to the waypoint on the ground, then the
                // destination. Arrival within 5 (Manhattan in X/Y) advances or stops.
                const float kArrived = 5.0f;
                bool final = vecEq(obj->waypoint, obj->move_destination);
                if (fabsf(obj->waypoint.x - obj->position.x) +
                        fabsf(obj->waypoint.y - obj->position.y) <
                    kArrived) {
                    if (final) {
                        obj->move_command = 0;
                        obj->speed = 0.0f;
                        obj->contact_flags &= ~MdkObject::ContactStuck;
                        return;
                    }
                    obj->waypoint = obj->move_destination;
                }
                float targetYaw = obj->yaw;
                if (obj->waypoint.x != obj->position.x || obj->waypoint.y != obj->position.y) {
                    targetYaw = yawTo(obj, obj->waypoint);
                }
                float diff = wrapAngle(targetYaw - obj->yaw);
                float maxStep = kTurnSpeed * kTick;
                obj->yaw = wrap360(obj->yaw + clampf(diff, -maxStep, maxStep));
                float targetSpeed = obj->max_speed * (fabsf(diff) > kSlowTurnAngle ? 0.5f : 1.0f);
                accelerate(obj, targetSpeed);
                Vec3 direction = fromAngle(obj->yaw);
                obj->push.x += obj->speed * direction.x;
                obj->push.y += obj->speed * direction.y;
            } else {
                // Fly (0x45ae74): turn toward the waypoint (unless NoTurning), then
                // step along the heading, climbing by the vertical fraction of the
                // remaining distance.
                const float kTurnFrom = 3.0f;
                const float kArrivedAcross = 4.0f;
                const float kArrivedUp = 3.0f;
                bool final = vecEq(obj->waypoint, obj->move_destination);
                Vec3 toW{obj->waypoint.x - obj->position.x, obj->waypoint.y - obj->position.y,
                         obj->waypoint.z - obj->position.z};
                float diff = 0.0f;
                bool noTurning = (obj->flags & MdkObject::FlagNoTurning) != 0;
                if (!noTurning && fabsf(toW.x) + fabsf(toW.y) > kTurnFrom) {
                    float targetYaw = obj->yaw;
                    if (obj->waypoint.x != obj->position.x || obj->waypoint.y != obj->position.y) {
                        targetYaw = yawTo(obj, obj->waypoint);
                    }
                    diff = wrapAngle(targetYaw - obj->yaw);
                    float maxStep = kTurnSpeed * kTick;
                    obj->yaw = wrap360(obj->yaw + clampf(diff, -maxStep, maxStep));
                }
                float targetSpeed = obj->max_speed * (fabsf(diff) > kSlowTurnAngle ? 0.5f : 1.0f);
                accelerate(obj, targetSpeed);
                if (!noTurning) {
                    float horizontal = sqrtf(toW.x * toW.x + toW.y * toW.y);
                    float vertical = fabsf(toW.z);
                    float climb = horizontal + vertical > 0.0f ? vertical / (horizontal + vertical)
                                                               : 0.0f;
                    Vec3 direction = fromAngle(obj->yaw);
                    obj->push.x += direction.x * obj->speed * (1.0f - climb);
                    obj->push.y += direction.y * obj->speed * (1.0f - climb);
                    obj->push.z += obj->speed * climb * signf(toW.z);
                    if (horizontal < kArrivedAcross && vertical < kArrivedUp) {
                        if (final) {
                            obj->move_command = 0;
                            obj->speed = 0.0f;
                            obj->contact_flags &= ~MdkObject::ContactStuck;
                        } else {
                            obj->waypoint = obj->move_destination;
                        }
                    }
                } else {
                    float distance = fabsf(toW.x) + fabsf(toW.y) + fabsf(toW.z);
                    if (distance < 1.0f) {
                        if (final) {
                            obj->position = obj->waypoint;
                            obj->move_command = 0;
                            obj->speed = 0.0f;
                            obj->contact_flags &= ~MdkObject::ContactStuck;
                            return;
                        }
                        obj->waypoint = obj->move_destination;
                    }
                    if (distance > 0.0f) {
                        float scale = obj->speed * kTick / distance;
                        float stepX = toW.x * scale;
                        float stepY = toW.y * scale;
                        float stepZ = toW.z * scale;
                        if (fabsf(stepX) > fabsf(toW.x)) {
                            stepX = toW.x;
                        }
                        if (fabsf(stepY) > fabsf(toW.y)) {
                            stepY = toW.y;
                        }
                        if (fabsf(stepZ) > fabsf(toW.z)) {
                            stepZ = toW.z;
                        }
                        obj->push.x += stepX / kTick;
                        obj->push.y += stepY / kTick;
                        obj->push.z += stepZ / kTick;
                    }
                }
            }
            // DetectStuck then HandleStuck(|yaw - yawAtEntry|) (ObjectMotion.cs:354-358).
            // The guard is the oracle's, not a port choice. Stop() sets MoveCommand = 0,
            // Speed = 0 and clears ContactStuck — StuckCount is NOT touched
            // (ObjectMotion.cs:792-797) — and on the tick it runs the C# skips BOTH calls.
            // A Walk arrival gets that skip from Walk's own early return (:811-812), which
            // is why the walk case cannot see this guard; it is load-bearing on the FLY
            // arrival, where Fly returns to the switch at :860 with MoveCommand already 0.
            // Without the guard DetectStuck's !collided || speed <= 2 branch would run
            // ResetStuck and zero a stuck_count an earlier replan left, and opcode 231
            // (if_move_idle_flag) reads move_command == 0 && stuck_count != 0, so it would
            // branch the other way on an object that arrived after a replan. Pinned by
            // ArrivalKeepsTheStuckCountTheOracleKeeps (both directions).
            if (obj->move_command != 0) {
                detectStuck(obj, walking);
                handleStuck(obj, fabsf(wrapAngle(obj->yaw - yaw)));
            }
            break;
        }
        default:
            break;
    }
}


Bsp* ScriptRuntime::bspOf(const std::string& arena) {
    std::map<std::string, std::unique_ptr<Bsp>>::iterator it = bsps_.find(arena);
    if (it != bsps_.end()) {
        return it->second.get();
    }
    // The full arena set (MTO arenas AND SNI corridors), as the C# builds _bsps
    // from every level.Arenas entry -- not just the MTO.
    const formats::Arena* a = level_->arenaNamed(arena);
    if (a == nullptr) {
        return nullptr;
    }
    std::unique_ptr<Bsp> built(new Bsp(a));
    Bsp* raw = built.get();
    bsps_.emplace(arena, std::move(built));  // unique_ptr keeps the address stable
    return raw;
}

float ScriptRuntime::getArenaFloor(const std::string& arena) {
    std::map<std::string, float>::iterator it = floors_.find(arena);
    if (it != floors_.end()) {
        return it->second;
    }
    const float kNoArenaFloor = -1000.0f;  // C# NoArenaFloor
    float floor = kNoArenaFloor;
    const formats::Arena* a = level_->arenaNamed(arena);  // full set, as _floors
    if (a != nullptr && !a->vertices().empty()) {
        float minz = a->vertices()[0].z;
        for (size_t i = 1; i < a->vertices().size(); ++i) {
            if (a->vertices()[i].z < minz) {
                minz = a->vertices()[i].z;
            }
        }
        floor = minz;
    }
    floors_.emplace(arena, floor);
    return floor;
}

Box ScriptRuntime::poseBounds(const MdkObject* obj) {
    const Model* model = obj->model;
    if (model == nullptr) {
        Box d;  // C# DefaultBounds
        d.min = Vec3f{-1.0f, -1.0f, 0.0f};
        d.max = Vec3f{1.0f, 1.0f, 4.0f};
        return d;
    }
    // One pose: the rest vertices, or the baked animation frame -- from the SAME
    // per-(model, animation) cache the C# keeps (MdkObject.RestPoses / Baked,
    // scripts/pose_cache.cpp), read by reference. pose_parts() applies exactly
    // the selection this function used to do by hand: the rest pose without an
    // animation, else the frame clamped into [0, frames-1], else empty.
    //
    // This used to BAKE THE WHOLE ANIMATION ON EVERY CALL and copy one frame out.
    // Measured 2026-11-17 (callgrind, `tick1 dump transform 3 301`):
    // ModelAnimation::bake was 79% of all instructions, reached through
    // worldBounds from the VM, and level 3's script tick cost 20x level 7's. On
    // the Old 3DS that made one 60 Hz step take 35.5 ms, so levels 3, 5 and 6
    // ran in the fixed-step death spiral at 1.7-2.5 fps.
    const std::vector<std::vector<Vec3f> >& pose = obj->pose_parts();
    // PoseBoxes per part (min/max over its vertices), combined over the visible
    // parts (hidden_parts bit clear); fall back to the model's own bounds.
    bool have = false;
    Box result;
    for (size_t i = 0; i < pose.size(); ++i) {
        if ((obj->hidden_parts & (1 << i)) != 0 || pose[i].empty()) {
            continue;
        }
        Vec3f mn = pose[i][0];
        Vec3f mx = pose[i][0];
        for (size_t v = 1; v < pose[i].size(); ++v) {
            const Vec3f& p = pose[i][v];
            if (p.x < mn.x) mn.x = p.x;
            if (p.y < mn.y) mn.y = p.y;
            if (p.z < mn.z) mn.z = p.z;
            if (p.x > mx.x) mx.x = p.x;
            if (p.y > mx.y) mx.y = p.y;
            if (p.z > mx.z) mx.z = p.z;
        }
        if (!have) {
            result.min = mn;
            result.max = mx;
            have = true;
        } else {
            if (mn.x < result.min.x) result.min.x = mn.x;
            if (mn.y < result.min.y) result.min.y = mn.y;
            if (mn.z < result.min.z) result.min.z = mn.z;
            if (mx.x > result.max.x) result.max.x = mx.x;
            if (mx.y > result.max.y) result.max.y = mx.y;
            if (mx.z > result.max.z) result.max.z = mx.z;
        }
    }
    return have ? result : model->bounds;
}

// GetAnimation (0x442e50): a CMI-stored animation parsed once per offset and kept,
// or — when the record's first u32 is 0 — the arena animation named by its 8 chars.
// The 1996 demo's file animations (IsBeta) are not reachable: the runtime always
// builds the retail decoder.
const formats::ModelAnimation* ScriptRuntime::getAnimation(MdkObject* obj, int32_t offset) {
    if (offset == 0) {
        return nullptr;
    }
    const std::vector<uint8_t>& bytes = cmi_->bytes();
    if (offset >= 0 && offset + 4 <= (int32_t)bytes.size()) {
        uint32_t first = 0;
        memcpy(&first, &bytes[offset], sizeof(first));
        if (first == 0) {
            char name[9] = {0};
            formats::binAscii(bytes.data(), offset + 4, 8, name);
            const formats::Arena* arena = level_->mto().has(obj->arena) ? level_->mto().getArena(obj->arena)
                                                                        : nullptr;
            return arena != nullptr ? arena->animation(name) : nullptr;
        }
    }
    std::map<int32_t, formats::ModelAnimation>::iterator it = animations_.find(offset);
    if (it != animations_.end()) {
        return &it->second;
    }
    char name[24];
    snprintf(name, sizeof(name), "CMI_%x", (unsigned)offset);
    formats::ModelAnimation animation;
    if (!formats::ModelAnimation::parse(&animation, name, bytes.data(), (int32_t)bytes.size(), offset)) {
        return nullptr;
    }
    std::pair<std::map<int32_t, formats::ModelAnimation>::iterator, bool> inserted =
        animations_.emplace(offset, std::move(animation));
    return &inserted.first->second;
}

// MdkObject.PlayAnimation: a loop sets FlagLoop; replaying the same live animation
// only clears a hold frame, otherwise the animation restarts from frame 0.
void ScriptRuntime::playAnimation(MdkObject* obj, const formats::ModelAnimation* animation, bool loop) {
    if (loop) {
        obj->flags |= MdkObject::FlagLoop;
    } else {
        obj->flags &= ~MdkObject::FlagLoop;
    }
    if (animation == obj->animation && obj->animation_end_frame != MdkObject::AnimationEnded) {
        if (obj->animation_end_frame >= 0) {
            obj->animation_end_frame = MdkObject::NoHoldFrame;
        }
        return;
    }
    obj->animation = animation;
    obj->animation_time = 0.0f;
    obj->animation_frame = 0;
    obj->animation_end_frame = animation != nullptr ? MdkObject::NoHoldFrame : MdkObject::AnimationEnded;
}

// MdkObject.AdvanceAnimation (object_anim_update): the frame clock, the hold frame,
// the loop clamp, and the root motion of every passed frame joined into the push
// (divided by delta so ApplyVelocity's *Dt puts it back to per-tick units).
void ScriptRuntime::advanceAnimation(MdkObject* obj, float delta) {
    const formats::ModelAnimation* animation = obj->animation;
    if (animation == nullptr || obj->animation_end_frame == MdkObject::AnimationEnded) {
        return;
    }
    if (obj->animation_end_frame >= 0 && obj->animation_frame == obj->animation_end_frame) {
        return;
    }
    int32_t count = animation->frameCount;
    if (count <= 0) {
        return;
    }
    obj->animation_time += delta * obj->animation_rate * animation->speed;
    if (obj->animation_end_frame >= 0 && obj->animation_frame < obj->animation_end_frame &&
        roundToEven(obj->animation_time) >= obj->animation_end_frame) {
        obj->animation_time = (float)obj->animation_end_frame;
    }
    if (obj->animation_time >= (float)(count - 1)) {
        if ((obj->flags & MdkObject::FlagLoop) != 0) {
            if (obj->animation_time >= (float)count) {
                obj->animation_time -= (float)count;
            }
        } else {
            obj->animation_time = (float)(count - 1);
        }
    }
    /* @vfp-exempt R11 — the frame wrap's modulo by the animation's frame count, a
     * 1:1 port of the C#'s Round(AnimationTime) % count; the divisor is a small
     * positive per-animation constant, and a restructure would risk the pinned
     * frame sequence. measured n/a (corpus empty). Verified 2026-10-23. */
    int32_t frame = roundToEven(obj->animation_time) % count;
    if (frame != obj->animation_frame) {
        int32_t step = frame > obj->animation_frame ? frame - obj->animation_frame
                                                   : frame + count - obj->animation_frame;
        if ((obj->flags & MdkObject::FlagNoRootMotion) != 0) {
            step = 0;
        }
        for (int32_t i = 0; i < step; i++) {
            /* @vfp-exempt R11 — the passed-frames walk's modulo by the frame count,
             * a 1:1 port of the C#'s (AnimationFrame + 1 + i) % count; the divisor
             * is the same small positive constant as the wrap above.
             * measured n/a (corpus empty). Verified 2026-10-23. */
            size_t f = (size_t)((obj->animation_frame + 1 + i) % count);
            if (f < animation->rootMotion.size()) {
                const Vec3f& rm = animation->rootMotion[f];
                Vec3 turned = rotatedZ(Vec3{rm.x, rm.y, rm.z}, obj->yaw);
                /* @vfp-exempt R11 — root motion divided by the compile-time constant
                 * delta (1/30), matching the oracle's Push += motion/Dt bit-for-bit;
                 * not an integer divide. measured n/a. Verified 2026-10-23. */
                obj->push.x += turned.x / delta;
                obj->push.y += turned.y / delta;
                obj->push.z += turned.z / delta;
            }
        }
        obj->animation_frame = frame;
    }
    if (obj->animation_frame == count - 1 && (obj->flags & MdkObject::FlagLoop) == 0 &&
        obj->animation_frame != obj->animation_end_frame) {
        obj->animation_end_frame = MdkObject::AnimationEnded;
    }
}

// MdkObject.RestartAnimation: starts from frame 0 even when already playing.
void ScriptRuntime::restartAnimation(MdkObject* obj, const formats::ModelAnimation* animation,
                                     bool loop) {
    obj->animation = nullptr;
    playAnimation(obj, animation, loop);
}

// The door state bits (ObjectBehaviors): the low 4 are the engine's, the rest the
// scripts' (door_set_flags writes only them).
static const int32_t kDoorOpen = 0x1;
static const int32_t kDoorOpening = 0x2;
static const int32_t kDoorClosing = 0x4;
static const int32_t kDoorClosed = 0x8;
static const int32_t kDoorOpenNotSolid = 0x10;
static const int32_t kDoorStaysOpen = 0x20;
static const int32_t kDoorLocked = 0x40;
static const int32_t kDoorLockHidden = 0x100;
static const int32_t kDoorScriptBits = 0xF0;
static const int32_t kDoorOpenKept = 0x70;

// MdkObject.IsAnimationDone, as the runtime reads it (the VM has its own copy).
static bool animationDone(const MdkObject* obj) {
    return obj->animation == nullptr || obj->animation_end_frame == MdkObject::AnimationEnded;
}

// ObjectBehaviors.MoveDoor (0x43ca00): a door seen from its other side turns around,
// so it stays live, solid and drawn in Kurt's arena after the arena behind it goes.
static void moveDoor(MdkObject* obj) {
    std::string arena = obj->arena;
    obj->arena = obj->connects;
    obj->connects = arena;
    obj->yaw += 180.0f;
    if (obj->yaw >= 360.0f) {
        obj->yaw -= 360.0f;
    }
}

// ObjectBehaviors.ShowBehind: the arena the door connects to — or its own, when it
// connects to nothing — shows when the door opens.
void ScriptRuntime::showBehindDoor(MdkObject* obj) {
    for (int side = 0; side < 2; side++) {
        const std::string& arena = side == 0 ? obj->connects : obj->arena;
        if (arena.empty() || arena == currentArena_ ||
            (secondActive_ && arena == secondArena_)) {
            continue;
        }
        showArena(arena);
        return;
    }
}

// ObjectBehaviors.SetupDoor (spawn_connector): new doors are closed (0x45cdec), and
// the masks of their LOCK and HC... parts are found by name.
void ScriptRuntime::setupDoor(MdkObject* obj) {
    obj->door_state = kDoorClosed;
    obj->lock_parts = 0;
    obj->hatch_parts = 0;
    if (obj->model == nullptr) {
        return;
    }
    const std::vector<formats::Model::Part>& parts = obj->model->partList;
    for (size_t i = 0; i < parts.size(); i++) {
        const std::string name = parts[i].nameString();
        if (name == "LOCK") {
            obj->lock_parts |= (int32_t)(1 << i);
        } else if (name.size() >= 2 && name[0] == 'H' && name[1] == 'C') {
            obj->hatch_parts |= (int32_t)(1 << i);
        }
    }
}

// ObjectBehaviors.UpdateDoor: opens when Kurt comes closer than its distance, closes
// when he goes away, and plays the configured door sounds at the transitions.
void ScriptRuntime::updateDoor(MdkObject* obj) {
    if (obj->arena != currentArena_ && obj->connects == currentArena_) {
        moveDoor(obj);
    }

    int32_t state = obj->door_state;
    // ObjectBehaviors.Play: a door sound is played at the door unless it is
    // empty or the explicit "NONE" name.
    const auto play = [this, obj](const std::string& name) {
        if (!name.empty() && name != "NONE") {
            playSound(obj, name, 0, formats::ScriptValue());
        }
    };
    if ((state & kDoorOpening) != 0) {
        if (animationDone(obj)) {
            state = (state & kDoorOpenKept) | kDoorOpen;
            play(obj->door_sounds[2]);
        }
    } else if ((state & kDoorClosing) != 0 && animationDone(obj)) {
        state = (state & kDoorScriptBits) | kDoorClosed;
        play(obj->door_sounds[3]);
        showArena("");  // the arena behind it goes
    }

    const float dx = kurtPosition_.x - obj->position.x;
    const float dy = kurtPosition_.y - obj->position.y;
    const float dz = kurtPosition_.z - obj->position.z;
    if (sqrtf(dx * dx + dy * dy + dz * dz) >= obj->door_distance) {
        if ((state & (kDoorClosing | kDoorClosed | kDoorStaysOpen)) == 0) {
            restartAnimation(obj, getAnimation(obj, obj->door_animations[1]), false);
            state = (state & kDoorScriptBits) | kDoorClosing;
            play(obj->door_sounds[1]);
        }
    } else if ((state & (kDoorOpen | kDoorOpening | kDoorLocked)) == 0) {
        restartAnimation(obj, getAnimation(obj, obj->door_animations[0]), false);
        state = (state & kDoorScriptBits) | kDoorOpening;
        play(obj->door_sounds[0]);
        showBehindDoor(obj);
    }
    obj->door_state = state;

    // LOCK parts show a locked door; HC... parts hide while it's closed.
    int32_t hidden = obj->hidden_parts;
    if ((state & kDoorClosed) == 0) {
        hidden = (hidden | obj->lock_parts) & ~obj->hatch_parts;
    } else {
        hidden |= obj->hatch_parts;
        if ((state & kDoorLocked) != 0 && (state & kDoorLockHidden) == 0) {
            hidden &= ~obj->lock_parts;
        } else {
            hidden |= obj->lock_parts;
        }
    }
    obj->hidden_parts = hidden;
    if ((state & kDoorOpenNotSolid) == 0) {
        return;
    }
    obj->flags = (state & kDoorOpen) != 0 ? (obj->flags | MdkObject::FlagNotSolid)
                                         : (obj->flags & ~MdkObject::FlagNotSolid);
}

// GetWorldBounds: the pose box's eight corners, scaled, turned by yaw, lifted, and
// placed at the position. An object with no model is a point box at its position.
formats::Box ScriptRuntime::worldBounds(const MdkObject* obj) {
    if (obj->model == nullptr) {
        formats::Box box;
        box.min = Vec3f{obj->position.x, obj->position.y, obj->position.z};
        box.max = box.min;
        return box;
    }
    return worldBounds(obj, poseBounds(obj));
}

// GetWorldBounds(obj, bounds): the same corner transform on a caller-supplied box
// (a part's box from partBounds), not the object's pose bounds. The C# skips the
// no-model point-box branch when bounds is given (ScriptRuntime.cs:3120), and a
// part box only exists when there is a model, so this path always has one.
formats::Box ScriptRuntime::worldBounds(const MdkObject* obj, const formats::Box& box) {
    formats::Box world;
    bool have = false;
    for (int i = 0; i < 8; i++) {
        Vec3f corner{(i & 1) != 0 ? box.max.x : box.min.x, (i & 2) != 0 ? box.max.y : box.min.y,
                     (i & 4) != 0 ? box.max.z : box.min.z};
        Vec3 turned = rotatedZ(Vec3{corner.x * obj->scale, corner.y * obj->scale, corner.z * obj->scale},
                               obj->yaw);
        Vec3 point;
        point.x = obj->position.x + turned.x;
        point.y = obj->position.y + turned.y;
        point.z = obj->position.z + obj->lift() + turned.z;
        if (!have) {
            world.min = Vec3f{point.x, point.y, point.z};
            world.max = world.min;
            have = true;
        } else {
            if (point.x < world.min.x) world.min.x = point.x;
            if (point.y < world.min.y) world.min.y = point.y;
            if (point.z < world.min.z) world.min.z = point.z;
            if (point.x > world.max.x) world.max.x = point.x;
            if (point.y > world.max.y) world.max.y = point.y;
            if (point.z > world.max.z) world.max.z = point.z;
        }
    }
    return world;
}

// PartBounds: each part's box in the current pose, from the cached pose_parts().
// An empty part is invalid (the C# null). Same pose and per-part min/max as
// poseBounds, but every part is returned -- hidden parts included -- because the C#
// PartBounds has no HiddenParts filter; callers apply the hidden_parts bit.
std::vector<ScriptRuntime::PartBox> ScriptRuntime::partBounds(const MdkObject* obj) {
    std::vector<PartBox> result;
    if (obj->model == nullptr) {
        return result;  // C# PartBounds returns [] with no model
    }
    const std::vector<std::vector<Vec3f> >& pose = obj->pose_parts();
    result.resize(pose.size());
    for (size_t i = 0; i < pose.size(); ++i) {
        if (pose[i].empty()) {
            result[i].valid = false;
            continue;
        }
        Vec3f mn = pose[i][0];
        Vec3f mx = pose[i][0];
        for (size_t v = 1; v < pose[i].size(); ++v) {
            const Vec3f& p = pose[i][v];
            if (p.x < mn.x) mn.x = p.x;
            if (p.y < mn.y) mn.y = p.y;
            if (p.z < mn.z) mn.z = p.z;
            if (p.x > mx.x) mx.x = p.x;
            if (p.y > mx.y) mx.y = p.y;
            if (p.z > mx.z) mx.z = p.z;
        }
        result[i].valid = true;
        result[i].box.min = mn;
        result[i].box.max = mx;
    }
    return result;
}

// IsWeakPart: the part name's character at weak_prefix_length is an ASCII digit and
// the name starts with the weak prefix (ordinal). The C# char.IsAsciiDigit is the
// '0'..'9' range only, not isdigit's locale set.
bool ScriptRuntime::isWeakPart(const MdkObject* obj, int32_t index) {
    // The C# indexes PartList[index] and name[length] with a managed bounds check
    // that throws; the port is -fno-exceptions, so guard the raw operands (index and
    // weak_prefix_length come from opcode 198's int operands). No valid caller is
    // out of range (the C# loops i < PartList.Count), so this changes no in-contract
    // behaviour -- it turns a would-be throw into "not a weak part" instead of UB.
    if (index < 0 || static_cast<size_t>(index) >= obj->model->partList.size()) {
        return false;
    }
    const std::string& name = obj->model->partList[static_cast<size_t>(index)].nameString();
    const int32_t length = obj->weak_prefix_length;
    if (length < 0 || static_cast<int32_t>(name.size()) <= length) {
        return false;
    }
    const char c = name[static_cast<size_t>(length)];
    if (c < '0' || c > '9') {
        return false;
    }
    const std::string& prefix = obj->weak_prefix;
    const size_t plen = std::min(static_cast<size_t>(length), prefix.size());
    return name.compare(0, plen, prefix, 0, plen) == 0;
}


// GetKurtBox: half width 0.6 in XY, height 5 from his feet.
formats::Box ScriptRuntime::kurtBox() const {
    const float kKurtHalfWidth = 0.6f;
    const float kKurtHeight = 5.0f;
    formats::Box box;
    box.min = Vec3f{kurtPosition_.x - kKurtHalfWidth, kurtPosition_.y - kKurtHalfWidth, kurtPosition_.z};
    box.max = Vec3f{kurtPosition_.x + kKurtHalfWidth, kurtPosition_.y + kKurtHalfWidth,
                    kurtPosition_.z + kKurtHeight};
    return box;
}

// The one Kurt health, seen from the scripts' side. With a Player bound this is
// the entity's own combat half — the C# hands its single Kurt to the runtime
// (ScriptRuntime.cs:168) and every writer (hurt_kurt, the hard landing) and every
// reader (if_player_health, the death transition, the HUD) goes through it. Out of
// line because the seam needs the complete Player.
const Kurt& ScriptRuntime::kurt() const { return player_ ? player_->combat() : unboundKurt_; }

Kurt& ScriptRuntime::kurt() { return player_ ? player_->combat() : unboundKurt_; }

// C# HurtKurt (ScriptRuntime.cs:2676, 0x46a498): riding the XD2 or the XE the
// ridden object takes the hits (and dies at 0 unless indestructible); on foot or
// on the snowboard Kurt does. Rides.TakesHits is Ridden != null && no board
// (scripts/rides.h, package #9).
void ScriptRuntime::hurtKurt(int32_t damage) {
    if (!rides_.takesHits()) {
        kurt().hurt(damage);
        return;
    }

    MdkObject* ridden = rides_.ridden();
    if (ridden->health >= kIndestructible) {
        return;
    }

    ridden->health -= damage;
    if (ridden->health < 1) {
        kill(ridden);
    }
}

// RaiseShake (ScriptRuntime.cs:1986, 0x467f7c): keep the strongest request
// made during this tick. MathF.Max propagates NaN; mathMax preserves that.
// Then the C#'s ShakeRaised event with the amount asked, which the follow camera
// takes (Viewer.cs:238: FollowCamera.RaiseShake; package #15).
void ScriptRuntime::raiseShake(float amount) {
    shake = mathMax(shake, amount);
    if (shakeRaisedHook_ != nullptr) {
        shakeRaisedHook_(shakeRaisedUser_, amount);
    }
}

// C# FlattenTown (ScriptRuntime.cs:2139-2154, package #15): the minecrawler reached the
// town -- the screen shakes and a message says which town is gone (OOT_L1, "There goes
// Laguna Beach!", or OOT_L1A for the second town), and the global flags record which one
// for the debriefing (GameStats.TownFlags, copied when the level ends). The C# raises no
// GameOver: the level goes on.
void ScriptRuntime::flattenTown() {
    raiseShake(kTownShake);
    std::string text = "OOT_L" + std::to_string(flow::indexOf(level_->number()) + 1);
    if ((globalFlags & kFlagSecondTown) != 0) {
        text += "A";
        globalFlags |= kFlagSecondTownFlattened;
    } else {
        globalFlags |= kFlagTownFlattened;
    }
    // Messages?.Push(text, FlagZoom | FlagFront, TownMessageSeconds).
    pushMessage(text, Messages::kFlagZoom | Messages::kFlagFront, kTownMessageSeconds);
}

// C# SpecialEvent (ScriptRuntime.cs:1007-1076, opcode 131 special_event, 0x4456d2): 50 and
// below end the level; above 50 are the cutscenes (0x477cf4). Every retail call above 50 is
// an ARENA CONTROLLER's (L5 MUSE_5: 51-55, 61, 81; L7 DANT_10: 92, 93; L8 GUNT_10: 91, 92),
// so `obj` -- the target of 51, 81, 91 and 93 -- is that controller. A value the C#'s switch
// has no case for (54, the L5 script's own) is the C#'s no-op, and handled.
bool ScriptRuntime::specialEvent(MdkObject* obj, int32_t value) {
    if (value <= kLastEndEvent) {
        endTheLevel();
        return true;
    }
    switch (value) {
        case 51: {
            // Kurt strikes (0x4779e0): X_STRIKD full screen (0x4398f0), then he's put at the
            // object, which moves 4 units along y.
            // The scene is its own object: the frontend waits on it (package 14).
            playStrikeScene(StrikeScene::Kurt, StrikeScene::WithPilot);
            if (player_ != nullptr) {
                player_->teleport(obj->position, obj->yaw);
            }
            obj->position.y += 4.0f;
            startCutscene(kCutsceneStrike, obj);
            cameraDistance = 10.0f;
            // obj.Position - new Vector3(10f, 4f, -8f), component by component.
            cameraPosition = Vec3{obj->position.x - 10.0f, obj->position.y - 4.0f,
                                  obj->position.z - -8.0f};
            cameraPitch = 0.0f;
            cameraYaw = 90.0f - obj->yaw;
            return true;
        }
        case 52: {
            MdkObject* dog = findObjectNamed("XBN");
            if (dog != nullptr) {
                startCutscene(kCutsceneDog, dog);
                cameraMode = 0;
            }
            return true;
        }
        case 53:
        case 92:
            endCutscene();
            return true;
        case 55:
            cameraMode = 2;
            return true;
        case 61: {
            MdkObject* gunter = findObjectNamed("XGUNTAM");
            if (gunter != nullptr) {
                startCutscene(kCutsceneAll, gunter);
                cameraMode = 11;
                // FromAngle(gunter.Yaw) * 45f, then Position + (heading.X, heading.Y, 3f).
                const Vec3 facing = fromAngle(gunter->yaw);
                const float hx = facing.x * 45.0f;
                const float hy = facing.y * 45.0f;
                cameraPosition = Vec3{gunter->position.x + hx, gunter->position.y + hy,
                                      gunter->position.z + 3.0f};
                cameraPitch = -20.0f;
                cameraYaw = 270.0f - gunter->yaw;
                cameraDistance = 45.0f;
            }
            return true;
        }
        case 81:
            // The end of the game: MISC/FLIC/MDKEND.FLC and MDKBZK.MVE (game state 8). The
            // movies and the credits are the frontends' (GameFinished, Flow/Game.cs:458).
            startCutscene(kCutsceneEnd, obj);
            gameFinished_ = true;
            return true;
        case 91:
            startCutscene(kCutsceneAllFlagged, obj);
            cameraMode = 12;
            cameraPosition = Vec3{-121.0f, 3347.0f, -350.0f};
            return true;
        case 93:
            startCutscene(kCutsceneBoss, obj);
            cameraMode = 12;
            cameraPosition = Vec3{1158.0f, 5006.0f, 315.0f};
            return true;
        default:
            return true;
    }
}

// C# FindObjectNamed (ScriptRuntime.cs:1338): Objects.FirstOrDefault(o => !o.Dead &&
// o.TypeName == type). A removed object is dead (removeObject sets it first), so the deque's
// order with the dead skipped is the C# list's.
MdkObject* ScriptRuntime::findObjectNamed(const std::string& type) {
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        if (!it->dead && it->type_name == type) {
            return &(*it);
        }
    }
    return nullptr;
}

// C# StartCutscene (ScriptRuntime.cs:1341-1347): Kurt stops firing (0x4779b0) and stands
// still; the camera looks at the target from the next tick (cutsceneTick).
void ScriptRuntime::startCutscene(int32_t state, MdkObject* target) {
    cutscene = state;
    cutsceneTarget = target;
    // Kurt.StopFiring, as endTheLevel spells it: with no Player bound there is no gun, voice
    // or muzzle to stop, only the combat half's flag.
    if (player_ != nullptr) {
        player_->stopFiring();
        player_->frozen = true;
    } else {
        kurt().setFiring(false);
        kurt().clearMuzzle();
    }
}

// C# EndCutscene (ScriptRuntime.cs:1349-1359): every object is drawn again, and Kurt moves
// and shows. The shot (cameraMode ..) is left as it was, as in the C#.
void ScriptRuntime::endCutscene() {
    cutscene = 0;
    cutsceneTarget = nullptr;
    if (player_ != nullptr) {
        player_->frozen = false;
        player_->visible = true;
    }
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        it->visible = true;
    }
}

// C# CutsceneTick (ScriptRuntime.cs:1363-1410, 0x478704): a tick during a cutscene. Only
// Kurt's arena's objects are considered; below kCutsceneAll only the cutscene types run
// (BOLT and BIGBOLT unseen); from it on the objects that are not pickups or Kurt's items run
// and show (kCutsceneAllFlagged runs those too, unseen); the end of the game runs and shows
// nothing. Then the camera takes its shot of the target, unless it died.
void ScriptRuntime::cutsceneTick() {
    // C# CutsceneTypes (0x47868c).
    static const char* const kCutsceneTypes[] = {"XM5_FLAP", "XBN",      "BOLT",
                                                 "BIGBOLT",  "SW_SBONE", "SW_SEAL"};
    if (player_ != nullptr) {
        player_->visible = cutscene == kCutsceneStrike || cutscene == kCutsceneEnd;
    }
    // ListCopy<MdkObject>.Of(Objects): what this tick spawns waits for the next.
    std::vector<MdkObject*> snapshot;
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        snapshot.push_back(&(*it));
    }
    for (size_t i = 0; i < snapshot.size(); i++) {
        MdkObject* obj = snapshot[i];
        if (obj->dead || obj->arena != currentArena_) {
            continue;
        }
        const bool flagged = (obj->flags & kCutsceneHiddenFlags) != 0 || obj->thrown_kind > 0;
        bool runs;
        bool shown;
        if (cutscene == kCutsceneEnd) {
            runs = false;
            shown = false;
        } else if (cutscene < kCutsceneAll) {
            runs = false;
            for (size_t t = 0; t < sizeof(kCutsceneTypes) / sizeof(kCutsceneTypes[0]); t++) {
                if (obj->type_name == kCutsceneTypes[t]) {
                    runs = true;
                    break;
                }
            }
            shown = runs && obj->type_name != "BOLT" && obj->type_name != "BIGBOLT";
        } else {
            runs = cutscene == kCutsceneAllFlagged || !flagged;
            shown = !flagged;
        }
        obj->visible = shown;
        if (!runs) {
            continue;
        }
        vm_.run(obj);
        if (!obj->dead) {
            motionUpdate(obj);
        }
    }
    // A script that ran above may have ended the cutscene (53, 92): no shot then.
    if (cutscene != 0 && cutsceneTarget != nullptr && !cutsceneTarget->dead) {
        updateCutsceneCamera();
    }
}

// C# UpdateCutsceneCamera (ScriptRuntime.cs:1415-1492, 0x477d94). Every shot but 11 looks at
// the target, 3 units above its origin. Shot 0 starts from (423, 85) and switches to 1 once
// the target is 12 units away; 1 and 2 orbit behind the target (12 or 25 units), easing in
// for kCameraBlendTicks; 12 looks from the stored position; 11 holds it all.
//
// Gameplay-exact arithmetic, operation for operation (the golden pins every bit): the C#'s
// Vector3 operators are per component and unfused, Vector3.Distance is Sqrt(Dot(d, d)) with
// Dot summing x, y, then z, and heading()/fromAngle()/atan2f are the libm calls that match
// .NET's MathF bit for bit here (see heading's note). It runs once per tick, not per object,
// so it is not a hot path (AGENTS.md section 2).
void ScriptRuntime::updateCutsceneCamera() {
    const float kHalfTurn = 180.0f;
    const float kFullTurn = 360.0f;
    const float kMotionTicks = 1.0f;  // C# ObjectMotion.Ticks
    const Vec3 target = cutsceneTarget->position;
    Vec3 point = cameraPosition;
    float yaw = cameraYaw;
    float distance = cameraDistance;
    switch (cameraMode) {
        case 0: {
            point = Vec3{423.0f, 85.0f, mathMax(target.z - 25.0f, -2260.0f)};
            yaw = 90.0f - heading(point, target);
            const float dx = point.x - target.x;
            const float dy = point.y - target.y;
            const float dz = point.z - target.z;
            distance = sqrtf((dx * dx + dy * dy) + dz * dz);
            if (distance >= 12.0f) {
                cameraMode = 1;
            }
            cameraTimer = kCameraBlendTicks;
            break;
        }
        case 1:
        case 2: {
            distance = (cameraMode == 1 ? 12.0f : 25.0f) * 0.1f + cameraDistance * 0.9f;
            const Vec3 facing = fromAngle(cutsceneTarget->yaw + 150.0f);
            const float aroundX = facing.x * distance;
            const float aroundY = facing.y * distance;
            point = Vec3{target.x + aroundX, target.y + aroundY, -2258.0f};
            if (cameraMode == 2) {
                point.z = mathMin(cameraPosition.z + kMotionTicks, -2246.0f);
            }
            if (cameraTimer > 0) {
                point = Vec3{point.x * 0.2f + cameraPosition.x * 0.8f,
                             point.y * 0.2f + cameraPosition.y * 0.8f,
                             point.z * 0.2f + cameraPosition.z * 0.8f};
            }
            yaw = 120.0f - cutsceneTarget->yaw;
            break;
        }
        case 12: {
            yaw = 90.0f - heading(point, target);
            const float dx = point.x - target.x;
            const float dy = point.y - target.y;
            const float dz = point.z - target.z;
            distance = sqrtf((dx * dx + dy * dy) + dz * dz);
            cameraTimer = kCameraBlendTicks;
            break;
        }
        default:
            break;
    }

    float pitch = cameraPitch;
    if (cameraMode != 11) {
        /* @vfp-exempt R6 — the cutscene shot's pitch, once a tick; glibc atan2f matches the
         * C# oracle's MathF.Atan2 bit-for-bit on this machine (as heading's pin records),
         * and golden/L5_cut_*.txt compare the pitch's bits, which the fastAtan2 LUT would
         * move. measured n/a (corpus empty). Verified 2026-11-20. */
        pitch = -radiansToDegrees(atan2f(target.z + 3.0f - point.z, distance));
        if (pitch < -kHalfTurn) {
            pitch += kFullTurn;
        }
    }

    if (cameraMode == 1 || cameraMode == 2) {
        if (cameraTimer > 0) {
            pitch = pitch * 0.2f + cameraPitch * 0.8f;
            yaw = yaw * 0.2f + cameraYaw * 0.8f;
            cameraTimer--;
        } else {
            pitch = pitch * 0.7f + cameraPitch * 0.3f;
            yaw = yaw * 0.3f + cameraYaw * 0.7f;
        }
    }

    cameraDistance = distance;
    cameraPitch = pitch;
    cameraYaw = yaw;
    if (cutscene != kCutsceneBoss) {
        cameraPosition = point;
    }
    cameraPoint = point;
}

// C# EndTheLevel (ScriptRuntime.cs:1496-1511): once (LevelOver, 0x573b60), the town flags
// are kept for the debriefing, Kurt stops firing, NUKE and TORNADO play, and the arena
// breaks up around him as he rises (EndLevel.Start, then its StartShake).
void ScriptRuntime::endTheLevel() {
    if (levelOver) {
        return;
    }
    levelOver = true;
    stats.townFlags = globalFlags;
    // Kurt.StopFiring. With no Player bound there is no gun, no voice and no muzzle to
    // stop, so the combat half's flag is all there is.
    if (player_ != nullptr) {
        player_->stopFiring();
    } else {
        kurt().setFiring(false);
        kurt().clearMuzzle();
    }
    mixer_.play("NUKE");
    mixer_.play("TORNADO");
    // _sprites.GetAnimation(EndLevel.Takeoff).FrameCount: K_TAKEOF's frames (the C#
    // throws on an archive without it; here the pose is K_FLOATC from the first tick).
    int32_t takeoffFrames = 0;
    formats::SpriteAnimation takeoff;
    if (sprites_ != nullptr && sprites_->has(EndLevel::kTakeoff) &&
        sprites_->animation(EndLevel::kTakeoff, &takeoff)) {
        takeoffFrames = takeoff.frameCount();
    }
    endLevel_.reset(new EndLevel(takeoffFrames));
    endLevel_->start(level_, currentArena_, player_, &rng_);
    raiseShake(EndLevel::kStartShake);
}

// CameraTrack (ScriptRuntime.cs:1188-1216, opcode 203): calculate the pitch
// the follow camera should ease towards while Kurt faces a tall object. The
// camera is presentation, so its two atan2 calls use the shared fastAtan2 LUT
// on both targets (R6); the table's angular error is far below one pixel of
// camera pitch.
void ScriptRuntime::cameraTrack(MdkObject* obj, int32_t mode, float height) {
    const float maxUp = -30.0f;
    const float halfTurn = 180.0f;
    const float fullTurn = 360.0f;

    const float heading = radiansToDegrees(
        fastAtan2(obj->position.y - kurtPosition_.y, obj->position.x - kurtPosition_.x));
    float off = wrap360(kurtYaw_ - heading);
    if (off > halfTurn) {
        off = fullTurn - off;
    }

    float top = obj->position.z + height * obj->scale;
    if (mode == 0) {
        float boundsTop = obj->position.z;
        if (obj->model != nullptr) {
            boundsTop += obj->model->bounds.max.z * obj->scale;
        }
        top = 0.3f * obj->position.z + 0.7f * boundsTop;
    }

    const float rest = level_->arenaPitch(currentArena_);
    float target = rest;
    if (off <= 90.0f && top >= kurtPosition_.z) {
        const float dx = obj->position.x - kurtPosition_.x;
        const float dy = obj->position.y - kurtPosition_.y;
        const float distance = sqrtf(dx * dx + dy * dy);
        const float angle = radiansToDegrees(fastAtan2(top - kurtPosition_.z, distance));
        target = clampf(-angle * (120.0f - off) / 120.0f, maxUp, rest);
    }

    cameraTrackPitch = target;
    cameraTrackTicks = 2;
}

void ScriptRuntime::placeNearKurt(MdkObject* obj, float xMin, float xMax, float yLimit) {
    const float kLead = 2.67333f;
    const int32_t kTries = 100;
    const float kSpacing = 12.0f;

    // PlaceNearKurt (ScriptRuntime.cs:1139-1186). The first draw chooses the
    // branch; the random branch alone consumes a second draw. The operation
    // order and float literals are kept as in the oracle.
    const int32_t choice = rng_.next(100);
    float x;
    if (choice < 25 || kurtPosition_.x < xMin || kurtPosition_.x > xMax ||
        kurtPosition_.y > yLimit) {
        x = xMin + (xMax - xMin) * rng_.next(10000) * 0.0001f;
    } else if (choice < 50) {
        x = kurtPosition_.x;
    } else {
        x = kurtPosition_.x + kurtVelocity.x * kLead;
    }

    if (x < xMin) {
        x += xMax - xMin;
    }

    for (int32_t attempt = 0; attempt < kTries; attempt++) {
        bool moved = false;
        for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
            MdkObject& other = *it;
            // The C# iterates its live Objects list. The C++ deque keeps
            // removed objects in stable slots, but remove() sets dead on
            // them, so the dead test IS the list membership.
            if (&other == obj || other.dead || other.arena != obj->arena ||
                other.position.y != obj->position.y ||
                fabsf(x - other.position.x) >= kSpacing - 0.5f) {
                continue;
            }

            x = other.position.x - kSpacing;
            moved = true;
        }

        if (!moved) {
            break;
        }
    }

    obj->position.x = x;
}

// GunAim (ScriptRuntime.cs:1084-1134, opcode 219): the shooting galleries'
// guns (level 6) only fire along -y (270 degrees +/- 3) and lead Kurt by the
// 1.67 s their shot takes (sideways only). Faster sideways movement makes
// them likelier to fire at him; otherwise they aim at a raised target (the
// objects within 2 of y and 3 of z in their cone), then at Kurt anyway, and
// else face 270 degrees and fire 29% of the time. Sets the yaw; returns
// whether to fire.
bool ScriptRuntime::gunAim(MdkObject* obj, float y, float z) {
    const float kLead = 1.67333f;
    const float kDown = 270.0f;
    const float kCone = 3.0f;
    const int32_t kMaxTargets = 4;

    // The C#'s lead point: Kurt's X advanced by the observed sideways
    // velocity over the shot's flight time, at Kurt's Y.
    Vec3 lead{kurtPosition_.x + kurtVelocity.x * kLead, kurtPosition_.y, 0.0f};
    // The C#'s `obj.YawTo(lead) + Rng.Next(500) * 0.001f - 0.25f`: the aim
    // jitter of up to a quarter of a degree. The draw order matters, so the
    // multiplication stays where the oracle has it.
    float angle = yawTo(obj, lead) + rng_.next(500) * 0.001f - 0.25f;
    const bool aimed = fabsf(angle - kDown) <= kCone;

    // ScriptMath.Round is MathF.Round(AwayFromZero), which C's roundf is
    // (unlike the banker's round the animation clock uses).
    if (aimed && rng_.next(70) < (int32_t)roundf(fabsf(kurtVelocity.x)) + 10) {
        obj->yaw = angle;
        return true;
    }

    // The raised targets: the live objects of the gun's arena within 2 in Y
    // and 3 in Z of the target row whose bearing from the gun is inside the
    // cone. The C# collects at most four and picks one of them at random.
    float angles[kMaxTargets];
    int32_t count = 0;
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        MdkObject& other = *it;
        // The C# iterates its live Objects list. The C++ deque keeps removed
        // objects in stable slots, but remove() sets dead on them, so the
        // dead test IS the list membership.
        if (other.dead || other.arena != obj->arena ||
            fabsf(other.position.y - y) > 2.0f || fabsf(other.position.z - z) > 3.0f) {
            continue;
        }

        const float otherAngle = yawTo(obj, other.position);
        if (fabsf(otherAngle - kDown) > kCone) {
            continue;
        }

        angles[count] = otherAngle;
        count++;
        if (count == kMaxTargets) {
            break;
        }
    }

    if (count != 0) {
        obj->yaw = angles[rng_.next(count)];
        return true;
    }

    if (aimed) {
        obj->yaw = angle;
        return true;
    }

    obj->yaw = kDown;
    return rng_.next(100) > 70;
}

// A numeric operand as a float (the C#'s ScriptMath.F).
static float operandFloat(const formats::ScriptValue& v) {
    if (v.kind == formats::ScriptValueKind::Float) {
        return v.floatValue;
    }
    if (v.kind == formats::ScriptValueKind::Int) {
        return (float)v.intValue;
    }
    return 0.0f;
}

// An operand by index, or the null one past the end. The C# would throw
// IndexOutOfRangeException on a short list; the port reads a missing operand as the
// null one, which is what ScriptMath.F and ScriptMath.I already answer 0 for, so a
// malformed script stops the opcode instead of the level.
static const formats::ScriptValue& operandAt(const std::vector<formats::ScriptValue>& args,
                                             size_t index) {
    static const formats::ScriptValue kNull;
    return index < args.size() ? args[index] : kNull;
}

// push_kurt (opcode 248, ScriptRuntime.cs:2826-2852, dispatched at ScriptVm.cs:1600):
// knock Kurt down and shove him. It is the ONLY writer of a non-zero knock-down push in
// the whole oracle. The operand list comes in whole because its arity depends on args[0]:
//   [0, a, b, up]  the push is (a, b) turned into the world by the OBJECT's yaw, and
//                  the lift comes from args[3];
//   [n, a, up]     the push points straight away from the object in XY at a u/s, and
//                  the lift is args[2].
void ScriptRuntime::pushKurt(MdkObject* obj, const std::vector<formats::ScriptValue>& args) {
    // The guard, in the C#'s order: a Kurt already on his back is not knocked down
    // again, and a corpse is not shoved. Kurt.Knocked is Knocked or GetUp
    // (Kurt.Combat.cs:102), so the whole animation is the refused window.
    if (kurt().knocked() || kurt().health() == 0) {
        return;
    }
    // The C# cannot reach here without a Kurt: its constructor requires one
    // (ScriptRuntime.cs:168). With no Player bound there is no entity to shove, and
    // writing only the combat half would invent a Kurt who is knocked but cannot move.
    if (player_ == nullptr) {
        return;
    }

    const Vec3 origin = obj != nullptr ? obj->position : Vec3{};
    Vec2 push;
    float up = 0.0f;
    if (operandInt(operandAt(args, 0)) == 0) {
        // fromAngle supplies (cos, sin); preserve the C#'s asymmetric expression.
        const Vec3 heading = fromAngle(obj != nullptr ? obj->yaw : 0.0f);
        const float c = heading.x;
        const float s = heading.y;
        const float a = operandFloat(operandAt(args, 1));
        const float b = operandFloat(operandAt(args, 2));
        push.x = -a * c - b * s;
        push.y = -b * c - a * s;
        up = operandFloat(operandAt(args, 3));
    } else {
        const float a = operandFloat(operandAt(args, 1));
        // C# reads its per-tick KurtPosition snapshot, not live Kurt.Feet.
        const float awayX = kurtPosition_.x - origin.x;
        const float awayY = kurtPosition_.y - origin.y;
        if (awayX != 0.0f || awayY != 0.0f) {
            const float length = sqrtf(awayX * awayX + awayY * awayY);
            // System.Numerics.Vector2.Normalize divides each component before the
            // following multiplication by a; keep that rounding order.
            push.x = awayX / length * a;
            push.y = awayY / length * a;
        } else {
            // C# fallback when the object and Kurt share an XY position.
            push.x = a;
            push.y = 0.0f;
        }
        up = operandFloat(operandAt(args, 2));
    }

    // C# scales by Tick * TicksPerSecond (1/30 * 30), retained in the same order.
    push.x *= kTick * kTicksPerSecond;
    push.y *= kTick * kTicksPerSecond;
    player_->knockDown(push);
    // Kurt.AddVerticalSpeed is a plain += on the movement field.
    player_->verticalSpeed += up;
}

// play_sound (opcode 89, 0x442402). flags & 3 is the start mode — play, restart,
// play unless it is already playing, stop — and the place comes from the rest:
// 0x80 without position, 0x10 following the object at an offset, 0x20 at a
// reference point, 0x40 at a point, none of them where the object is. Flag 4
// makes the sound the object's TRACKED sound, the one if_own_sound (249) asks
// about, and plays it at the object, following it.
void ScriptRuntime::playSound(MdkObject* obj, const std::string& name, int32_t flags,
                              const formats::ScriptValue& position) {
    const int32_t kStopMode = 3;
    const int32_t kTracked = 4;
    const int32_t kFollowing = 0x10;
    const int32_t kAtReference = 0x20;
    const int32_t kAtPoint = 0x40;
    const int32_t kFlat = 0x80;
    // SoundStarts (ScriptRuntime.cs:3257): the mode is the index. Mode 3 never
    // indexes it — the C# stops (3D) or does nothing (2D) before reading it.
    static const int32_t kSoundStarts[3] = {audio::SoundMixer::New, audio::SoundMixer::Restart,
                                            audio::SoundMixer::Once};

    const int32_t mode = flags & 3;
    if ((flags & kFlat) != 0) {
        if (mode != kStopMode) {
            mixer_.play(name, kSoundStarts[mode]);
        }
        return;
    }
    if (mode == kStopMode) {
        mixer_.stop(name);
        return;
    }
    const int32_t start = kSoundStarts[mode];

    // The point operand: three floats of a list (the C#'s V(list)), or the zero
    // vector when the operand is not a list.
    Vec3 point;
    if (position.kind == formats::ScriptValueKind::List) {
        const std::vector<formats::ScriptValue>& items = position.items;
        point.x = items.size() > 0 ? operandFloat(items[0]) : 0.0f;
        point.y = items.size() > 1 ? operandFloat(items[1]) : 0.0f;
        point.z = items.size() > 2 ? operandFloat(items[2]) : 0.0f;
    }

    int32_t voice = 0;
    if ((flags & kFollowing) != 0) {
        voice = playOn(obj, name, start, point);
    } else if ((flags & kAtReference) != 0) {
        voice = mixer_.playAt(name, obj->reference_point(operandInt(position)), start);
    } else if ((flags & kAtPoint) != 0) {
        voice = mixer_.playAt(name, point, start);
    } else if ((flags & kTracked) != 0) {
        voice = playOn(obj, name, start, Vec3());
    } else {
        voice = mixer_.playAt(name, obj->position, start);
    }

    if ((flags & kTracked) != 0) {
        obj->tracked_sound = name;
        obj->tracked_voice = voice;
    }
}

// set_loop_sound (opcode 107): start the object's looping sound, stopping the
// previous one; an empty name just stops it. It only loops if the sound itself
// does (its SNI flag) — the mixer plays the entry as stored.
void ScriptRuntime::setLoopSound(MdkObject* obj, const std::string& name) {
    mixer_.stopVoice(obj->loop_sound);
    obj->loop_sound = 0;
    obj->loop_sound_name = name;
    if (name.empty()) {
        return;
    }
    obj->loop_sound = playOn(obj, name, audio::SoundMixer::New, Vec3());
}

// if_sound_playing (opcode 233): the name is upper-cased before the search, as
// the C# does — the archives' keys are upper case, the scripts' are not always.
bool ScriptRuntime::isSoundPlaying(const std::string& name) const {
    return mixer_.isPlaying(formats::toUpperAscii(name));
}

// C# PlayOn (ScriptRuntime.cs:3317): a sound following an object at an offset in
// its frame, kept per object so Remove can stop it with the object. Ended voices
// are forgotten as the list is walked, and a voice of 0 (every voice busy) is
// kept too, exactly as the C# adds it.
int32_t ScriptRuntime::playOn(MdkObject* obj, const std::string& name, int32_t start,
                              const Vec3& offset) {
    const int32_t voice = mixer_.playOn(name, &obj->position, &obj->yaw, offset, start);
    std::vector<int32_t>& voices = following_[obj];
    for (size_t i = voices.size(); i-- > 0;) {
        if (!mixer_.isVoicePlaying(voices[i])) {
            voices.erase(voices.begin() + (long)i);
        }
    }
    voices.push_back(voice);
    return voice;
}

bool ScriptRuntime::sweepBox(MdkObject* obj, const Vec3& motion, Vec3* normal) {
    const float kWidthFactor = 0.25f;
    const float kMinHeight = 0.5f;
    const float kVerticalLift = 0.05f;
    const float kLift = 0.5f;
    Bsp* bsp = bspOf(obj->arena);
    if (bsp == nullptr) {
        obj->position.x += motion.x;
        obj->position.y += motion.y;
        obj->position.z += motion.z;
        return false;
    }
    Box bounds = poseBounds(obj);
    /* @vfp-exempt R6 — the footing sweep's yaw rotation of the collision box; glibc
     * sinf/cosf match the C# oracle's MathF.SinCos bit-for-bit on this machine
     * (probed), and the conversion is .NET's left-associative DegreesToRadians;
     * the fastSin/fastCos LUT's error is far larger and would move the box and its
     * landing. measured n/a (corpus empty). Verified 2026-10-23. */
    float s = fabsf(sinf(degreesToRadians(obj->yaw)));
    float c = fabsf(cosf(degreesToRadians(obj->yaw)));
    float sx = (bounds.max.x - bounds.min.x) * obj->scale;
    float sy = (bounds.max.y - bounds.min.y) * obj->scale;
    Vec half{(c * sx + s * sy) * kWidthFactor, (s * sx + c * sy) * kWidthFactor,
             // MathF.Max (ObjectMotion.cs:1138): a NaN lift makes the half-height NaN and
             // the sweep's centre NaN, which the oracle keeps; fmaxf would return the 0.5
             // floor and sweep an ordinary box instead. Measured on .NET 10, probe
             // `nanclamp`, row SWEEP nan_lift: level 7 (arena DANT_1) ends at 20,4,NaN and
             // level 3 (arena HMO_1) at 16,0,NaN — the Z is NaN in both, and the X/Y are
             // each level's own slide of the same step.
             mathMax(bounds.max.z * obj->scale + obj->lift(), kMinHeight) * 0.5f};
    Vec3 centre = rotatedZ(Vec3{(bounds.min.x + bounds.max.x) * 0.5f, (bounds.min.y + bounds.max.y) * 0.5f,
                                (bounds.min.z + bounds.max.z) * 0.5f},
                           obj->yaw);
    Vec centrep{centre.x * obj->scale, centre.y * obj->scale, centre.z * obj->scale};
    centrep.z = half.z + (motion.z != 0.0f ? kVerticalLift : kLift);
    if (motion.z != 0.0f) {
        float drop = obj->height_offset - obj->lift();
        half.z += drop * 0.5f;
        centrep.z -= drop * 0.5f;
    }
    Vec a{obj->position.x + centrep.x, obj->position.y + centrep.y, obj->position.z + centrep.z};
    Vec b{a.x + motion.x, a.y + motion.y, a.z + motion.z};
    Vec end{b.x, b.y, b.z};
    int node = -1;
    int firstNode = -1;
    int firstTri = Bsp::kNone;
    bsp->sweepBox(a, b, half, 2, 0.75f, &end, &node, &firstNode, &firstTri);
    obj->position.x += end.x - a.x;
    obj->position.y += end.y - a.y;
    obj->position.z += end.z - a.z;
    // Contact is the FIRST contact across the sweep passes (the C# Sweep returns its
    // first-contact normal), not the stopping pass: a slide that ends free still hit
    // a face, and an empty arena reports no contact at all (firstTri stays kNone).
    bool contacted = firstTri != Bsp::kNone;
    if (contacted && firstNode >= 0) {
        const formats::BspNode& n = bsp->arena()->nodes()[firstNode];
        *normal = Vec3{n.normal[0], n.normal[1], n.normal[2]};
    }
    return contacted;
}

void ScriptRuntime::fallOut(MdkObject* obj) {
    // The C# FallOut kills a death-scripted object (Kill switches it to its death
    // script) and fully removes an unscripted one (Remove takes a live Attached with
    // it). kill()'s death-script branch and remove() both match that now; only
    // kill()'s Explode branch stays the documented gap. The threshold and restore
    // height match.
    const float kRestore = 150.0f;
    if (obj->death_script == 0) {
        remove(obj);
        return;
    }
    kill(obj);
    obj->velocity.z = 0.0f;
    obj->position.z = getArenaFloor(obj->arena) - kRestore;
    obj->flags &= ~MdkObject::FlagGravity;
}

const Model* ScriptRuntime::findModel(const std::string& arena, const std::string& type) {
    std::map<std::string, Model>::iterator cached = modelCache_.find(type);
    if (cached != modelCache_.end()) {
        return &cached->second;
    }
    Model model;
    if (cmi_->model(type, &model)) {
        std::pair<std::map<std::string, Model>::iterator, bool> inserted =
            modelCache_.emplace(type, model);
        return &inserted.first->second;
    }
    if (level_->mto().has(arena)) {
        const formats::Arena* arenaModel = level_->mto().getArena(arena);
        if (arenaModel != nullptr) {
            const Model* found = arenaModel->model(type);
            if (found != nullptr) {
                return found;
            }
        }
    }
    return nullptr;
}

// C# ScriptRuntime.SelectTarget: an object's script aims at Kurt, or at the decoy
// while it walks, or at the aliens' target (opcode 251), unless the object always
// targets Kurt (target mode 2). Items.Decoy is not ported yet, so the decoy branch
// stays null until the Items slice lands; the alien-target branch is live.
void ScriptRuntime::selectTarget(MdkObject* obj) {
    const int32_t alwaysKurt = 2;
    targetPosition_ = kurtPosition_;
    targetYaw_ = kurtYaw_;
    if (obj->target_mode == alwaysKurt) {
        return;
    }
    // The decoy while it walks (Items.Decoy), else the aliens' target (opcode 251).
    MdkObject* other = nullptr;
    if (itemsDecoy != nullptr && !itemsDecoy->dead) {
        other = itemsDecoy;
    } else if (alienTarget_ != nullptr && !alienTarget_->dead) {
        other = alienTarget_;
    }
    if (other != nullptr) {
        targetPosition_ = other->position;
        targetYaw_ = other->yaw;
    }
}

// C# FirstOther(obj, type, AnyInstance): the first live object of the same arena,
// of the named type, other than obj. The type compare is case-insensitive (C# Is).
MdkObject* ScriptRuntime::firstOther(MdkObject* obj, const std::string& type, int32_t instance) {
    const int32_t kAnyInstance = -1;
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        MdkObject& other = *it;
        if (&other == obj || other.dead || other.arena != obj->arena) {
            continue;
        }
        if (instance != kAnyInstance && other.instance_id != instance) {
            continue;
        }
        if (iequalsType(other.type_name, type)) {
            return &other;
        }
    }
    return nullptr;
}

// C# ScriptVm case 74: match the named TYPE_ID in this arena, retaining the last live match.
void ScriptRuntime::attachTo(MdkObject* obj, int32_t pointA, int32_t pointB,
                             const std::string& name) {
    MdkObject* found = nullptr;
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        MdkObject& other = *it;
        if (&other == obj || other.dead || isRemoved(&other) || other.arena != obj->arena) {
            continue;
        }
        const std::string objectName = other.type_name + "_" + std::to_string(other.instance_id);
        if (formats::toLowerAscii(objectName) == formats::toLowerAscii(name)) {
            found = &other;
        }
    }
    if (found != nullptr) {
        obj->leader = found;
        obj->attach_point_a = pointA;
        obj->attach_point_b = pointB;
        obj->move_command = 74;
    }
}

// C# ScriptVm.FindObject: mode 0 random, mode 1 nearest, mode 2 nearest thrown item
// within sight/range. Collection order is the stable object-list order.
MdkObject* ScriptRuntime::findObject(MdkObject* obj, const std::string& type, int32_t mode) {
    const float kItemRange = 200.0f;
    const float kItemAbove = 9.0f;
    const auto eligible = [&](MdkObject& other, float* outDistance) {
        if (&other == obj || other.dead || isRemoved(&other) || other.arena != obj->arena ||
            !iequalsType(other.type_name, type)) {
            return false;
        }
        float dx = other.position.x - obj->position.x;
        float dy = other.position.y - obj->position.y;
        float dz = other.position.z - obj->position.z;
        float distance = sqrtf(dx * dx + dy * dy + dz * dz);
        if (mode == 2) {
            if (distance > kItemRange || (other.flags & 0x1000) == 0 ||
                other.position.z > obj->position.z + kItemAbove) {
                return false;
            }
            Vec3 from{obj->position.x, obj->position.y, obj->position.z + 5.0f};
            Vec3 to{other.position.x, other.position.y, other.position.z + 2.0f};
            if (!raycastClear(from, to)) {
                return false;
            }
        }
        *outDistance = distance;
        return true;
    };
    MdkObject* best = nullptr;
    float bestDistance = FLT_MAX;
    int32_t count = 0;
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        float distance = 0.0f;
        if (!eligible(*it, &distance)) {
            continue;
        }
        if (distance < bestDistance) {
            best = &*it;
            bestDistance = distance;
        }
        count++;
    }
    if (count == 0) {
        return nullptr;
    }
    if (mode != 0) {
        return best;
    }
    int32_t selected = rng_.next(count);
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        float distance = 0.0f;
        if (!eligible(*it, &distance)) {
            continue;
        }
        if (selected-- == 0) {
            return &*it;
        }
    }
    return nullptr;
}

// C# ScriptVm.LinkPartners: pair the arena's live, matching-type objects when
// they are closer than 100 units. Collection order is the stable object-list
// order, and a pair takes the first later eligible object, not the nearest.
void ScriptRuntime::linkPartners(MdkObject* obj, const std::string& type) {
    const float kRangeSquared = 10000.0f;
    std::vector<MdkObject*> loose;
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        MdkObject& other = *it;
        if (other.dead || other.arena != obj->arena ||
            !iequalsType(other.type_name, type)) {
            continue;
        }
        // The C# filter admits a null link or a link to a DEAD object, then
        // skips every non-null link in the pairing loop. Admitting the dead
        // link is unobservable for the pairing itself — a widow is skipped as
        // i and as j — but the clause is the oracle's filter and is kept
        // verbatim; the retained link it admits is what variable source 3 and
        // getFlags/setFlags read, and what keeps the widow from being paired
        // again here. The other `linked` consumers (180, 213/214, 246) read the
        // field too, but they reject a dead partner exactly as they reject a
        // null one.
        if (other.linked != nullptr && !other.linked->dead) {
            continue;
        }
        loose.push_back(&other);
    }
    for (size_t i = 0; i < loose.size(); i++) {
        if (loose[i]->linked != nullptr) {
            continue;
        }
        for (size_t j = i + 1; j < loose.size(); j++) {
            if (loose[j]->linked != nullptr) {
                continue;
            }
            const float dx = loose[i]->position.x - loose[j]->position.x;
            const float dy = loose[i]->position.y - loose[j]->position.y;
            const float dz = loose[i]->position.z - loose[j]->position.z;
            if (dx * dx + dy * dy + dz * dz >= kRangeSquared) {
                continue;
            }
            loose[i]->linked = loose[j];
            loose[j]->linked = loose[i];
            break;
        }
    }
}

// LastFollowerId (ScriptVm.cs:273): the highest instance id of the live objects
// following the leader, or -1 when it has none. The C# takes the max over the
// followers' ids (so a follower with a negative id is the answer) and returns
// -1 only for the "no follower" case.
int32_t ScriptRuntime::lastFollowerId(const MdkObject* leader) const {
    bool found = false;
    int32_t maxId = 0;
    for (std::deque<MdkObject>::const_iterator it = objects_.begin(); it != objects_.end(); ++it) {
        const MdkObject& other = *it;
        if (other.dead || other.leader != leader) {
            continue;
        }
        if (!found || other.instance_id > maxId) {
            maxId = other.instance_id;
            found = true;
        }
    }
    return found ? maxId : -1;
}

// LastChainLink (ScriptVm.cs:288): the live same-arena follower with movement
// command 30 and the highest instance id, or null. The leader itself and the
// objects of another arena are skipped, as the C#'s IsArenaOther does.
MdkObject* ScriptRuntime::lastChainLink(const MdkObject* leader) {
    const int32_t kChainCommand = 30;
    MdkObject* best = nullptr;
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        MdkObject& other = *it;
        if (&other == leader || other.dead || other.arena != leader->arena) {
            continue;
        }
        if (other.leader != leader || other.move_command != kChainCommand) {
            continue;
        }
        if (best == nullptr || other.instance_id > best->instance_id) {
            best = &other;
        }
    }
    return best;
}

// C# MayCommand (0x440384): the receiver has no other leader of a higher priority,
// and obeys the sender's priority.
bool ScriptRuntime::mayCommand(const MdkObject* sender, const MdkObject* receiver) {
    const MdkObject* leader = receiver->leader;
    if (leader != nullptr && leader != sender && !leader->dead &&
        sender->priority <= leader->priority) {
        return false;
    }
    return receiver->obey_level >= sender->priority;
}

// ObjectMotion.FormationPosition: the leader's frame; note the off-diagonal signs
// are NOT the standard rotation — X uses (ox*s - oy*c) and Y uses (-oy*s - ox*c).
static Vec3 formationPosition(const MdkObject* leader, const Vec3& offset) {
    /* @vfp-exempt R6 — cold formation placement (command 1); libm sinf/cosf track
     * the C# oracle's MathF.SinCos bit-for-bit on this machine (probed).
     * measured n/a (cold). Verified 2026-10-23. */
    const float s = sinf(degreesToRadians(leader->yaw));
    const float c = cosf(degreesToRadians(leader->yaw));
    Vec3 out;
    out.x = leader->position.x + offset.x * s - offset.y * c;
    out.y = leader->position.y - offset.y * s - offset.x * c;
    out.z = leader->position.z + offset.z;
    return out;
}

// ObjectMotion.TurnTowards (ObjectMotion.cs:766): turn the yaw toward a point by at
// most TurnSpeed × Dt, returning the angle still to turn measured BEFORE the turn.
// The point-equals-position test is the C#'s, and it is not redundant: with it a
// zero-length offset keeps the object's own yaw instead of atan2's arbitrary answer.
float ScriptRuntime::turnTowards(MdkObject* obj, const Vec3& point) {
    float targetYaw = obj->yaw;
    if (point.x != obj->position.x || point.y != obj->position.y) {
        targetYaw = yawTo(obj, point);
    }
    const float diff = wrapAngle(targetYaw - obj->yaw);
    const float maxStep = kTurnSpeed * kTick;
    obj->yaw = wrap360(obj->yaw + clampf(diff, -maxStep, maxStep));
    return diff;
}

// ObjectMotion.FollowFormation (ObjectMotion.cs:929, movement command 1). The slot
// is the leader's frame, so a turning leader swings the formation around with him.
// The step test is LengthSquared against (MaxSpeed × Dt)², and inside it the object
// is PLACED on the slot: the push branch below can never land exactly.
void ScriptRuntime::followFormation(MdkObject* obj) {
    const float kTicks = 1.0f;  // ObjectMotion.Ticks: one tick, in ticks
    MdkObject* leader = obj->leader;
    if (leader == nullptr || leader->dead) {
        obj->move_command = 0;
        return;
    }
    const Vec3 destination = formationPosition(leader, obj->waypoint);
    obj->move_destination = destination;
    const float dx = destination.x - obj->position.x;
    const float dy = destination.y - obj->position.y;
    const float dz = destination.z - obj->position.z;
    const float maxStep = obj->max_speed * kTick;
    if (dx * dx + dy * dy + dz * dz < maxStep * maxStep) {
        obj->position = destination;
        obj->yaw = moveToward(obj->yaw, leader->yaw, kTurnSpeed * kTick);
        return;
    }
    turnTowards(obj, destination);
    // The push is a per-tick velocity, and ApplyVelocity below multiplies it by Dt;
    // the C# adds the unscaled heading × MaxSpeed here, so the step is one frame of
    // MaxSpeed, not MaxSpeed × Dt.
    const Vec3 direction = fromAngle(obj->yaw);
    obj->push.x += direction.x * obj->max_speed;
    obj->push.y += direction.y * obj->max_speed;
    // The vertical is three-way: a unit per tick while a whole unit away, else the
    // exact remaining gap so the follower lands on its slot's Z.
    if (obj->position.z <= destination.z - kTicks) {
        obj->push.z += kTicks;
    } else if (obj->position.z >= destination.z + kTicks) {
        obj->push.z -= kTicks;
    } else {
        obj->push.z = destination.z - obj->position.z;
    }
}

// ObjectMotion.Chase (ObjectMotion.cs:894, movement command 6, 0x45e448): the speed
// is in units per TICK, unlike every other movement command, and it is the object's
// own `Speed` field carried from tick to tick — the gain and the brake are what make
// it accelerate. Its clamp pair is the C#'s OWN Chase formula (ObjectMotion.cs:916-917),
// not Accelerate — different constants, no MaxSpeed/TurnSpeedFactor target and no Dt — so
// it is a second oracle function, not a fifth copy of the first, and must not be folded
// into accelerate(). The step is applied to the POSITION (no push, no friction), and the
// object only turns when the point is ahead of it or far away: a point close and
// behind is ignored until it comes round.
void ScriptRuntime::chase(MdkObject* obj, const Vec3& point) {
    const float kBehind = 90.0f;      // degrees: at or past this, do not turn
    const float kFar = 30.0f;         // units: this far to a side overrides kBehind
    const float kAhead = 22.5f;       // degrees: within this, gain instead of brake
    const float kGain = 0.0444444f / 6.0f;
    const float kMaxChase = 2.3333333f;
    const float kBrake = 0.05f;
    const float kMinChase = 0.3333333f;
    const float kClimb = 0.25f;       // fraction of the step spent closing Z
    const float kTicks = 1.0f;        // ObjectMotion.Ticks: one tick, in ticks
    float targetYaw = obj->yaw;
    if (point.x != obj->position.x || point.y != obj->position.y) {
        targetYaw = yawTo(obj, point);
    }
    const float offAngle = fabsf(wrapAngle(obj->yaw - targetYaw));
    float chaseSpeed = obj->speed;
    if (offAngle < kBehind || fabsf(obj->position.x - point.x) > kFar ||
        fabsf(obj->position.y - point.y) > kFar) {
        turnTowards(obj, point);
        // The gain's multiply order is the C#'s: (Ahead - offAngle) * Ticks * Gain.
        chaseSpeed = offAngle <= kAhead
            ? mathMin(chaseSpeed + (kAhead - offAngle) * kTicks * kGain, kMaxChase)
            : mathMax(chaseSpeed - kTicks * kBrake, kMinChase);
        obj->speed = chaseSpeed;
    }
    const float step = chaseSpeed * kTicks;
    const Vec3 direction = fromAngle(obj->yaw);
    obj->position.x += direction.x * step;
    obj->position.y += direction.y * step;
    obj->position.z = moveToward(obj->position.z, point.z, step * kClimb);
}

// C# NearTargetDestination (command 43's operand): forward/side off the target's
// heading, scaled by 1% of the 3D distance to it, landing on the target's Z with
// the object's height offset.
Vec3 ScriptRuntime::nearTargetDestination(const MdkObject* obj, float forward, float side) {
    /* @vfp-exempt R6 — cold command path; libm atan2f/sinf/cosf track the C#
     * oracle's MathF calls bit-for-bit on this machine (probed), with .NET's
     * left-associative conversions. measured n/a (cold). Verified 2026-10-23. */
    const float direction =
        radiansToDegrees(atan2f(targetPosition_.y - obj->position.y, targetPosition_.x - obj->position.x));
    const float s = sinf(degreesToRadians(direction));
    const float c = cosf(degreesToRadians(direction));
    const float dx = obj->position.x - targetPosition_.x;
    const float dy = obj->position.y - targetPosition_.y;
    const float dz = obj->position.z - targetPosition_.z;
    // The C# multiplies left-to-right: (rotated * 0.01f) * distance. Keeping
    // that order matters — the destination feeds PlanMove and the goldens.
    const float distance = sqrtf(dx * dx + dy * dy + dz * dz);
    Vec3 out;
    out.x = targetPosition_.x + (forward * c - side * s) * 0.01f * distance;
    out.y = targetPosition_.y + (forward * s + side * c) * 0.01f * distance;
    out.z = targetPosition_.z + obj->height_offset;
    return out;
}

// One receiver of command_objects (0x4405d0). True when it joined a formation.
bool ScriptRuntime::commandObject(MdkObject* sender, MdkObject* receiver, int32_t command,
                                 int32_t target, const Vec3& destination, int32_t formationIndex) {
    switch (command) {
        case 1: {  // join a formation around the sender: places alternate left/right
            const bool wide = iequalsType(receiver->type_name, "XE");
            const float spread = wide ? 4.0f : 1.0f;
            const float back = wide ? 2.5f : 1.0f;
            const float side = (formationIndex & 1) != 0 ? 1.0f : -1.0f;
            receiver->waypoint.x = side * (float)(formationIndex / 2 + 1) * 5.0f * spread;
            receiver->waypoint.y = back * -4.0f;
            receiver->waypoint.z = 8.0f;
            receiver->move_destination = formationPosition(sender, receiver->waypoint);
            receiver->leader = sender;
            receiver->move_command = 1;
            receiver->path = 0;
            return true;
        }
        case 7:  // goto: the receiver's script restarts at the target
            receiver->wait_time = 0.0f;
            receiver->restart = target;
            receiver->wait_resume = target;
            receiver->leader = sender;
            receiver->gosub_returns.clear();
            receiver->gosub_restarts.clear();
            receiver->level_timers[0] = 0.0f;
            receiver->command_target = target;
            break;
        case 0xFC: {  // gosub: the receiver's script gosubs the target from where it is
            if ((int32_t)receiver->gosub_returns.size() < ScriptVm::kGosubDepth) {
                receiver->gosub_returns.push_back(receiver->restart);
                receiver->gosub_restarts.push_back(receiver->restart);
                receiver->wait_time = 0.0f;
                receiver->restart = target;
                receiver->wait_resume = target;
                receiver->leader = sender;
                receiver->level_timers[receiver->gosub_returns.size()] = 0.0f;
            }
            break;
        }
        case 43:  // move to a destination
            if (receiver->arena == currentArena_) {
                receiver->move_destination = destination;
                receiver->leader = sender;
                receiver->move_command = 43;
                receiver->path = 0;
                planMove(receiver);
            }
            break;
        default:
            break;
    }
    return false;
}

// command_objects (0x440384): every matching live object of the sender's arena gets
// the command. Selectors: 2 all of a type, 3 everything, 4 the first of a type,
// 5 a type by instance id, 6 of a type in sight, 7 the sender's followers,
// 8 everything following the sender, 9 the linked object, 10 of a type beyond a Y.
void ScriptRuntime::commandObjects(MdkObject* sender, int32_t command, int32_t target,
                                  const Vec3& destination, int32_t selector,
                                  const std::string& type, float param, int32_t id) {
    const float kSightHeight = 8.0f;
    if (selector == 9) {
        if (sender->linked != nullptr && !sender->linked->dead) {
            commandObject(sender, sender->linked, command, target, destination, 0);
        }
        return;
    }
    int32_t formationIndex = 0;
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        MdkObject* receiver = &(*it);
        if (receiver == sender || receiver->dead || receiver->arena != sender->arena) {
            continue;
        }
        if (selector != 3 && selector != 8 && !iequalsType(receiver->type_name, type)) {
            continue;
        }
        if (!mayCommand(sender, receiver)) {
            continue;
        }
        if ((selector == 7 || selector == 8) && receiver->leader != sender) {
            continue;
        }
        if (selector == 5 && receiver->instance_id != id) {
            continue;
        }
        if (selector == 6) {
            const float dx = sender->position.x - receiver->position.x;
            const float dy = sender->position.y - receiver->position.y;
            const float dz = sender->position.z - receiver->position.z;
            Vec3 from{sender->position.x, sender->position.y, sender->position.z + kSightHeight};
            Vec3 to{receiver->position.x, receiver->position.y, receiver->position.z + kSightHeight};
            if (sqrtf(dx * dx + dy * dy + dz * dz) > param || !raycastClear(from, to)) {
                continue;
            }
        }
        if (selector == 10 && receiver->position.y < param) {
            continue;
        }
        if ((command == 7 && receiver->command_target == target) || receiver->health == 0) {
            continue;
        }
        if (commandObject(sender, receiver, command, target, destination, formationIndex)) {
            formationIndex++;
        }
        if (selector == 4) {
            return;
        }
    }
}

void ScriptRuntime::kill(MdkObject* obj, float yaw) {
    // C# Kill (ScriptRuntime.cs:1678): Health=0, then either Explode (no death
    // script) or switch to the death script. NoteAlarmEnded is the 1996 demo's alarm
    // (ScriptRuntime.Beta.cs), which this runtime does not reach. The death-script
    // branch is pure VM state and is exercised by the L3 BOLT (its death script ends in
    // opcode 110 delete_self, which removes it the next tick); the explosion branch is
    // the chain gun's kill of an object with no death script (golden/L8_fire.txt).
    obj->health = 0;
    if (obj->death_script == 0) {
        explode(obj, yaw);
        return;
    }
    obj->move_command = 0;
    obj->flags |= MdkObject::FlagNotTarget;
    obj->restart = obj->death_script;
    obj->wait_time = 0.0f;
    obj->wait_resume = obj->death_script;
    obj->death_script = 0;
}

namespace {

// The explosion's numbers (ScriptRuntime.cs:96-111).
const int32_t kFireSparks = 16;
const float kExplosionFlash = 1000.0f;
const float kExplosionFlashMax = 150.0f;
const int32_t kGoreDrops = 32;
const float kGoreScale = 5.0f;
const float kExplosionLift = 5.0f;
const float kExplosionNear = 5.0f;
const float kExplosionNearHeight = 8.0f;
const int32_t kExplosionFrames = 26;
const float kExplosionSizeFactor = 1.5f;
const char kExplodeSound[] = "EXPLODE";

// The chain gun's numbers (ScriptRuntime.cs:30-36, 116, 140).
const int32_t kChainGunDamage = 1;
const int32_t kSuperChainGunFactor = 6;
const float kChainGunReach = 140.0f;
const float kChainGunWallReach = 150.0f;
const float kSuperChainGunPush = 20.0f;
const int32_t kMaxBarHealth = 900;
const float kGunEyeHeight = 5.0f;
const int32_t kHitChainGun = 2;

// The sparks' colours (palette base, range) by ScriptRuntime::Spark, and the flesh
// spark without gore (ScriptRuntime.cs:92-95).
const int32_t kSparkColours[4][2] = {{3, 3}, {0x25, -16}, {10, 3}, {0x30, 0x10}};
const int32_t kSparkFleshNoGore[2] = {13, 3};
const float kSparkSize = 0.5f;
const float kSparkSlow = 0.5f;
const char* const kRicochets[3] = {"RICO1", "RICO2", "RICO3"};

// ScriptMath.Center / Size (ScriptMath.cs:74-76).
Vec3 boxCenter(const formats::Box& box) {
    return Vec3{(box.min.x + box.max.x) * 0.5f, (box.min.y + box.max.y) * 0.5f,
                (box.min.z + box.max.z) * 0.5f};
}

Vec3 boxSize(const formats::Box& box) {
    return Vec3{box.max.x - box.min.x, box.max.y - box.min.y, box.max.z - box.min.z};
}

float length3(const Vec3& v) {
    return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
}

} // namespace

void ScriptRuntime::explode(MdkObject* obj, float yaw) {
    // C# Explode (ScriptRuntime.cs:1710-1723), in its order: the sound and the flash at
    // the centre, the pieces, the removal, then the explosion effect scaled to the
    // object's height (at least 0.1 of the effect's own).
    const float kMinHeight = 0.1f;
    const formats::Box bounds = worldBounds(obj);
    const Vec3 center = obj->model != nullptr ? boxCenter(bounds) : obj->position;
    playSoundAt(!obj->labels[0].empty() ? obj->labels[0] : std::string(kExplodeSound), center);
    flashAt(center);
    breakUp(obj);
    remove(obj);
    MdkObject* effect = spawnExplosion(obj->arena, center, 1.0f, yaw, kExplosionLift);
    if (effect != nullptr && effect->model != nullptr) {
        const float height = boxSize(effect->model->bounds).z;
        effect->scale = boxSize(bounds).z / mathMax(height, kMinHeight) * kExplosionSizeFactor;
    }
}

void ScriptRuntime::flashAt(const Vec3& point) {
    // C# FlashAt (ScriptRuntime.cs:1725-1735). MathF.Round is the banker's round, which
    // rintf is under the default rounding mode.
    const float distance = length3(Vec3{kurtPosition_.x - point.x, kurtPosition_.y - point.y,
                                        kurtPosition_.z - point.z});
    Kurt& combat = kurt();
    if (distance <= 0.0f || combat.whiteFlash() >= kExplosionFlashMax) {
        return;
    }
    combat.setWhiteFlash(mathMin(combat.whiteFlash() + rintf(kExplosionFlash / distance),
                                 kExplosionFlashMax));
}

void ScriptRuntime::breakUp(MdkObject* obj) {
    // C# BreakUp (ScriptRuntime.cs:1738-1765).
    const float kDropSpread = 1.0f / 16384.0f;
    const formats::Model* model =
        obj->model != nullptr ? findModel(obj->arena, obj->model->name + "D") : nullptr;
    if (model == nullptr) {
        debris_.spark(obj->arena, obj->position, kFireSparks, 1.0f,
                      kSparkColours[(int)Spark::Fire][0], kSparkColours[(int)Spark::Fire][1]);
        return;
    }

    debris_.breakUp(*obj, *model, materialNamesOf(model));

    // Slime drops thrown like the pieces, with gore (0x43d55e).
    if (option == 0) {
        return;
    }

    const Vec3 velocity{obj->position.x - obj->previous_position.x,
                        obj->position.y - obj->previous_position.y,
                        obj->position.z - obj->previous_position.z};
    for (int32_t i = 0; i < kGoreDrops; i++) {
        // The C#'s argument order: x, y and z of the throw, then the scale.
        const int32_t tx = rng_.next(32768) - 0x4000;
        const int32_t ty = rng_.next(32768) - 0x4000;
        const int32_t tz = rng_.next(32768) - 0x800;
        const Vec3 thrown{(float)tx * kDropSpread, (float)ty * kDropSpread,
                          (float)tz * kDropSpread};
        const Vec3 launch{velocity.x + thrown.x, velocity.y + thrown.y, velocity.z + thrown.z};
        const float scale = kGoreScale + (float)rng_.next(32768) * 5e-5f;
        effects.spawnDrop(obj->arena, obj->position, launch, scale);
    }
}

const std::vector<std::string>* ScriptRuntime::materialNamesOf(const formats::Model* model) {
    std::map<const formats::Model*, std::vector<std::string>>::iterator found =
        materialNames_.find(model);
    if (found != materialNames_.end()) {
        return &found->second;
    }
    std::vector<std::string> names;
    for (size_t i = 0; i < model->materials.size(); i++) {
        names.push_back(model->materials[i].nameString());
    }
    return &materialNames_.emplace(model, names).first->second;
}

bool ScriptRuntime::findTextureFrames(const std::string& arena, const std::string& name,
                                      int32_t* frames) {
    // C# FindTexture (ScriptRuntime.cs:3224): the arena's own archive when the MTO has
    // the arena, then the level's. False is the C#'s null.
    if (level_->mto().has(arena)) {
        const formats::Arena* owner = level_->mto().getArena(arena);
        if (owner != nullptr) {
            const formats::Texture* found = owner->textures().findTexture(name);
            if (found != nullptr) {
                *frames = found->frameCount();
                return true;
            }
        }
    }
    const formats::Texture* level = level_->levelTextures().findTexture(name);
    if (level == nullptr) {
        return false;
    }
    *frames = level->frameCount();
    return true;
}

MdkObject* ScriptRuntime::spawnExplosion(const std::string& arena, const Vec3& center, float scale,
                                         float yaw, float lift) {
    // C# SpawnExplosion (ScriptRuntime.cs:1770-1796): the CMI's FIRST model by the file's
    // order (Cmi.ModelOffsets is an insertion-ordered dictionary, and modelOffsets()
    // keeps the file's order), spawned plain under the arena's controller.
    const std::vector<std::pair<std::string, int32_t>>& models = cmi_->modelOffsets();
    const std::string modelName = models.empty() ? std::string() : models[0].first;
    MdkObject* effect =
        spawn(&getArenaState(arena).controller, modelName, center, yaw, -1, 0, false);
    if (effect == nullptr || effect->model == nullptr) {
        return nullptr;
    }

    effect->flags |= MdkObject::FlagNotTarget | MdkObject::FlagNotSolid2;
    effect->effect_frames = kExplosionFrames;
    int32_t frames = 0;
    if (!effect->model->materials.empty() &&
        findTextureFrames(arena, effect->model->materials[0].nameString(), &frames)) {
        effect->effect_frames = frames;
    }

    effect->scale = scale;
    const float hx = eye.x - center.x;
    const float hy = eye.y - center.y;
    const float horizontal = sqrtf(hx * hx + hy * hy);
    effect->yaw = yaw == 0.0f ? wrap360(heading(center, eye)) : yaw;
    const float rise = eye.z + lift - center.z;
    if (horizontal > kExplosionNear || fabsf(rise) > kExplosionNearHeight) {
        /* @vfp-exempt R6 — the explosion turns to the camera once, when it spawns (cold);
         * the C#'s MathF.Atan2 is glibc's atan2f bit for bit on this machine (see yawTo's
         * pin) and the LUT would tilt it differently. measured n/a (corpus empty).
         * Verified 2026-11-19. */
        effect->pitch = radiansToDegrees(atan2f(rise, horizontal));
    }

    effect->texture_frame = 0;
    return effect;
}

void ScriptRuntime::motionEffect(MdkObject* obj) {
    // ObjectMotion.Update's first branch (ObjectMotion.cs:54-68), where Ticks is 1: an
    // effect plays its texture animation one frame a tick and goes after the last.
    obj->effect_time += 1.0f;
    if (obj->effect_time >= (float)obj->effect_frames) {
        remove(obj);
    } else {
        obj->texture_frame = (int32_t)obj->effect_time;
    }
}

void ScriptRuntime::playSoundAt(const std::string& name, const Vec3& point) {
    mixer_.playAt(name, point);
}

void ScriptRuntime::sparkAt(const Vec3& point, int32_t count, const std::string& sound,
                            Spark kind) {
    // C# SparkAt (ScriptRuntime.cs:2201-2212). The ricochet's pick is drawn from the
    // game's random stream ONLY when it plays and the caller named no sound, ahead of the
    // sparks' own draws, exactly as the C# orders them.
    const int32_t kRicochetEvery = 3;
    const bool quiet = !effects.hasEffects(currentArena_) && !debris_.hasPieces(currentArena_);
    if (quiet || count > 1 || (tickCount_ & kRicochetEvery) == 0) {
        playSoundAt(!sound.empty() ? sound : std::string(kRicochets[rng_.next(3)]), point);
    }
    const bool gore = kind != Spark::Flesh || option != 0;
    const int32_t colour = gore ? kSparkColours[(int)kind][0] : kSparkFleshNoGore[0];
    const int32_t range = gore ? kSparkColours[(int)kind][1] : kSparkFleshNoGore[1];
    debris_.spark(currentArena_, point, count, kSparkSize, colour, range,
                  kind == Spark::Hard ? kSparkSlow : 1.0f);
}

void ScriptRuntime::showBarValues(int32_t health, int32_t maxHealth) {
    // C# ShowBarValues (ScriptRuntime.cs:2157-2163).
    barObject = nullptr;
    barHealth = health;
    barMax = maxHealth;
    barTime = health > 0 ? 1.0f : 0.0f;
}

ScriptRuntime::BarValues ScriptRuntime::bar() const {
    // C# GetBar (ScriptRuntime.cs:2165-2174).
    BarValues values{0, 0};
    if (barTime <= 0.0f) {
        return values;
    }
    if (barObject != nullptr) {
        values.health = barObject->health;
        values.max = barObject->max_health;
    } else {
        values.health = barHealth;
        values.max = barMax;
    }
    return values;
}

void ScriptRuntime::updateBar() {
    // C# UpdateBar (ScriptRuntime.cs:2177-2197).
    if (barTime <= 0.0f) {
        return;
    }
    barTime -= kTick;
    if (barObject != nullptr && barObject->dead) {
        barObject = nullptr;
        barTime = 0.0f;
        return;
    }
    const BarValues values = bar();
    if (values.max == 0 || values.max > kMaxBarHealth || values.health < 1) {
        barTime = 0.0f;
    }
}

float ScriptRuntime::aimScore(const formats::Box& bounds, const Vec3& origin, float bestScore) {
    // C# AimScore (ScriptRuntime.cs:2029-2053). The heading is ScriptMath.Heading's libm
    // atan2f (heading(), pinned there): the LUT would move the cone's edge and with it
    // which object the gun hits.
    const float kMinSize = 10.0f;
    const float kFullTurn = 360.0f;
    const float size = mathMax(length3(boxSize(bounds)), kMinSize);
    const Vec3 center = boxCenter(bounds);
    const float distance =
        length3(Vec3{origin.x - center.x, origin.y - center.y, origin.z - center.z});
    if (distance > size + kChainGunReach) {
        return -1.0f;
    }

    const float angle = wrap360(heading(origin, center) - targetYaw_);
    const float cone = (size - 2.0f) * 90.0f / (size - 2.0f + distance);
    if (angle > cone && angle < kFullTurn - cone) {
        return -1.0f;
    }

    const float height = center.z - kurtPosition_.z;
    const float dx = center.x - origin.x;
    const float dy = center.y - origin.y;
    const float score = (dx * dx + dy * dy) + 4.0f * height * height;
    if (bestScore >= 0.0f && score > bestScore) {
        return -1.0f;
    }

    return raycastClear(origin, center) ? score : -1.0f;
}

void ScriptRuntime::fireChainGun() {
    // C# FireChainGun (ScriptRuntime.cs:1818-1900, 0x41a304).
    const int32_t kShotsPerTick = 6;
    const Vec3 origin{kurtPosition_.x, kurtPosition_.y, kurtPosition_.z + kGunEyeHeight};

    // The super chain gun does 6 times the damage while its time lasts.
    Kurt& combat = kurt();
    const bool superGun = combat.superChainGun() > 0;
    const int32_t damage = kChainGunDamage * (superGun ? kSuperChainGunFactor : 1);
    if (superGun) {
        combat.tickSuperChainGun(1);
    }

    stats.shots += kShotsPerTick;
    MdkObject* best = nullptr;
    float bestScore = -1.0f;
    int32_t bestPart = -1;
    formats::Box bestBounds{};
    // The C#'s foreach over Objects: the deque's slots in creation order, skipping the
    // removed ones (`dead`), which is the C# list exactly.
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        MdkObject* obj = &(*it);
        if (obj->dead || obj->arena != currentArena_ || obj->health == 0 ||
            (obj->flags & (MdkObject::FlagNotSolid | MdkObject::FlagNotTarget)) != 0) {
            continue;
        }

        // Weak parts are targets of their own.
        if ((obj->flags & MdkObject::FlagWeakParts) != 0 && obj->model != nullptr) {
            const std::vector<PartBox> parts = partBounds(obj);
            const size_t partCount = obj->model->partList.size();
            for (size_t i = 0; i < partCount; i++) {
                // C#'s `1 << i` takes the shift count modulo 32.
                if ((obj->hidden_parts & (1 << (i & 31))) != 0 ||
                    !isWeakPart(obj, (int32_t)i) || i >= parts.size() || !parts[i].valid) {
                    continue;
                }
                const formats::Box partBox = worldBounds(obj, parts[i].box);
                const float partScore = aimScore(partBox, origin, bestScore);
                if (partScore < 0.0f) {
                    continue;
                }
                best = obj;
                bestScore = partScore;
                bestPart = (int32_t)i;
                bestBounds = partBox;
            }
            if (best == obj) {
                continue;
            }
        }

        const formats::Box bounds = worldBounds(obj);
        const float score = aimScore(bounds, origin, bestScore);
        if (score >= 0.0f) {
            best = obj;
            bestScore = score;
            bestPart = -1;
            bestBounds = bounds;
        }
    }

    if (best != nullptr) {
        chainGunHit(best, bestPart, bestBounds, origin, damage, superGun);
        return;
    }

    // No target: the arena, up to 150 units along Kurt's yaw, level.
    const Vec3 direction = fromAngle(targetYaw_);
    const Vec3 shot{direction.x * kChainGunWallReach, direction.y * kChainGunWallReach, 0.0f};
    DebrisRayHit hit;
    if (!raycastHit(origin, Vec3{origin.x + shot.x, origin.y + shot.y, origin.z + shot.z},
                    &hit)) {
        return;
    }

    const bool reacted =
        (hitGroup(hit.arena, hit.group, damage, kHitChainGun, superGun ? -2 : -1) & 1) != 0;
    // Vector3.Normalize: the vector over its length.
    const float length = length3(shot);
    const Vec3 back{hit.point.x - shot.x / length, hit.point.y - shot.y / length,
                    hit.point.z - shot.z / length};
    sparkAt(back, 1, std::string(), reacted ? Spark::Group : Spark::Hard);
}

void ScriptRuntime::chainGunHit(MdkObject* obj, int32_t part, const formats::Box& bounds,
                                const Vec3& origin, int32_t damage, bool superGun) {
    // C# ChainGunHit (ScriptRuntime.cs:2075-2135). The 1996 demo's hit-part guess
    // (IsBeta && BetaHitPart) is not reached by this runtime (see the IsBeta note by the
    // animation lookup).
    const Vec3 center = boxCenter(bounds);
    const float direction = heading(origin, center);
    stats.shotHits++;
    obj->hit_event = -1;

    if (part >= 0 && part < (int32_t)obj->part_health.size()) {
        const size_t index = (size_t)part;
        obj->part_health[index] -= kurtDamage(obj, damage, obj->part_health[index]);
        if (obj->part_health[index] <= 0) {
            obj->part_health[index] = 0;
            obj->hit_event = part + 1;
        }
        if (index < obj->part_max_health.size() && obj->part_max_health[index] <= kMaxBarHealth) {
            showBarValues(obj->part_health[index], obj->part_max_health[index]);
        }
    }

    if (obj->health < kIndestructible) {
        obj->health -= kurtDamage(obj, damage);
    }

    obj->hit_type = superGun ? -2 : -1;
    obj->hit_direction = direction;
    if (obj->health > 0) {
        if (part < 0 && obj->max_health <= kMaxBarHealth) {
            barObject = obj;
            barTime = 1.0f;
        }

        // Sparks on the side of the box facing Kurt: center - (tx*sx, ty*sy, 0) * 0.5.
        const Vec3 toward = fromAngle(direction);
        const Vec3 size = boxSize(bounds);
        const Vec3 offset{toward.x * size.x * 0.5f, toward.y * size.y * 0.5f, 0.0f * 0.5f};
        const Vec3 point{center.x - offset.x, center.y - offset.y, center.z - offset.z};
        sparkAt(point, 1, obj->labels[1],
                obj->indestructible && part < 0 ? Spark::Hard : Spark::Flesh);
        return;
    }

    obj->health = 0;
    if (superGun) {
        // The super chain gun throws what it kills away.
        const Vec3 push = fromAngle(direction);
        obj->velocity.x += push.x * kSuperChainGunPush;
        obj->velocity.y += push.y * kSuperChainGunPush;
    }

    stats.countEnemy(obj->type_name, GameStats::Killed);
    kill(obj, direction + 180.0f);
}

// Items.cs BlastLift: the blast origin is lifted off the floor so the wall test
// doesn't hit the floor the blast may lie on.
static const float kBlastLift = 1.0f;

int32_t ScriptRuntime::kurtDamage(MdkObject* obj, int32_t damage, int32_t health) {
    // C# KurtDamage (ScriptRuntime.OneHit.cs:21). The one-hit console cheat
    // (Lethality::OneHit) makes a hit take all an enemy's remaining health; that
    // console setting is unported, so Lethality is always Normal and this returns
    // `damage` -- the value the C# returns for every non-cheat game.
    (void)obj;
    (void)health;
    return damage;
}

int32_t ScriptRuntime::kurtDamage(MdkObject* obj, int32_t damage) {
    return kurtDamage(obj, damage, obj->health);
}

int32_t ScriptRuntime::blastHit(MdkObject* obj, int32_t damage, int32_t health, bool counted) {
    // C# BlastHit (Items.cs:783): a counted blast scales by KurtDamage; an
    // uncounted one (Kills.Ignored) deals the raw damage.
    return counted ? kurtDamage(obj, damage, health) : damage;
}

std::pair<int32_t, float> ScriptRuntime::blastDamage(const formats::Box& box, const Vec3& center,
                                                    int32_t damage, float radius) {
    // C# BlastDamage (Items.cs:788): falloff to the box centre less half the box's
    // diagonal; 0 beyond the radius or behind a wall.
    const float sx = box.max.x - box.min.x;
    const float sy = box.max.y - box.min.y;
    const float sz = box.max.z - box.min.z;
    const float size = sqrtf(sx * sx + sy * sy + sz * sz);
    const Vec3 target{(box.min.x + box.max.x) * 0.5f,
                      (box.min.y + box.max.y) * 0.5f,
                      (box.min.z + box.max.z) * 0.5f};
    const Vec3 lifted{center.x, center.y, center.z + kBlastLift};
    const float dx = lifted.x - target.x;
    const float dy = lifted.y - target.y;
    const float dz = lifted.z - target.z;
    const float squared = (dx * dx + dy * dy + dz * dz) - size * size * 0.25f;
    if (squared > radius * radius || !raycastClear(lifted, target)) {
        return std::make_pair(0, 0.0f);
    }
    const float distance = sqrtf(mathMax(squared, 0.0f));
    // ScriptMath.Round is AwayFromZero (ScriptMath.cs:48); C roundf matches it.
    return std::make_pair((int32_t)roundf((float)damage * (1.0f - distance / radius)), distance);
}

// BlastWeakParts (Items.cs:680): the blast on an object's weak parts. For each
// visible, weak, in-range part with a box, take the blast damage; keep the best
// (strict >, so the first max wins), and drop each part's health by the hit,
// recording the last part destroyed (index + 1) as the event.
ScriptRuntime::BlastPartResult ScriptRuntime::blastWeakParts(MdkObject* obj, const Vec3& center,
                                                             int32_t damage, float radius,
                                                             bool counted) {
    BlastPartResult result;
    result.best = 0;
    result.distance = 0.0f;
    result.point = obj->position;
    result.event = 0;
    if ((obj->flags & MdkObject::FlagWeakParts) == 0 || obj->model == nullptr) {
        return result;
    }
    std::vector<PartBox> parts = partBounds(obj);
    const size_t partCount = obj->model->partList.size();
    for (size_t i = 0; i < partCount; ++i) {
        if ((obj->hidden_parts & (1 << i)) != 0 || !isWeakPart(obj, static_cast<int32_t>(i)) ||
            i >= obj->part_health.size() || i >= parts.size() || !parts[i].valid) {
            continue;
        }
        const formats::Box box = worldBounds(obj, parts[i].box);
        const std::pair<int32_t, float> hit = blastDamage(box, center, damage, radius);
        if (hit.first <= 0) {
            continue;
        }
        if (hit.first > result.best) {
            result.best = hit.first;
            result.distance = hit.second;
            result.point = Vec3{(box.min.x + box.max.x) * 0.5f,
                                (box.min.y + box.max.y) * 0.5f,
                                (box.min.z + box.max.z) * 0.5f};
        }
        obj->part_health[i] -= blastHit(obj, hit.first, obj->part_health[i], counted);
        if (obj->part_health[i] < 1) {
            obj->part_health[i] = 0;
            result.event = static_cast<int32_t>(i) + 1;
        }
    }
    return result;
}


void ScriptRuntime::remove(MdkObject* obj) {
    // C# Remove (ScriptRuntime.cs:1648): a live Attached object (a pickup's chute) goes
    // with it FIRST — Remove(obj.Attached) runs before obj is marked dead — then Dead,
    // then out of the list. The deque's slots are never vacated (the pointer model
    // depends on it), so the object is marked removed here and remains readable through
    // surviving references. The oracle clears NO inbound reference — not Linked, not
    // Leader, not CowTarget, not Attached — so none is cleared here either: a widow
    // keeps pointing at its dead partner (link_partners must not re-pair it), a
    // removed chute keeps its leader, and a pickup keeps a chute removed under it.
    // Every read site twins the oracle's test — null-or-dead for linked/leader,
    // null-only for attached, elsewhere a comparison against a live object — so a
    // retained pointer to the permanent slot behaves exactly like the oracle's
    // retained managed reference. A pickup that falls out (FallOut -> remove) takes
    // its chute with it.
    // Rides.Lost (ScriptRuntime.cs:1650, package #9), the C#'s first statement: the
    // ridden object going puts Kurt off it (and hurts him but for the board). A removed
    // object is never the ridden one (its first removal let him off), so this before
    // the idempotence return is the C#'s order without a second effect.
    rides_.lost(obj);
    if (isRemoved(obj)) {
        return;  // C# Objects.Remove is idempotent; do not double-process
    }
    if (obj->attached != nullptr && !obj->attached->dead) {
        remove(obj->attached);
    }
    // The object's sounds go with it (C# Remove: the loop sound, the tracked
    // sound, and every voice that was following it).
    mixer_.stopVoice(obj->loop_sound);
    mixer_.stopVoice(obj->tracked_voice);
    std::map<MdkObject*, std::vector<int32_t> >::iterator following = following_.find(obj);
    if (following != following_.end()) {
        for (size_t i = 0; i < following->second.size(); i++) {
            mixer_.stopVoice(following->second[i]);
        }
        following_.erase(following);
    }
    obj->loop_sound = 0;
    obj->tracked_voice = 0;
    obj->dead = true;
    removed_.push_back(obj);
}

bool ScriptRuntime::isRemoved(const MdkObject* obj) const {
    for (size_t i = 0; i < removed_.size(); i++) {
        if (removed_[i] == obj) {
            return true;
        }
    }
    return false;
}

} // namespace scripts
} // namespace mdk

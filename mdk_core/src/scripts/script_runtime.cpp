// ============================================================================
// script_runtime.cpp — see script_runtime.h.
//
// ONE TRANSLATION UNIT IN TEN FILES (split 2026-11-25 so it reads in pieces):
// this file holds the file-local constants and helpers, construction, update
// and the doorway geometry, and #includes the rest at the bottom, in the
// original order:
//     script_runtime_tick.inc     runTick, motionUpdate, runControllers
//     script_runtime_arenas.inc   the drawn arenas, loading, teleports, spawning
//     script_runtime_motion.inc   paths and the motions
//     script_runtime_world.inc    raycasts, debris callbacks, sight, groups
//     script_runtime_ai.inc       the movement AI, projectiles, updateCommand
//     script_runtime_objects.inc  floors, poses, animation, doors, bounds, Kurt's damage
//     script_runtime_events.inc   special events, cutscenes, camera, sounds, the sweep
//     script_runtime_squads.inc   targets, finding, linking, formations, commands
//     script_runtime_combat.inc   kills, explosions, the chain gun, blasts, removal
// The .inc files are not headers -- no guards, no includes -- and share this
// file's helpers; the split moved code and nothing else (the object code is
// unchanged). The other ScriptRuntime members (items.cpp, rides.cpp, ...) are
// separate translation units, as before.
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
        // The tick probe (script_runtime.h): a fresh row per tick, kept at its end
        // when it is among the slowest.
        uint64_t probeStart = 0;
        if (probeClock != nullptr) {
            probeNow_ = ProbeRow();
            probeNow_.frame = probeFrame;
            probeNow_.tick = tickCount_;
            probeStart = probeClock();
        }
        // The C# snapshots every object's pose before the tick runs
        // (ScriptRuntime.cs:383-386), because the frames until the next tick are
        // drawn from here to there. It walks its live Objects list; the port keeps
        // removed objects in the deque for the pointers other objects hold, and
        // snapshotting one that is not drawn costs a pose copy and changes nothing
        // — the snapshot of a removed object is never read.
        for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
            it->remember();
        }
        if (probeClock != nullptr) {
            probeNow_.remember = probeClock() - probeStart;
        }
        runTick();
        tickCount_++;
        if (probeClock != nullptr) {
            probeNow_.total = probeClock() - probeStart;
            probeKeep();
        }
    }
    // Rides.Follow (ScriptRuntime.cs:392, package #9): each step, after the ticks, the
    // XD2 is where Kurt is.
    rides_.follow();
}

// The tick probe's keep: the slowest kProbeRows ticks, in fixed storage. A full
// table replaces its fastest row when this tick was slower.
void ScriptRuntime::probeKeep() {
    if (probeRowCount < kProbeRows) {
        probeRows[probeRowCount++] = probeNow_;
        return;
    }
    int32_t fastest = 0;
    for (int32_t i = 1; i < kProbeRows; i++) {
        if (probeRows[i].total < probeRows[fastest].total) {
            fastest = i;
        }
    }
    if (probeNow_.total > probeRows[fastest].total) {
        probeRows[fastest] = probeNow_;
    }
}

namespace {

// A type name into a probe row's fixed field, cut to fit.
void probeName(char* out, const std::string& name) {
    const size_t room = (size_t)ScriptRuntime::kProbeName - 1;
    const size_t n = name.size() < room ? name.size() : room;
    memcpy(out, name.data(), n);
    out[n] = '\0';
}

} // namespace

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

#include "script_runtime_tick.inc"

#include "script_runtime_arenas.inc"

#include "script_runtime_motion.inc"

#include "script_runtime_world.inc"

#include "script_runtime_ai.inc"

#include "script_runtime_objects.inc"

#include "script_runtime_events.inc"

#include "script_runtime_squads.inc"

#include "script_runtime_combat.inc"

} // namespace scripts
} // namespace mdk

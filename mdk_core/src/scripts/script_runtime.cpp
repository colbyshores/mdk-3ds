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
#include "game/player.h"
#include "util/math_lut.h"

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

// MathF.Max: NaN-propagating. fmaxf is not, because it returns the numeric
// side when exactly one argument is NaN.
float mathMax(float a, float b) {
    if (a != a) {
        return a;
    }
    if (b != b) {
        return b;
    }
    return a > b ? a : b;
}

// MathF.Min: NaN-propagating, for the same reason as mathMax. The oracle's speed
// clamps are MathF.Min/MathF.Max (ObjectMotion.cs:784,788,916,917), so a NaN
// speed stays NaN through them there; fminf would silently turn it into the
// finite bound. Same shape as script_vm.cpp's pair.
float mathMin(float a, float b) {
    if (a != a) {
        return a;
    }
    if (b != b) {
        return b;
    }
    return a < b ? a : b;
}

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
    // Debris delegates (ScriptRuntime.cs:340-346): Ray and GroupFacets are
    // wired; Trail (Effects.SpawnTrail) and Updraft (Fans.Query) stay unset
    // until those slices land — neither perturbs the RNG stream (see debris.h).
    debris_.setCallbacks(this, &ScriptRuntime::debrisRayTrampoline, nullptr, nullptr,
                         &ScriptRuntime::debrisFacetsTrampoline);
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
}

float ScriptRuntime::tick_blend(float ahead) const {
    // Math.Clamp((time + ahead) / Tick, 0f, 1f) — the C#'s TickBlend, with the
    // runtime's own leftover time as `time`.
    const float alpha = (tickAccumulator_ + ahead) / kTick;
    return alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
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

    // The alarm countdown decays once per tick, before any object runs
    // (ScriptRuntime.cs:896). There is no cutscene state in this slice, so the
    // C#'s cutscene early-return cannot suppress this decrement here.
    alarmTicks = alarmTicks > 0 ? alarmTicks - 1 : 0;

    // The camera-track request lasts for the current tick and the next one
    // (ScriptRuntime.cs:898).
    cameraTrackTicks = cameraTrackTicks > 0 ? cameraTrackTicks - 1 : 0;

    // The screen-shake request belongs to one tick (ScriptRuntime.cs:899).
    // Calls made later in this tick replace it only when they are stronger.
    shake = 0.0f;

    // The first tick has no movement to observe (ScriptRuntime.cs:911-917,
    // after the decrements above, as in the oracle). A later teleport can set
    // the previous position in the C#; no teleport source exists in this
    // slice, so the ordinary delta is kept.
    if (tickCount_ == 0) {
        previousKurtPosition_ = kurtPosition_;
    }

    // Kurt changes arena only through a connection; on the first tick he is
    // placed and the arena under him is found.
    std::string arena;
    if (currentArena_.empty()) {
        formats::Vec3f at;
        at.x = kurtPosition_.x;
        at.y = kurtPosition_.y;
        at.z = kurtPosition_.z;
        const std::string* found = space_.arenaAt(at);
        arena = found ? *found : std::string();
    }

    if (!arena.empty() && arena != currentArena_) {
        if (!currentArena_.empty()) {
            secondArena_ = currentArena_;
        }
        secondActive_ = true;
        currentArena_ = arena;
        showArena(arena);
    }

    // ScriptRuntime.cs:945-946 records Kurt's observed feet delta before the
    // object scripts run. The C# computes it after CollectPickups; pickups and
    // the other movement writers that can precede it are later slices, so the
    // copied Player position is the complete source in this runtime.
    //
    // Two oracle details this slice does not have yet: the C#'s cutscene
    // early-return (ScriptRuntime.cs:889-892) skips this write AND the
    // previous-position update during a cutscene — there is no cutscene state
    // here, so both happen on every tick; and the C#'s save/load snapshot
    // resets the previous position (ScriptRuntime.Snapshot.cs:37-39), so the
    // first tick after a load observes no movement. The snapshot slice must
    // reset previousKurtPosition_ the same way.
    kurtVelocity.x = (kurtPosition_.x - previousKurtPosition_.x) * kTicksPerSecond;
    kurtVelocity.y = (kurtPosition_.y - previousKurtPosition_.y) * kTicksPerSecond;
    kurtVelocity.z = (kurtPosition_.z - previousKurtPosition_.z) * kTicksPerSecond;
    previousKurtPosition_ = kurtPosition_;

    // Debris.Update(1f) (ScriptRuntime.cs:955): the pieces move before the
    // object loop, as in the C#. The neighbouring subsystem updates (Items,
    // Effects, Fans, SniperRounds, AirStrike) are later slices.
    debris_.update(1.0f);

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
        vm_.run(obj);
        // The path-following half of Motion.Update, gated on liveness exactly as
        // the C# (Vm.Run then `if (!Dead) Motion.Update`): a script that kills the
        // object this tick must not have it path-moved afterwards.
        if (!obj->dead) {
            motionPath(obj);
            motionVelocity(obj);
            // UpdatePickup runs after ApplyVelocity (ObjectMotion.cs:118), only if the
            // object survived the velocity step (the C# returns on Dead before it). The
            // C# dispatch gives ThrownKind>0 precedence over FlagPickup (UpdateThrown,
            // not UpdatePickup), and returns for effect objects before this section.
            if (!obj->dead && obj->effect_frames == 0 && obj->thrown_kind <= 0 &&
                (obj->flags & MdkObject::FlagPickup) != 0) {
                motionPickup(obj);
            }
            // AdvanceAnimation (ObjectMotion.cs:132), after ApplyVelocity and the
            // pickup dispatch. An effect object returned from Update before this
            // point, so it never advances an animation here.
            if (!obj->dead && obj->effect_frames == 0) {
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

    runControllers();
}

void ScriptRuntime::runControllers() {
    if (!currentArena_.empty()) {
        vm_.run(&getArenaState(currentArena_).controller);
    }
    if (secondActive_ && !secondArena_.empty()) {
        vm_.run(&getArenaState(secondArena_).controller);
    }
}

void ScriptRuntime::showArena(const std::string& arena) {
    if (arena.empty() || arena == "NONE") {
        secondArena_ = "";
        secondActive_ = false;
        return;
    }

    if (arena == currentArena_) {
        // LoadArena: the resident arena and its doors. No objects exist yet on
        // the first tick, so there is nothing to pull.
    } else if (arena != secondArena_) {
        secondArena_ = arena;
    }

    secondActive_ = true;
    ArenaState& state = getArenaState(arena);
    if (!state.started) {
        state.started = true;
        spawnDtiAliens(arena);
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
    // FlagPathPushes drives horizontal velocity (Push) instead of position; that
    // belongs to the velocity/footing motion slice.
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
        obj->velocity.z -= obj->gravity * kTick;
        // Updraft (Fans.Query) is not ported; no fan reaches these free-fall cases
        // (the oracle's velocity.Z is exactly -gravity*Dt here). When Fans lands, the
        // terminal clamp MUST move after the fan query (C# order: gravity, Updraft,
        // clamp — ObjectMotion.cs:93-95), or a fan updraft will clamp differently.
        if (obj->velocity.z < -220.0f) {
            obj->velocity.z = -220.0f;  // TerminalVelocity
        }
    }

    // ApplyVelocity (0x45e810). Friction acts on the horizontal only for a gravity
    // object (its vertical is gravity's, not friction's).
    obj->contact_flags &= ~(MdkObject::ContactCollided | MdkObject::ContactFloor | 0x10);
    float mx = obj->velocity.x;
    float my = obj->velocity.y;
    float mz = gravity ? 0.0f : obj->velocity.z;
    float length = sqrtf(mx * mx + my * my + mz * mz);
    if (length > 0.0f) {
        float factor = fmaxf(length - obj->friction * kTick, 0.0f) / length;
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
        return;  // collected shrink/remove: later slice
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
    if (cone < kHalfTurn &&
        fabsf(wrapAngle(yawTo(obj, kurtPosition_) - obj->yaw)) > fmaxf(cone, kMinCone)) {
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

// C# SetGroupTexture (ScriptRuntime.cs:1983) → TriangleGroups.SetMaterial: the
// group must exist and not be group 0; it stores the group's material. The
// scene reads it when it is built — a group retextured mid-play needs a scene
// rebuild to show it (open rendering-integration item).
void ScriptRuntime::setGroupTexture(const std::string& arena, int32_t group, int32_t value) {
    formats::Arena* mesh = level_->arenaNamedMutable(arena);
    if (mesh == nullptr) {
        return;
    }
    mesh->setGroupMaterial(group, value);
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

// ObjectMotion.MoveForward (command 88, set_move_x): accelerate to the max speed
// (or to a stop, parameter 0) and push along the yaw — and the pitch, when the
// object is not held on the ground by gravity.
void ScriptRuntime::moveForward(MdkObject* obj) {
    const float targetSpeed = obj->move_parameter != 0 ? obj->max_speed : 0.0f;
    if (obj->speed > targetSpeed) {
        obj->speed = mathMax(obj->speed - obj->deceleration * kTick, targetSpeed);
    } else if (obj->speed < targetSpeed) {
        obj->speed = mathMin(obj->speed + obj->acceleration * kTick, targetSpeed);
    }
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
        enter = fmaxf(enter, fminf(t0, t1));
        leave = fminf(leave, fmaxf(t0, t1));
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
                if (obj->speed > targetSpeed) {
                    obj->speed = mathMax(obj->speed - obj->deceleration * kTick, targetSpeed);
                } else if (obj->speed < targetSpeed) {
                    obj->speed = mathMin(obj->speed + obj->acceleration * kTick, targetSpeed);
                }
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
                if (obj->speed > targetSpeed) {
                    obj->speed = mathMax(obj->speed - obj->deceleration * kTick, targetSpeed);
                } else if (obj->speed < targetSpeed) {
                    obj->speed = mathMin(obj->speed + obj->acceleration * kTick, targetSpeed);
                }
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
    // One pose: the rest vertices, or the baked animation frame. The C# caches the
    // whole bake per (model, animation); baking per call is correct and the test
    // set is small, so caching is a later optimisation, not a correctness need.
    std::vector<std::vector<Vec3f> > pose;
    if (obj->animation == nullptr) {
        pose = model->restPose();
    } else {
        std::vector<std::vector<std::vector<Vec3f> > > frames = obj->animation->bake(*model);
        int32_t f = obj->animation_frame;
        if (f < 0) {
            f = 0;
        }
        if (f >= static_cast<int32_t>(frames.size())) {
            f = static_cast<int32_t>(frames.size()) - 1;
        }
        if (!frames.empty()) {
            pose = frames[f];
        }
    }
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
    formats::Box box;
    if (obj->model == nullptr) {
        box.min = Vec3f{obj->position.x, obj->position.y, obj->position.z};
        box.max = box.min;
        return box;
    }
    box = poseBounds(obj);
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
// on the snowboard Kurt does. Rides.TakesHits is Ridden != null && no board; the
// ride slice is unported, so ridesRidden stays null and every hit reaches Kurt —
// what the C# reads while nothing is ridden.
void ScriptRuntime::hurtKurt(int32_t damage) {
    if (ridesRidden_ == nullptr || ridesBoard_) {
        kurt().hurt(damage);
        return;
    }

    MdkObject* ridden = ridesRidden_;
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
// The C# also raises its ShakeRaised event for the follow camera. The C++
// camera's shake consumer is not ported yet, so this records the runtime half.
void ScriptRuntime::raiseShake(float amount) {
    shake = mathMax(shake, amount);
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
             fmaxf(bounds.max.z * obj->scale + obj->lift(), kMinHeight) * 0.5f};
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
    MdkObject* other = nullptr;
    if (alienTarget_ != nullptr && !alienTarget_->dead) {
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
// it accelerate. The step is applied to the POSITION (no push, no friction), and the
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

void ScriptRuntime::kill(MdkObject* obj) {
    // C# Kill (ScriptRuntime.cs:1678): Health=0, then either Explode (no death
    // script) or switch to the death script. NoteAlarmEnded is the unported alarm
    // subsystem (no observable state here). The death-script branch is pure VM state
    // and is exercised by the L3 BOLT (its death script ends in opcode 110
    // delete_self, which removes it the next tick). The Explode branch's EFFECTS
    // (PlaySoundAt/FlashAt/BreakUp/SpawnExplosion — mixer, flash, debris, explosion
    // model) are unported, but its removal accounting is not: Explode calls
    // Remove(obj) (cs:1717), so the object leaves the list here too. No golden in
    // levels 3-8 to t30000 exercises this branch — every death in the corpus is a
    // death-script death — so the missing effects stay the documented gap.
    obj->health = 0;
    if (obj->death_script == 0) {
        remove(obj);  // Explode branch: removal accounting only (see CONTINUATION)
        return;
    }
    obj->move_command = 0;
    obj->flags |= MdkObject::FlagNotTarget;
    obj->restart = obj->death_script;
    obj->wait_time = 0.0f;
    obj->wait_resume = obj->death_script;
    obj->death_script = 0;
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

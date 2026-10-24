// ============================================================================
// script_runtime.cpp — see script_runtime.h.
// ============================================================================

#include "scripts/script_runtime.h"

#include <math.h>
#include <string.h>

#include "formats/bin_reader.h"
#include "formats/dti.h"
#include "formats/model_animation.h"
#include "game/bsp.h"

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

float signf(float value) {
    return value > 0.0f ? 1.0f : (value < 0.0f ? -1.0f : 0.0f);
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

ScriptRuntime::ScriptRuntime(LevelData* level, const formats::Cmi* cmi, int32_t seed)
    : level_(level), cmi_(cmi), decoder_(cmi->bytes().data(), (int32_t)cmi->bytes().size()),
      rng_(seed), vm_(*this, decoder_) {
    // The arenas the world is built from, as the driver does: the reachable
    // ones, so the arena under Kurt is found.
    const std::vector<const formats::Arena*>& arenas = level_->arenas();
    for (size_t i = 0; i < arenas.size(); i++) {
        if (level_->isReachable(arenas[i]->name())) {
            space_.add(arenas[i]);
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
}

void ScriptRuntime::update(float) {
    runTick();
}

void ScriptRuntime::runTick() {
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
                // clock crosses its frame. PlaySound itself is not ported (no mixer);
                // the trigger clears the request exactly as the C# does.
                if (!obj->frame_sound.empty() && obj->animation != nullptr &&
                    time < (float)obj->frame_sound_frame &&
                    (float)obj->frame_sound_frame <= time + kTick * obj->animation_rate *
                                                           obj->animation->speed) {
                    obj->frame_sound.clear();
                }
            }
        }
    }

    runControllers();
    tickCount_++;
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
        // C# ApplyVelocity returns here, but Update still records PreviousPosition
        // at its end (ObjectMotion.cs:146), so set it even with no motion.
        obj->previous_position = obj->position;
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
    obj->previous_position = obj->position;  // C# Update end (ObjectMotion.cs:146)
}

// UpdatePickup (0x43d8f0), run after ApplyVelocity for a FlagPickup object. Ports the
// branches reachable before the chute/attach subsystem (command 74) and the Runner's
// RNG-synchronized animations exist: a falling pickup lands on floor contact (Fall),
// and a landed/non-gravity pickup not in the Still list spins in place. The chute
// deploy (velocity.Z below PickupChuteSpeed, unreachable until ~tick 14 at gravity 32),
// the Runner, and the Collected shrink are later slices.
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
    // The Runner's RNG-synchronized animations are a later slice.
    // Landed or non-gravity: a pickup not in the Still list spins in place at
    // PickupTurnSpeed.
    if (obj->type_name != "SW_H150" && obj->type_name != "SW_SEAL" &&
        obj->type_name != "SW_SBONE") {
        obj->yaw = wrap360(obj->yaw + kTick * kPickupTurnSpeed);
    }
}

// Raycast (0x421680): whether any triangle of the active arenas crosses [from,to].
// Only Kurt's arena and the second one stop rays; the rest are passed through. The
// planner only needs hit/no-hit, so the hit point/normal/group are not carried.
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

bool ScriptRuntime::raycastClear(const Vec3& from, const Vec3& to) {
    Vec a{from.x, from.y, from.z};
    Vec b{to.x, to.y, to.z};
    // RayArenas: the current + second arena when a current is set, else every arena
    // (the C# _bsps is filled from level.Arenas at construction, so "all" is the full
    // set, not just the arenas touched so far). bspOf builds each on demand.
    if (!currentArena_.empty()) {
        const std::string* names[2] = {&currentArena_, &secondArena_};
        for (int i = 0; i < 2; i++) {
            Bsp* bsp = bspOf(*names[i]);
            if (bsp == nullptr) {
                continue;
            }
            Vec point{};
            if (bsp->segment(a, b, Bsp::kSegmentAny, &point, Bsp::kClipPassesThrough) != Bsp::kNone) {
                return false;
            }
        }
        return true;
    }
    const std::vector<const formats::Arena*>& arenas = level_->arenas();
    for (size_t i = 0; i < arenas.size(); i++) {
        Bsp* bsp = bspOf(arenas[i]->name());
        if (bsp == nullptr) {
            continue;
        }
        Vec point{};
        if (bsp->segment(a, b, Bsp::kSegmentAny, &point, Bsp::kClipPassesThrough) != Bsp::kNone) {
            return false;
        }
    }
    return true;
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
        obj->speed = fmaxf(obj->speed - obj->deceleration * kTick, targetSpeed);
    } else if (obj->speed < targetSpeed) {
        obj->speed = fminf(obj->speed + obj->acceleration * kTick, targetSpeed);
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

// UpdateCommand (0x45a7d4): the walk/fly dispatch for a commanded object. Only
// move_to (78) is dispatched by the VM so far; the stuck detect/replan is a later
// slice and does not affect the first commanded ticks.
void ScriptRuntime::updateCommand(MdkObject* obj) {
    switch (obj->move_command) {
        case 74:
            followAttachment(obj);
            break;
        case 88:  // MoveForward (command of set_move_x): straight along the yaw
            moveForward(obj);
            break;
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
                    obj->speed = fmaxf(obj->speed - obj->deceleration * kTick, targetSpeed);
                } else if (obj->speed < targetSpeed) {
                    obj->speed = fminf(obj->speed + obj->acceleration * kTick, targetSpeed);
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
                    obj->speed = fmaxf(obj->speed - obj->deceleration * kTick, targetSpeed);
                } else if (obj->speed < targetSpeed) {
                    obj->speed = fminf(obj->speed + obj->acceleration * kTick, targetSpeed);
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
            // DetectStuck then HandleStuck(|yaw - yawAtEntry|) (ObjectMotion.cs:354):
            // both only fire when the command survived Walk/Fly.
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
// when he goes away. The door sounds are the audio slice's (no mixer); everything
// else is state the golden dumps read.
void ScriptRuntime::updateDoor(MdkObject* obj) {
    if (obj->arena != currentArena_ && obj->connects == currentArena_) {
        moveDoor(obj);
    }

    int32_t state = obj->door_state;
    if ((state & kDoorOpening) != 0) {
        if (animationDone(obj)) {
            state = (state & kDoorOpenKept) | kDoorOpen;
        }
    } else if ((state & kDoorClosing) != 0 && animationDone(obj)) {
        state = (state & kDoorScriptBits) | kDoorClosed;
        // The closed sound plays here (obj->door_sounds[3]).
        showArena("");  // the arena behind it goes
    }

    const float dx = kurtPosition_.x - obj->position.x;
    const float dy = kurtPosition_.y - obj->position.y;
    const float dz = kurtPosition_.z - obj->position.z;
    if (sqrtf(dx * dx + dy * dy + dz * dz) >= obj->door_distance) {
        if ((state & (kDoorClosing | kDoorClosed | kDoorStaysOpen)) == 0) {
            restartAnimation(obj, getAnimation(obj, obj->door_animations[1]), false);
            state = (state & kDoorScriptBits) | kDoorClosing;
            // The closing sound plays here (obj->door_sounds[1]).
        }
    } else if ((state & (kDoorOpen | kDoorOpening | kDoorLocked)) == 0) {
        restartAnimation(obj, getAnimation(obj, obj->door_animations[0]), false);
        state = (state & kDoorScriptBits) | kDoorOpening;
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
    // NOTE: kill() is the death-slice placeholder (it only sets dead); the C#
    // FallOut switches a death-scripted object to its death script, while
    // remove() now takes an unscripted falling-out object out of the object
    // list with full removal semantics. The threshold and restore height match.
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

void ScriptRuntime::selectTarget(MdkObject*) {
    targetPosition_ = kurtPosition_;
    targetYaw_ = wrap360(kurtYaw_);
}

// C# FirstOther(obj, type, AnyInstance): the first live object of the same arena,
// of the named type, other than obj. The type compare is case-insensitive (C# Is).
MdkObject* ScriptRuntime::firstOther(MdkObject* obj, const std::string& type) {
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        MdkObject& other = *it;
        if (&other == obj || other.dead || other.arena != obj->arena) {
            continue;
        }
        if (iequalsType(other.type_name, type)) {
            return &other;
        }
    }
    return nullptr;
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
    // The full death path (death script, debris, drops) lands with its slice.
    obj->dead = true;
}

void ScriptRuntime::remove(MdkObject* obj) {
    // C# Remove (ScriptRuntime.cs:1673): Dead, then out of the list. The deque's
    // slots are never vacated (the pointer model depends on it), so the object is
    // marked removed and every reference to it is dropped; the readable list and
    // every Objects iteration skip it through dead + isRemoved.
    obj->dead = true;
    removed_.push_back(obj);
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        MdkObject& other = *it;
        if (other.linked == obj) {
            other.linked = nullptr;
        }
        if (other.leader == obj) {
            other.leader = nullptr;
        }
        if (other.attached == obj) {
            other.attached = nullptr;
        }
        if (other.cow_target == obj) {
            other.cow_target = nullptr;
        }
    }
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

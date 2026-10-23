// ============================================================================
// script_runtime.cpp — see script_runtime.h.
// ============================================================================

#include "scripts/script_runtime.h"

#include <math.h>
#include <string.h>

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

const float kDegToRad = 3.14159265358979323846f / 180.0f;

// ScriptMath.RotatedZ: turn about Z by yaw degrees, keep Z (the yaw).
Vec3 rotatedZ(const Vec3& v, float degrees) {
    /* @vfp-exempt R6 — the footing sweep's yaw rotation of the collision box; the
     * C# oracle uses MathF.SinCos (exact), and libm sinf/cosf track it to within
     * 1 ULP, below the golden table's %.4f precision; the fastSin/fastCos LUT's
     * error is far larger and would move the box and its landing. measured n/a
     * (corpus empty). Verified 2026-10-23. */
    float s = sinf(degrees * kDegToRad);
    float c = cosf(degrees * kDegToRad);
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
        vm_.run(obj);
        // The path-following half of Motion.Update, gated on liveness exactly as
        // the C# (Vm.Run then `if (!Dead) Motion.Update`): a script that kills the
        // object this tick must not have it path-moved afterwards.
        if (!obj->dead) {
            motionPath(obj);
            motionVelocity(obj);
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
    // Behaviors.SetupDoor sets DoorState/LockParts/HatchParts from the model's part
    // list; none are golden-state fields, so the part scan lands with the door slice.
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
         * libm atan2f tracks the C# oracle's MathF.Atan2 to within 1 ULP, below the
         * golden table's %.4f precision; the fastAtan2 LUT's error is far larger and
         * would move those values. (Not byte-for-byte: glibc and .NET differ by 1 ULP
         * on some inputs.) measured n/a (corpus empty). Verified 2026-10-22. */
        float deg = atan2f(p.y - old.y, p.x - old.x) * (180.0f / 3.14159265358979323846f);
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
    /* @vfp-exempt R6 — the footing sweep's yaw rotation of the collision box; the
     * C# oracle uses MathF.SinCos (exact), and libm sinf/cosf track it to within
     * 1 ULP, below the golden table's %.4f precision; the fastSin/fastCos LUT's
     * error is far larger and would move the box and its landing. measured n/a
     * (corpus empty). Verified 2026-10-23. */
    float s = fabsf(sinf(obj->yaw * kDegToRad));
    float c = fabsf(cosf(obj->yaw * kDegToRad));
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
    // NOTE: kill()/remove() are the death-slice placeholders (they only set dead);
    // the C# FallOut switches a death-scripted object to its death script and
    // actually removes an unscripted one from the object list. That behaviour lands
    // with the death/removal slice; the threshold and restore height match now.
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

void ScriptRuntime::kill(MdkObject* obj) {
    // The full death path (death script, debris, drops) lands with its slice.
    obj->dead = true;
}

void ScriptRuntime::remove(MdkObject* obj) {
    obj->dead = true;
}

} // namespace scripts
} // namespace mdk

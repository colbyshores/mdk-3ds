// ============================================================================
// script_runtime.cpp — see script_runtime.h.
// ============================================================================

#include "scripts/script_runtime.h"

#include <math.h>
#include <string.h>

#include "formats/dti.h"

namespace mdk {
namespace scripts {

using formats::Model;

const float ScriptRuntime::kTick = 1.0f / 30.0f;

namespace {

const uint32_t kDtiAlien = 2;
const uint32_t kDtiStatic = 4;
const int32_t kDtiStaticFlags = MdkObject::FlagPickup | MdkObject::FlagNotSolid2 |
                                MdkObject::FlagNoBanking | MdkObject::FlagNotTarget;

float wrap360(float degrees) {
    return degrees - 360.0f * floorf(degrees / 360.0f);
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
        // The path-following half of Motion.Update. Gravity/footing/commands land
        // with their motion slices; at tick 1 path-following is the only motion
        // that moves an object (gravity objects are footing-held at their spawn).
        motionPath(obj);
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
    return (int32_t)readU32(cmi_->bytes().data(), path);
}

int32_t ScriptRuntime::pathKeyFrame(int32_t path, int32_t key) {
    return readS32(cmi_->bytes().data(), path + kPathCountSize + key * kPathKeySize);
}

Vec3 ScriptRuntime::pathVector(int32_t path, int32_t key, int32_t field) {
    const uint8_t* b = cmi_->bytes().data();
    int off = path + kPathCountSize + key * kPathKeySize + kPathFrameSize + field * kPathVectorSize;
    Vec3 v;
    v.x = readF32(b, off);
    v.y = readF32(b, off + 4);
    v.z = readF32(b, off + 8);
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
    if (obj->path == 0) {
        return;
    }
    int32_t path = obj->path;
    if (obj->path_stop >= 0) {
        // Round halves away from zero (ObjectMotion.Round).
        int32_t r = obj->path_time >= 0.0f ? (int32_t)(obj->path_time + 0.5f)
                                           : (int32_t)(obj->path_time - 0.5f);
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
        /* @vfp-exempt R6 — path-following turn; the heading must match the C#
         * oracle's MathF.Atan2 byte-for-byte for golden-state parity, which the
         * fastAtan2 LUT would not reproduce. measured n/a (cold at tick 1).
         * Verified 2026-10-22. */
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

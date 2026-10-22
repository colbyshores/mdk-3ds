#pragma once

// ============================================================================
// script_runtime.h — the state a script runs against (arena objects, Kurt, the
// level's models and scripts).
//
// A port of src/Mdk.Game/Scripts/ScriptRuntime.cs, in its first slice: only
// what the level-3 tick-1 path needs. The full runtime drives Kurt, motion,
// audio, effects, items and the rest; those land in their own slices. What is
// here is the arena state, the spawn machinery, the model lookup, and the tick
// that shows Kurt's arena and runs the arena controllers.
//
// THE TICK-1 CONTRACT. On the first tick Kurt is placed at the level's start,
// the arena under him is found, ShowArena marks it started (spawning its DTI
// aliens, of which HMO_1 has none), the object loop runs over an empty list,
// and RunControllers runs the arena controller, which spawns the level's
// objects. A spawned object runs its type's init script inside Spawn, so its
// health, flags and yaw are set before the tick ends; it does NOT get a motion
// update this tick, so it stays at its spawn position.
//
// STABLE ADDRESSES. Objects and arena controllers are handed to the VM as raw
// pointers, and spawning grows the object list while a script runs. The object
// list is therefore a std::deque (insertion never moves existing elements) and
// the arena states a std::map (node-based), so a pointer taken before a later
// spawn stays valid. See the ownership note in object.h.
// ============================================================================

#include <stdint.h>

#include <deque>
#include <map>
#include <string>
#include <vector>

#include "formats/cmi.h"
#include "formats/model.h"
#include "formats/script_decoder.h"
#include "game/arena_space.h"
#include "game/level_data.h"
#include "render/render_types.h"
#include "scripts/object.h"
#include "scripts/script_vm.h"
#include "util/random.h"

namespace mdk {
namespace scripts {

// Per-arena script state (the arena's embedded object).
struct ArenaState {
    std::string name;
    MdkObject controller;
    bool started = false;
    int32_t flags = 0;
    std::array<float, 4> variables{{0.0f, 0.0f, 0.0f, 0.0f}};

    ArenaState(const std::string& arenaName, int32_t scriptOffset) : name(arenaName) {
        controller.arena = arenaName;
        controller.restart = scriptOffset;
    }
};

class ScriptRuntime {
public:
    static const int32_t kSpawnFlaggedFlags = 0x2008A6;
    static const int32_t kIndestructible = 65000;
    static const int32_t kGroupCount = 16;
    static const float kTick;

    // The level and its CMI are BORROWED and must outlive the runtime (the
    // decoder borrows the CMI's bytes, and the models borrow the level's).
    // The level is non-const because the MTO parses its arenas on demand.
    ScriptRuntime(LevelData* level, const formats::Cmi* cmi, int32_t seed);

    // Advance the world by one frame (the driver calls this once per tick).
    void update(float delta);

    // ---- The seam the VM calls ----
    Random& rng() { return rng_; }
    const std::deque<MdkObject>& objects() const { return objects_; }
    MdkObject* spawn(MdkObject* parent, const std::string& type, const Vec3& position, float yaw,
                     int32_t instance, int32_t script, bool flagged);
    void spawnConnector(MdkObject* obj, const std::string& type, const Vec3& position, float yaw,
                        int32_t instance, const std::string& otherArena, int32_t script);
    // Path following (ObjectMotion's path reads): a path is a run of keys in the
    // CMI; PathPosition interpolates the Hermite segment at time.
    int32_t pathKeyCount(int32_t path);
    int32_t pathKeyFrame(int32_t path, int32_t key);
    Vec3 pathVector(int32_t path, int32_t key, int32_t field);
    Vec3 pathPosition(int32_t path, float time);
    void startPath(MdkObject* obj, int32_t path, float time, const Vec3& origin);
    void showArena(const std::string& arena);
    ArenaState& getArenaState(const std::string& arena);
    void setGroupState(const std::string&, int32_t, int32_t) {}  // triangle groups: later slice
    void kill(MdkObject* obj);
    void remove(MdkObject* obj);
    const Vec3& kurtPosition() const { return kurtPosition_; }
    const Vec3& targetPosition() const { return targetPosition_; }
    void selectTarget(MdkObject* obj);
    const formats::Model* findModel(const std::string& arena, const std::string& type);

    int32_t globalFlags = 0;
    std::array<float, 4> globalVariables{{0.0f, 0.0f, 0.0f, 0.0f}};
    int32_t option = 0;  // the driver sets 0 (the oracle's Option)

    // ---- Readable state (the driver dumps these) ----
    const std::string& currentArena() const { return currentArena_; }

private:
    void runTick();
    void runControllers();
    void spawnDtiAliens(const std::string& arena);

    LevelData* level_;
    const formats::Cmi* cmi_;
    formats::ScriptDecoder decoder_;
    Random rng_;
    ScriptVm vm_;

    std::deque<MdkObject> objects_;
    std::map<std::string, ArenaState> arenas_;
    std::map<std::string, formats::Model> modelCache_;  // global models, stable addresses
    int32_t nextInstance_ = 0;

    ArenaSpace space_;
    std::string currentArena_;
    std::string secondArena_;
    bool secondActive_ = false;
    Vec3 kurtPosition_{};
    Vec3 targetPosition_{};
    float kurtYaw_ = 0.0f;
    float targetYaw_ = 0.0f;
    int32_t tickCount_ = 0;
};

} // namespace scripts
} // namespace mdk

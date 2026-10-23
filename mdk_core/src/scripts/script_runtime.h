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
#include <memory>
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
    // The path-following half of ObjectMotion.Update: advances a path-riding
    // object one tick and turns it along the path. Gravity/footing/commands are
    // later motion slices; at tick 1 path-following is the only motion that moves
    // an object (gravity objects are footing-held at their spawn).
    void motionPath(MdkObject* obj);
    // The gravity + ApplyVelocity half of ObjectMotion.Update (0x45e74c, 0x45e810):
    // gravity integrates velocity.Z, friction damps the horizontal, and the object
    // moves by (velocity + push) * Dt. The BSP sweep that stops an object on a
    // floor (landing/footing) and the fall-out check are the collision slice; until
    // they land, a free-air object moves by motion exactly as the sweep yields when
    // it finds nothing, so free-fall matches the oracle.
    void motionVelocity(MdkObject* obj);
    // Movement command (move_to, opcode 78): records the destination and plans the
    // first waypoint (ObjectMotion.PlanMove). The VM seam; the walk/fly half runs in
    // motionVelocity's command step each following tick.
    void startMove(MdkObject* obj, int32_t command, const Vec3& destination);
    // CanSeeKurt (if_sees_kurt / if_not_sees_kurt): within range, inside the view
    // cone, and with a clear ray from the object's eye to Kurt's chest. The VM seam.
    bool canSeeKurt(MdkObject* obj, float range, float cone);
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
    // Diagnostic: opcodes the scripts reached that this VM does not implement yet.
    const std::vector<std::pair<int32_t, int32_t>>& unimplemented() const {
        return vm_.unimplemented();
    }

private:
    void runTick();
    void runControllers();
    void spawnDtiAliens(const std::string& arena);

    // Collision / footing (ObjectMotion's Sweep 0x45fec4 + the arena BSP/floor maps).
    // The arena's BSP, built once per arena name (C# BspOf); null when the arena
    // is not in the MTO (the sweep then moves by the motion exactly).
    Bsp* bspOf(const std::string& arena);
    // The lowest vertex Z of the arena (C# GetArenaFloor), or -1000 when it has none.
    float getArenaFloor(const std::string& arena);
    // The bounds of the visible parts in the object's current pose (model space):
    // the rest pose when it has no animation, else the baked animation frame.
    formats::Box poseBounds(const MdkObject* obj);
    // Sweeps obj's collision box by motion through its arena BSP, moves obj by the
    // swept result, and writes the contact normal. Returns whether it contacted a
    // face (C# Sweep's non-null first-contact normal).
    bool sweepBox(MdkObject* obj, const Vec3& motion, Vec3* normal);
    // An object that fell below its arena floor by 200 (C# FallOut): removed when it
    // has no death script, else killed and parked on the floor without gravity.
    void fallOut(MdkObject* obj);
    // The movement-command half of ObjectMotion.Update (UpdateCommand 0x45a7d4):
    // walks or flies a commanded object toward its waypoint, adding to push before
    // gravity/ApplyVelocity. Stuck detection/replan are a later slice.
    void updateCommand(MdkObject* obj);
    // PlanMove (0x45a434): the first waypoint is the destination when a ray clears,
    // else the nearest detour around the block.
    void planMove(MdkObject* obj);
    // Raycast (0x421680): whether any triangle of the active arenas crosses the
    // segment. Only the hit/no-hit answer is used by the move planner.
    bool raycastClear(const Vec3& from, const Vec3& to);


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
    std::map<std::string, std::unique_ptr<Bsp>> bsps_;  // per-arena BSP (C# _bsps), stable addresses
    std::map<std::string, float> floors_;               // per-arena floor Z (C# _floors)
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

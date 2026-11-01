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

#include "audio/sound_mixer.h"
#include "formats/bni.h"
#include "formats/cmi.h"
#include "formats/model.h"
#include "formats/model_animation.h"
#include "formats/script_decoder.h"
#include "game/arena_space.h"
#include "game/kurt.h"
#include "game/level_data.h"
#include "render/render_types.h"
#include "scripts/debris.h"
#include "scripts/game_stats.h"
#include "scripts/object.h"
#include "scripts/script_vm.h"
#include "util/random.h"

namespace mdk {

class Player;

namespace scripts {

// Per-arena script state (the arena's embedded object).
struct ArenaState {
    std::string name;
    MdkObject controller;
    MdkObject hit_scripts;
    bool started = false;
    int32_t flags = 0;
    std::array<float, 4> variables{{0.0f, 0.0f, 0.0f, 0.0f}};
    std::array<int32_t, 16> group_hit_flags{{}};
    std::array<int32_t, 16> group_hit_masks{{}};
    std::array<int32_t, 16> group_hit_scripts{{}};
    std::array<int32_t, 16> group_counters{{}};

    ArenaState(const std::string& arenaName, int32_t scriptOffset) : name(arenaName) {
        controller.arena = arenaName;
        controller.restart = scriptOffset;
        hit_scripts.arena = arenaName;
    }
};

class ScriptRuntime {
public:
    static const int32_t kSpawnFlaggedFlags = 0x2008A6;
    static const int32_t kIndestructible = 65000;
    static const int32_t kGroupCount = 16;
    static const int32_t kGroupStopsHit = 0x20;
    static const int32_t kGroupAlwaysHit = 0x40;
    static const int32_t kGroupDestructible = 0x80;
    static const int32_t kGroupHide = 2;
    static const int32_t kGroupShow = 3;
    static const int32_t kHitTypeKurt = -11;
    static const int32_t kHitPart = -3;
    static const int32_t kHitTypePart = -4;
    static const float kTick;
    static constexpr float kTicksPerSecond = 30.0f;

    // The level and its CMI are BORROWED and must outlive the runtime (the
    // decoder borrows the CMI's bytes, and the models borrow the level's).
    // The level is non-const because the MTO parses its arenas on demand.
    // The sound bank is borrowed too; it is what decides whether a script's
    // play_sound names a sound the level has. The Player is borrowed as the
    // live source of Kurt's velocity, feet and yaw. The C# constructor requires its Kurt;
    // keep that decision explicit here too: pass the live Player from a
    // frontend, or pass null deliberately from a harness that does not use
    // Kurt's live state.
    //
    // The sprites archive (TRAVERSE/TRAVSPRT.BNI) is the C#'s `sprites`, and is
    // borrowed for the same reason the CMI is: `Items.GetAnimation` parses the
    // animations the built-in behaviours play out of its bytes (SW_H150's
    // H150_R / H150_I). It is REQUIRED, as in the C# constructor, and there is
    // no default for it on purpose: a runtime built without the archive is a
    // world where those animations do not exist, and because the Runner draws
    // its idle chance only while idle or done, such a world also runs a
    // DIFFERENT random stream from the oracle's. Omitting the argument must be
    // a compile error, not a silent different game; a harness that really has
    // no archive says so by passing nullptr out loud.
    ScriptRuntime(LevelData* level, const formats::Cmi* cmi, int32_t seed,
                  audio::SoundBank* bank, const Player* player,
                  const formats::Bni* sprites);

    // Advance the world by the elapsed time. ScriptRuntime.cs:378-390 keeps
    // its own 1/30-second tick clock: a 1/60-second frontend step completes a
    // tick every second call. The driver may pass a whole tick directly.
    void update(float delta);

    // The C#'s TickBlend (ScriptRuntime.cs:393): how far a frame `ahead` seconds
    // after the last step is from the last tick to the next, 0-1. A frontend that
    // presents faster than the scripts tick passes its own leftover time here and
    // draws the objects with MdkObject::transform_at() at the answer. A fixed-step
    // capture has no leftover to pass: the oracle turns blending OFF there
    // (Viewer.cs:280-282, Blending.Off) and draws the un-interpolated transform,
    // which is what transform_at(1.0f) gives.
    float tick_blend(float ahead) const;

    // ---- The seam the VM calls ----
    Random& rng() { return rng_; }
    const std::deque<MdkObject>& objects() const { return objects_; }
    // The mutable list: the objects the scripts make (the chain links of
    // spawn_chain) are read and changed through it.
    std::deque<MdkObject>& objects() { return objects_; }
    ArenaState& getArenaState(const std::string& arena);
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
    // Automatic banking (ObjectMotion.Bank, 0x43b65c): the only writer of roll from
    // MOTION -- the script VM's turn/reset opcodes are the other writers, and they
    // run before this. Called once per object per tick, after Roll and before
    // PreviousPosition is written, exactly where the C# calls it.
    void motionBank(MdkObject* obj);
    // Movement command (move_to, opcode 78): records the destination and plans the
    // first waypoint (ObjectMotion.PlanMove). The VM seam; the walk/fly half runs in
    // motionVelocity's command step each following tick.
    void startMove(MdkObject* obj, int32_t command, const Vec3& destination);
    // TurnAndJump (ObjectMotion.cs:426): turn towards the move destination at
    // the given degrees-per-second rate; once facing it, enter movement command
    // 229 and set the ballistic velocity for the jump.
    void turnAndJump(MdkObject* obj, float rate);
    // Fire (ScriptRuntime.cs:2659): a projectile from a reference point of the
    // object, flying along its yaw and pitch under the projectile's script.
    // The VM seam for the scripts' fire opcode (case 61). Null when the spawn
    // failed, as the C#'s null return.
    MdkObject* fire(MdkObject* obj, const std::vector<formats::ScriptValue>& origin,
                    const std::string& bullet, int32_t script);
    // CanSeeKurt (if_sees_kurt / if_not_sees_kurt): within range, inside the view
    // cone, and with a clear ray from the object's eye to Kurt's chest. The VM seam.
    bool canSeeKurt(MdkObject* obj, float range, float cone);
    bool kurtLooksAt(const MdkObject* obj, float minRange, float maxRange, float cone);
    void showArena(const std::string& arena);
    void setGroupState(const std::string& arena, int32_t group, int32_t operation);
    void setGroupHitFlags(const std::string& arena, int32_t group, int32_t flags);
    void setGroupHitScript(const std::string& arena, int32_t group, int32_t mask, int32_t script);
    void setGroupCounter(const std::string& arena, int32_t group, int32_t value);
    int32_t groupCounter(const std::string& arena, int32_t group);
    int32_t getKurtFloorGroup();
    int32_t hitGroup(const std::string& arena, int32_t group, int32_t amount, int32_t kind,
                     int32_t weapon);
    void setGroupTexture(const std::string& arena, int32_t group, int32_t value);

    void kill(MdkObject* obj);
    // C# Remove: the object leaves the object list (Dead set, then removed). The
    // C++ deque keeps its slot — every held pointer stays valid — so the object
    // is marked removed here. The oracle clears no inbound reference, so none is
    // cleared here either: the widow still points at its dead partner, and a
    // removed chute still points at its leader. The readable list (isRemoved)
    // skips the removed object, matching the C# list exactly. Kill, in contrast,
    // leaves the object PRESENT with health == 0 and dead == false: `dead` is
    // set only by remove(), in both languages.
    void remove(MdkObject* obj);
    // Whether the object was removed (no longer one of the C#'s Objects).
    bool isRemoved(const MdkObject* obj) const;
    const Vec3& kurtPosition() const { return kurtPosition_; }
    // C# Kurt.Velocity (Kurt.cs:132-133), in u/s: the walking terms, the
    // horizontal knock-down push and the vertical speed. inherit_kurt_vel
    // (opcode 170) reads this at dispatch time. It is distinct from the C#
    // ScriptRuntime.KurtVelocity field: that is the observed position delta
    // multiplied by 30 (below), while this is derived from Kurt's own velocity
    // terms. The bound Player is borrowed and must outlive the runtime. With no
    // Player bound, the source is zero, as it was before the frontend
    // integration.
    Vec3 kurtLiveVelocity() const;
    // Kurt's combat/inventory state (the Hurt/Damage half of Kurt.Combat.cs):
    // hurt_kurt (opcode 109) writes it, if_inventory (175) reads the slots.
    const Kurt& kurt() const { return kurt_; }
    // The C# VM writes the same fields directly (Kurt and the screen,
    // ScriptVm.cs:1580-1600: 215/240/244); the mutable overload is that seam,
    // while reads and the mutators keep the const path above.
    Kurt& kurt() { return kurt_; }
    // C# HurtKurt (ScriptRuntime.cs:2676, 0x46a498): riding the XD2 or the XE,
    // the ridden object takes the hits instead of Kurt; on foot or on the board,
    // Kurt does. Rides is a later slice — nothing mounts Kurt yet — so ridesRidden
    // stays null and Kurt takes every hit, which is what the C# reads while
    // nothing is ridden.
    void hurtKurt(int32_t damage);
    // RaiseShake (ScriptRuntime.cs:1986): the strongest screen shake requested
    // during the current tick. The renderer-side camera consumer is a later
    // slice; this records the oracle's per-tick state.
    void raiseShake(float amount);
    // CameraTrack (ScriptRuntime.cs:1188-1216, opcode 203): calculate the
    // camera pitch requested for an object. The runtime keeps the pitch and
    // its two-tick lifetime; the follow-camera consumer and the save/load
    // snapshot consumer are deferred with their subsystems.
    void cameraTrack(MdkObject* obj, int32_t mode, float height);
    // PlaceNearKurt (ScriptRuntime.cs:1136-1186, opcode 220): one of three X
    // choices near Kurt, then pushed left out of the chosen X range and apart
    // from same-Y objects by twelve units. Only X changes.
    void placeNearKurt(MdkObject* obj, float xMin, float xMax, float yLimit);
    // GunAim (ScriptRuntime.cs:1084-1134, opcode 219): a shooting-gallery gun
    // aims along -y (270 degrees) and leads Kurt by the 1.67 s its shot takes.
    // It sets the receiver's yaw and answers whether the gun fires.
    bool gunAim(MdkObject* obj, float y, float z);
    // ---- The mixer seam (the sounds the scripts make) ----
    // play_sound (opcode 89, 0x442402): the start mode and the place come from
    // `flags`, the point from `position` when that operand is a list.
    void playSound(MdkObject* obj, const std::string& name, int32_t flags,
                   const formats::ScriptValue& position);
    // set_loop_sound (opcode 107): the object's looping sound, replacing the
    // previous one; an empty name only stops it.
    void setLoopSound(MdkObject* obj, const std::string& name);
    // if_sound_playing (opcode 233): a sound by name, playing anywhere.
    bool isSoundPlaying(const std::string& name) const;
    // if_own_sound (opcode 249) reads the mixer's voice table.
    const audio::SoundMixer& mixer() const { return mixer_; }
    const Vec3& targetPosition() const { return targetPosition_; }
    float targetYaw() const { return targetYaw_; }
    void selectTarget(MdkObject* obj);
    MdkObject* alienTarget() const { return alienTarget_; }
    void setAlienTarget(MdkObject* obj) { alienTarget_ = obj; }
    // The first live object of obj's arena whose type matches (C# FirstOther with
    // instance = AnyInstance): face_mode 3 copies its yaw. Null when none.
    MdkObject* firstOther(MdkObject* obj, const std::string& type, int32_t instance = -1);
    MdkObject* findObject(MdkObject* obj, const std::string& type, int32_t mode);
    // link_partners (opcode 228): pair live, unlinked objects of the named
    // type in this arena when they are less than 100 units apart.
    void linkPartners(MdkObject* obj, const std::string& type);
    // The chain that hangs off an object: opcode 29 (spawn_chain) makes it,
    // opcode 49 (release_children) lets go of its links, and movement command
    // 30 moves them.
    // LastFollowerId (ScriptVm.cs:273): the highest instance id of the live
    // followers of the leader, or -1 when it has none.
    int32_t lastFollowerId(const MdkObject* leader) const;
    // LastChainLink (ScriptVm.cs:288): the live same-arena follower with
    // movement command 30 and the highest instance id, or null.
    MdkObject* lastChainLink(const MdkObject* leader);
    // attach_to (opcode 74): bind to the last live same-arena object named TYPE_ID.
    void attachTo(MdkObject* obj, int32_t pointA, int32_t pointB, const std::string& name);
    // command_objects (opcode 4): sends a command to the matching objects of the
    // arena (0x440384). Commands: 1 join a formation around the sender, 7 goto
    // target, 0xFC gosub target, 43 go to a destination. The VM parses the operands
    // (the action, the near-target offset) and calls this with the command.
    void commandObjects(MdkObject* sender, int32_t command, int32_t target, const Vec3& destination,
                        int32_t selector, const std::string& type, float param, int32_t id);
    // A move command's near-target destination (command 43): forward/side metres off
    // the target, scaled by 1% of the distance to it (C# NearTargetDestination).
    Vec3 nearTargetDestination(const MdkObject* obj, float forward, float side);
    // MdkObject.YawTo: the compass yaw (degrees, [0, 360)) from the object to a
    // point — the fmod wrap, not ScriptMath.Wrap360's floor form.
    float yawTo(const MdkObject* obj, const Vec3& point) const;
    // CrossesFloor (0x421708's floor probe): whether an up-facing face of the
    // arena crosses the segment. False when the arena has no BSP, as the C#.
    bool crossesFloor(const std::string& arena, const Vec3& from, const Vec3& to);
    // Raycast (0x421680) as its hit/no-hit answer: whether the segment is clear
    // of every triangle of the active arenas (the C# Raycast's null test).
    bool raycastClear(const Vec3& from, const Vec3& to);
    // Raycast (ScriptRuntime.cs:2886) with its answer: the NEAREST arena hit
    // point across the ray arenas (nearest by distance-squared, as the C#).
    // When normal is non-null, it also receives that hit triangle's normalized
    // geometric normal (the C# RayHit's Normal), or +Z for a degenerate face.
    bool raycastPoint(const Vec3& from, const Vec3& to, Vec3* out, Vec3* normal = nullptr);
    bool raycastGroup(const Vec3& from, const Vec3& to, int32_t* group);
    // FindCoverSpot (ScriptRuntime.cs:2447, opcode 46): pick a DTI cover spot in
    // the object's arena that Kurt cannot see but the object can reach, and command
    // a walk to it (MoveCommand 43). Clears MoveCommand when no spot qualifies.
    void findCoverSpot(MdkObject* obj);
    // FindAdvanceSpot (ScriptRuntime.cs:2413, opcode 197): advance toward Kurt
    // (command 197) via a DTI type-5 spot, or stop (clears MoveCommand).
    void findAdvanceSpot(MdkObject* obj);
    // C# MayCommand (command_objects / if_count_objects): the receiver has no
    // other leader of a higher priority and obeys the sender's priority.
    static bool mayCommand(const MdkObject* sender, const MdkObject* receiver);
    const formats::Model* findModel(const std::string& arena, const std::string& type);
    // Animation playback (MdkObject.PlayAnimation/AdvanceAnimation). The animation
    // operand of anim_once/anim_loop (GetAnimation 0x442e50): a CMI-stored animation
    // named after its offset, or (first u32 == 0) the arena animation named in the
    // record. The 1996 demo's file animations are not reached by the retail decoder.
    const formats::ModelAnimation* getAnimation(MdkObject* obj, int32_t offset);
    void playAnimation(MdkObject* obj, const formats::ModelAnimation* animation, bool loop);
    // MdkObject.RestartAnimation: starts from frame 0 even when already playing.
    void restartAnimation(MdkObject* obj, const formats::ModelAnimation* animation, bool loop);
    void advanceAnimation(MdkObject* obj, float delta);
    // Doors (ObjectBehaviors.UpdateDoor/SetupDoor): a door opens when Kurt comes
    // closer than its distance and closes when he goes away.
    void updateDoor(MdkObject* obj);
    void setupDoor(MdkObject* obj);
    // The object's pose bounds turned into the world (GetWorldBounds): the eight
    // corners scaled, turned by yaw, lifted, and placed at the position.
    formats::Box worldBounds(const MdkObject* obj);
    // The bounds of the visible parts in the object's current pose (model space):
    // the rest pose when it has no animation, else the baked animation frame.
    formats::Box poseBounds(const MdkObject* obj);
    // Kurt's bounding box (GetKurtBox): half-width 0.6, height 5 from his feet.
    formats::Box kurtBox() const;

    int32_t globalFlags = 0;
    std::array<float, 4> globalVariables{{0.0f, 0.0f, 0.0f, 0.0f}};
    int32_t option = 0;  // the driver sets 0 (the oracle's Option)
    // C# ScriptRuntime.KurtVelocity (ScriptRuntime.cs:204): the observed feet
    // delta from the previous tick, multiplied by 30. place_x_near_player (220)
    // uses its X to aim 2.67 s ahead (Lead = 2.67333f); if_gun_aim (219) uses
    // its X to lead the shot the 1.67 s it takes, and its magnitude to decide
    // how likely the gun is to fire at Kurt. It is not Kurt's own velocity
    // (C# Kurt.Velocity; see kurtLiveVelocity above). The per-tick tick writer
    // is in runTick; like the C# public field, a test may set it directly to
    // isolate the consumer.
    Vec3 kurtVelocity{};
    // The alarm countdown shared by movement command 15 and opcode 27. The C#
    // decrements it once at the start of every non-cutscene tick, then command
    // 15 sets it to ten while the alarm object is in Kurt's current arena.
    int32_t alarmTicks = 0;
    // How the sky is drawn (C# ScriptRuntime.SkyMode, opcode 202): 0 shows
    // the sky, 1 clears to black, and any other value keeps the last frame.
    // The renderer-side backdrop consumer and the save/load snapshot consumer
    // are later slices; the VM writes this shared level state exactly as the
    // oracle does.
    int32_t skyMode = 0;
    // camera_track (C# ScriptRuntime.CameraTrackPitch/CameraTrackTicks,
    // opcode 203): the requested follow-camera pitch and how many more ticks
    // it remains active. The renderer-side follow-camera consumer and the
    // save/load snapshot consumer are deferred with their subsystems.
    float cameraTrackPitch = 0.0f;
    int32_t cameraTrackTicks = 0;
    // The strongest screen shake requested during this tick (opcode 135). It
    // is cleared at the start of every simulation tick, as in the oracle.
    float shake = 0.0f;
    // Items.Bomb: the World's Most Interesting Bomb Kurt threw. The Items
    // subsystem is a later slice, so this stays null until it lands, which is
    // what the C# reads when no bomb is in play (if_no_bomb / if_bomb_visible).
    MdkObject* itemsBomb = nullptr;
    // C# ScriptRuntime.ShatterPoint / ShatterDirection (ScriptRuntime.cs:193):
    // the shatter origin the last 138/139 set; 137 reuses them. The direction
    // starts straight up (Vector3.UnitZ in MDK's Z-up world).
    Vec3 shatterPoint{};
    Vec3 shatterDirection{0.0f, 0.0f, 1.0f};
    // The debris subsystem (Debris.cs): shattered groups, sparks, break-ups.
    const Debris& debris() const { return debris_; }
    Debris& debris() { return debris_; }
    // The level's Score-o-matic counts (GameStats.cs: shots, sniper rounds,
    // head shots, enemies created/killed, town flags). Opcode 217 writes
    // headShots; the other writers (chain-gun fire, sniper rounds, the kill
    // path, town flags) land with their own slices — the struct starts at
    // zero, which is what a fresh level has.
    GameStats stats;

    // ---- Readable state (the driver dumps these) ----
    const std::string& currentArena() const { return currentArena_; }
    // The C# TickCount: the completed simulation ticks. It is zero during the
    // first tick and increments after that tick, as the oracle does.
    int32_t tickCount() const { return tickCount_; }
    // The C# Objects.Count: the objects minus the removed ones.
    int32_t objectCount() const { return (int32_t)objects_.size() - (int32_t)removed_.size(); }
    // Diagnostic: opcodes the scripts reached that this VM does not implement yet.
    const std::vector<std::pair<int32_t, int32_t>>& unimplemented() const {
        return vm_.unimplemented();
    }

private:
    void runTick();
    // ObjectBehaviors.ShowBehind: the arena a door opens onto.
    void showBehindDoor(MdkObject* obj);
    // One receiver of command_objects (0x4405d0); true when it joined a formation.
    bool commandObject(MdkObject* sender, MdkObject* receiver, int32_t command, int32_t target,
                       const Vec3& destination, int32_t formationIndex);
    void runControllers();
    void spawnDtiAliens(const std::string& arena);

    // Collision / footing (ObjectMotion's Sweep 0x45fec4 + the arena BSP/floor maps).
    // The arena's BSP, built once per arena name (C# BspOf); null when the arena
    // is not in the MTO (the sweep then moves by the motion exactly).
    Bsp* bspOf(const std::string& arena);
    // The lowest vertex Z of the arena (C# GetArenaFloor), or -1000 when it has none.
    float getArenaFloor(const std::string& arena);
    // Sweeps obj's collision box by motion through its arena BSP, moves obj by the
    // swept result, and writes the contact normal. Returns whether it contacted a
    // face (C# Sweep's non-null first-contact normal).
    bool sweepBox(MdkObject* obj, const Vec3& motion, Vec3* normal);
    // An object that fell below its arena floor by 200 (C# FallOut): removed when it
    // has no death script, else killed and parked on the floor without gravity.
    void fallOut(MdkObject* obj);
    // The movement-command half of ObjectMotion.Update (UpdateCommand 0x45a7d4):
    // walks or flies a commanded object toward its waypoint, adding to push before
    // gravity/ApplyVelocity, then watches for a stall (DetectStuck) and answers it
    // (HandleStuck: replan once, slide through, or give up). Commands 1/6/61/74/
    // 88/30/15 are their own slices; 229 is handled above by turnAndJump and
    // by the command's landing check.
    void updateCommand(MdkObject* obj);
    // ObjectMotion.HandleStuck (0x45b6c8): the stuck object's once-replan, slide,
    // or give-up, driven by how far it turned this update (degrees).
    void handleStuck(MdkObject* obj, float turned);
    // ObjectMotion.MoveForward (command 88, set_move_x): along the yaw/pitch.
    void moveForward(MdkObject* obj);
    // FollowAttachment (command 74): an attached object (a pickup's chute) tracks its
    // leader's reference point and copies its yaw/pitch; it clears its command when
    // the leader is gone.
    void followAttachment(MdkObject* obj);
    // UpdateChain (ObjectMotion.cs:464, movement command 30): the chain links hang
    // off their leader — each link's reference point 0 sits at the previous link's
    // reference point 1, and the links turn 90 degrees apart. The link with instance
    // id 0 drives the chain; a link whose chain is broken lets go of the leader.
    void updateChain(MdkObject* link);
    // Links (ObjectMotion.cs:503): the live followers of the leader, ordered by
    // instance id. The C# keeps the list in a field and reuses it; so does this.
    void chainLinks(const MdkObject* leader);
    // ObjectMotion.FlyProjectile (command 61, fire): straight along the yaw and
    // pitch until the lifetime (parameter) runs out, the raycast hits the arena,
    // or the grown Kurt box is entered (touched; hurt and die when it damages).
    void flyProjectile(MdkObject* obj);
    // PlanMove (0x45a434): the first waypoint is the destination when a ray clears,
    // else the nearest detour around the block.
    void planMove(MdkObject* obj);
    // The replan on a stuck object (0x45a434, Detour): head for the destination
    // again when the waypoint was a detour, else probe detours from 3/4 there
    // and then from itself.
    void replan(MdkObject* obj);
    // Raycast (0x421680): whether any triangle of the active arenas crosses the
    // segment. Only the hit/no-hit answer is used by the move planner.
    // The pickup half of ObjectMotion.Update (UpdatePickup 0x43d8f0), run after
    // ApplyVelocity for a FlagPickup object. A falling pickup lands on floor contact
    // (Fall), or — once it reaches terminal speed — clamps its fall and opens its
    // chute (spawns SW_CHUTE, attach command 74); a landed/non-gravity pickup not in
    // the Still list spins in place, and SW_H150 runs away (motionRunner). The
    // Collected shrink is a later slice.
    void motionPickup(MdkObject* obj);

    // The Runner (ObjectBehaviors.UpdateRunner, 0x43daf4's SW_H150 branch): idle it
    // replays H150_I on a 72-in-32768 chance a tick, and it runs for H150_R when Kurt
    // comes within 20 units. See motionRunner's note on the RNG.
    void motionRunner(MdkObject* obj);

    // C# Items.GetAnimation: a model animation of the sprites archive, parsed once
    // per name (the C#'s _animations dictionary). Null when there is no archive or no
    // such entry.
    const formats::ModelAnimation* itemsAnimation(const char* name);

    // C# PlayOn (ScriptRuntime.cs:3317): a sound following an object, at an
    // offset in its frame, kept so Remove can stop it with the object.
    int32_t playOn(MdkObject* obj, const std::string& name, int32_t start, const Vec3& offset);

    // Debris delegates (Debris.cs Ray / GroupFacets, wired in the constructor):
    // RayIn (ScriptRuntime.cs:2249) and GroupFacets (ScriptRuntime.cs:2261).
    bool rayIn(const std::string& arena, const Vec3& from, const Vec3& to, DebrisRayHit* out);
    bool groupFacets(const std::string& arena, int32_t group, DebrisGroup* out);
    static bool debrisRayTrampoline(void* ctx, const std::string& arena, const Vec3& from,
                                    const Vec3& to, DebrisRayHit* out);
    static bool debrisFacetsTrampoline(void* ctx, const std::string& arena, int32_t group,
                                       DebrisGroup* out);

    const Player* player_ = nullptr;  // borrowed; the live Kurt movement source
    LevelData* level_;
    const formats::Cmi* cmi_;
    formats::ScriptDecoder decoder_;
    Random rng_;
    ScriptVm vm_;
    // C# ScriptRuntime.Mixer, over the level's sound bank. The voices that
    // follow an object hold pointers into objects_, whose deque slots are never
    // vacated (see the ownership note in object.h).
    audio::SoundBank* bank_;
    audio::SoundMixer mixer_;
    Debris debris_;  // C# ScriptRuntime.Debris, sharing rng_ (declared above it)
    // C# _following: the voices started with playOn per object, so Remove can
    // stop them with the object. The C# pools the lists; a vector is pooled by
    // its own allocator, so the spare stack is not ported.
    std::map<MdkObject*, std::vector<int32_t> > following_;
    // CMI-offset animations parsed on demand (C# _animations); the map nodes are
    // stable, so pointers into it stay valid as more animations parse.
    std::map<int32_t, formats::ModelAnimation> animations_;
    // C# ScriptRuntime.Items' sprites archive (TRAVSPRT.BNI), borrowed, and the
    // animations parsed out of it. Same stable-node rule as animations_ above:
    // ObjectBehaviors compares an animation by IDENTITY (`obj.Animation != run`),
    // so the same name must keep answering the same address.
    const formats::Bni* sprites_ = nullptr;
    std::map<std::string, formats::ModelAnimation> itemsAnimations_;
    // The chain followers of the last updateChain call (the C#'s _links list),
    // reused so the per-tick chain does not grow a container.
    std::vector<MdkObject*> chainLinks_;

    std::deque<MdkObject> objects_;
    // The removed (C# Objects.Remove) objects, still in their deque slots.
    std::vector<MdkObject*> removed_;
    std::map<std::string, ArenaState> arenas_;
    std::map<std::string, formats::Model> modelCache_;  // global models, stable addresses
    int32_t nextInstance_ = 1000;

    ArenaSpace space_;
    std::map<std::string, std::unique_ptr<Bsp>> bsps_;  // per-arena BSP (C# _bsps), stable addresses
    std::map<std::string, float> floors_;               // per-arena floor Z (C# _floors)
    std::string currentArena_;
    std::string secondArena_;
    bool secondActive_ = false;
    Vec3 kurtPosition_{};
    Kurt kurt_;  // C# ScriptRuntime.Kurt — the combat/inventory slice (game/kurt.h)
    // Rides.Ridden (Rides.cs:40) and Rides.OnBoard (Rides.cs:89): the ridden
    // object, and whether it is the snowboard. The ride slice (mount/dismount)
    // is unported, so both stay at these values — what the C# reads while Kurt
    // rides nothing, which is every tick of the corpus.
    MdkObject* ridesRidden_ = nullptr;
    bool ridesBoard_ = false;
    Vec3 targetPosition_{};
    Vec3 previousKurtPosition_{};  // C# _previousKurtPosition
    float kurtYaw_ = 0.0f;
    float targetYaw_ = 0.0f;
    MdkObject* alienTarget_ = nullptr;  // C# ScriptRuntime.AlienTarget (opcode 251)
    int32_t tickCount_ = 0;
    float tickAccumulator_ = 0.0f;  // C# ScriptRuntime._time
};

} // namespace scripts
} // namespace mdk

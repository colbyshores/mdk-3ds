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
#include <set>
#include <string>
#include <utility>
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
#include "game/messages.h"
#include "render/render_types.h"
#include "scripts/air_strike.h"
#include "scripts/animated_textures.h"
#include "scripts/debris.h"
#include "scripts/effects.h"
#include "scripts/end_level.h"
#include "scripts/fans.h"
#include "scripts/game_stats.h"
#include "scripts/object.h"
#include "scripts/rides.h"
#include "scripts/script_vm.h"
#include "scripts/sniper_rounds.h"
#include "scripts/strike_scene.h"
#include "scripts/twister.h"
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
    // C# ScriptRuntime.TeleportFlash (:112): the white flash a teleport-with-no-arena
    // leaves on Kurt, as a MathF.Max so a stronger flash already standing is kept.
    static constexpr float kTeleportFlash = 255.0f;
    static const float kTick;
    static constexpr float kTicksPerSecond = 30.0f;

    // The level and its CMI are BORROWED and must outlive the runtime (the
    // decoder borrows the CMI's bytes, and the models borrow the level's).
    // The level is non-const because the MTO parses its arenas on demand.
    // The sound bank is borrowed too; it is what decides whether a script's
    // play_sound names a sound the level has. The Player is borrowed as the
    // live source of Kurt's velocity, feet and yaw, and the owner of Kurt's combat
    // half (see kurt() below). The C# constructor requires its Kurt;
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
    // no archive says so by passing nullptr out loud — and after 2026-11-01 no
    // shipped entry point does: the Linux frontend gates the runtime on the archive
    // and the golden harness fails outright without it, because a runtime without
    // it is a different random stream, not a degraded one.
    ScriptRuntime(LevelData* level, const formats::Cmi* cmi, int32_t seed,
                  audio::SoundBank* bank, Player* player,
                  const formats::Bni* sprites);
    // Unbinds Kurt's item hooks (ScriptRuntime.cs:331-333 subscribes them for the
    // runtime's life) when they still point here: a frontend that replaces its runtime
    // with `reset(new ...)` builds the new one first, and must keep ITS hooks.
    ~ScriptRuntime();
    ScriptRuntime(const ScriptRuntime&) = delete;
    ScriptRuntime& operator=(const ScriptRuntime&) = delete;

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
    // PreloadArena (ScriptRuntime.cs:602-612, arena_set_neighbour 0x41a2d0 / opcode 223,
    // and the DTI type-3 trigger box): the arena takes the SECOND slot and is loaded, but
    // the second stays INACTIVE, so the room is loaded ahead of Kurt rather than drawn and
    // live with him. The `arena == SecondArena` test is the C#'s and it runs BEFORE the
    // assignment, so preloading the arena that is already the second does nothing at all --
    // not even LoadArena, so it pulls that arena's doors no second time.
    void preloadArena(const std::string& arena);
    // TeleportKurt (ScriptRuntime.cs:460-481, teleport_player 0x41bce4 / opcode 112 and
    // teleport_player_keep 173): Kurt goes where the script says, and -- when the script
    // NAMED an arena -- so does the arena state: EnterArena, CurrentArena, TeleportSecond,
    // ShowArena, and the previous position set to the destination so no move crosses a
    // connection. An EMPTY arena name moves Kurt inside his current arena and white-flashes
    // him instead (the level.gd enter_arena split). Public because the VM seam needs it;
    // the C# has it public too and its other callers are the soak tour, the soak test, the
    // level console and the viewer's demo teleports.
    void teleportKurt(const std::string& arena, const Vec3& position, float yaw);
    // Which arena a move enters when it crosses a connection of `arena` (C#
    // CrossedArena, ScriptRuntime.cs:791-803), or "" when it crosses none. The C#
    // keeps this private and reaches it through RunTick and FollowArenas; the port
    // exposes it for the same reason it exposes debris() below — the crossing
    // golden has to name the arena a move enters, not only the arena the tick ended
    // in.
    std::string crossedArena(const std::string& arena, const Vec3& from, const Vec3& to);
    // FollowArenas (ScriptRuntime.cs:780): an object with FlagChangesArena goes
    // into the arena its OWN last move crossed, so the board follows Kurt through a
    // door and its script keeps running (LEVEL4's first run ends in MEAT_3). The
    // ObjectMotion.Update call site is the object loop in runTick, and it carries
    // that method's effect gate: an object with effect_frames > 0 returns from
    // ObjectMotion.Update (:54-68) before FollowArenas (:106) is ever reached. The
    // call site is the inverse of that predicate: `effect_frames <= 0`.
    void followArenas(MdkObject* obj);
    void setGroupState(const std::string& arena, int32_t group, int32_t operation);
    void setGroupHitFlags(const std::string& arena, int32_t group, int32_t flags);
    void setGroupHitScript(const std::string& arena, int32_t group, int32_t mask, int32_t script);
    void setGroupCounter(const std::string& arena, int32_t group, int32_t value);
    int32_t groupCounter(const std::string& arena, int32_t group);
    int32_t getKurtFloorGroup();
    int32_t hitGroup(const std::string& arena, int32_t group, int32_t amount, int32_t kind,
                     int32_t weapon);
    void setGroupTexture(const std::string& arena, int32_t group, int32_t value);
    // ---- Package #15: the group textures' consumer ----
    // C# TriangleGroups.Changed -> LevelView.Rebuild (LevelView.cs:251), as a list the
    // frontend takes once a step instead of an event: the arenas a group_set_texture
    // retextured since clearRetexturedArenas(), each once. The frontend rebuilds their
    // views (ArenaViews::refresh -> LevelView::refresh). Nothing in the simulation reads it.
    // (A group_set_state needs no rebuild: the views read Hidden at draw time, which is
    // the same picture the C#'s Rebuild-on-every-Changed draws.)
    const std::vector<std::string>& retexturedArenas() const { return retextured_; }
    void clearRetexturedArenas() { retextured_.clear(); }

    // C# Kill (ScriptRuntime.cs:1678, object_kill 0x43d670): the object switches to its
    // death script if it has one, otherwise it explodes, the explosion facing `yaw`.
    void kill(MdkObject* obj, float yaw = 0.0f);
    // C# Explode (ScriptRuntime.cs:1710, 0x43d224): its explosion sound (opcode 25's
    // label, or EXPLODE), a white flash by distance, its pieces flying off, its removal,
    // and the global model 0 (EXPLODE, an animated texture) scaled to its height.
    void explode(MdkObject* obj, float yaw);
    // C# SpawnExplosion (ScriptRuntime.cs:1770, 0x43cb2c): the CMI's first model,
    // whose animated texture plays once, one frame per tick, pitched towards the eye
    // `lift` units above it unless the eye is close. Null when it could not spawn.
    MdkObject* spawnExplosion(const std::string& arena, const Vec3& center, float scale,
                              float yaw, float lift);

    // ---- Sniper rounds (package #6: Scripts/SniperRounds.cs, ScriptRuntime.cs:1222-1293) ----
    // C# ScriptRuntime.SniperRounds (:176): the three round slots, updated once a tick after
    // the debris (runTick). bomb_follow_path (opcode 28) reaches guideLastMortar through it.
    SniperRounds& sniperRounds() { return sniperRounds_; }
    const SniperRounds& sniperRounds() const { return sniperRounds_; }

    // ---- The air strike and the strike scene (package #14: Scripts/AirStrike.cs,
    //      Scripts/StrikeScene.cs, ScriptRuntime.cs:178-180, :957, :1022, :1284, :1697) ----
    // C# ScriptRuntime.AirStrike (:178): Bones' strike, updated once a tick after the rounds
    // (runTick); fireSniper calls it. The frontends set usedUp from the saved game's
    // StrikeUsed and read it back when Kurt dies (Viewer.cs:236, :891).
    AirStrike& airStrike() { return airStrike_; }
    const AirStrike& airStrike() const { return airStrike_; }
    // C# ScriptRuntime.Strike (:180): the full-screen strike last played, or null before the
    // first. The frontend drives it (StrikeScene.update/skip) and waits while it is active.
    StrikeScene* strikeScene() { return strikePlayed_ ? &strikeScene_ : nullptr; }
    const StrikeScene* strikeScene() const { return strikePlayed_ ? &strikeScene_ : nullptr; }
    // C# PlayStrikeScene (:1697): plays the full-screen strike (0x4398f0) while the game
    // waits -- a fresh scene each time. The C#'s StrikeSceneChanged event has no reader
    // (nothing subscribes to it in src/), so it is not ported.
    void playStrikeScene(StrikeScene::Kind kind, StrikeScene::Plane plane);
    friend class AirStrike;
    friend class StrikeScene;
    // C# ScriptRuntime.SniperTarget (:177, 0x573a8c): the object locked in the scope, which
    // homing rounds chase; written each tick by updateSniperTarget, null out of sniper mode.
    MdkObject* sniperTarget = nullptr;
    // C# Sweep (:2928, bsp_sweep_box, the mortar round in 0x462708): a box of half extents
    // `half` swept without sliding against Kurt's arena, then the second one. Faces stop it
    // only from their front. The point is the box centre at the contact, the normal the
    // face's plane; the arena and group as Raycast's. False when nothing stops it.
    bool sweep(const Vec3& from, const Vec3& to, const Vec3& half, DebrisRayHit* out);

    // ---- Bullet holes (package #16: Scripts/BulletHoles.cs; scripts/bullet_holes.cpp) ----
    // C# StampBulletHole (ScriptRuntime.cs:1297, special_130 0x45d140, opcode 130): the
    // hole sprite (BHOLE, BHOLE2 without gore) copied onto the texture of the face of
    // shot_part nearest shot_point -- the texture every object using it shares. The
    // texture is recorded for the drawing (stampedTextures) and kept in memory from then
    // on (formats::Texture::pinPixels: the arena stream would otherwise read the hole away).
    void stampBulletHole(MdkObject* obj);
    // C# TextureStamped (:1327), as a list the frontend takes once a step instead of an
    // event: the textures a hole changed since clearStampedTextures(), each once. The
    // frontend re-uploads them (SurfaceCache::refresh, the C#'s MaterialResolver.Refresh
    // subscribed at Viewer.cs:263). Nothing in the simulation reads it.
    const std::vector<const formats::Texture*>& stampedTextures() const { return stamped_; }
    void clearStampedTextures() { stamped_.clear(); }
    // The C# event itself, raised synchronously after EACH stamp (two in one step are
    // two calls, where the list above holds the texture once): the golden harness's
    // view (tick1 dump holes). Null by default.
    typedef void (*TextureStampedHook)(void* user, const formats::Texture* texture);
    void setTextureStampedHook(TextureStampedHook hook, void* user) {
        textureStampedHook_ = hook;
        textureStampedUser_ = user;
    }

    // ---- Contact damage and spawned boxes (hazards.cpp) ----
    // C# TouchDamage (ScriptRuntime.cs:2758, touch_damage 0x45cf60, opcode 158):
    // `targets` 1 hurts Kurt once per visible part touching him, 2 hurts the other
    // objects touching its box (hit event -3, hit type -4). With flags 1 the object
    // dies when it hits. Returns whether it hit.
    bool touchDamage(MdkObject* obj, int32_t targets, int32_t damage, int32_t flags);
    // C# SpawnBox (ScriptRuntime.cs:1593, spawn_box, opcode 159): an object whose model
    // is a box of `size` (model_create_box 0x404188, 8 corners, 12 triangles, not
    // drawn) named after the texture its sprite shows (FIRE, PULSE). No init script
    // runs. The box models are kept by (size, texture), as the C#'s _boxModels.
    MdkObject* spawnBox(MdkObject* parent, const Vec3& position, const Vec3& size,
                        const std::string& texture, int32_t script);
    // C# Boxes (ScriptRuntime.cs:1647): the objects spawn_box made, dead ones too (skip
    // them); their model's name is the texture their sprite shows.
    const std::vector<MdkObject*>& boxes() const { return boxes_; }

    // ---- The chain gun (ScriptRuntime.cs:1816-2137) ----
    // C# FireChainGun (0x41a304): one tick of fire. It hits the best target in front of
    // Kurt (aimScore) or, without one, the arena up to 150 units away. runTick calls it
    // while Kurt fires, where the C# does (after CollectPickups and the KurtVelocity
    // write).
    void fireChainGun();
    // C# ScriptRuntime.Spark (:89): what a spark is made of.
    enum class Spark { Flesh, Hard, Group, Fire };
    // C# SparkAt (:2201, 0x41e8f4): sparks where a shot hits, and a ricochet sound (the
    // given one, or RICO1-RICO3) every 4 ticks, always for bursts and when the arena has
    // no effects.
    void sparkAt(const Vec3& point, int32_t count, const std::string& sound, Spark kind);
    // C# PlaySoundAt (:2503): a sound at a point.
    void playSoundAt(const std::string& name, const Vec3& point);
    // The health bar (C# BarTime/BarObject/BarHealth/BarMax, :224-227): an object's
    // health while the chain gun hits it, or a script's values (boss_bar, opcode 181).
    // showBarValues is the C#'s ShowBarValues (:2157); bar() is GetBar (:2165), zeros
    // while the bar is hidden.
    void showBarValues(int32_t health, int32_t maxHealth);
    struct BarValues {
        int32_t health;
        int32_t max;
    };
    BarValues bar() const;
    float barTime = 0.0f;
    MdkObject* barObject = nullptr;
    int32_t barHealth = 0;
    int32_t barMax = 0;
    // ---- Solid objects and platforms (package #3, scripts/platforms.cpp) ----
    // C# GetKurtPlatform (:2720): the object Kurt stands on (Kurt.Platform, 0x573b84),
    // when he is on a floor; null without a Player. Opcodes 114 (if_player_on_me) and
    // 209 (if_player_on_me_low) read it.
    MdkObject* getKurtPlatform() const;
    // C# SolidsWithin (:2725): what Kurt collides with (damp_collide_move) -- the
    // visible parts of the objects of his arena that are alive, walls or floors
    // (MdkObject.Footing), but the one he rides and doors' LOCK parts. The list is one
    // scratch list, overwritten by the next call (Kurt uses each answer at once).
    const std::vector<solids::Solid>& solidsWithin(const formats::Box& region);
    // C# MdkObject.Footing (MdkObject.cs:57): how Kurt meets the object; false when he
    // passes through it.
    static bool footingOf(const MdkObject* obj, solids::Footing* footing);
    // C# KeeperAt (:2229): where a platform that keeps Kurt is (flag 0x800000, alive).
    static bool keeperAt(const MdkObject* obj, Vec3* at);
    // C# ClipView (:1995, camera_clearance 0x417ee8): the camera's line of sight from
    // `from` to `to` against the objects of Kurt's arena that block it (flag 0x1000000,
    // not 0x810): where it first meets one of their parts, or its end.
    Vec3 clipView(const Vec3& from, const Vec3& to);
    // The FollowCamera's ClipViewHook over clipView (Viewer.cs:237:
    // `new FollowCamera(_space) { ClipView = _scripts.ClipView }`).
    static Vec3 clipViewTrampoline(void* ctx, const Vec3& from, const Vec3& to);
    // ---- end of solid objects and platforms ----
    // C# ScriptRuntime.Eye (:206): the camera's position, which the frontend sets each
    // frame (Viewer.cs:699) and an explosion turns its face to. A harness with no camera
    // leaves it at the origin, as the C#'s headless probe does.
    Vec3 eye{};
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
    // C# ScriptRuntime.KurtYaw (:202), the same per-tick snapshot of Kurt's facing that
    // KurtPosition is of his feet: written at :889 and read by the scripts until the next
    // tick. teleport_player_keep (opcode 173) turns it, so it needs the reader.
    float kurtYaw() const { return kurtYaw_; }
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
    // WITH A PLAYER BOUND this is `player_->combat()`, i.e. the one health the C#
    // has (Kurt.Combat.cs:76): the runtime borrows the entity's combat half exactly
    // as C# Viewer hands its single Kurt to the ScriptRuntime (ScriptRuntime.cs:
    // 168), so hurt_kurt, the death transition and the HUD move on one field.
    // There is deliberately no copy here: a copied Kurt is the two-healths defect,
    // and golden/L3_kurt_hurt.txt is the pin that says so. WITH NO PLAYER BOUND
    // there is no Kurt entity in the world at all, and the runtime's own unbound
    // combat state answers — there is then nothing second to diverge from.
    const Kurt& kurt() const;
    // The C# VM writes the same fields directly (Kurt and the screen,
    // ScriptVm.cs:1580-1600: 215/240/244); the mutable overload is that seam,
    // while reads and the mutators keep the const path above.
    Kurt& kurt();
    // C# HurtKurt (ScriptRuntime.cs:2676, 0x46a498): riding the XD2 or the XE,
    // the ridden object takes the hits instead of Kurt; on foot or on the board,
    // Kurt does (Rides.TakesHits, scripts/rides.h; package #9).
    void hurtKurt(int32_t damage);
    // push_kurt (opcode 248, ScriptRuntime.cs:2826-2852, dispatched at
    // ScriptVm.cs:1600): knock Kurt down and shove him. It is the ONLY writer of a
    // non-zero knock-down push in the oracle, which is why the push's drain and its
    // clearing at the K_BANG -> K_BFLIP flip were unpinned until it landed.
    // The operand list comes in whole because its arity depends on args[0]:
    // [0, a, b, up] takes the push from the object's frame with the lift at args[3],
    // and [n, a, up] shoves Kurt away from the object at a u/s with args[2] up.
    void pushKurt(MdkObject* obj, const std::vector<formats::ScriptValue>& args);
    // RaiseShake (ScriptRuntime.cs:1986): the strongest screen shake requested
    // during the current tick, and the C#'s ShakeRaised event for the follow camera
    // (Viewer.cs:238, `ShakeRaised += _follow.RaiseShake`; package #15).
    void raiseShake(float amount);
    // C# ShakeRaised (ScriptRuntime.cs:218), raised synchronously by every raiseShake
    // with the amount asked: the frontends bind FollowCamera::raiseShakeHook with their
    // camera. Null by default (the C#'s event with no subscriber).
    typedef void (*ShakeRaisedHook)(void* user, float amount);
    void setShakeRaisedHook(ShakeRaisedHook hook, void* user) {
        shakeRaisedHook_ = hook;
        shakeRaisedUser_ = user;
    }

    // ---- Package #15: the town timer's end (ScriptRuntime.cs:43-47, 901-908, 2139) ----
    // (The timer itself, townTicks, is package #19's member below: set from Kurt's
    // difficulty when the runtime is built, counted down in runTick.)
    // C# FlagSecondTown / FlagTownFlattened / FlagSecondTownFlattened: global flags -- the
    // minecrawler went for the second town (set by the game), the first or the second town
    // was flattened (read by the debriefing through GameStats.TownFlags).
    static const int32_t kFlagSecondTown = (int32_t)0x80000000u;
    static const int32_t kFlagTownFlattened = 1 << 30;
    static const int32_t kFlagSecondTownFlattened = 1 << 29;
    // C# TownShake / TownMessageSeconds.
    static constexpr float kTownShake = 5.0f;
    static constexpr float kTownMessageSeconds = 5.0f;

    // ---- Package 8 (scripts/updraft_wind.cpp): Kurt's updraft, the fans' sparks and
    // the wind zones ----
    // C# ScriptRuntime.Wind (ScriptRuntime.cs:2300).
    enum class Wind { Off, On };
    // wind_zone (opcode 224, ScriptRuntime.WindZone, ScriptRuntime.cs:2302-2328, 0x4579e0):
    // while Kurt is inside one of the arena's type-9 boxes, the wind blows him along the
    // yaw at the speed, sliding him on his back; in the air without sliding it only
    // pulls him down twice as fast. Off ends the slide.
    void windZone(const std::string& arena, Wind wind, float yaw, float speed);
    // C# ScriptRuntime.KurtUpdraft (ScriptRuntime.cs:2234-2243, damp_gravity 0x469efc):
    // the vertical speed his arena's fans give him at `feet`, else the second arena's,
    // or NaN outside them. Kurt asks through Player::UpdraftHook (ScriptRuntime.cs:345).
    float kurtUpdraft(const Vec3& feet, float vz, float dt);

    // ---- The end of a level (package 1: ScriptRuntime.cs SpecialEvent/EndTheLevel,
    //      Scripts/EndLevel.cs) ----
    // C# SpecialEvent (ScriptRuntime.cs:1009, opcode 131 special_event, 0x4456d2): 50 and
    // below end the level (EndTheLevel); above 50 are the cutscenes (0x477cf4): the dog
    // (52), Gunter (61), the end of the game (81), the boss intros (91, 93), the shot
    // change (55) and the cutscene's end (53, 92) -- package 13, ported below. Returns
    // true for every value since package 14 ported 51's full-screen strike
    // (PlayStrikeScene): an arm, or the C#'s own no-op for a value its switch has no
    // case for. The bool stays so the VM's reached-but-unported bookkeeping keeps its
    // seam. The VM's control flow is the C#'s: the opcode has no branch, and the next
    // instruction follows.
    bool specialEvent(MdkObject* obj, int32_t value);
    // C# ScriptRuntime.LastEndEvent (ScriptRuntime.cs:59).
    static constexpr int32_t kLastEndEvent = 50;
    // C# ScriptRuntime.LevelOver (0x573b60): the level is over (EndTheLevel's guard).
    bool levelOver = false;
    // C# ScriptRuntime.EndLevel (ScriptRuntime.cs:186): the end running, or null before
    // special_event <= 50. The frontends read it to tear the arena (torn()) and draw the
    // pieces (pieces()) -- the C#'s LevelView.DrawEnd (Viewer.cs:703).
    const EndLevel* endLevel() const { return endLevel_.get(); }
    // C# ScriptRuntime.LevelEnded(GameOver.No) (ScriptRuntime.cs:245, raised by
    // EndLevel.Finished at :1509): a LATCH, since section 10 forbids the event. It is set
    // on the tick the white flash passes its end and stays set; the frontends poll it
    // after their steps (the C#'s subscriber is Viewer.cs:240: the level ends 0.5 s
    // later). GameOver.Yes (the town flattened) belongs to the town timer, package 15.
    bool levelEnded() const { return levelEnded_; }

    // ---- Cutscenes (package 13: ScriptRuntime.cs SpecialEvent 51-93, StartCutscene,
    //      EndCutscene, CutsceneTick, UpdateCutsceneCamera; ScriptRuntime.cs:49-62,
    //      :229-246, :1009-1077, :1337-1492) ----
    // C# Cutscene* (0x573c60, set by special_event): 0 = none.
    static constexpr int32_t kCutsceneDog = 0x34;
    static constexpr int32_t kCutsceneAll = 0x3d;
    static constexpr int32_t kCutsceneStrike = 0x47;
    static constexpr int32_t kCutsceneEnd = 0x51;
    static constexpr int32_t kCutsceneAllFlagged = 0x5b;
    static constexpr int32_t kCutsceneBoss = 0x5d;
    // C# CutsceneHiddenFlags: pickups (0x200000) and Kurt's items (0x1000) are hidden in
    // the cutscenes from kCutsceneAll on.
    static constexpr int32_t kCutsceneHiddenFlags = 0x201000;
    // C# CameraBlendTicks: the shots 1 and 2 ease in over this many ticks.
    static constexpr int32_t kCameraBlendTicks = 1800;
    // C# ScriptRuntime.Cutscene / CutsceneTarget. While Cutscene is not 0 the tick is
    // cutsceneTick() instead of the game's (RunTick's early return, :890-894).
    int32_t cutscene = 0;
    MdkObject* cutsceneTarget = nullptr;
    // The cutscene camera (0x599920...0x599940, C# CameraMode .. CameraPoint): the shot,
    // yaw (90 degrees - heading), pitch (positive down), distance to the target, the
    // stored position and the blend timer in ticks; the frontends look from cameraPoint
    // (game/cutscene_camera.h, the C#'s CutsceneCamera).
    int32_t cameraMode = 0;
    float cameraYaw = 0.0f;
    float cameraPitch = 0.0f;
    float cameraDistance = 0.0f;
    Vec3 cameraPosition{0.0f, 0.0f, 0.0f};
    int32_t cameraTimer = 0;
    Vec3 cameraPoint{0.0f, 0.0f, 0.0f};
    // C# FindObjectNamed (ScriptRuntime.cs:1338): the first live object of a type (the
    // cutscenes' targets), in the object list's order; null when none.
    MdkObject* findObjectNamed(const std::string& type);
    // C# ScriptRuntime.GameFinished (ScriptRuntime.cs:246, raised by special_event 81):
    // a LATCH, as levelEnded() is (section 10 forbids the event). The C#'s subscriber is
    // Viewer.cs:241 (`_next = Event.GameFinished`, at once: the end movie, MDKEND.FLC and
    // MDKBZK.MVE, then the credits); the frontends poll it after their steps.
    bool gameFinished() const { return gameFinished_; }
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
    // The frontend owns the listener and the frame's mix: the C# sets
    // ListenerPosition/Right/Forward/Up/ScopeZoom and calls _mixer.Update from
    // DrawScene (Viewer.cs:688-696), so the mixer has to be reachable to be told.
    audio::SoundMixer& mixer() { return mixer_; }
    const Vec3& targetPosition() const { return targetPosition_; }
    float targetYaw() const { return targetYaw_; }
    void selectTarget(MdkObject* obj);
    MdkObject* alienTarget() const { return alienTarget_; }
    void setAlienTarget(MdkObject* obj) { alienTarget_ = obj; }
    // Rides.Ridden (Rides.cs:40): the object Kurt rides, or null on foot; opcode 171
    // (if_is_573c30) branches on it. C# ScriptRuntime.Rides (package #9).
    MdkObject* ridesRidden() const { return rides_.ridden(); }
    Rides& rides() { return rides_; }
    const Rides& rides() const { return rides_; }
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
    // PickWaypoint8 (ScriptRuntime.cs:2338, opcode 221): pick one of the arena's
    // DTI type-8 waypoints by weighted random (command 221) and make it the
    // object's destination. Mode 1 keeps the waypoints whose id is within 3 of the
    // nearest one; any other mode keeps those 50-500 units away. Clears MoveCommand
    // when nothing qualifies. Command 221 has no per-tick consumer in the oracle's
    // UpdateCommand, so setting the destination is the whole effect.
    void pickWaypoint8(MdkObject* obj, int32_t mode);
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

    // Blast damage math (Items.cs), the foundation of Items.Blast -- ported ahead
    // of the dispatcher so the falloff is pinned by tests before it is wired.
    // C# KurtDamage (ScriptRuntime.OneHit.cs:21): the damage of Kurt's hit on an
    // object with `health` left. The one-hit console cheat (Lethality::OneHit)
    // makes a hit take all an enemy's remaining health; that console setting is
    // unported, so Lethality is always Normal and this returns `damage` -- the
    // value the C# returns for every non-cheat game, which is every tick of the
    // corpus.
    int32_t kurtDamage(MdkObject* obj, int32_t damage);
    int32_t kurtDamage(MdkObject* obj, int32_t damage, int32_t health);
    // C# BlastHit (Items.cs:783): a counted blast scales by KurtDamage; an
    // uncounted one (Kills.Ignored) deals the raw damage.
    int32_t blastHit(MdkObject* obj, int32_t damage, int32_t health, bool counted);
    // C# BlastDamage (Items.cs:788): a blast's damage on a world box and its
    // distance -- falloff to the box centre less half the box's diagonal, 0 beyond
    // the radius or behind a wall. Returns {damage, distance}.
    std::pair<int32_t, float> blastDamage(const formats::Box& box, const Vec3& center,
                                          int32_t damage, float radius);
    // C# MdkObject.PartBounds(): each part's box in the object's current pose
    // (model space), invalid for an empty part (the C# Box?[] with null entries).
    // Reads the cached pose_parts(). Unlike poseBounds, it does NOT skip hidden
    // parts -- the C# PartBounds has no HiddenParts filter; callers (BlastWeakParts,
    // Solids, Aim) apply the hidden_parts bit themselves.
    struct PartBox {
        bool valid;
        formats::Box box;
    };
    std::vector<PartBox> partBounds(const MdkObject* obj);
    // C# GetWorldBounds(obj, bounds): the worldBounds transform applied to a
    // caller-supplied box (a part's box) instead of the object's pose bounds.
    formats::Box worldBounds(const MdkObject* obj, const formats::Box& bounds);
    // C# ScriptRuntime.IsWeakPart (ScriptRuntime.cs:2059): a part is weak when its
    // name is longer than the weak-prefix length, the character at that index is an
    // ASCII digit, and the name starts with the weak prefix (first weak_prefix_length
    // characters of weak_prefix, ordinal).
    static bool isWeakPart(const MdkObject* obj, int32_t index);
    // C# Items.BlastWeakParts (Items.cs:680): a blast's effect on an object's weak
    // parts. Returns the best per-part damage, its distance, the point (that part's
    // world-box centre), and the destroyed-part event (part index + 1, or 0). Mutates
    // obj.part_health. `counted` is Kills.Counted (Kurt's blast) vs Kills.Ignored.
    struct BlastPartResult {
        int32_t best;
        float distance;
        Vec3 point;
        int32_t event;
    };
    BlastPartResult blastWeakParts(MdkObject* obj, const Vec3& center, int32_t damage,
                                   float radius, bool counted);

    int32_t globalFlags = 0;
    std::array<float, 4> globalVariables{{0.0f, 0.0f, 0.0f, 0.0f}};
    // Gore (C# ScriptRuntime.Option, if_option, 0x5742dc): 1 on, 0 off. The C#
    // initialises this field to 1 (ScriptRuntime.cs:214) and the Viewer overwrites
    // it from the gore setting, which also defaults to on (Settings.cs:75,
    // Viewer.cs:234), so 1 is the oracle's default and the port's. A driver that
    // wants the gore-off build sets 0; leaving the field alone is the default run.
    int32_t option = 1;
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
    // Both frontends draw their backdrop from it (game/sky_modes.h, the C#'s
    // SkyModes.BackdropOf / ClearOf in Viewer.Background; package #15); the
    // save/load snapshot consumer waits for the snapshot slice.
    int32_t skyMode = 0;
    // camera_track (C# ScriptRuntime.CameraTrackPitch/CameraTrackTicks,
    // opcode 203): the requested follow-camera pitch and how many more ticks
    // it remains active. BOTH frontends consume the pitch now: the 3DS frontend's
    // step loop eases the follow camera to it ahead of the arena's rest pitch
    // (mdk_3ds/src/main_3ds.cpp, the `PitchGoal` of Viewer.cs:746-747), and the
    // Linux frontend reads it the same way. What is still deferred is the save/load
    // snapshot consumer, which waits for the snapshot slice.
    float cameraTrackPitch = 0.0f;
    int32_t cameraTrackTicks = 0;
    // The strongest screen shake requested during this tick (opcode 135). It
    // is cleared at the start of every simulation tick, as in the oracle.
    float shake = 0.0f;
    // Items.Bomb / Items.Decoy (0x573c24, 0x573c20): the World's Most Interesting
    // Bomb and the decoy Kurt threw, while they are out. if_no_bomb / if_bomb_visible
    // read the bomb; selectTarget aims the aliens at the walking decoy.
    MdkObject* itemsBomb = nullptr;
    MdkObject* itemsDecoy = nullptr;

    // ---- Kurt's items (Items.cs, 0x46ce78 throws, 0x43deac updates) ----
    //   use ──► thrown (pickup model, 25 u/s forward, 15 up) ──lands, hits, times out──► active:
    //     grenade: blast │ decoy: walks, aliens aim at it │ bomb: spins, then blast │ tornado: twisters
    //     mortar: thumps │ key: the nuke │ seal, bone: pickups again
    // Thrown items' flags (0x1000) and active ones' (0x4000); the XE's bombs' kind.
    static const int32_t kItemFlagThrown = 0x1000;
    static const int32_t kItemFlagActive = 0x4000;
    static const int32_t kItemKindBomb = 0x81;
    // C# CollectPickups (ScriptRuntime.cs:2573, damp_collect_pickups 0x46c448): Kurt takes
    // the pickups his tick's move crossed (their bounds grown by 1, and 5 downwards).
    void collectPickups();
    // Items.CanUse / TriggerBomb / UseItem (Items.cs:155-210), Kurt's ItemHooks.
    bool itemsCanUse(Kurt::Item item);
    void itemsTriggerBomb();
    void itemsUseItem();
    // Items.Blast (Items.cs:614, 0x463a94): `targets` 1 Kurt, 2 objects, 4 triangle groups.
    // `counted` is Kills.Counted (Kurt's blasts: KurtDamage, the kill counted).
    static const int32_t kBlastKurt = 1;
    static const int32_t kBlastObjects = 2;
    static const int32_t kBlastGroups = 4;
    void blast(const Vec3& center, int32_t damage, float radius, int32_t targets, int32_t hitType,
               MdkObject* source, bool counted = true);
    // Items.Twisters: the tornado's twisters, in the C#'s list order.
    const std::vector<Twister>& twisters() const { return twisters_; }
    // C# GroupCenters (ScriptRuntime.cs:1963): the centres of a group's triangles, none
    // while the group is not solid.
    void groupCenters(const std::string& arena, int32_t group, std::vector<Vec3>* out);
    // C# SparkFire (ScriptRuntime.cs:2215): a burst of fire sparks (the nuke's blast).
    void sparkFire(const std::string& arena, const Vec3& point);
    // C# FindArenaAnimation (ScriptRuntime.cs:3208): an arena's animation by name.
    const formats::ModelAnimation* findArenaAnimation(const std::string& arena,
                                                      const std::string& name);
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

    // Arena texture animation (AnimatedTextures.cs, opcode 133): the
    // script-driven texture frames. The renderer-side consumer (drawing
    // the animated frame) is a later slice; the VM writes this state
    // exactly as the oracle does.
    AnimatedTextures animatedTextures;

    // The arenas' fans (Fans.cs, opcodes 142/143/190): created by
    // scripts, lifting what their boxes hold. The updraft consumers
    // (Kurt's and the objects' motion) ask fans_.query with their mask.
    Fans fans;

    // The sprite effects (Effects.cs, opcodes 128/132/136): wounds,
    // drops, bubbles, trails. Their billboard draw (EffectsView) is a
    // later slice; the pool and its motion are the runtime state.
    Effects effects;

    // C# ScriptRuntime.Messages (ScriptRuntime.cs:188): the HUD's message stream,
    // which hud_message (opcode 247, ScriptVm.cs:1186) pushes into. BORROWED; the
    // frontends bind their HudView's (Viewer.cs:245) when they build the runtime.
    Messages* hudMessages = nullptr;

    // One hud_message call, as the VM made it. With no stream bound (the C#'s null
    // Messages: nothing is shown) the runtime keeps the calls here instead, for the
    // VM tests to read; with one bound nothing is kept, so a level grows no list.
    struct ScriptMessage {
        std::string text;
        int32_t flags = 0;
        float seconds = 0.0f;
    };
    const std::vector<ScriptMessage>& messages() const { return messages_; }
    void pushMessage(const std::string& text, int32_t flags, float seconds) {
        if (hudMessages != nullptr) {
            hudMessages->push(text.c_str(), flags, seconds);
            return;
        }
        ScriptMessage message;
        message.text = text;
        message.flags = flags;
        message.seconds = seconds;
        messages_.push_back(message);
    }

    // ---- Readable state (the driver dumps these) ----
    const std::string& currentArena() const { return currentArena_; }
    // The door bubble (frontend residency, not game state): the arenas
    // through the current arena's doorways whose doorway box comes within
    // `radius` of `position`, appended to `out` (deduped, current and
    // empty excluded). Read-only: it changes no script state, so the
    // drawn list's one writer rule is untouched -- the frontend uses it
    // only to START residency work (views, warm, music) earlier than the
    // scripts' own second-arena trigger would.
    void doorBubbleArenas(const Vec3& position, float radius,
                          std::vector<std::string>* out) const;
    // C# SecondArena / SecondActive (ScriptRuntime.cs:259-260): the arena left
    // behind on a crossing, and whether it still counts as one of Kurt's arenas.
    const std::string& secondArena() const { return secondArena_; }
    bool secondActive() const { return secondActive_; }
    // C# ScriptRuntime.SolidArenas (ScriptRuntime.cs:265-282): the arenas Kurt
    // COLLIDES with, which the oracle hands ArenaSpace::setSolid every step
    // (Viewer.cs:533). It is the DRAWN list -- the current arena plus the active
    // second, the list UpdateArenas builds at :658-664 -- and NOT the arena under
    // Kurt's feet: it moves on a doorway crossing or a teleport and stays put while
    // he falls out of the world.
    //
    // It is a SNAPSHOT, not a live view of the three fields. The C# fills _drawnArenas
    // only at the end of UpdateArenas (:694-695) and UpdateArenas runs EARLY in RunTick
    // (:935): after the doorway crossing and CheckTriggers, but BEFORE the object loop
    // and the arena controllers. So a showArena that lands later in the same tick -- a
    // door closing (ObjectBehaviors.cs:99) or opening (:411), or an arena_show opcode
    // (ScriptVm.cs:474) -- does NOT change what the next step collides against; that
    // list changes on the NEXT tick's snapshot. Deriving the list from the fields here
    // instead was one tick early for every one of those writers (review-4 F1), which
    // drops the active second a whole tick before the oracle does: over CDANT_1 a fall
    // then finds no floor, and bottom() reports the wrong arena's minimum Z.
    // updateDrawnArenas() is the ported drawn-list half of UpdateArenas and is the only
    // writer; before the first tick the list is EMPTY, exactly as the C#'s _drawnArenas
    // is then, which is no pin at all.
    //
    // The current arena is pushed UNCONDITIONALLY by that snapshot, as :660 does, so an
    // arena-less runtime snapshots a list of one name that ArenaSpace::setSolid cannot
    // resolve (its C# Named() answers null) rather than a list this function emptied. The
    // snowboard half is the oracle's too: on the board Kurt collides with his own
    // arena ALONE (ScriptRuntime.cs:274-280), so the active second drops out. The
    // snowboard (package #10) reaches that branch from the mount on (golden/L4_board_*.txt),
    // and it is the one case where a one-name pin IS the oracle's value.
    //
    // The scratch is a member, not a temporary, for the C#'s own reason ("kept: no
    // list per step", :285): a frontend reads this once per simulation step.
    const std::vector<std::string>& solidArenas() const;
    // C# ScriptRuntime.DrawnArenas (ScriptRuntime.cs:262): the drawn list ITSELF, without
    // the snowboard's drop of element 1 (solidArenas() applies it while Kurt rides). solidArenas() above is what the
    // COLLISION world reads (Viewer.cs:533 SetSolid); this is what the VIEW reads in the
    // oracle (Viewer.cs:702 `_view.Draw(_scripts.DrawnArenas, ...)`) and what the rest of
    // UpdateArenas will read when the loaded-arena bookkeeping lands. Read it instead of
    // reaching for currentArena()/secondArena(): those two are the tick's END, and this is
    // its snapshot -- which is exactly the mistake review-4 F1 removed from the collision
    // path. Same object as solidArenas() off the board, and the only writer is
    // updateDrawnArenas().
    //
    // No view reads the list itself: the frontends draw on obj.visible, which
    // updateDrawnArenas() derives from it (and the cutscenes, cutsceneTick(), override), so a
    // wrong order here would redden only a test. That is why the trigger cases assert it directly
    // (size and order), rather than trusting it as an oracle for the view.
    const std::vector<std::string>& drawnArenas() const { return drawnArenas_; }
    // C# ScriptRuntime._residentArena (:312): the last arena whose MTO was resident, i.e.
    // the last arena LoadArena found in the MTO rather than a corridor. The C# reads it in
    // exactly one place, IsCorridorLoaded (:522-531), and that reader is ported
    // (`isCorridorLoaded`, script_runtime.cpp) -- so this field IS read by the scripts, and
    // has been since the teleport landed. The accessor exists for the separate reason the
    // comment used to give: it lets a test pin the write instead of trusting the reader.
    const std::string& residentArena() const { return residentArena_; }
    // The scripts' ArenaSpace. THE PORT HAS TWO WHERE THE C# HAS ONE: the oracle's Viewer
    // builds a single ArenaSpace and hands the SAME object to Kurt and to the runtime
    // (Viewer.cs:194, :207), so EnterArena's add is immediately a collision surface. Here
    // the frontend gives the Player its own and the runtime builds its own from the
    // reachable arenas (the constructor, as the driver does). This one is the one the
    // scripts read: it is what runTick's geometric arena lookup uses and what enterArena()
    // adds to, so a teleport-only arena reaches the ARENA RULE here and not yet the
    // collision. Unifying the two is recorded as an open item, not papered over. Exposed
    // for the reason residentArena() is: so the write is pinned by a test instead of
    // looking like dead state.
    ArenaSpace& arenaSpace() { return space_; }
    // The C# TickCount: the completed simulation ticks. It is zero during the
    // first tick and increments after that tick, as the oracle does.
    int32_t tickCount() const { return tickCount_; }
    // The C# Objects.Count: the objects minus the removed ones.
    int32_t objectCount() const { return (int32_t)objects_.size() - (int32_t)removed_.size(); }
    // Diagnostic: opcodes the scripts reached that this VM does not implement yet.
    const std::vector<std::pair<int32_t, int32_t>>& unimplemented() const {
        return vm_.unimplemented();
    }

    // ---- Full saves (package #19; script_snapshot.cpp, C# ScriptRuntime.Snapshot.cs) ----
    //
    // The level's state and Kurt's for a full save (F2): the runtime's variables,
    // the arenas' states and script objects, the objects with every public field
    // the C#'s MdkObject has (Scripts/Snapshot.cs packs them by reflection; the
    // port lists them in the same order), the fans, the bomb and the decoy, the
    // triangle groups and the level's counts. The text is the C#'s byte for byte
    // (util/json.h), so a C# save loads here and this one loads in the C#.
    //
    // C# CanSnapshot (Snapshot.cs:24): no cutscene running (`cutscene` == 0), the
    // level not over, no EndLevel, no air strike out, not riding, Kurt alive, and in
    // an arena. The Viewer also refuses while Kurt snipes; that half
    // is the frontend's (Player::sniping).
    bool canSnapshot() const;
    // C# Capture (Snapshot.cs:29): {"kurt": ..., "level": ...}. Needs the bound
    // Player (Kurt is saved from it); an empty string without one.
    std::string capture() const;
    // C# Load (Snapshot.cs:32-40): the level and Kurt of a full save, BEFORE the
    // first tick (the objects are created again and their scripts carry on where
    // they were). False, with `why`, when the text is not a save this runtime can
    // read; the runtime is then as the restore left it, which is not a level to
    // play -- the frontend reloads the level.
    bool load(const std::string& text, std::string* why);
    // C# Snapshot.Hash: the first 16 hex digits of the SHA-256 of a save's text
    // (the C#'s log line and the goldens compare a save with its reload by it).
    static std::string snapshotHash(const std::string& text);
    // The arenas a restore entered (C# EnterArena at the end of Restore, :119-124):
    // the C# adds them to the ONE ArenaSpace Kurt and the scripts share. The port
    // has two (docs/CONTINUATION.md "the two ArenaSpaces"), so a frontend adds
    // these to Kurt's space after load(), or a save made in an arena only a
    // teleport reaches would leave Kurt in an arena his collisions do not have.
    std::vector<std::string> enteredArenas() const;

    // C# TownTicks (ScriptRuntime.cs:255): the town's timer, set from the
    // difficulty when the runtime is made (not on the last level) and counted down
    // once a tick. A save carries it. At 0, FlattenTown (package #15: the shake,
    // the OOT_L message and the global flag); the C# never raises GameOver for it,
    // so a flattened town does not end the game.
    int32_t townTicks = 0;
    // (C# AirStrike.UsedUp is airStrike().usedUp since package #14; a save carries it.)
    // The arenas' states in the order they were made (the C#'s Dictionary order,
    // which the save follows; arenas_ is a map and sorts).
    const std::vector<std::string>& arenaOrder() const { return arenaOrder_; }

private:
    // FindConnections (ScriptRuntime.cs:856): pair the DTI type-6 connection
    // records of every arena by their id, so (arena, id) names the arena on the
    // other side of that doorway. Called once from the constructor, as the C# does.
    void findConnections();
    // C# ArenaRecords (:834): an arena's DTI records, found once. Null when no
    // arena has that name, which is the C#'s empty list.
    const std::vector<formats::Dti::Record>* arenaRecords(const std::string& arena);
    void runTick();
    // CheckTriggers (ScriptRuntime.cs:707-740, 0x41bf1c): the DTI trigger boxes of KURT'S
    // arena -- type 1 shows the arena of its id, type 3 loads it ahead, and an id of -1 (or
    // one past the arena list) names no arena, which shows "" and so clears the second.
    // Called from runTick at the oracle's own position, immediately above
    // updateDrawnArenas(), because it is a writer of the three fields that snapshot reads.
    void checkTriggers();
    // LoadArena (ScriptRuntime.cs:615-623, arena_load 0x419d00): records the resident
    // arena when the MTO has it, and pulls its doors.
    void loadArena(const std::string& arena);
    // PullDoors (ScriptRuntime.cs:628-643): the doors leading INTO `arena` are taken out
    // of the neighbour that holds them and turned around, so Kurt meets them in the room he
    // is walking towards -- except from his own arena and the active second, which keep
    // theirs.
    void pullDoors(const std::string& arena);
    // EnterArena (ScriptRuntime.cs:449-456): the arena joins Kurt's collision space, which
    // is how an arena no doorway leads to becomes solid. The C# also raises its ArenaEntered
    // event; its one subscriber is LevelView.Enter (Viewer.cs:255), which marks the arena
    // Reachable for the DRAW, and the level view is presentation this port defers.
    void enterArena(const std::string& arena);
    // TeleportSecond (ScriptRuntime.cs:496-519, 0x41bce4): what happens to the SECOND slot
    // when a teleport moves Kurt. None into a room; into a corridor that is not loaded, the
    // last arena in DTI order that leads to it, loaded ahead; into a corridor already
    // loaded, unchanged.
    void teleportSecond(const std::string& arena, const std::string& from);
    // IsCorridorLoaded (ScriptRuntime.cs:522-531): the corridor is Kurt's present, his
    // previous arena, or a corridor of the resident arena.
    bool isCorridorLoaded(const std::string& corridor, const std::string& from);
    // ObjectBehaviors.ShowBehind: the arena a door opens onto.
    void showBehindDoor(MdkObject* obj);
    // One receiver of command_objects (0x4405d0); true when it joined a formation.
    bool commandObject(MdkObject* sender, MdkObject* receiver, int32_t command, int32_t target,
                       const Vec3& destination, int32_t formationIndex);
    void runControllers();
    // The DRAWN-LIST half of C# ScriptRuntime.UpdateArenas (ScriptRuntime.cs:648-699),
    // called at the oracle's own position in the tick (see runTick). The rest of that
    // method -- putting away the objects of arenas no longer loaded, their loop sounds,
    // and obj.Visible -- is unported, so this caches only the list SolidArenas returns.
    // It is the ONLY writer of drawnArenas_, and it must stay the only one: the C#'s
    // _drawnArenas is assigned nowhere else (:694-695), which is what makes the pin a
    // snapshot of the tick's start rather than of its end.
    void updateDrawnArenas();
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
    // (HandleStuck: replan once, slide through, or give up). Every command the C#
    // dispatches has an arm here now: 1 formation, 6 chase, 43/78/197 walk-or-fly,
    // 61 projectile, 74 attachment, 88 forward, 30 chain, 15 alarm, and 229, which
    // turnAndJump launches and its landing check ends.
    void updateCommand(MdkObject* obj);
    // ObjectMotion.TurnTowards (ObjectMotion.cs:766): turn the yaw toward a point by
    // at most TurnSpeed × Dt, and return the angle that was still to turn BEFORE the
    // turn. The oracle's Walk and Fly read that return value for their speed target
    // (ObjectMotion.cs:818, 837). This port's arms (case 43/78/197) do not call here:
    // they compute the same `diff` inline, so the return value is unused by them, and
    // the two callers that do call — followFormation and chase — discard it exactly as
    // the oracle's do. Routing Walk and Fly through here would be faithful (the oracle
    // calls it from inside Fly's own NoTurning guard, and returning the pre-turn angle
    // is what the helper is FOR); it is simply not what this file does today, so do not
    // read the inline copies as required by anything other than history.
    float turnTowards(MdkObject* obj, const Vec3& point);
    // FollowFormation (ObjectMotion.cs:929, movement command 1): keep the slot
    // command_objects gave the object around its leader. A follower whose leader is
    // gone lets go (command 0); one within one step is placed exactly on the slot and
    // turns toward the leader's heading; the rest push along the yaw toward it.
    void followFormation(MdkObject* obj);
    // Chase (ObjectMotion.cs:894, movement command 6, move_to_target): fly at a
    // point at a speed in units per TICK (not per second), gaining speed when the
    // point is ahead, braking when it is to the side, and climbing a quarter of the
    // step toward the point's Z.
    void chase(MdkObject* obj, const Vec3& point);
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
    // C# Raycast (ScriptRuntime.cs:2886) with its whole RayHit: the nearest hit's point,
    // the triangle's normal, its arena and its group. The chain gun's wall shot needs the
    // arena and group (HitGroupAt) as well as the point.
    bool raycastHit(const Vec3& from, const Vec3& to, DebrisRayHit* out);
    // C# UpdateSniperTarget (:1227, the lock during projection 0x43b65c): the nearest
    // object whose screen box overlaps the lock square, not flagged 0x30; the zoom limit
    // on it (0x4678b0). Called from runTick after SniperRounds.Update, as the C# does.
    void updateSniperTarget();
    // C# FireSniper (:1282), Kurt.SniperFire: a round of `type`, or Bones' air strike
    // (AirStrike::call, package #14). False when nothing went.
    bool fireSniper(int32_t type);
    static bool fireSniperTrampoline(void* ctx, int32_t type);
    friend class SniperRounds;
    // C# ArenaTexture (ScriptRuntime.cs:1330): a texture as an arena's objects find it --
    // the first of the arena's archives (LevelData::archivesOf) that has it, or the
    // level's when the arena is not the level's. Writable: special_130 stamps it.
    formats::Texture* arenaTexture(const std::string& arena, const std::string& name);
    // C# FlattenTown (ScriptRuntime.cs:2139, package #15): the minecrawler reached the town.
    void flattenTown();
    // The arenas setGroupTexture retextured and the frontend has not taken (package #15).
    std::vector<std::string> retextured_;
    ShakeRaisedHook shakeRaisedHook_ = nullptr;
    void* shakeRaisedUser_ = nullptr;
    // The textures stampBulletHole changed and the frontend has not taken (package #16).
    std::vector<const formats::Texture*> stamped_;
    TextureStampedHook textureStampedHook_ = nullptr;
    void* textureStampedUser_ = nullptr;
    friend class Rides;
    friend class Bomber;  // package #11: the XE moves Kurt (Bomber.Update), as Rides does
    // C# AimScore (:2029, 0x41ab2c): a box is a target when it is within its size + 140
    // of the origin, inside a cone around Kurt's yaw that is wider for big and close boxes,
    // and visible. Its score (squared distance, height counting double; lower is better),
    // or -1.
    float aimScore(const formats::Box& bounds, const Vec3& origin, float bestScore);
    // C# ChainGunHit (:2075): a tick of the chain gun on an object (or one of its weak
    // parts): the damage, the bar, sparks on the side facing Kurt, or the kill.
    void chainGunHit(MdkObject* obj, int32_t part, const formats::Box& bounds, const Vec3& origin,
                     int32_t damage, bool superGun);
    // C# UpdateBar (:2177): the bar goes after its second, or when its object dies or
    // cannot show a bar.
    void updateBar();
    // C# FlashAt (:1725): an explosion's white flash, 1000 / distance more, up to 150.
    void flashAt(const Vec3& point);
    // C# BreakUp (:1738): the parts of an exploding object's break-up model (model + "D")
    // and slime drops with gore, or 16 fire sparks when it has none.
    void breakUp(MdkObject* obj);
    // The effect half of ObjectMotion.Update (ObjectMotion.cs:54-68): an effect only
    // plays its texture animation, one frame per tick, and goes after its last frame.
    void motionEffect(MdkObject* obj);
    // C# FindTexture (:3224): an arena's texture, else the level's. False is the C#'s
    // null; otherwise `frames` is the texture's frame count.
    bool findTextureFrames(const std::string& arena, const std::string& name, int32_t* frames);
    // A model's material names as the C#'s Model.Materials list (a piece of its
    // break-up borrows them). Cached per model, stable addresses.
    const std::vector<std::string>* materialNamesOf(const formats::Model* model);

    // Raycast (0x421680): whether any triangle of the active arenas crosses the
    // segment. Only the hit/no-hit answer is used by the move planner.
    // The pickup half of ObjectMotion.Update (UpdatePickup 0x43d8f0), run after
    // ApplyVelocity for a FlagPickup object. A falling pickup lands on floor contact
    // (Fall), or — once it reaches terminal speed — clamps its fall and opens its
    // chute (spawns SW_CHUTE, attach command 74); a landed/non-gravity pickup not in
    // the Still list spins in place, and SW_H150 runs away (motionRunner). The
    // Collected shrink is a later slice.
    void motionPickup(MdkObject* obj);
    void motionUpdraft(MdkObject* obj);
    // Items.cs's private half (see items.cpp).
    friend class Twister;
    void itemsForget(MdkObject* obj);
    void dropCow();
    MdkObject* cowTarget();
    void updateCow(MdkObject* obj);
    void cowHits(MdkObject* obj);
    void itemsUpdateThrown(MdkObject* obj);
    bool itemsHitsObject(MdkObject* obj);
    void itemsActivate(MdkObject* obj);
    void itemsExplode(MdkObject* obj, int32_t damage, int32_t kurtDamage, float radius,
                      int32_t hitType, MdkObject* source, float scale);
    void itemsStartNuke(MdkObject* obj);
    void itemsUpdateActive(MdkObject* obj);
    void itemsUpdateTornado(MdkObject* obj);
    void itemsUpdateBomb(MdkObject* obj);
    void itemsUpdateMortar(MdkObject* obj);
    void itemsUpdateNuke(MdkObject* obj);
    void itemsUpdateSeal(MdkObject* obj);
    void updateTwisters();
    void blastObjects(const Vec3& center, int32_t damage, float radius, int32_t hitType,
                      MdkObject* source, bool counted);
    void blastKurt(const Vec3& center, int32_t damage, float radius);
    void blastGroups(const Vec3& center, int32_t damage, float radius, int32_t hitType,
                     bool counted);
    // ---- Solid objects and platforms (package #3, scripts/platforms.cpp) ----
    // C# KurtTouchesGroups (:554): the triangle groups of his arena that Kurt ran into
    // this tick get a hit (0x46634e).
    void kurtTouchesGroups();
    // C# Kurt.Carry's call site in the object loop (:983): Kurt moves and turns with
    // the platform he stands on.
    void carryKurt(MdkObject* obj, const Vec3& position, float yaw);
    static const std::vector<solids::Solid>* solidsWithinTrampoline(void* ctx,
                                                                   const formats::Box& region);
    static bool keeperAtTrampoline(void* ctx, MdkObject* owner, Vec3* at);
    std::vector<solids::Solid> solidsWithin_;
    std::vector<int32_t> touchedGroups_;
    // ---- end of solid objects and platforms ----
    static void itemUsedTrampoline(void* ctx);
    static void bombTriggeredTrampoline(void* ctx);
    static bool canUseItemTrampoline(void* ctx, Kurt::Item item);

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
    // Debris.Trail (Effects.SpawnTrail) and Debris.Updraft (Fans.Query, MaskEffects).
    static void debrisTrailTrampoline(void* ctx, const std::string& arena, const Vec3& point);
    static float debrisUpdraftTrampoline(void* ctx, const std::string& arena, const Vec3& point,
                                         float vz, float dt);
    // AnimatedTextures' frame-count lookup (TextureFrames, ScriptRuntime.cs:2246).
    static int32_t animatedFrameCount(void* ctx, const std::string& arena,
                                      const std::string& texture);
    // The fans' live-arena test (IsLiveArena, ScriptRuntime.cs:703).
    static bool fanIsLive(void* ctx, const std::string& arena);
    // Fans.Spark = FanSpark (ScriptRuntime.cs:337, 2222) and Kurt.Updraft =
    // KurtUpdraft(kurt.Feet, ...) (ScriptRuntime.cs:345), in scripts/updraft_wind.cpp.
    void fanSpark(const std::string& arena, const Vec3& point);
    static void fanSparkTrampoline(void* ctx, const std::string& arena, const Vec3& point);
    static float kurtUpdraftTrampoline(void* ctx, float vz, float dt);

    Player* player_ = nullptr;  // borrowed; the live Kurt entity (movement + combat)
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
    SniperRounds sniperRounds_{*this};  // C# ScriptRuntime.SniperRounds
    AirStrike airStrike_{*this};        // C# ScriptRuntime.AirStrike (package #14)
    StrikeScene strikeScene_;           // C# ScriptRuntime.Strike, valid while strikePlayed_
    bool strikePlayed_ = false;
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
    std::vector<ScriptMessage> messages_;
    std::map<std::string, ArenaState> arenas_;
    std::map<std::string, formats::Model> modelCache_;  // global models, stable addresses
    // materialNamesOf's cache: a model's materials as names (map nodes are stable, so a
    // debris piece may borrow the vector for as long as the runtime lives).
    std::map<const formats::Model*, std::vector<std::string>> materialNames_;
    int32_t nextInstance_ = 1000;

    ArenaSpace space_;
    std::map<std::string, std::unique_ptr<Bsp>> bsps_;  // per-arena BSP (C# _bsps), stable addresses
    std::map<std::string, float> floors_;               // per-arena floor Z (C# _floors)
    // C# ScriptRuntime.CurrentArena (ScriptRuntime.cs:200): the arena the scripts
    // think Kurt is in, and the source of the camera's rest pitch.
    //
    // THE ORACLE MOVES IT IN TWO WAYS, and one of them is still not ported:
    //   * a move that crosses a doorway CONNECTION (CrossedArena, ScriptRuntime.cs
    //     :791-803 over the DTI type-6 records paired by FindConnections, :856-881)
    //     -- ported, and pinned by golden/L4_crossing.txt and golden/L7_crossing.txt;
    //   * a teleport (TeleportKurt, :460-481) -- NOT ported, so nothing can put Kurt
    //     in an arena no doorway leads to.
    // So on the first tick it is still the geometric arena under the feet (the C#
    // does the same while CurrentArena is empty), and after that it moves exactly
    // when a move crosses a doorway. A frontend that collides against it must know
    // the first-tick half: review-6 F1 was the frozen pin freezing Kurt's floor at
    // the arena he was born in. The frontends do NOT pin this field though -- they
    // pin solidArenas() below, because the oracle's collision world is the LIST of
    // the current arena AND the active second (ScriptRuntime.cs:265, Viewer.cs:533)
    // and one name where the oracle has two drops the room behind the door Kurt came
    // through.
    std::string currentArena_;
    std::string secondArena_;
    bool secondActive_ = false;
    // C# ScriptRuntime._residentArena (ScriptRuntime.cs:312), written only by loadArena()
    // when the MTO has the arena, exactly as :617-620 does.
    std::string residentArena_;
    // C# ScriptRuntime._connections (ScriptRuntime.cs:301): (arena, connection id)
    // -> the arena on the other side. Built once, from EVERY arena in the DTI (not
    // just the reachable ones), because a doorway is a doorway.
    std::map<std::pair<std::string, int32_t>, std::string> connections_;
    // C# _records (:840): each arena's DTI records, found once. The C# caches the
    // DTI's own List reference; the C++ copies the records into runtime-owned
    // storage instead of borrowing `&ArenaEntry::records`, because LevelData's
    // mutable DTI seam (LevelData::mutableDti()/Dti::setArenas) can replace the
    // arenas vector and free the element a borrowed pointer would name. A std::map
    // keeps element addresses stable, so the pointer arenaRecords hands back stays
    // valid for the runtime's life. `unknownArenas_` caches the C#'s empty-list
    // answer for a name no arena has, so a miss is not re-scanned every tick.
    std::map<std::string, std::vector<formats::Dti::Record>> records_;
    std::set<std::string> unknownArenas_;
    Vec3 kurtPosition_{};
    // C# _drawnArenas (ScriptRuntime.cs:310): the arenas drawn -- Kurt's plus the
    // active second -- as the tick's UpdateArenas left them. Written only by
    // updateDrawnArenas(), empty before the first tick, and read by solidArenas().
    std::vector<std::string> drawnArenas_;
    // C# _solidFirst (ScriptRuntime.cs:285): the scratch SolidArenas returns on the
    // snowboard so a per-step read does not allocate. Derived on every read, never
    // cached -- and only on the board, since off it solidArenas() returns drawnArenas_.
    mutable std::vector<std::string> solidArenas_;
    // The combat state of a runtime with NO Kurt entity bound (a harness that
    // passes a null Player): kurt() falls back to this, and there is no second
    // copy of it anywhere. With a Player bound, kurt() IS the entity's own.
    Kurt unboundKurt_;  // C# ScriptRuntime.Kurt — the combat/inventory slice (game/kurt.h)
    // C# ScriptRuntime.Rides (package #9): what Kurt rides (scripts/rides.h).
    Rides rides_{*this};
    Vec3 targetPosition_{};
    Vec3 previousKurtPosition_{};  // C# _previousKurtPosition
    // C# _teleported (:316): set by TeleportKurt and read once, by the first-tick rule
    // (:911). The first tick normally has no move to observe, so it takes Kurt's place as
    // the previous one; a teleport BEFORE that first tick has already chosen the previous
    // position, and overwriting it is the soak-tour bug ArenaSwitchTests.FirstTickKeepsTeleport
    // records ("he stayed in the room while in the corridor and fell out when it went").
    bool teleported_ = false;
    float kurtYaw_ = 0.0f;
    float targetYaw_ = 0.0f;
    MdkObject* alienTarget_ = nullptr;  // C# ScriptRuntime.AlienTarget (opcode 251)
    std::vector<Twister> twisters_;     // C# Items._twisters
    std::vector<Twister> twisterScratch_;
    std::vector<Vec3> groupCentres_;    // blastGroups' scratch
    std::vector<std::string> loadedArenas_;  // C# _loadedArenas (UpdateArenas)
    // C# EndTheLevel (ScriptRuntime.cs:1496): see specialEvent above.
    void endTheLevel();
    std::unique_ptr<EndLevel> endLevel_;  // C# ScriptRuntime.EndLevel
    bool levelEnded_ = false;             // the C#'s LevelEnded(GameOver.No), latched
    // Package 13 (ScriptRuntime.cs:1341-1492): see `cutscene` above.
    void startCutscene(int32_t state, MdkObject* target);
    void endCutscene();
    void cutsceneTick();
    void updateCutsceneCamera();
    // C# ObjectMotion.Update for one object (the part of RunTick's object loop after
    // Vm.Run, and CutsceneTick's): the game's tick and the cutscene's run the same motion.
    void motionUpdate(MdkObject* obj);
    bool gameFinished_ = false;           // the C#'s GameFinished, latched
    int32_t tickCount_ = 0;
    float tickAccumulator_ = 0.0f;  // C# ScriptRuntime._time
    // spawn_box (hazards.cpp): C# _boxModels and _boxes. A deque so a model's address
    // (the pose caches' key, MdkObject::model) never moves; the keys are parallel.
    struct BoxModelKey {
        Vec3 size;
        std::string texture;
    };
    std::deque<formats::Model> boxModels_;
    std::vector<BoxModelKey> boxModelKeys_;
    std::vector<MdkObject*> boxes_;
    // The destructor's half of the box models: the pose caches drop their keys.
    void forgetBoxModels();

    // ---- Full saves (package #19, script_snapshot.cpp) ----
    std::vector<std::string> arenaOrder_;  // getArenaState's creation order
    // C# FindAnimationNamed (Snapshot.cs:200): a CMI animation by its "CMI_<hex>"
    // name, else the arena's, else the items'.
    const formats::ModelAnimation* findAnimationNamed(MdkObject* obj, const std::string& name);
    // The group state and its triangles, as TriangleGroups.SetFlags writes them.
    void applyGroupFlags(const std::string& arena, int32_t group, uint32_t state);
    friend class SnapshotIo;
};

} // namespace scripts
} // namespace mdk

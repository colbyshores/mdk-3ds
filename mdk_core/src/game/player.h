#pragma once

// ============================================================================
// player.h — Kurt's movement: the ground walk and the vertical (gravity, jump,
// the chute, the fall and the landing).
//
// This is the movement half of src/Mdk.Game/Kurt/Kurt.cs, using the C#'s exact
// constants and the BSP collision in game/arena_space.h. It carries the C#'s
// visual state machine too (Kurt.UpdateState, Kurt.Animate, Kurt.UpdateSpecial
// and Kurt.UpdateDeath), so the renderer can pick Kurt's frame, and it carries the
// knock-down too (Kurt.KnockDown and the Knocked/GetUp arms of
// Kurt.UpdateCombatState), and the chain gun (Kurt.UpdateFiring, StopFiring,
// UpdateMuzzle and the SHOT / RUN_FIRE states; the hits are the scripts
// runtime's ScriptRuntime::fireChainGun), and the items (Kurt.UpdateItems and the
// THROW state, whose ItemUsed reaches the scripts runtime's Items through
// ItemHooks). The states whose branches belong to other subsystems (the beta
// moves) are not reachable here, and the C#'s branches on them are taken the way
// they are false. The ride (Kurt.Ride.cs) is ported (package 9): Walk::Riding in
// the XD2 (scripts/rides.cpp mounts him), and the RideHook seam the snowboard and
// the bomber (packages 10, 11) step him through; the bomber's top-down view is
// topDownHeight (package 11). The fans' updraft (Kurt.Updraft.cs) and the wind
// zones' slide (Kurt.Slide.cs) are ported (package 8): the scripts runtime answers
// the updraft through UpdraftHook and starts, pushes and stops the slide. The
// ledges (Kurt.Ledge.cs: the grab, the hang and the climb) are ported in
// game/player_ledge.cpp (package 12).
//
// Units and convention match the C#: world units, Z up; Yaw in degrees,
// counter-clockwise, Facing = (cos yaw, sin yaw, 0); the follow camera's pitch
// is in degrees with positive looking DOWN.
// ============================================================================

#include "audio/sound_mixer.h"
#include "game/arena_space.h"
#include "game/camera_roll.h"
#include "game/input.h"
#include "game/kurt.h"
#include "game/scope.h"
#include "game/solids.h"
#include "render/render_types.h"
#include "util/random.h"

namespace mdk {

class Player {
public:
    // The C# Kurt state this increment owns.
    Vec3 feet = {0.0f, 0.0f, 0.0f};
    float yaw = 0.0f;          // degrees, unnormalised, CCW
    // C# Kurt.CameraPivot and Kurt.CameraTilt (Kurt.Ride.cs:32,37). They belong
    // to Kurt, not to the camera: the follow camera reads them when it places
    // the view and when it blends between two steps. The snowboard dips the pivot
    // while K_SURFJ takes off (scripts/snowboard.cpp, package #10); the fields live
    // here so the camera does not keep a second copy of them.
    float cameraPivot = 4.5f;   // C# Kurt.DefaultPivot
    float cameraTilt = 0.0f;    // degrees, negative looks up
    // C# Kurt.Frozen (Kurt.Combat.cs:89): cutscenes and the level's end -- he stands
    // still (update() takes the C#'s Frozen branch, Kurt.cs:141-150) and the follow
    // camera skips its clearance (FollowCamera.cs:132). The writers are the end of a
    // level (scripts/end_level.h) and the cutscenes (ScriptRuntime::startCutscene).
    bool frozen = false;
    // C# Kurt.Visible (Kurt.Combat.cs:90): KurtSprite draws him only while it is set
    // (KurtSprite.cs:182). The cutscenes hide him except in the strike and the end of the
    // game (ScriptRuntime::cutsceneTick) and show him again at their end. The rides
    // hide him in the XD2 (Rides.cs:178, scripts/rides.cpp, package #9) and show him
    // when he gets out (Kurt.Ride.cs:81, :95: jumpOff, getOffBoard); the XE hides him too
    // (Rides.cs:187, MountBomber, package #11).
    bool visible = true;
    // C# Kurt.Rising (Kurt.Ride.cs:38): the end of a level is lifting him, so the
    // follow camera holds no air tilt (FollowCamera.cs:113-117).
    bool rising = false;
    // C# Kurt.Pose (Kurt.Ride.cs:31): a frame shown instead of the state's (the end's
    // K_TAKEOF / K_FLOATC; the board's K_SURF / K_SURFJ, package #10). KurtSprite::pick
    // shows it, wrapped into the animation, when the animation has frames. The name
    // is a static string (EndLevel::kTakeoff / kFloating), never owned here.
    bool hasPose = false;
    const char* poseName = nullptr;
    int32_t poseFrame = 0;
    // ---- Rides (C# Kurt.Ride.cs, package #9) ----
    // C# Kurt.Walk / Kurt.Walking (Kurt.Ride.cs:20, 29): in the XD2 Kurt walks
    // hidden, without strafing, jumping, items or the gun (update() takes the C#'s
    // `Walking == Walk.Riding` arms, Kurt.cs:207 and :228). scripts/rides.cpp sets it.
    enum Walk { WalkNormal, WalkRiding };
    Walk walkMode = WalkNormal;
    // C# Kurt.RideKeys (Kurt.Ride.cs:23): what a walker ride reads, written each
    // riding step -- steering (forward or turn held) and fire.
    struct RideKeys {
        bool steer = false;
        bool fire = false;
    };
    const RideKeys& rideKeys() const { return _rideKeys; }
    // C# Kurt.Ride (Kurt.Ride.cs:27, Action<Input, float>): a ride's step (the
    // snowboard, the bomber; packages #10 and #11) instead of walking. §10 forbids the
    // delegate, so it is a function pointer over a context, the UpdraftHook shape.
    // Null is the C#'s null delegate: Kurt walks. The snowboard (Rides::mountBoard,
    // package #10) and the XE (Rides::mountBomber, package #11) set it; getOffBoard
    // clears it.
    struct RideHook {
        void* context = nullptr;
        void (*step)(void* context, const Input& input, float delta) = nullptr;
    };
    void setRideHook(const RideHook& hook) { _rideHook = hook; }
    const RideHook& rideHook() const { return _rideHook; }
    // C# Kurt.Glide (Kurt.Ride.cs:50): a ride's move at `velocity` (u/s) -- the walk
    // move against the arena and the objects, sticking to downhill floors, gravity
    // and the fall. The snowboard's (package #10).
    void glide(const Vec3& velocity, float delta);
    // C# Kurt.UpdateGun (Kurt.Ride.cs:71): the chain gun and its flash on the board.
    void updateGun(const Input& input, float delta);
    // C# Kurt.JumpOff (Kurt.Ride.cs:78): out of the XD2, a running jump forward
    // (40 u/s up, 20 u/s ahead).
    void jumpOff();
    // C# Kurt.GetOffBoard (Kurt.Ride.cs:89): off the board or the bomber, keeping the
    // ride's speeds (u/s) as his walking ones.
    void getOffBoard(float forward, float strafe);
    // C# `_kurt.CameraRoll.Follow(bank, delta)` (Snowboard.cs:428, package #10): the view
    // rolls with the board's bank. The one writer outside update() the C# has, so the
    // roll stays read-only to everything else (see cameraRoll() below).
    void followCameraRoll(float bank, float delta) { _cameraRoll.follow(bank, delta); }
    // C# Kurt.TopDownHeight (Kurt.Ride.cs:34, a float?): the bomber's view, this high
    // above Kurt, looking down (0x4183f0). scripts/bomber.cpp writes it each step of the
    // XE ride (package #11), getOffBoard clears it, and FollowCamera looks down from it
    // while it is set (FollowCamera.cs:98, LookDown).
    bool hasTopDownHeight = false;
    float topDownHeight = 0.0f;
    // ---- end of rides ----
    float forwardSpeed = 0.0f;
    float strafeSpeed = 0.0f;
    float turnRate = 0.0f;     // degrees/s
    float verticalSpeed = 0.0f;
    bool onFloor = false;
    bool chuteOpen = false;

    // Health and death (C# Kurt.Combat). What is ported here is the health, the death
    // beat, the knock-down (K_BANG, K_BFLIP and the way back), the hurt and white flashes,
    // the invulnerability timer and the recent-damage counter that decides a knock-down.
    // What is NOT is the rest of that class: weapons and difficulty scaling and riding
    // (the Frozen branch landed with the end of a level, `frozen` above; the ledges are
    // Kurt.Ledge.cs, game/player_ledge.cpp). What this half needs is
    // narrow: a hard landing damages Kurt and a fall out of the arena kills him. health 0
    // on the floor is death (C# "health 0 --on the floor--> DEAD"); update() then runs the
    // C#'s UpdateDeath (the corpse falls on and the death frame advances) and stops once
    // the skull passes its end, which is where the C# raises Died.
    //
    // THE ONE HEALTH. The C# Kurt is one class with one Health (Kurt.Combat.cs:76);
    // this port splits the class in two, so the health is not duplicated across the
    // split: it lives in `combat_` and nowhere else, and every reader -- the death
    // transition below, the HUD, and ScriptRuntime::kurt() for the scripts VM --
    // goes through this object. A Player that owns its own second health is the
    // defect golden/L3_kurt_hurt.txt exists to catch: hurt_kurt (109) would move a
    // field the death check and the HUD never read.
    Kurt combat_;
    bool dead = false;

    // Kurt's combat half, and the health that lives in it. `combat()` is the seam
    // ScriptRuntime borrows (C# Viewer owns the one Kurt and hands it to the
    // ScriptRuntime, ScriptRuntime.cs:168); `health()` is what the HUD and the
    // death transition read.
    Kurt& combat() { return combat_; }
    const Kurt& combat() const { return combat_; }
    int health() const { return combat_.health(); }

    // The number of frames in each C# Kurt state animation. The renderer
    // fills this after loading the sprite archive; Player's state machine
    // needs it to know when a Once animation is done and to wrap a Loop one.
    static constexpr int kStateCount = Kurt::HelmetOff + 1;
    struct FrameCounts {
        int32_t values[kStateCount] = {};
    };

    // Starts with another health than full (C# SetHealth: max(health, 0)).
    void setHealth(int health);

    // THE CONSOLE'S CHEATS (C# Kurt.Cheats.cs, gap package #21). God mode is the
    // combat half's Mortality (Kurt::setMortality: no damage, and fall() below skips
    // the fall-out death). Noclip is C# Kurt.Clipping: off, Kurt flies through
    // everything -- the walking keys move him, jump and Down (Q) rise and sink, turbo
    // is three times faster (updateNoclip). Both are the C#'s public fields, written
    // by LevelCommands.SetGod/SetNoclip; here by the frontends' cheats.
    enum Clipping { ClipOn, ClipOff };
    void setClipping(Clipping clipping) { _clipping = clipping; }
    Clipping clipping() const { return _clipping; }
    void setMortality(Kurt::Mortality mortality) { combat_.setMortality(mortality); }
    Kurt::Mortality mortality() const { return combat_.mortality(); }

    // The C# Kurt state machine's visual state (Kurt.cs:104). The movement half
    // drives it, because the state changes the movement (a hard landing stops
    // walking), but the FIELD lives in the combat half: Kurt.Hurt reads the state
    // the machine is in (Kurt.Combat.cs:125 -- no hurt while Dead, Knocked, GetUp
    // or HardLand), and a copy here is a copy hurt() would never see. Same shape
    // as the one health above, and pinned by the same kind of golden.
    // The renderer reads it to choose a sprite frame (KurtSprite.Pick).
    Kurt::State state() const { return combat_.state(); }
    // The C#'s `Died` event (Kurt.Combat.cs:94, raised by UpdateDeath at :253 when the
    // skull finishes fading in) without a delegate, which §10 forbids: a latch set on
    // the one step the skull crosses SkullEnd and cleared when Kurt leaves the Dead
    // state, so it fires once per death exactly as the event does -- the guard at
    // Kurt.Combat.cs:238 stops the rise, so nothing re-crosses it. The C#'s subscriber
    // is the viewer's (Viewer.cs:239 -> Game.cs:448: write LASTGAME, go to the menu);
    // this port has no menu and no save, so the Linux frontend uses it as the respawn
    // beat it already had, and the level-end flow stays deferred with them.
    bool died() const { return _died; }
    float animationFrame() const { return _animationFrame; }
    bool chuteClosing() const { return _chuteClosing; }
    void setFrameCounts(const FrameCounts& counts) { _frameCounts = counts; }

    // The C# Kurt.Wrap (Kurt.cs:712), also copied inline by KurtSprite.Pick
    // (KurtSprite.cs:325). It is an exact integer wrap, not a search or an
    // approximate clamp.
    static int32_t wrapAnimationFrame(int32_t frame, int32_t count);

    // The BSP world Kurt moves through. Without it the move is a flat-plane
    // stand-in (used only before the level is loaded).
    void setSpace(ArenaSpace* space) { _space = space; }

    // The mixer his sounds go to (C# Kurt's SoundMixer constructor argument).
    // The C# always has one; a Player without one plays nothing, which is what a
    // frontend with no audio -- or a host harness that only wants the physics --
    // is. The sounds are the ones the ported state machine makes: FOOT1-4, LAND,
    // CHUTEOUT, CHUTEON, CHUTEIN and the chain gun's loop (MULTIFIRE, GATTFIRE with
    // the super chain gun), sniper mode's (SNIPERON, SNIPEROFF, the BREATH and ZOOM
    // loops, SNIPRELD, SNIPERSHOT), the updraft's FAN loop and the slide's BUTSLIDE /
    // BUTBRAKE loop. The roll belongs to a subsystem that is not ported (OPEN_ITEMS.md).
    void setMixer(audio::SoundMixer* mixer) { _mixer = mixer; }

    // Kurt's item events (Kurt.Combat.cs:95-101), which the C# scripts runtime
    // subscribes to (ScriptRuntime.cs:331-333: ItemUsed += Items.UseItem,
    // BombTriggered += Items.TriggerBomb, CanUseItem = Items.CanUse). §10 forbids the
    // delegates, so they are plain function pointers over a context, the shape the
    // Debris callbacks already have. They are called synchronously from update(), where
    // the C# invokes them, because Items.UseItem reads Kurt's feet, yaw and state at
    // that moment and spawns into the object list before the scripts' tick. A hook left
    // null is the C#'s event with no subscriber: nothing happens (CanUseItem null:
    // every item may be used). ScriptRuntime binds them when it is built with a Player
    // and unbinds them when it goes.
    struct ItemHooks {
        void* context = nullptr;
        void (*itemUsed)(void* context) = nullptr;
        void (*bombTriggered)(void* context) = nullptr;
        bool (*canUseItem)(void* context, Kurt::Item item) = nullptr;
    };
    void setItemHooks(const ItemHooks& hooks) { _itemHooks = hooks; }
    const ItemHooks& itemHooks() const { return _itemHooks; }

    // C# Kurt.Collect (Kurt.Combat.cs:160): takes a pickup by model name, returning its
    // sound or "" (the console's `give`, LevelCommands.Give, and the scripts' pickups).
    const char* collect(const std::string& pickup) { return combat_.collect(pickup); }

    // C# Kurt.StopFiring (Kurt.Combat.cs:183): sniper mode ends (silently), the gun
    // stops, its looped voice is stopped and the muzzle flash goes. Public as in the C#:
    // the knock-down, the hard landing and the death call it here, and the level's end and
    // the rides (scripts/rides.cpp) call it from the scripts runtime; the cutscenes will.
    void stopFiring();

    // ---- Sniper mode (src/Mdk.Game/Kurt/Kurt.Sniper.cs; game/player_sniper.cpp) ----
    // The sniper key on the floor enters it (SNIPERON, the BREATH loop); Kurt stands,
    // sidesteps at a quarter of the speed, looks and zooms through the scope (Scope
    // above), loads and fires its clip and picks the ammo with the item keys. The
    // sniper key again (SNIPEROFF), falling, death and StopFiring (knock-down, hard
    // landing; cutscenes and rides when they land) leave it.
    //
    //   sniper key ──on the floor──► SNIPERON, BREATH ──► look, zoom, sidestep, clip, fire
    //        ▲                                                │
    //        └── sniper key (SNIPEROFF), falling, knock-down, death, cutscenes (StopFiring)
    //
    // C# Kurt.MouseDegrees (Kurt.cs:45): degrees per mouse count, for the walk's turn
    // and the scope's look alike.
    static constexpr float kMouseDegrees = 0.15f;
    // Kurt.SniperExit: whether leaving plays SNIPEROFF.
    enum SniperExit { SniperSilent, SniperWithSound };
    // C# Kurt.Sniping (0x573a60): sniper mode is on; Kurt isn't drawn (the frontends'
    // part, gap package #7).
    bool sniping() const { return _sniping; }
    // C# Kurt.Scope.
    Scope& scope() { return _scope; }
    const Scope& scope() const { return _scope; }
    // C# Kurt.SniperEye / SniperForward: the eye and the line of sight in sniper mode
    // (FollowCamera's scope view, the listener, and #6's rounds and target lock).
    Vec3 sniperEye() const { return _scope.eye(feet, facing()); }
    Vec3 sniperForward() const { return _scope.forward(yaw); }
    // C# Kurt.EnterSniper (damp_move 0x46883c): only standing on a floor, alive, not
    // knocked down or throwing. Kurt stops firing and stands; the scope resets.
    void enterSniper();
    // C# Kurt.LeaveSniper (0x4645c8).
    void leaveSniper(SniperExit exit = SniperSilent);
    // C# Kurt.SniperFire, the scripts runtime's round-slot test (ScriptRuntime.cs:330
    // binds FireSniper; the C++ runtime's constructor binds it, scripts/sniper_rounds.cpp).
    // Null is the C#'s `SniperFire ?? (_ => true)`.
    void setSniperHooks(const Scope::FireHook& hook) { _sniperFire = hook; }
    const Scope::FireHook& sniperHooks() const { return _sniperFire; }

    Vec3 facing() const;
    Vec3 right() const;

    // C# Kurt.Velocity (Kurt.cs:132-133), in u/s: the walking terms, the
    // horizontal knock-down push and the vertical speed. The push is live state, not
    // a write-only field: ScriptRuntime::pushKurt (opcode 248) is its only non-zero
    // writer in the whole oracle, and drainKnockdownPush is its only reader that
    // lowers it, so a port that drops this term moves a shoved Kurt back.
    Vec3 velocity() const;

    // C# Kurt.CameraRoll (Kurt.cs:114): the view's bank, which Kurt's moves write and
    // the follow camera reads (FollowCamera.cs:139). It is live state on the same
    // terms as the flashes: walk() runs inside update() and settle() runs at the top
    // of it, and FollowCamera::update reads the result every frame, so a copy here
    // that nothing reads is the write-only-field defect again. Its two writers and
    // the order they run in are pinned by golden/L3_kurt_camroll.txt.
    // Read-only on purpose: C# Kurt.CameraRoll.Roll is `{ get; private set; }`, and
    // the only writers are CameraRoll's own methods, which run inside update(). A
    // mutable accessor here would let a caller set the bank behind CameraRoll's back.
    const CameraRoll& cameraRoll() const { return _cameraRoll; }
    // The velocity half of C# Kurt.KnockDown (Kurt.Combat.cs:177, `_push += push`),
    // split out so the push term can be pinned on its own. Every push the oracle adds
    // goes through knockDown: push_kurt (opcode 248, ScriptRuntime.cs:2850) hands it a
    // shove and Kurt.UpdateKnockDamage (Kurt.Combat.cs:225) hands it Vector2.Zero.
    void addKnockdownPush(const Vec2& push);

    // C# Kurt.KnockDown (Kurt.Combat.cs:173, state 901): the counter is spent, he is
    // invulnerable for KnockdownInvulnerability seconds, the push is added, the chute
    // closes, and K_BANG starts, after the gun stops (the C#'s StopFiring() there).
    // The push argument is non-zero only from ScriptRuntime::pushKurt (opcode 248);
    // updateKnockDamage passes zero, exactly as Kurt.Combat.cs:225 does.
    void knockDown(const Vec2& push);

    // One fixed step of delta seconds (the C# Kurt.Update: turning, the walk
    // move against the BSP, then the vertical).
    void update(const Input& input, float delta);

    // The per-frame mouse look (the C# Kurt.MouseLook + FollowCamera.MouseLook):
    // yaw from the mouse's X, the camera's look pitch from its Y. Consumes the
    // accumulated mouse so the in-step terms stay zero.
    void mouseLook(Input& input);

    // Moves Kurt by offset (horizontal) with collision, sliding along what he
    // meets (the camera's push, C# Kurt.Shove). Returns how far he went.
    Vec3 shove(Vec3 offset);

    // C# Kurt.Teleport (Kurt.cs:249, "Moves Kurt (a script's teleport): he stops").
    // The chute closes silently, the feet and yaw are set, and all three speeds go:
    // a teleport is not a move he is still carrying. ScriptRuntime::teleportKurt
    // (opcode 112 / 173) is its only caller in this port, as in the oracle.
    // A keeping platform lets him go (`_keeper = null`, Kurt.cs:254). The
    // `Teleports++` counter below has no reader until FrameBlend's Kurt half does.
    void teleport(const Vec3& at, float newYaw);

    // ---- Platforms, solid objects and touches (package #3) ----
    // Kurt.cs:107-110 (SolidsWithin, Platform), Kurt.Ride.cs:40-45 and :110-143
    // (Touched, TakeContacts, Contact, UpdateTouched), Kurt.Keeper.cs (Keeper). The C#'s
    // two delegates the scripts runtime sets (ScriptRuntime.cs:334, 346) are plain
    // function pointers over a context here, the ItemHooks shape (§10: no
    // std::function). A hook left null is the C#'s null delegate: no solids (Nearby
    // answers []), and no platform keeps him.
    struct SolidHooks {
        void* context = nullptr;
        // C# Kurt.SolidsWithin (ScriptRuntime.SolidsWithin): the solid object boxes of
        // Kurt's arena within a region. The list is the runtime's one scratch list,
        // used at once and overwritten by the next call, exactly as the C#'s is.
        const std::vector<solids::Solid>* (*solidsWithin)(void* context,
                                                          const formats::Box& region) = nullptr;
        // C# Kurt.Keeper (ScriptRuntime.KeeperAt): where a platform that keeps Kurt is
        // (flag 0x800000, alive), or false when it doesn't keep him.
        bool (*keeperAt)(void* context, scripts::MdkObject* owner, Vec3* at) = nullptr;
    };
    void setSolidHooks(const SolidHooks& hooks) { _solidHooks = hooks; }

    // C# Kurt.Platform (Kurt.cs:110): the object Kurt stands on (0x573b84), or null.
    // Set by the fall move that landed him on it (and by the keeping platform), cleared
    // by every other fall move.
    scripts::MdkObject* platform() const { return _platform; }
    // C# Kurt.Touched (Kurt.Ride.cs:45): the objects Kurt's box touches
    // (damp_collide_move's 0x573c2c), and his platform.
    const std::vector<scripts::MdkObject*>& touched() const { return _touched; }
    // C# Kurt.TakeContacts (Kurt.Ride.cs:112): the arena triangles Kurt's moves ran
    // into since the last call (0x46634e: his arena's groups get a hit). The returned
    // list is kept until the next call, as the C#'s _taken is.
    struct Contact {
        const formats::Arena* arena;
        int32_t triangle;
    };
    const std::vector<Contact>& takeContacts();
    // C# Kurt.Carry (Kurt.cs:282): the platform Kurt stands on moved from `from` to
    // `to` and turned by `turn` degrees: he moves and turns with it.
    void carry(const Vec3& from, const Vec3& to, float turn);
    // The runtime that owns the objects these pointers name is going (a level's end):
    // the platform, the keeping platform and the touched list are let go, so nothing
    // dangles into a freed object list. The C# holds references that keep the objects
    // alive; this is the pointer port's version of that, not a behaviour.
    void releaseSolids();
    // C# Kurt._keeper (Kurt.Keeper.cs:160): the platform that keeps him, or null. The
    // C# field is private and its probe reads it by reflection; this is that read.
    scripts::MdkObject* keeper() const { return _keeper; }
    // ---- end of platforms, solid objects and touches ----

    // C# Kurt.Teleports (Kurt.cs:126, incremented by Teleport at :260). Its only
    // reader in the oracle is FrameBlend's jump test (FrameBlend.cs:53: a pose whose
    // teleport count moved is never interpolated), and that half of FrameBlend is
    // deferred with the ride state (frame_blend.h), so this is written and not yet
    // read -- the same shape as ScriptRuntime::residentArena_, and for the same
    // reason: so the write is pinned by a test instead of looking like dead state.
    int teleports = 0;

    // ---- The fans' updrafts (C# Kurt.Updraft.cs, damp_gravity 0x469efc) ----
    // C# Kurt.UpdraftExitSpeed: out of an updraft Kurt rises at most this fast.
    static constexpr float kUpdraftExitSpeed = 40.0f;
    // C# Kurt.Updraft (Func<float, float, float>), set by the scripts runtime
    // (ScriptRuntime.cs:345 -> KurtUpdraft): (vertical speed, seconds) -> the new
    // vertical speed, or NaN outside every fan. §10 forbids the delegate, so it is a
    // function pointer over a context, the ItemHooks shape. Null is the C#'s null
    // delegate: no updraft (NaN).
    struct UpdraftHook {
        void* context = nullptr;
        float (*updraft)(void* context, float verticalSpeed, float delta) = nullptr;
    };
    void setUpdraftHook(const UpdraftHook& hook) { _updraftHook = hook; }
    const UpdraftHook& updraftHook() const { return _updraftHook; }
    // C# Kurt.InUpdraft: whether a fan holds Kurt.
    bool inUpdraft() const { return _inUpdraft; }

    // ---- The wind zones' slide (C# Kurt.Slide.cs, damp_buttslide 0x468db8) ----
    // C# Kurt.SlideGravity: extra gravity of the wind zones and the slide (u/s^2).
    static constexpr float kSlideGravity = 128.0f;
    // C# Kurt.Sliding (0x573be8).
    bool sliding() const { return _sliding; }
    // C# Kurt.SlideVelocity (0x573bf0), u/s.
    Vec2 slideVelocity() const { return _slideVelocity; }
    // The slide's private state, for the goldens: the speed cap, the ticks left after an
    // arena change, the ticks in the air.
    float slideCap() const { return _slideCap; }
    float slideLeft() const { return _slideLeft; }
    float slideAir() const { return _slideAir; }
    // C# Kurt.StartSlide (0x468b64): the wind zones call it while Kurt is in their box.
    void startSlide();
    // C# Kurt.EnterArena (0x41c550): Kurt crossed into another arena; a slide ends 15
    // ticks later. The scripts runtime calls it at the crossing (ScriptRuntime.cs:926).
    void enterArena();
    // C# Kurt.StopSlide: wind_zone off, death, the fall or rest that ends it.
    void stopSlide();
    // C# Kurt.SlideAccel (slide_accel 0x468be0): adds a push (u/s per axis) to the
    // slide velocity over dt seconds.
    void slideAccel(const Vec2& push, float dt);
    // C# Kurt.AddVerticalSpeed (Kurt.cs:278): push_kurt.
    void addVerticalSpeed(float speed) { verticalSpeed += speed; }
    // C# Kurt.Blow (upstream dce8d904): in a wind zone's box in the air (opcode 224 sets
    // 0x573bec) the wind pulls him down and keeps his chute shut for his next step
    // (damp_vertical 0x4694bc), so he lands in the box and slides instead of gliding past.
    void blow(float dt) {
        _blown = true;
        verticalSpeed -= kSlideGravity * dt;
    }

    // ---- Ledges (C# Kurt.Ledge.cs, damp_ledge_grab 0x469868; game/player_ledge.cpp) ----
    // The climb's tick counter (C# Kurt._climbTicks), for the goldens.
    int32_t climbTicks() const { return _climbTicks; }
    // ---- end of ledges ----

private:
    // ---- Ledges (package #12, game/player_ledge.cpp) ----
    // C# Kurt.GrabLedge: after a step that started at `before` with the forward key
    // `forward`, Kurt hangs from a flat top his hands crossed. True when he hangs.
    bool grabLedge(const Vec3& before, float forward);
    // C# Kurt.HangFrom: hangs from the edge of `top` towards Kurt (`ahead` back).
    bool hangFrom(const ArenaSpace::Floor& top, const Vec3& ahead);
    // C# Kurt.UpdateClimb: K_HANG at 2 ticks a frame, moved by the climb tables.
    void updateClimb(float delta);
    int32_t _climbTicks = 0;  // C# Kurt._climbTicks
    // ---- end of ledges ----

    // Sniper mode's step (Kurt.Sniper.cs): the sniper key, the ways out, then the
    // controls, the sidestep and the clip. True when the step was a sniper one (the C#'s
    // Update returns after it).
    bool updateSniper(const Input& input, bool turbo, float delta);
    bool canSnipe() const;
    void sniperControls(const Input& input, bool turbo, float delta);
    // Kurt.SniperMove: the sidestep and the fall. In player.cpp, beside the walk it
    // shares the movement constants with.
    void sniperMove(const Input& input, bool turbo, float delta);
    // ---- Platforms, solid objects and touches (package #3) ----
    // C# Kurt.Nearby (Kurt.cs:290): the solid object boxes around Kurt and `to`.
    const std::vector<solids::Solid>& nearby(const Vec3& to);
    // C# Kurt.FindPlatform (Kurt.cs:303): a platform the fall of dz reaches.
    bool findPlatform(float dz, float* top, scripts::MdkObject** owner);
    // C# Kurt.Contact (Kurt.Ride.cs:123): records the triangle a move hit.
    ArenaSpace::Result contact(const ArenaSpace::Result& result);
    // C# Kurt.UpdateTouched (Kurt.Ride.cs:133).
    void updateTouched();
    // C# Kurt.RideWalker (Kurt.Ride.cs:101): in the XD2, gravity and the fall only.
    void rideWalker(const Input& input, float forward, float delta);
    RideKeys _rideKeys;
    RideHook _rideHook;
    // Kurt.Keeper.cs: CheckKeeper, KeptPlatform, KeepOn, StayOnKeeper, Kept.
    bool keeperAt(scripts::MdkObject* owner, Vec3* at) const;
    void checkKeeper();
    bool keptPlatform(float dz, float* top, scripts::MdkObject** owner) const;
    void keepOn(scripts::MdkObject* owner);
    void stayOnKeeper();
    bool kept() const { return _keeper != nullptr; }
    SolidHooks _solidHooks;
    scripts::MdkObject* _platform = nullptr;
    scripts::MdkObject* _keeper = nullptr;   // C# Kurt._keeper
    float _keptHeight = 0.0f;                // C# Kurt._keptHeight (0x573a20)
    std::vector<scripts::MdkObject*> _touched;
    std::vector<Contact> _contacts;
    std::vector<Contact> _taken;
    // ---- end of platforms, solid objects and touches ----

    void updateVertical(const Input& input, const Vec3& move, float delta);
    // C# Kurt.UpdateTurning (Kurt.cs:318-335): the turn keys' rate and the yaw. The
    // mouse term is mouseLook's (per frame).
    void updateTurning(const Input& input, bool turbo, float delta);
    // C# Kurt.UpdateNoclip (Kurt.Cheats.cs:24-41): a noclip step; false when Kurt
    // collides as usual.
    bool updateNoclip(const Input& input, float delta);
    Clipping _clipping = ClipOn;   // C# Kurt.Clipping
    // C# Kurt.UpdateUpdraft (Kurt.Updraft.cs:21).
    void updateUpdraft(float delta);
    // C# Kurt.UpdateSlide and its helpers (Kurt.Slide.cs:138-301).
    bool updateSlide(const Input& input, float delta);
    float slideSpeed(float forward, float delta);
    void steer(const Input& input, float speed, float delta);
    void slideMove(float delta);
    void updateSlideState(float forward, float delta);
    // C# Kurt.Facing's (x, y) in libm, the oracle's MathF.Cos/Sin, for the slide.
    Vec2 slideFacing() const;
    void updateAir(bool jumpHeld, float delta);
    void glueDownhill(const Vec3& move, float delta, bool exact = false);
    void fall(float delta);
    void fallMove(float delta);
    void land(const Vec3& before, const Vec3& normal, float delta);
    // The hard landing's damage: Kurt::damage, i.e. the C#'s Damage whole (scaled
    // damage, the red flash, health, the knock counter). Of the terms the C#'s Damage
    // leaves the Player out, only the difficulty scaling is still missing: the KnockDamage
    // it adds is zeroed again by HardLanding itself (Kurt.cs:583, reproduced in
    // Player::land), and the scaling is inert because difficulty_ has no writer yet -- the
    // same deferral the note at land() records. The red flash is NOT left out any more:
    // it is live state now that Kurt::fadeFlashes runs, and the HUD reads it.
    void damage(int damage);
    void drainKnockdownPush(float delta);
    // C# Kurt.UpdateKnockDamage (Kurt.Combat.cs:214) whole: the trigger first (at
    // KnockdownDamage, on a floor, or in the air above one the floor probe reaches),
    // and Kurt::declineKnockDamage as its tail on every other step.
    void updateKnockDamage(float delta);
    void updateState(float forward, float strafe, float delta);
    // C# Kurt.Pressed (Kurt.Combat.cs:257): whether a key went down since the last call
    // for it (the C#'s _held set, one flag per key here).
    bool pressed(const Input& input, Key key);
    // C# Kurt.UpdateItems (Kurt.Combat.cs:271): the item keys select a slot or use the
    // selected item -- on the floor Kurt throws it (THROW, K_SPWEP), in the air it is
    // used at once. The slot keys 1-5 (Key.Item1-5) have no action in input.h yet.
    void updateItems(const Input& input);
    void itemUsed();
    // C# Kurt.UpdateFiring (Kurt.Combat.cs:345): holding fire fires the chain gun and
    // loops its sound; a change of gun (the super chain gun's time) restarts the loop.
    void updateFiring(const Input& input);
    // C# Kurt.UpdateMuzzle (Kurt.Combat.cs:363): every other tick while firing, a
    // random K_MUZZF frame behind Kurt in the states whose frames don't show it.
    void updateMuzzle(float delta);
    // C# Kurt.MuzzleOffset (Kurt.Combat.cs:383): the flash's offset in the current
    // state, or false when the state's frames show the flash themselves (SHOT,
    // RUN_FIRE) or have none. The board's K_SURF pose comes first (package #10).
    bool muzzleOffset(int32_t* x, int32_t* y) const;
    void updateSpecial(float delta);
    void updateDeath(float delta);
    void animate(float delta);
    void animateJump(float delta);
    void footstep(int32_t previous, int32_t current);  // C# Kurt.Footstep
    void updateChuteSound();                           // C# Kurt.UpdateChuteSound
    void stopChuteSound();                             // C# Kurt.StopChuteSound
    void playSound(const char* name, int32_t start = audio::SoundMixer::New);
    void stopSound(const char* name);
    bool soundPlaying(const char* name) const;
    void setState(Kurt::State state);
    int32_t frameCount(Kurt::State state) const;
    float runRate() const;

    ArenaSpace* _space = nullptr;
    audio::SoundMixer* _mixer = nullptr;
    bool _footstepPair = true;  // C# Kurt._footstepPair: FOOT3/4 first, then FOOT1/2
    float _stateTime = 0.0f;
    // How long Kurt has been off the floor (s; upstream 3f9315c5's _airFor).
    float _airFor = 0.0f;
    // A jump's frames went past its top (0x573a48, upstream a91c0870): held jump may open
    // the chute without waiting to fall faster than kFallStartSpeed.
    bool _pastJumpTop = false;
    float _animationFrame = 0.0f;
    bool _chuteClosing = false;
    bool _died = false;  // the C#'s Died event, once per death; see died() above
    FrameCounts _frameCounts;
    Vec2 _knockdownPush = {0.0f, 0.0f};  // C# Kurt.Combat.cs:_push, in u/s
    // C# Kurt.CameraRoll (Kurt.cs:114). One instance, written by update() and read by
    // the follow camera; see cameraRoll() above.
    CameraRoll _cameraRoll;
    Vec3 _floorNormal = {0.0f, 0.0f, 1.0f};
    int _jumpTicksLeft = 0;
    bool _jumpReleased = true;
    // The chain gun's private state (Kurt.Combat.cs:110-114): the looped voice, which
    // gun it is, and the muzzle flash's tick clock, last tick and frame.
    int32_t _gunVoice = 0;
    bool _gunSuper = false;
    // C# Kurt._held (Kurt.Combat.cs:117): the keys held at the previous Pressed call.
    bool _held[kKeyCount] = {};
    ItemHooks _itemHooks;
    float _muzzleTicks = 0.0f;
    int32_t _muzzleTick = -1;
    int32_t _muzzleFrame = 0;
    // C# Kurt._random (Kurt.Combat.cs:115): the muzzle flash's OWN generator, never the
    // scripts runtime's -- the oracle builds it unseeded (`new Random()`), so its draws
    // are not part of the game's golden random stream and must not consume from it. The
    // port seeds it with a constant so a host run is reproducible; the oracle's flash
    // frame and jitter are not, which is why the fire golden prints only whether a
    // flash is up.
    Random _muzzleRandom{0x4D555A};
    // Sniper mode's state (Kurt.Sniper.cs:28-34): the mode, the scope, the two looped
    // voices, the rounds' hook, and C# Kurt._mouseTurn's Scope value (the last step
    // looked through the scope, so the frame's mouse look turns the scope).
    bool _sniping = false;
    Scope _scope;
    int32_t _breathVoice = 0;
    int32_t _zoomVoice = 0;
    Scope::FireHook _sniperFire;
    bool _mouseTurnScope = false;

    // The updraft (Kurt.Updraft.cs:17-19): the hook, the looped FAN voice, and the flag.
    UpdraftHook _updraftHook;
    int32_t _fanVoice = 0;
    bool _inUpdraft = false;
    // The slide (Kurt.Slide.cs:59-71), at the C#'s initial values.
    bool _sliding = false;
    Vec2 _slideVelocity = {0.0f, 0.0f};
    float _slideCap = 50.0f;
    Vec3 _slideNormal = {0.0f, 0.0f, 1.0f};
    Vec2 _slidePush = {0.0f, 0.0f};
    float _slideAir = 0.0f;
    float _slideLeft = 0.0f;
    // A wind zone blew on him since his last step (0x573bec, upstream dce8d904).
    bool _blown = false;
    int32_t _slideVoice = 0;
    const char* _slideSound = "";
    // The frame's mouse X while sliding. The C# slide steers by input.MouseX inside the
    // step (Steer, Kurt.Slide.cs:217) and its MouseLook takes nothing while sliding (the
    // step left _mouseTurn at None), so every step of a frame reads the same motion and
    // the Viewer clears it after the steps. This port's frontends drain the mouse in
    // mouseLook() and never clear it otherwise, so mouseLook takes it into this field
    // while sliding and the steps read it: the same per-frame value, cleared per frame.
    float _slideMouseX = 0.0f;
};

} // namespace mdk

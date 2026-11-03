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
// Kurt.UpdateCombatState); the states
// whose branches belong to other subsystems (the firing and throwing states,
// the crouch-slide, the ledges, the ride, the beta moves) are not reachable
// here, and the C#'s branches on them are taken the way they are false.
//
// Units and convention match the C#: world units, Z up; Yaw in degrees,
// counter-clockwise, Facing = (cos yaw, sin yaw, 0); the follow camera's pitch
// is in degrees with positive looking DOWN.
// ============================================================================

#include "audio/sound_mixer.h"
#include "game/arena_space.h"
#include "game/input.h"
#include "game/kurt.h"
#include "render/render_types.h"

namespace mdk {

class Player {
public:
    // The C# Kurt state this increment owns.
    Vec3 feet = {0.0f, 0.0f, 0.0f};
    float yaw = 0.0f;          // degrees, unnormalised, CCW
    float forwardSpeed = 0.0f;
    float strafeSpeed = 0.0f;
    float turnRate = 0.0f;     // degrees/s
    float verticalSpeed = 0.0f;
    bool onFloor = false;
    bool chuteOpen = false;

    // Health and death (C# Kurt.Combat). What is ported here is the health, the death
    // beat, the knock-down (K_BANG, K_BFLIP and the way back), the hurt and white flashes,
    // the invulnerability timer and the recent-damage counter that decides a knock-down.
    // What is NOT is the rest of that class: weapons and difficulty scaling, the Frozen
    // branch of the state machine, riding, and grabbing ledges. What this half needs is
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
    // CHUTEOUT, CHUTEON and CHUTEIN. The gun, the slide, the sniper, the roll and
    // the updraft fan belong to subsystems that are not ported (OPEN_ITEMS.md).
    void setMixer(audio::SoundMixer* mixer) { _mixer = mixer; }

    Vec3 facing() const;
    Vec3 right() const;

    // C# Kurt.Velocity (Kurt.cs:132-133), in u/s: the walking terms, the
    // horizontal knock-down push and the vertical speed. The push is live state, not
    // a write-only field: ScriptRuntime::pushKurt (opcode 248) is its only non-zero
    // writer in the whole oracle, and drainKnockdownPush is its only reader that
    // lowers it, so a port that drops this term moves a shoved Kurt back.
    Vec3 velocity() const;
    // The velocity half of C# Kurt.KnockDown (Kurt.Combat.cs:177, `_push += push`),
    // split out so the push term can be pinned on its own. Every push the oracle adds
    // goes through knockDown: push_kurt (opcode 248, ScriptRuntime.cs:2850) hands it a
    // shove and Kurt.UpdateKnockDamage (Kurt.Combat.cs:225) hands it Vector2.Zero.
    void addKnockdownPush(const Vec2& push);

    // C# Kurt.KnockDown (Kurt.Combat.cs:173, state 901): the counter is spent, he is
    // invulnerable for KnockdownInvulnerability seconds, the push is added, the chute
    // closes, and K_BANG starts. The C#'s StopFiring() is inert here for the reason
    // Player::land records for its own call to it -- nothing in this port fires.
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

private:
    void updateVertical(const Input& input, const Vec3& move, float delta);
    void updateAir(bool jumpHeld, float delta);
    void glueDownhill(const Vec3& move, float delta);
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
    void updateSpecial(float delta);
    void updateDeath(float delta);
    void animate(float delta);
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
    float _animationFrame = 0.0f;
    bool _chuteClosing = false;
    bool _died = false;  // the C#'s Died event, once per death; see died() above
    FrameCounts _frameCounts;
    Vec2 _knockdownPush = {0.0f, 0.0f};  // C# Kurt.Combat.cs:_push, in u/s
    Vec3 _floorNormal = {0.0f, 0.0f, 1.0f};
    int _jumpTicksLeft = 0;
    bool _jumpReleased = true;
};

} // namespace mdk

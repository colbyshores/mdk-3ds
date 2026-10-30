#pragma once

// ============================================================================
// player.h — Kurt's movement: the ground walk and the vertical (gravity, jump,
// the chute, the fall and the landing).
//
// This is the movement half of src/Mdk.Game/Kurt/Kurt.cs, using the C#'s exact
// constants and the BSP collision in game/arena_space.h. It carries the C#'s
// visual state machine too (Kurt.UpdateState, Kurt.Animate, Kurt.UpdateSpecial
// and Kurt.UpdateDeath), so the renderer can pick Kurt's frame; the states
// whose branches belong to other subsystems (the firing and knock-down states,
// the crouch-slide, the ledges, the ride, the beta moves) are not reachable
// here, and the C#'s branches on them are taken the way they are false.
//
// Units and convention match the C#: world units, Z up; Yaw in degrees,
// counter-clockwise, Facing = (cos yaw, sin yaw, 0); the follow camera's pitch
// is in degrees with positive looking DOWN.
// ============================================================================

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

    // Health and death (C# Kurt.Combat). The full combat (knock-down, the hurt
    // flash, invulnerability, difficulty scaling and weapons) is not ported; this
    // is the health and death the vertical physics' consequences need: a hard
    // landing damages Kurt and a fall out of the arena kills him. health 0 on the
    // floor is death (C# "health 0 --on the floor--> DEAD"); update() then runs
    // the C#'s UpdateDeath (the corpse falls on and the death frame advances)
    // until the frontend respawns him.
    static constexpr int kMaxHealth = 100; // C# Inventory.MaxHealth
    int health = kMaxHealth;
    bool dead = false;

    // The number of frames in each C# Kurt state animation. The renderer
    // fills this after loading the sprite archive; Player's state machine
    // needs it to know when a Once animation is done and to wrap a Loop one.
    static constexpr int kStateCount = Kurt::HelmetOff + 1;
    struct FrameCounts {
        int32_t values[kStateCount] = {};
    };

    // Starts with another health than full (C# SetHealth: max(health, 0)).
    void setHealth(int health);

    // The C# Kurt state machine's visual state. The movement half owns it
    // because the state changes the movement (a hard landing stops walking);
    // the renderer reads it to choose a sprite frame (KurtSprite.Pick).
    Kurt::State state() const { return _state; }
    float animationFrame() const { return _animationFrame; }
    bool chuteClosing() const { return _chuteClosing; }
    void setFrameCounts(const FrameCounts& counts) { _frameCounts = counts; }

    // The BSP world Kurt moves through. Without it the move is a flat-plane
    // stand-in (used only before the level is loaded).
    void setSpace(ArenaSpace* space) { _space = space; }

    Vec3 facing() const;
    Vec3 right() const;

    // C# Kurt.Velocity (Kurt.cs:132-133), in u/s: the walking terms, the
    // horizontal knock-down push and the vertical speed. The combat state
    // machine that writes the push is not ported, but the push state and its
    // drain are, so a future writer cannot silently omit this term.
    Vec3 velocity() const;
    // The velocity half of C# Kurt.KnockDown (Kurt.Combat.cs:173-177): add the
    // horizontal knock-down push in u/s. The knock-down state itself lands with
    // the combat slice.
    void addKnockdownPush(const Vec2& push);

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
    void fallMove(float delta);
    void land(const Vec3& before, const Vec3& normal, float delta);
    void damage(int damage); // C# Damage (Normal difficulty): health -= damage, floored at 0
    void drainKnockdownPush(float delta);
    void updateState(float forward, float strafe, float delta);
    void updateSpecial(float delta);
    void updateDeath(float delta);
    void animate(float delta);
    void setState(Kurt::State state);
    int32_t frameCount(Kurt::State state) const;
    float runRate() const;

    ArenaSpace* _space = nullptr;
    Kurt::State _state = Kurt::Still;
    float _stateTime = 0.0f;
    float _animationFrame = 0.0f;
    bool _chuteClosing = false;
    FrameCounts _frameCounts;
    Vec2 _knockdownPush = {0.0f, 0.0f};  // C# Kurt.Combat.cs:_push, in u/s
    Vec3 _floorNormal = {0.0f, 0.0f, 1.0f};
    int _jumpTicksLeft = 0;
    bool _jumpReleased = true;
};

} // namespace mdk

#pragma once

// ============================================================================
// player.h — Kurt's movement: the ground walk and the vertical (gravity, jump,
// the chute, the fall and the landing).
//
// This is the movement half of src/Mdk.Game/Kurt/Kurt.cs, using the C#'s exact
// constants and the BSP collision in game/arena_space.h. The knock-down, the
// crouch-slide, the ledges, the platforms, the updrafts, the weapons and the
// death land with their own subsystems; where the C# branches on them this
// takes the branch they are false on (no slide, no updraft, no platform, no
// damage), which is the state a fresh Kurt is in.
//
// Units and convention match the C#: world units, Z up; Yaw in degrees,
// counter-clockwise, Facing = (cos yaw, sin yaw, 0); the follow camera's pitch
// is in degrees with positive looking DOWN.
// ============================================================================

#include "game/arena_space.h"
#include "game/input.h"
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
    float lookPitch = 0.0f;    // the camera's look offset, degrees, + looks down
    float verticalSpeed = 0.0f;
    bool onFloor = false;
    bool chuteOpen = false;

    // The follow camera's arena pitch (the DTI arena's pitch; the C# default
    // is 4 degrees). The view pitch is arenaPitch + lookPitch.
    float arenaPitch = 0.0f;

    // The BSP world Kurt moves through. Without it the move is a flat-plane
    // stand-in (used only before the level is loaded).
    void setSpace(ArenaSpace* space) { _space = space; }

    Vec3 facing() const;
    Vec3 right() const;

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

    ArenaSpace* _space = nullptr;
    Vec3 _floorNormal = {0.0f, 0.0f, 1.0f};
    int _jumpTicksLeft = 0;
    bool _jumpReleased = true;
};

} // namespace mdk

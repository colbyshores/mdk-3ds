#pragma once

// ============================================================================
// player.h — Kurt's ground movement, the first increment of the Kurt port.
//
// This is the horizontal half of src/Mdk.Game/Kurt/Kurt.cs: the walk/run
// acceleration, the keyboard + mouse turning, and the facing, using the C#'s
// exact constants. The vertical half (gravity, jump, the chute) and the BSP
// collision (ArenaSpace.Move) land with the collision port; until then Kurt
// walks a flat plane at his spawn height (OnFloor is always true).
//
// Units and convention match the C#: world units, Z up; Yaw in degrees,
// counter-clockwise, Facing = (cos yaw, sin yaw, 0); the follow camera's pitch
// is in degrees with positive looking DOWN.
// ============================================================================

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

    // The follow camera's arena pitch (the DTI arena's pitch; the C# default
    // is 4 degrees). The view pitch is kDefaultPitch + lookPitch.
    float arenaPitch = 0.0f;

    Vec3 facing() const;
    Vec3 right() const;

    // One fixed step of delta seconds (the C# Kurt.Update, horizontal only).
    void update(const Input& input, float delta);

    // The per-frame mouse look (the C# Kurt.MouseLook + FollowCamera.MouseLook):
    // yaw from the mouse's X, the camera's look pitch from its Y. Consumes the
    // accumulated mouse so the in-step terms stay zero.
    void mouseLook(Input& input);
};

} // namespace mdk

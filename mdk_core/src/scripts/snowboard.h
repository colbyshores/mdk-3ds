#pragma once

// ============================================================================
// snowboard.h — Kurt on the snowboard of level 4 (C# src/Mdk.Game/Scripts/
// Snowboard.cs whole; XSNOWB, snowboard_update 0x46ac4c; snowboard.gd): package
// #10, on the rides core of package #9 (scripts/rides.{h,cpp}).
//
// The board's path only guides the heading H, which turns towards the path at
// 40 deg/s (faster carving into the turn, slower against it). Kurt steers with the
// carve angle S (+-30 deg), which slides him sideways (L), and speeds up or brakes
// (V); slopes speed him up or slow him down. The board sits under his feet at the
// yaw H + S.
//
//   input ──► S ──► L (sideways)     path ──► H (heading)     slope, keys ──► V (forward)
//
// Rides::tryMount makes one when Kurt touches the rideable XSNOWB (CMEAT_3's,
// script 147319: Kurt lands on it, the script makes it rideable) and hands
// Player::RideHook its update; Rides::dismount calls getOff when the script clears
// the flag (the end box of MEAT_5, script 147357) or Kurt dies. While he rides,
// ScriptRuntime::solidArenas() is his arena alone (ScriptRuntime.cs:265-282).
//
// The goldens are tests/scripts/golden/L4_board_*.txt (the C# probe
// tests/scripts/board_probe; `tick1 dump board`).
//
// Ordinary concrete class: no virtuals, no templates, no exceptions (AGENTS.md §10).
// ============================================================================

#include <stdint.h>

#include "game/input.h"
#include "render/render_types.h"

namespace mdk {

class Player;

namespace scripts {

class ScriptRuntime;
struct MdkObject;

class Snowboard {
public:
    // C# Snowboard.Pose (Snowboard.cs:94): K_SURF looping, K_SURFJ in the air and
    // landing.
    enum Pose { PoseSurf, PoseJump, PoseLand };

    // C# Snowboard(runtime, board) (Snowboard.cs:113): the heading and the path's
    // start along the board's yaw. `kurt` is the runtime's Kurt (C# runtime.Kurt).
    Snowboard(ScriptRuntime& runtime, Player& kurt, MdkObject* board);

    // C# Snowboard.Update (Snowboard.cs:126): one step of riding, instead of Kurt's
    // walking. Player::update calls it through the RideHook (step below).
    void update(const Input& input, float delta);
    // The RideHook's step (the C#'s `kurt.Ride = _board.Update`).
    static void step(void* context, const Input& input, float delta);

    // C# Snowboard.GetOff (Snowboard.cs:174): the script cleared the rideable flag or
    // Kurt died -- he keeps the board's speeds as his walking ones, and the board is
    // thrown on at 1.1 x his velocity.
    void getOff();

    // C# Snowboard.Board (Snowboard.cs:96).
    MdkObject* board() const { return board_; }

    // The ride's state, for the goldens (the C# probe reads the private fields).
    float speed() const { return speed_; }
    float side() const { return side_; }
    float heading() const { return heading_; }
    float steerAngle() const { return steer_; }
    float pitch() const { return pitch_; }
    bool grounded() const { return grounded_; }
    float pathHeading() const { return pathHeading_; }
    float turnTicks() const { return turnTicks_; }
    float jumpTicks() const { return jumpTicks_; }
    float airTicks() const { return airTicks_; }
    Pose pose() const { return pose_; }
    float frame() const { return frame_; }

    // C# Snowboard's public statics (Snowboard.cs:193, :224, :240, :481, :486).
    static float steer(float steer, float right, bool grounded, float* turnTicks, float dt);
    static float accelerate(float speed, float forward, float dt);
    static float slopeSpeed(float speed, float pitch, float dt);
    static float jumpPivot(float frame, float pivot, float seconds);
    static float turnInput(const Input& input, float dt);

    // The board's frames (C# SurfFrames / JumpFrames): static strings, as Player's
    // poseName requires.
    static const char* const kSurfFrames;
    static const char* const kJumpFrames;

private:
    void updateSpeed(float forward, bool controls, float dt);
    void updatePitch(float delta);
    bool ground(const Vec3& end, Vec3* point);
    void jump(const Input& input, bool controls, float dt);
    void updateSounds(float dt);
    void followPath(const Vec3& previous, float dt);
    Vec3 pathPoint(float t);
    float distance(float t, float kurtX, float kurtY);
    void turn(float delta, float dt);
    void place(float delta);
    void ram();
    void animate(float dt);

    ScriptRuntime& runtime_;
    Player& kurt_;
    MdkObject* board_;
    float speed_ = 0.0f;
    float side_ = 0.0f;
    float heading_ = 0.0f;
    float steer_ = 0.0f;
    float pitch_ = 0.0f;
    float turnTicks_ = 0.0f;
    float jumpTicks_ = 0.0f;
    float airTicks_ = 0.0f;
    bool grounded_ = true;
    float pathHeading_ = 0.0f;
    Pose pose_ = PoseSurf;
    float frame_ = 0.0f;
};

}  // namespace scripts
}  // namespace mdk

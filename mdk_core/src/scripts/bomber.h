#pragma once

// ============================================================================
// bomber.h — the XE bomber ride of level 7's DANT_5 (C# src/Mdk.Game/Scripts/
// Bomber.cs; 0x46bf40; bomber.gd, godot-mdk docs/gameplay.md "The XE bomber"),
// package #11.
//
// Kurt sits in the flyer while its script flies it along a path; the view looks
// straight down from it and a cursor aims XBN_BOMBs at the ground:
//
//   mount ──► view sinks 50 → 0 u above Kurt in 2 s (then the XE isn't drawn)
//         ──► script unlocks (flag 0x4000000 cleared) ──► cursor, bombs, HUD
//         ──► script clears the rideable flag ──► Kurt drops from the XE (Rides)
//
// Rides (scripts/rides.cpp) mounts it (MountBomber) and binds update() as Kurt's
// ride step (Player::RideHook, the C#'s Kurt.Ride delegate): Player::update runs
// it instead of the walk, so it is called once per game step (1/60 s), before the
// scripts' tick. The view is FollowCamera's LookDown (game/follow_camera.cpp),
// read off Player::topDownHeight; the HUD is game/bomber_overlay.h, fed sight().
// The bombs are Items' thrown kind 0x81 (scripts/items.cpp): the drop only spawns
// and aims them.
// ============================================================================

#include <stdint.h>

#include "render/render_types.h"

namespace mdk {

struct Input;

namespace scripts {

class ScriptRuntime;
struct MdkObject;

class Bomber {
public:
    // C# Bomber.CursorCentre (Bomber.cs:23): the cursor starts at the 600×360
    // view's centre.
    static constexpr float kCursorCentreX = 300.0f;
    static constexpr float kCursorCentreY = 180.0f;
    // C# Bomber.Health (Bomber.cs:47): the XE's health is kept at 10000; what it
    // loses goes to Kurt (0x46a77c).
    static const int32_t kHealth = 10000;

    // C# Hud.BomberSight (BomberOverlay.cs:12): what the HUD shows -- the cursor
    // (pixels of the 600×360 view) and the bombs left.
    struct Sight {
        float cursorX = kCursorCentreX;
        float cursorY = kCursorCentreY;
        int32_t bombs = 0;
    };

    explicit Bomber(ScriptRuntime& runtime) : runtime_(runtime) {}

    // C# `new Bomber(runtime, xe)` (Rides.MountBomber, Rides.cs:188): a fresh ride
    // on `xe` -- the view 50 up, the cursor centred and still, 10 bombs, the refill
    // a second away, fire not held.
    void start(MdkObject* xe);

    // C# Bomber.Update (Bomber.cs:78): each step (Kurt's ride) -- Kurt goes with the
    // XE, the view sinks, the cursor moves and drops bombs, hits pass to Kurt.
    void update(const Input& input, float delta);
    // Player::RideHook's step: update() on the Bomber in `context`.
    static void stepTrampoline(void* context, const Input& input, float delta);

    // C# Bomber.ViewHeight / Cursor / BombsLeft (Bomber.cs:64-66).
    float viewHeight() const { return viewHeight_; }
    float cursorX() const { return cursorX_; }
    float cursorY() const { return cursorY_; }
    int32_t bombsLeft() const { return bombsLeft_; }
    // C# Bomber.Locked (Bomber.cs:69): whether the script still holds the controls
    // (and the HUD is off).
    bool locked() const;
    // C# Bomber.HidesXe (Bomber.cs:72): whether the view is inside the XE, which
    // then isn't drawn.
    bool hidesXe() const { return viewHeight_ == 0.0f; }
    // C# Bomber.Shown (Bomber.cs:75): the HUD's sight; false (the C#'s null) while
    // locked.
    bool shown(Sight* out) const;

    // The C#'s private state, read by the goldens' dump (tick1 dump bomber) the way
    // the probe reads it by reflection: _speed, _refill, _fireHeld.
    float speedX() const { return speedX_; }
    float speedY() const { return speedY_; }
    float refill() const { return refill_; }
    bool fireHeld() const { return fireHeld_; }

    // C# Bomber.CursorStep (Bomber.cs:132): a cursor speed after a step --
    // accelerating with the key (0x4688d0: the other way starts over), else braking.
    static float cursorStep(float speed, float axis, float ticks);
    // C# Bomber.AimDirection (Bomber.cs:206): the view's ray through a cursor pixel --
    // right is (s, −c), the top of the view the heading (c, s).
    static Vec3 aimDirection(float cursorX, float cursorY, float yaw);
    // C# Bomber.FallTime (Bomber.cs:215): the time to fall `height` from rest (2.5 s
    // when it isn't below).
    static float fallTime(float height);

private:
    void moveCursor(const Input& input, float delta);
    void refillBombs(float delta);
    void drop();
    Vec3 aim(const Vec3& kurt, float cursorX, float cursorY, float yaw, bool* hit);
    bool firstObject(const Vec3& start, const Vec3& end, Vec3* nearest);
    void passDamage();

    ScriptRuntime& runtime_;
    MdkObject* xe_ = nullptr;
    float speedX_ = 0.0f;
    float speedY_ = 0.0f;
    float refill_ = 1.0f;
    bool fireHeld_ = false;
    float viewHeight_ = 50.0f;
    float cursorX_ = kCursorCentreX;
    float cursorY_ = kCursorCentreY;
    int32_t bombsLeft_ = 10;
};

}  // namespace scripts
}  // namespace mdk

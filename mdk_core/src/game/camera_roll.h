#pragma once

// ============================================================================
// camera_roll.h — the view's bank angle, set by Kurt's moves
// (C# src/Mdk.Game/Kurt/CameraRoll.cs, original 0x573910 degrees).
//
// The follow camera rolls by this many degrees (positive banks the view to the
// right). The C# class owns the whole of that state: one writer per move and one
// leveller that runs at the top of every step unless a writer just ran.
//
//   running + turning (keys) -> +/-0.25 deg per tick up to +/-10 (damp_move 0x467fa4)
//   sliding                  -> 0.9*roll + 0.1*(90 - slope angle) per tick (0x468db8)
//   snowboard                -> towards the board's bank at 45 deg/s (0x46ac4c)
//   none of these            -> back to 0 by clamp(0.35*|roll|, 0.05, 2.5) deg per
//                               tick (damp_control)
//
// Only the walk writer and the leveller are ported, because those are the only two
// with a caller in this tree: the C#'s other two writers are Kurt.Slide
// (Kurt.Slide.cs:193) and the snowboard (Snowboard.cs:428), and neither subsystem
// exists here, exactly as the C#'s own guards on them take the false side in a
// build with no slide and no board. When the slide lands, Slide is a normal
// addition to this class; it is not stubbed today because nothing would call it.
//
// THE HELD STEP IS BEHAVIOUR, NOT BOOKKEEPING. Every writer sets a flag and Settle
// spends one whole step doing nothing instead of levelling (0x57ff74). A port that
// levels on the step a writer just set loses one tick of every bank, and — because
// the walk writer runs on every step of a turn — it banks less the longer Kurt
// turns. golden/L3_kurt_camroll.txt can see the held step at the edge of every
// window: the roll does not start to fall until the second step after the keys.
//
// THE CLAMPS PROPAGATE NaN. The C# writes them as MathF.Min/MathF.Max, so a NaN
// roll stays NaN instead of snapping to the limit; mathMin/mathMax are the twins
// that do that and fminf/fmaxf are not. Same rule as the eight speed clamps in
// scripts/script_runtime.cpp (review-20 F2), for the same reason: a silent
// recovery to a finite bound is a different game from a NaN that stays NaN.
//
// Ordinary concrete class: no virtuals, no templates, no exceptions (AGENTS.md §10).
// ============================================================================

namespace mdk {

class CameraRoll {
public:
    // The roll in DEGREES. The C# consumer converts it (FollowCamera.cs:139 passes
    // float.DegreesToRadians(kurt.CameraRoll.Roll) to Turn).
    float roll() const { return roll_; }

    // Running and turning with the keys (C# CameraRoll.Walk, called at Kurt.cs:218).
    // `right` is -Axis(TurnLeft, TurnRight) — the C#'s own negation, so a positive
    // `right` banks the view right. `forward` is the walk axis AFTER the
    // thrown/knocked/get-up/kept zeroing: the C# passes the same variable its
    // Accelerate calls got, so a knocked Kurt with both keys held does not bank.
    // Needs BOTH axes: standing still and turning banks nothing.
    void walk(float right, float forward, float delta);

    // The top of every step (C# CameraRoll.Settle, called at Kurt.cs:160, right
    // after FadeFlashes and before every early return below it). Levels out unless
    // a writer ran on the step just ended.
    void settle(float delta);

private:
    // C# CameraRoll's constants. Kurt.Ticks is 30 (Kurt.cs:25).
    static constexpr float kWalkRate = 0.25f * 30.0f;  // degrees per second
    static constexpr float kWalkLimit = 10.0f;         // degrees
    static constexpr float kWalkReverse = 2.0f;        // degrees, the jump back
    static constexpr float kDecayFactor = 0.35f;
    static constexpr float kDecayMin = 0.05f;          // degrees per tick
    static constexpr float kDecayMax = 2.5f;           // degrees per tick
    static constexpr float kTicks = 30.0f;             // C# Kurt.Ticks

    float roll_ = 0.0f;
    // "Something set the roll this step, so it doesn't level out" (0x57ff74).
    bool held_ = false;
};

} // namespace mdk

#pragma once

// ============================================================================
// strike_scene.h — the full-screen strike (0x4398f0). A 1:1 port of
// src/Mdk.Game/Scripts/StrikeScene.cs (itself a port of godot-mdk's
// strike_scene.gd): before Bones' air strike (X_STRIKB) and Kurt's strike at
// the end of the game (X_STRIKD) the game stops and only the sky and that model
// are drawn. The model plays its animation once while it turns at 45°/s; the
// camera sits at its reference point 1 looking at its reference point 0 (both
// moved by the animation), with a long lens (focal length 600 / 0.35265 pixels
// of the 360-high view). Esc skips it; the music goes on.
//
//   game paused ──► [ sky + model, camera on the model's points ] ──► finished ──► game resumes
//
// The runtime holds the last one played (ScriptRuntime::strikeScene(), the C#'s
// ScriptRuntime.Strike) and starts it in playStrikeScene: Bones' strike
// (AirStrike::call) and special_event 51 (Kurt's). The FRONTEND drives it, as
// the C#'s Viewer does through SniperView.DrawStrike (SniperView.cs:167-190):
// while active() the game's steps wait, update(elapsed) advances it (Esc /
// back: skip()), and the frame draws the sky and subject() alone from
// camera() (game/strike_view.h builds that view).
//
// EXACTNESS. Its frame, turn and camera are the C#'s to the bit (libm cosf/sinf
// for Matrix4x4.CreateRotationZ, fused Transform): golden/L5_strike_*.txt pins
// them. Presentation, cold: one model, once a frame while it plays.
// ============================================================================

#include <stdint.h>

#include "render/render_types.h"
#include "scripts/object.h"

namespace mdk {
namespace formats {
class Model;
class ModelAnimation;
} // namespace formats

namespace scripts {

class ScriptRuntime;

class StrikeScene {
public:
    // C# StrikeScene.Kind: Bones' plane, or Kurt's (event 51).
    enum Kind { Bones, Kurt };
    // C# StrikeScene.Plane: the plane with Bones in it, or the plane only (the dive).
    enum Plane { WithPilot, Only };

    // C# FieldOfView: the vertical field of view in degrees.
    static float fieldOfView();

    // C# Subject: the model shown, posed and turned (drawn like an object).
    const MdkObject& subject() const { return subject_; }
    // C# Active.
    bool active() const { return active_; }

    // C# Camera: its place and the point it looks at.
    void camera(Vec3* eye, Vec3* target) const;

    // C# Play: plays the strike of `kind` in Kurt's arena; it ends at once without
    // its model (the C#'s Finished, which nothing but the runtime's own event reads).
    void play(ScriptRuntime& runtime, Kind kind, Plane plane);

    // C# Update: advances by `delta` seconds; the animation's end finishes it.
    void update(float delta);

    // C# Skip: ends the scene (Esc).
    void skip();

    // The C#'s private state, read by the golden's dump (tick1 dump strike).
    float frame() const { return frame_; }
    float yaw() const { return yaw_; }

private:
    static int32_t hiddenParts(const formats::Model& model);
    void show();
    Vec3 referencePoint(int32_t index) const;

    const formats::ModelAnimation* animation_ = nullptr;
    float frame_ = 0.0f;
    float yaw_ = 0.0f;
    MdkObject subject_;
    bool active_ = false;
};

} // namespace scripts
} // namespace mdk

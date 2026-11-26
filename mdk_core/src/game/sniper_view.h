#pragma once

// ============================================================================
// sniper_view.h — what sniper mode draws besides the world through the scope
// (godot-mdk sniper_overlay.gd). A port of src/Mdk.Game/SniperView.cs.
//
//   world (scope view) ──► round cameras (insets: the world from behind each round)
//                     ──► HUD canvas (frame, mask, crosshair, zoom, ammo)
//                     ──► clip (inset over the canvas: the loaded rounds' models)
//
// WHO DOES WHAT.
//   * The world through the scope is the frame's own scene drawn with
//     buildSniperCamera (follow_camera.h, the C#'s FollowCamera.SniperView) in
//     place of the follow camera, Kurt hidden (KurtSprite::draw, as the C#'s
//     KurtSprite.Draw) -- the frontends choose the camera (Viewer.SceneView).
//   * The rounds in flight (SniperView.Draw's first loop, SniperRounds.Visuals)
//     are scripted-object models: the frontends add them to the frame's object
//     stream with the scripts' objects.
//   * draw() below is the rest of SniperView.Draw: the overlay's state
//     (SniperHud), the round cameras' insets and the clip's inset.
//
// THE INSETS (render.h). A round camera is the frame's scene again through the
// round's camera into its window (`Inset::scene`, under the canvas); the clip is
// three models of its own through a fixed camera over the whole view (over the
// canvas). Both backends draw both; the 3DS draws a round camera as the frame's
// scene records again through the round's camera (render.h). A frontend that
// passes `roundInsets` false gets a watching camera's window filled as an empty
// one (SniperOverlay::setRoundViewsDrawn).
//
// THE STRIKE. SniperView.Aim asks AirStrike.FindTarget (package #14's,
// scripts/air_strike.h), and the full-screen strike (DrawStrike) is #14's
// game/strike_view.{h,cpp}, which the frontends draw instead of this view.
//
// TRIG. The views' field of view (atan/tan) and the clip's rotations
// (sin/cos) are libm, as the C#'s MathF on the oracle's host:
// golden/L3_sniper_hud.txt pins the matrices bit for bit. Cold path: a handful
// of calls per frame, in sniper mode only.
// ============================================================================

#include <stdint.h>

#include "game/object_draw.h"
#include "game/object_view.h"
#include "game/sniper_overlay.h"
#include "render/render.h"
#include "render/render_types.h"
#include "scripts/object.h"
#include "util/matrix.h"

namespace mdk {

class Player;
class SurfaceCache;

namespace scripts {
class ScriptRuntime;
} // namespace scripts

// C# CameraMath.View (FollowCamera.cs:393): the view from `position` along
// `forward` with `up`, a `fieldOfView` degrees high. The look-at and the
// perspective are System.Numerics' (CreateLookAt, CreatePerspectiveFieldOfView)
// as .NET 10 computes them. Fills the camera's view, projection, position,
// forward, up (the given one) and planes. False when the line of sight is
// degenerate.
bool buildViewCamera(Camera* camera, const Vec3& position, const Vec3& forward, const Vec3& up,
                     float fieldOfView, float aspect, float nearPlane, float farPlane);

class SniperView {
public:
    // Round cameras are 90 degrees wide (0x461d80).
    static constexpr float kRoundFieldWidth = 90.0f;
    static constexpr float kRoundNear = 0.5f;
    static constexpr float kFar = 20000.0f;
    // The loaded rounds are seen with a focal length of 250 on the view.
    static constexpr float kClipFocal = 250.0f;
    static constexpr float kClipNear = 1.0f;
    static constexpr float kClipFar = 1000.0f;

    // C# ClipPlace: a loaded round's rotation, position and scale at clip time t
    // (between the clip's keys, 0 < t < 3).
    static void clipPlace(float t, Mat4* rotation, Vec3* position, float* scale);

    // C# OnCanvas: a rectangle of the 600x360 view on the canvas.
    static SniperOverlay::Rect onCanvas(const SniperOverlay::Rect& inView, float scale, float left);

    // DrawRoundCamera's view: a watching camera's view for its window `area`.
    static bool roundCameraView(Camera* camera, const scripts::SniperRounds::RoundCamera& round,
                                const SniperOverlay::Rect& area);
    // DrawClip's view, for its area.
    static bool clipView(Camera* camera, const SniperOverlay::Rect& area);

    // C# Aim: the air strike's target test (AirStrike.FindTarget), only with the
    // strike selected and the clip ready; None without `scripts`.
    static StrikeAim aim(const Player& player, const scripts::ScriptRuntime* scripts);

    // The SniperHud SniperView.Draw hands the overlay in sniper mode: the zoom, the
    // ammo, the rounds' three cameras (none without `scripts`) and Aim.
    // `tickAlpha`: the round cameras between ticks (upstream a73a5cd2).
    static SniperHud hudOf(const Player& player, const scripts::ScriptRuntime* scripts,
                           float tickAlpha = 1.0f);

    // C# RoundModels: the loaded rounds' model for an ammo type (clamped to 0-5).
    static const char* roundModel(int32_t type);

    // C# SniperView.Draw after its first loop: out of sniper mode the overlay is
    // told so; in it, the overlay's state, the round cameras' insets (when
    // `roundInsets`, above) and the clip's inset, whose
    // models are laid out by `objects` in `look` (the C#'s LookOf(CurrentArena))
    // and drawn through this view's own stream. `scripts` may be null (a level
    // without its scripts): no cameras and no clip.
    void draw(MdkRenderer renderer, SniperOverlay& overlay, const Player& player,
              scripts::ScriptRuntime* scripts, float delta, bool roundInsets, ObjectView& objects,
              SurfaceCache* surfaces, const ObjectLook& look, float tickAlpha = 1.0f);

    // The clip draw's refusal, for the frontend's log (ObjectDraw's).
    bool allocationFailed() const { return clipDraw_.allocationFailed(); }

private:
    void drawRoundCamera(MdkRenderer renderer, const scripts::SniperRounds::RoundCamera& camera,
                         const SniperOverlay::Rect& area);
    void drawClip(MdkRenderer renderer, const Player& player, scripts::ScriptRuntime& scripts,
                  const SniperOverlay::Rect& area, ObjectView& objects, SurfaceCache* surfaces,
                  const ObjectLook& look);

    // The loaded rounds (C# _clip): objects outside the list.
    scripts::MdkObject clip_[3];
    ObjectStream clipStream_;
    ObjectDraw clipDraw_;
};

} // namespace mdk

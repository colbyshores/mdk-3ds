#pragma once

// ============================================================================
// strike_view.h — the frontends' half of the full-screen strike (C# Mdk.Game
// SniperView.DrawStrike, SniperView.cs:167-190, called from Viewer.Update,
// Viewer.cs:389-410).
//
// While the runtime's strike scene (scripts/strike_scene.h) is active the game
// waits: neither Kurt nor the scripts step, the pause menu does not open (Esc /
// START skips the scene instead), and the frame is the sky and the scene's
// subject alone, seen from the scene's camera. Both frontends call these two
// in that order each frame:
//
//   advanceStrikeScene(runtime, skip, elapsed) ── true ──► buildStrikeCamera(...)
//        │                                                draw the sky + subject only
//        └── false ──► the ordinary frame (steps, arena, objects, Kurt, HUD)
//
// Presentation: the scene's own state is the oracle's to the bit (golden/
// L5_strike_*.txt pins StrikeScene); the view here is built the way the
// cutscene camera's is (render/camera_builder.h).
// ============================================================================

#include "render/render_types.h"

namespace mdk {

namespace scripts {
class ScriptRuntime;
class StrikeScene;
} // namespace scripts

// C# SniperView.DrawStrike's advance: when a strike scene plays, it advances by
// `elapsed` seconds (or ends, when `skip`: the C#'s Esc). True while it still
// holds the frame -- the caller then runs no step and draws only the scene.
bool advanceStrikeScene(scripts::ScriptRuntime* runtime, bool skip, float elapsed);

// C# SniperView.DrawStrike's view: CameraMath.View(eye, forward, UpOf(forward),
// StrikeScene.FieldOfView, aspect, RoundNear 0.5, Far 20000) from the scene's
// camera. False when the look-at is degenerate (the camera is left alone).
bool buildStrikeCamera(const scripts::StrikeScene& scene, float aspect, Camera* camera);

} // namespace mdk

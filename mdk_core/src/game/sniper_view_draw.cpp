// ============================================================================
// sniper_view_draw.cpp — the drawing half of src/Mdk.Game/SniperView.cs (its
// Draw, DrawRoundCamera and DrawClip). See sniper_view.h. Apart from
// sniper_view.cpp's arithmetic so the host suite (tests/scripts) can pin that
// without a renderer.
// ============================================================================

#include "game/sniper_view.h"

#include "game/player.h"
#include "game/scope.h"
#include "scripts/script_runtime.h"

namespace mdk {

namespace {

// Clip times at or past the last key are not shown (ClipKeys.Length - 1).
const float kClipShownBelow = 3.0f;

void putInset(MdkRenderer renderer, const Camera& camera, const SniperOverlay::Rect& area,
              bool scene, bool underCanvas) {
    Inset inset;
    inset.x = area.x;
    inset.y = area.y;
    inset.width = area.w;
    inset.height = area.h;
    inset.under_canvas = underCanvas;
    inset.scene = scene;
    inset.camera = camera;
    mdk_render_begin_inset(renderer, &inset);
}

} // namespace

void SniperView::draw(MdkRenderer renderer, SniperOverlay& overlay, const Player& player,
                      scripts::ScriptRuntime* scripts, float delta, bool roundInsets,
                      ObjectView& objects, SurfaceCache* surfaces, const ObjectLook& look) {
    // C# SniperView.Draw (SniperView.cs:49-76), after the rounds in flight.
    if (!player.sniping()) {
        overlay.update(delta, nullptr);
        return;
    }

    const SniperHud hud = hudOf(player, scripts);
    overlay.setRoundViewsDrawn(roundInsets);
    overlay.update(delta, &hud);

    float scale = 0.0f;
    float left = 0.0f;
    SniperOverlay::placement(mdk_render_canvas_width(renderer), &scale, &left);
    if (roundInsets) {
        for (int32_t i = 0; i < hud.cameraCount; i++) {
            drawRoundCamera(renderer, hud.cameras[i], onCanvas(SniperOverlay::kRoundViews[i], scale, left));
        }
    }

    if (scripts != nullptr) {
        const SniperOverlay::Rect view = {0.0f, 0.0f, SniperOverlay::kViewWidth,
                                          SniperOverlay::kViewHeight};
        drawClip(renderer, player, *scripts, onCanvas(view, scale, left), objects, surfaces, look);
    }
}

void SniperView::drawRoundCamera(MdkRenderer renderer,
                                 const scripts::SniperRounds::RoundCamera& camera,
                                 const SniperOverlay::Rect& area) {
    // C# DrawRoundCamera: a round camera watching its round, the world from behind it.
    if (camera.shot != scripts::SniperRounds::ShotWatching) {
        return;
    }
    Camera view;
    if (!roundCameraView(&view, camera, area)) {
        return;
    }
    putInset(renderer, view, area, true, true);
    mdk_render_end_inset(renderer);
}

void SniperView::drawClip(MdkRenderer renderer, const Player& player,
                          scripts::ScriptRuntime& scripts, const SniperOverlay::Rect& area,
                          ObjectView& objects, SurfaceCache* surfaces, const ObjectLook& look) {
    // C# DrawClip (SniperView.cs:109-147): the loaded rounds of the selected type
    // between the clip's keys; they slide into the chamber (key 0) after a shot.
    const formats::Model* model =
        scripts.findModel(scripts.currentArena(), roundModel(player.combat().selectedAmmo()));
    if (model == nullptr) {
        return;
    }

    Camera view;
    if (!clipView(&view, area)) {
        return;
    }
    const Scope& scope = player.scope();
    clipStream_.reset();
    bool open = false;
    for (int32_t i = 0; i < Scope::kClipSize; i++) {
        const float t = scope.clipTime + (float)i;
        if (i >= scope.clipRounds || t == 0.0f || t >= kClipShownBelow) {
            continue;
        }
        if (!open) {
            putInset(renderer, view, area, false, false);
            open = true;
        }
        scripts::MdkObject& round = clip_[i];
        round.model = model;
        round.flags = scripts::MdkObject::FlagRolling;
        clipPlace(t, &round.rolling_basis, &round.position, &round.scale);
        objects.addObject(round, look, round.transform(), &clipStream_);
    }

    if (open) {
        clipDraw_.draw(renderer, surfaces, clipStream_);
        mdk_render_end_inset(renderer);
    }
}

} // namespace mdk

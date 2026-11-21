#pragma once

// ============================================================================
// movie_view.h -- the GPU side of the movies and the INTRO1A splash (package
// #20c): the C#'s PalettedImage for a VideoPlayer's picture (Ui.cs) and the
// splash's, made through the renderer seam and drawn on the canvas. Shared by
// both frontends; what differs per target is a flag each frontend passes, not
// an #ifdef (AGENTS.md §7).
//
//   VideoPlayer --versions moved--> mdk_render_update_paletted --> canvas quad
//
// THE UPLOADS. MovieView::sync sends what moved since the last call: the
// indices when the player's image version moved, the palette when its palette
// version did. The 3DS turns the texture into a streamed one (render.h): each
// upload is a whole 600 x 360 picture through its palette on the ARM11, done at
// the next frame boundary, and uploads between two boundaries collapse into
// the newest. `halfRate` (EnhancedToggles::movieHalfRate) sends every second
// decoded frame -- and always the last one, so a finished movie stands on its
// own last frame as the C#'s does.
//
// THE SPLASH. `crossFade` false (Linux): one picture, its palette set from
// IntroSplash::palette() each frame (the C#'s Draw, exactly). `crossFade` true
// (the 3DS): the picture through palette 1 and through palette 2, each uploaded
// once, and the fade drawn by the GPU with IntroSplash::fade()'s weight --
// Blend's arithmetic up to the framebuffer's rounding, with no per-frame
// expansion (owner 2026-11-18: performance over accuracy on the 3DS).
//
// Everything is made inside the caller's renderer scope; release() gives it
// back early. Cold code (menus). No virtuals, no templates (AGENTS.md §10).
// ============================================================================

#include <stdint.h>

#include "game/screen_view.h"
#include "render/render.h"

namespace mdk {
namespace menu {
class IntroSplash;
class VideoPlayer;
}

struct MovieView {
    PalettedImage image;
    uint32_t imageVersion = 0;
    uint32_t paletteVersion = 0;
    // Uploads made (for the log: the 3DS's half rate halves them).
    uint32_t uploads = 0;

    // Sends the player's picture and palette if they moved (see the header).
    void sync(MdkRenderer renderer, const menu::VideoPlayer& player, bool halfRate);
    // C# VideoPlayer.Draw: the picture fitted in the player's view, centred.
    void draw(MdkRenderer renderer, const menu::VideoPlayer& player, float canvasWidth) const;
    void release(MdkRenderer renderer);
};

struct SplashView {
    PalettedImage image;
    // The cross-fade's second picture (palette 2); `image` is palette 1's.
    PalettedImage second;
    bool crossFade = false;
    uint8_t lastRgb[768] = {};
    bool hasLast = false;

    // Makes the picture(s) for an open, not-done splash. False when nothing
    // could be made (the splash then draws black, as without INTRO1A).
    bool open(MdkRenderer renderer, const menu::IntroSplash& splash, bool crossFade);
    // C# IntroSplash.Draw (nothing once done), on the 600 x 360 view fitted
    // inside the canvas.
    void draw(MdkRenderer renderer, menu::IntroSplash& splash, float canvasWidth);
    void release(MdkRenderer renderer);
};

} // namespace mdk

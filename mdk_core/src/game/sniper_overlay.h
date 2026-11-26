#pragma once

// ============================================================================
// sniper_overlay.h — sniper mode's screen (HUD 0x41e128, 0x420830;
// TRAVSPRT.BNI). A 1:1 port of src/Mdk.Game/Hud/SniperOverlay.cs.
//
// Drawn over the whole 640x480 screen, scaled to the 360-high canvas and
// centred, black beside it: the SNIPERS1 frame around the 600x360 view at
// (20, 60), the round cameras' fills, the air strike's iris, the SNIPERS2
// mask, the CROSS crosshair, the zoom in percent with its SNIP_RNG gauge, the
// ammo types and the health.
//
//   ┌────────── SNIPERS1 ───────────┐
//   │ [cam] [cam] [cam]             │   view (20, 60) 600x360
//   │      ┌─ scope ─┐   zoom %     │
//   │      │    +    │   gauge      │
//   │ ammo └─────────┘     health   │
//   └───────────────────────────────┘
//
// The round cameras' and the loaded rounds' 3D views are not drawn here: they
// are renderer insets (game/sniper_view.h, the C#'s SniperView), placed by
// kRoundViews and kViewX/kViewY through placement().
//
// HudView owns one (the C#'s HudView.Sniper) and draws it instead of the HUD
// while it shows (visible()).
//
// THE BUDGET (a 3DS concession, chosen by the frontend, not an #ifdef). The
// frame and the mask are 640x480 and 600x360 index images. The Citro3D backend
// pads a texture to powers of two and keeps a canvas texture RGBA8, so the two
// whole images would cost 2 MiB each of linear memory -- more than the HUD and
// the fonts together. With `tiles` they are cut into 63x63 pieces (in a 64x64
// texture: clamped, not wrapped, so a filtered edge does not bleed the far side
// in) and the pieces that are wholly transparent are not made at all (the cut
// frame's middle is the view); with `shrink` 2 every second texel of every
// second row is kept. On the 3DS's top screen an overlay texel is half a pixel
// (360/480 of the canvas, 240/360 of the panel), so the shrink takes nothing the
// panel can show (the owner's memory-over-texture-precision rule, 2026-11-19).
// Linux keeps the default: the whole images, every texel, as the C#.
//
// THE ROUND CAMERAS' WINDOWS. The C# leaves a watching camera's window to its
// inset (a second scene pass into a sub-viewport); both frontends draw it. A
// frontend that does not draw them calls setRoundViewsDrawn(false) and a
// watching camera's window is filled as an empty one instead of showing the
// scope's view through the mask's hole.
//
// Cold path: a few dozen canvas quads a frame in sniper mode (the iris, which
// needs a strike target, is a span per row of its 280 rows).
// ============================================================================

#include <stdint.h>

#include <vector>

#include "render/render.h"
#include "render/render_types.h"
#include "scripts/sniper_rounds.h"

namespace mdk {

namespace formats {
class Bni;
class Palette;
} // namespace formats

// C# StrikeAim: whether Bones' air strike has a target in the crosshair
// (0x4641ac), checked only with the strike selected and the clip ready.
enum class StrikeAim { NotChecked, None, Target };

// C# Blink: whether the health's digits show (they blink when it's low).
enum class Blink { Shown, Hidden };

// C# SniperHud: what the sniper screen shows this frame. A fixed record (the
// C#'s lists are Kurt's five ammo counts and the rounds' three cameras), so a
// frontend filling it every frame does not allocate.
struct SniperHud {
    float zoom = 1.0f;
    // Inventory.Ammo: rounds of types 1-5 (ammo[i] is type i + 1).
    int32_t ammo[5] = {0, 0, 0, 0, 0};
    int32_t selectedAmmo = 0;
    scripts::SniperRounds::RoundCamera cameras[scripts::SniperRounds::kSlots] = {};
    int32_t cameraCount = 0;
    StrikeAim aim = StrikeAim::NotChecked;
};

class SniperOverlay {
public:
    // SniperScreen's sizes.
    static constexpr float kScreenWidth = 640.0f;
    static constexpr float kScreenHeight = 480.0f;
    static constexpr float kViewWidth = 600.0f;
    static constexpr float kViewHeight = 360.0f;
    // C# View: where the 600x360 view sits on the screen.
    static constexpr float kViewX = 20.0f;
    static constexpr float kViewY = 60.0f;

    struct Rect {
        float x;
        float y;
        float w;
        float h;
    };
    // C# RoundViews: the round cameras' windows in the view (0x461d80).
    static const Rect kRoundViews[3];

    // The memory budget (above). The defaults are the C#'s.
    struct Budget {
        bool tiles = false;
        int32_t shrink = 1;  // 1, or 2 (every second texel); more is read as 2
    };
    Budget budget;

    // The C# constructor: the screen's images from `sprites`, indexed through
    // `palette`, drawn with the HUD's palette texture `paletteTexture`. False
    // when an image is missing (the C# throws there): the overlay then never
    // shows and the HUD stays.
    bool load(MdkRenderer renderer, const formats::Bni& sprites, const formats::Palette& palette,
              int32_t paletteTexture);
    bool loaded() const { return loaded_; }

    // C# Visible: whether sniper mode's screen replaces the HUD.
    bool visible() const { return shown_ && loaded_; }

    // C# Update: each frame, what to show (null out of sniper mode), the iris
    // and the gauge.
    void update(float delta, const SniperHud* hud);

    // C# Placement: the screen's scale and left edge on the canvas.
    static void placement(float canvasWidth, float* scale, float* left);

    // C# Draw: the sniper screen, and the health centred in the panel at
    // (panelX, panelY) (pixels of the view) of (panelW, panelH).
    void draw(MdkRenderer renderer, float canvasWidth, int32_t health, Blink blink, float panelX,
              float panelY, float panelW, float panelH);

    // The 3DS's missing round-camera insets (above).
    void setRoundViewsDrawn(bool drawn) { roundViewsDrawn_ = drawn; }

    // C# ZoomFraction and the percent DrawNumber prints (MathF.Round: to even).
    static float zoomFraction(float zoom);
    static int32_t zoomPercent(float zoom);

    // The state, for the host probe's dump (golden/L3_sniper_hud.txt).
    float iris() const { return iris_; }
    float pulse() const { return pulse_; }
    bool target() const { return target_; }
    int32_t gaugeShown() const { return gaugeShown_; }

private:
    // C# Image: a texture, its size and its hotspot. A tiled image (the budget)
    // is several pieces, each a texture at (x, y) of w x h screen pixels.
    struct Piece {
        int32_t texture = -1;
        float x = 0.0f;
        float y = 0.0f;
        float w = 0.0f;
        float h = 0.0f;
    };
    struct Image {
        int32_t texture = -1;
        float w = 0.0f;
        float h = 0.0f;
        float hotspotX = 0.0f;
        float hotspotY = 0.0f;
        std::vector<Piece> pieces;  // only for a tiled image
    };
    // C# Screen: draws in 640x480 screen pixels on the canvas.
    struct Screen {
        MdkRenderer renderer;
        int32_t palette;
        float scale;
        float left;
    };

    bool loadImage(const formats::Bni& sprites, const char* name, Image* out,
                   const formats::Palette& palette);
    bool loadIndices(const uint8_t* indices, int32_t width, int32_t height, Image* out,
                     const formats::Palette& palette, bool fit);
    void updateIris(float delta, const SniperHud& hud);
    void drawFrame(const Screen& screen);
    void drawCameras(const Screen& screen, const SniperHud& hud);
    void drawIris(const Screen& screen);
    void drawAmmo(const Screen& screen, const SniperHud& hud);
    void drawNumber(const Screen& screen, int32_t value, float atX, float atY);
    static void drawSpan(const Screen& screen, int32_t y, float nearX, float farX,
                         const Colour& colour);
    static void fill(const Screen& screen, float x, float y, float w, float h, const Colour& c);
    static void image(const Screen& screen, const Image& image, float atX, float atY);
    static void part(const Screen& screen, const Image& image, float sx, float sy, float sw,
                     float sh, float atX, float atY);
    static void quad(MdkRenderer renderer, float x, float y, float w, float h, float u0, float v0,
                     float u1, float v1, int32_t texture, int32_t palette, const Colour& colour);
    Colour colour(int32_t index) const;

    MdkRenderer renderer_ = nullptr;
    int32_t palette_ = -1;
    uint8_t colours_[256 * 4] = {0};
    bool loaded_ = false;
    Image frame_;
    Image mask_;
    Image cross_;
    Image gauge_;
    Image weapon_;
    Image digits_;
    Image icons_[6];
    Image labels_[6];
    std::vector<Image> miss_;

    bool shown_ = false;
    SniperHud hud_;
    int32_t gaugeShown_ = 0;
    // The iris: its opening (1 open, 0 closed), the pulse, and whether the
    // strike has a target.
    float iris_ = 1.0f;
    float pulse_ = 1.0f;
    bool target_ = false;
    bool roundViewsDrawn_ = true;
};

} // namespace mdk

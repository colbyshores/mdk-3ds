// enhanced_menu.cpp -- see enhanced_menu.h.

#include "game/enhanced_menu.h"

#include "game/font_view.h"

namespace mdk {
namespace {

// The rows, in the config file's order (SPEC.md 7.9's flag order).
const char* const kLabels[EnhancedMenu::kRowCount] = {
    "SKY SHADOWS", "FRAGMENT LIGHTS", "HAZE", "MIRRORS", "GLOW", "FILTERING",
    "MIPMAPS", "HD TEXTURES", "LIGHTING", "EDGE SMOOTHING",
};

bool valueOf(const EnhancedToggles& toggles, int32_t row) {
    switch (row) {
        case 0: return toggles.enableSkyShadows;
        case 1: return toggles.enableFragmentLights;
        case 2: return toggles.enableHaze;
        case 3: return toggles.enableMirrors;
        case 4: return toggles.enableGlowBillboards;
        case 5: return toggles.enableFiltering;
        case 6: return toggles.enableMipmaps;
        case 7: return toggles.enableHdTextures;
        case 8: return toggles.enableLighting;
        default: return toggles.enableEdgeSmoothing;
    }
}

void flip(EnhancedToggles* toggles, int32_t row) {
    switch (row) {
        case 0: toggles->enableSkyShadows = !toggles->enableSkyShadows; break;
        case 1: toggles->enableFragmentLights = !toggles->enableFragmentLights; break;
        case 2: toggles->enableHaze = !toggles->enableHaze; break;
        case 3: toggles->enableMirrors = !toggles->enableMirrors; break;
        case 4: toggles->enableGlowBillboards = !toggles->enableGlowBillboards; break;
        case 5: toggles->enableFiltering = !toggles->enableFiltering; break;
        case 6: toggles->enableMipmaps = !toggles->enableMipmaps; break;
        case 7: toggles->enableHdTextures = !toggles->enableHdTextures; break;
        case 8: toggles->enableLighting = !toggles->enableLighting; break;
        default: toggles->enableEdgeSmoothing = !toggles->enableEdgeSmoothing; break;
    }
}

// A flat canvas rectangle (the HudView::fillRect shape).
void fillRect(MdkRenderer renderer, float x, float y, float w, float h, const Colour& colour) {
    CanvasQuad q;
    q.x0 = x; q.y0 = y;
    q.x1 = x + w; q.y1 = y;
    q.x2 = x + w; q.y2 = y + h;
    q.x3 = x; q.y3 = y + h;
    q.u0 = 0.0f; q.v0 = 0.0f;
    q.u1 = 1.0f; q.v1 = 0.0f;
    q.u2 = 1.0f; q.v2 = 1.0f;
    q.u3 = 0.0f; q.v3 = 1.0f;
    q.colour = colour;
    q.texture = -1;
    q.palette = -1;
    mdk_render_canvas_quad(renderer, &q);
}

} // namespace

bool EnhancedMenu::handle(const Input& input, EnhancedToggles* toggles) {
    if (!open_ || !toggles) {
        return false;
    }
    if (input.close) {
        open_ = false;
        return false;
    }
    if (input.up && selected_ > 0) {
        selected_--;
    }
    if (input.down && selected_ < kRowCount - 1) {
        selected_++;
    }
    if (input.confirm || input.left || input.right) {
        flip(toggles, selected_);
        return true;
    }
    return false;
}

void EnhancedMenu::draw(MdkRenderer renderer, FontView& big, FontView& small,
                        float canvasWidth, const EnhancedToggles& toggles) const {
    if (!open_ || !big.ready() || !small.ready()) {
        return;
    }
    // Sized from the fonts' own metrics: the FONTBIG title is 341
    // units wide, the widest FONTSML row at 1.5x is 248, so a 400-wide
    // panel holds both with the value column clear of the labels.
    const float kPanelW = 400.0f;
    const float kPanelH = 304.0f;   // ten rows at 22 + the title and the footer
    const float px = (canvasWidth - kPanelW) * 0.5f;
    const float py = 58.0f;
    const Colour kPanel{0.0f, 0.0f, 0.0f, 0.72f};
    const Colour kTitle{1.0f, 0.85f, 0.4f, 1.0f};
    const Colour kRow{0.75f, 0.75f, 0.75f, 1.0f};
    const Colour kSelected{1.0f, 1.0f, 1.0f, 1.0f};
    const Colour kOn{0.45f, 1.0f, 0.45f, 1.0f};
    const Colour kOff{1.0f, 0.45f, 0.45f, 1.0f};
    const Colour kHint{0.55f, 0.55f, 0.55f, 1.0f};
    const Colour kMark{1.0f, 0.85f, 0.4f, 1.0f};

    fillRect(renderer, px, py, kPanelW, kPanelH, kPanel);

    const char* title = "VIDEO / GRAPHICS";
    const float titleX = px + (kPanelW - (float)big.width(title)) * 0.5f;
    big.draw(renderer, title, titleX, py + 40.0f, 1.0f, kTitle);

    for (int32_t row = 0; row < kRowCount; row++) {
        const float baseline = py + 74.0f + (float)row * 22.0f;
        const bool isSelected = row == selected_;
        if (isSelected) {
            small.draw(renderer, ">", px + 14.0f, baseline, 1.5f, kMark);
        }
        small.draw(renderer, kLabels[row], px + 30.0f, baseline, 1.5f,
                   isSelected ? kSelected : kRow);
        const bool on = valueOf(toggles, row);
        const char* value = on ? "ON" : "OFF";
        small.draw(renderer, value, px + kPanelW - 24.0f - (float)small.width(value) * 1.5f,
                   baseline, 1.5f, on ? kOn : kOff);
    }

    small.draw(renderer, "A: TOGGLE   SELECT: CLOSE", px + 30.0f, py + kPanelH - 14.0f, 1.0f,
               kHint);
}

} // namespace mdk

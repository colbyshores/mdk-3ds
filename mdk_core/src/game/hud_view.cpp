// ============================================================================
// hud_view.cpp — the port of HudView.cs. See hud_view.h.
// ============================================================================

#include "game/hud_view.h"

#include <cstdio>

#include "formats/bni.h"
#include "formats/palette.h"
#include "formats/sprite_animation.h"
#include "formats/texture.h"

namespace mdk {

namespace {

// The C# HudView's layout constants. The canvas height is render_types.h's
// kCanvasHeight; its width comes from the renderer for the current frame.
const float kPanelRight = 16.0f;
const float kPanelBottom = 10.0f;
const int32_t kLowHealth = 20;
const int32_t kBlinkMask = 31;
const int32_t kBlinkOn = 16;
const int32_t kMaxNumber = 999;
const float kDigitWidth = 8.0f;
const float kBarScale = 500.0f / 900.0f;
const float kBarWidth = 500.0f;
const float kBarTop = 4.0f;
const float kBarHeight = 7.0f;
const int32_t kBarFill = 3;
const int32_t kBarFrame = 4;
const float kSlotStep = 48.0f;
const float kIconAnchorX = 32.0f;
const float kIconAnchorY = 328.0f;
const float kSelectionX = 8.0f;
const float kSelectionY = 304.0f;
const float kSelectionW = 47.0f;
const float kSelectionH = 47.0f;
const float kCountRise = 12.0f;
const float kFlashMax = 255.0f;
const float kHurtAlpha = 0.4f;

Colour paletteColour(const formats::Palette& palette, int32_t index) {
    const uint8_t* rgba = palette.rgba();
    Colour c;
    c.r = rgba[index * 4] / 255.0f;
    c.g = rgba[index * 4 + 1] / 255.0f;
    c.b = rgba[index * 4 + 2] / 255.0f;
    c.a = 1.0f;
    return c;
}

float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

} // namespace

bool HudView::load(MdkRenderer renderer, const formats::Bni& sprites,
                   const formats::Palette& palette) {
    renderer_ = renderer;
    palette_ = mdk_render_create_palette(renderer, palette.rgba());
    if (palette_ < 0) {
        return false;
    }

    loadImage(sprites, "SC_STAT", &panel_, palette);
    loadImage(sprites, "SKULL", &skull_, palette);
    loadImage(sprites, "SNIP_TXT", &digits_, palette);

    formats::SpriteAnimation pickups;
    if (sprites.animation("PICKUPS", &pickups)) {
        for (int32_t i = 0; i < pickups.frameCount(); i++) {
            formats::SpriteAnimation::Frame frame;
            if (!pickups.frame(i, &frame)) {
                continue;
            }
            Image icon;
            icon.texture = mdk_render_create_index_texture(
                renderer, frame.image.width(), frame.image.height() * frame.image.frameCount(),
                frame.image.indices(), palette.rgba());
            icon.w = (float)frame.image.width();
            icon.h = (float)frame.image.height();
            icon.hotspotX = frame.hotspotX;
            icon.hotspotY = frame.hotspotY;
            icons_.push_back(icon);
        }
    }

    barFill_ = paletteColour(palette, kBarFill);
    barFrame_ = paletteColour(palette, kBarFrame);

    // The panel and the digits are required; the skull and the icons degrade.
    return panel_.texture >= 0 && digits_.texture >= 0;
}

void HudView::loadImage(const formats::Bni& sprites, const char* name, Image* out,
                        const formats::Palette& palette) {
    formats::Texture texture;
    if (!sprites.image(name, &texture)) {
        return;
    }
    // The palette's bytes go in with the indices. The C# hands its
    // `CreateIndexTexture` the indices alone and binds the palette at draw
    // (CanvasImages.cs:14), which is what the Vulkan backend does with them; the
    // 3DS has no palette texture, so it needs the bytes now to expand the indices
    // into texels. It is the SAME palette the draw names (`palette_` above and
    // `q.palette` in drawImage), so the two targets end up with one palette's
    // colours either way -- but only if the bytes are handed over here.
    out->texture = mdk_render_create_index_texture(renderer_, texture.width(),
                                                   texture.height() * texture.frameCount(),
                                                   texture.indices(), palette.rgba());
    out->w = (float)texture.width();
    out->h = (float)texture.height();
}

void HudView::tick() { blink_ = (blink_ + 1) & kBlinkMask; }

void HudView::draw(MdkRenderer renderer, const HudState& state) {
    renderer_ = renderer;
    const float canvasWidth = mdk_render_canvas_width(renderer_);

    // The flashes (C# DrawFlashes).
    if (state.dead) {
        if (skull_.texture >= 0) {
            const float alpha = clampf(state.hurtFlash / kFlashMax, 0.0f, 1.0f);
            drawImage(skull_, canvasWidth / 2.0f - skull_.w / 2.0f,
                      kCanvasHeight / 2.0f - skull_.h / 2.0f, Colour{1.0f, 1.0f, 1.0f, alpha});
        }
    } else if (state.hurtFlash > 0.0f) {
        fillRect(0.0f, 0.0f, canvasWidth, kCanvasHeight,
                 Colour{1.0f, 0.0f, 0.0f, state.hurtFlash / kFlashMax * kHurtAlpha});
    }
    if (state.whiteFlash > 0.0f) {
        fillRect(0.0f, 0.0f, canvasWidth, kCanvasHeight,
                 Colour{1.0f, 1.0f, 1.0f, clampf(state.whiteFlash / kFlashMax, 0.0f, 1.0f)});
    }

    // The health panel at the bottom right; the number blinks when health is low.
    const float panelX = canvasWidth - (panel_.w + kPanelRight);
    const float panelY = kCanvasHeight - (panel_.h + kPanelBottom);
    drawImage(panel_, panelX, panelY, Colour{1.0f, 1.0f, 1.0f, 1.0f});
    if (state.health > kLowHealth || blink_ < kBlinkOn) {
        const float centreX = panelX + (float)((int32_t)panel_.w >> 1);
        const float numberY = panelY + (float)(((int32_t)panel_.h - (int32_t)digits_.h) >> 1);
        drawNumber(state.health, centreX, numberY);
    }

    drawInventory(state);
    drawBar(state.barHealth, state.barMax);
}

void HudView::drawImage(const Image& image, float x, float y, const Colour& tint) {
    if (image.texture < 0) {
        return;
    }
    CanvasQuad q;
    q.x0 = x; q.y0 = y;
    q.x1 = x + image.w; q.y1 = y;
    q.x2 = x + image.w; q.y2 = y + image.h;
    q.x3 = x; q.y3 = y + image.h;
    q.u0 = 0.0f; q.v0 = 0.0f;
    q.u1 = 1.0f; q.v1 = 0.0f;
    q.u2 = 1.0f; q.v2 = 1.0f;
    q.u3 = 0.0f; q.v3 = 1.0f;
    q.colour = tint;
    q.texture = image.texture;
    q.palette = palette_;
    mdk_render_canvas_quad(renderer_, &q);
}

void HudView::drawNumber(int32_t value, float centreX, float y) {
    if (value > kMaxNumber) {
        value = kMaxNumber;
    }
    if (value < 0) {
        value = 0;
    }
    char text[8];
    std::snprintf(text, sizeof(text), "%d", (int)value);
    int32_t length = 0;
    while (text[length]) {
        length++;
    }
    float x = centreX - kDigitWidth / 2.0f * (float)length;
    const float digitW = kDigitWidth / (digits_.w > 0.0f ? digits_.w : 1.0f);
    for (const char* p = text; *p; ++p) {
        const int32_t digit = *p - '0';
        if (digit < 0 || digit > 9) {
            continue;
        }
        const float uL = (float)digit * digitW;
        CanvasQuad q;
        q.x0 = x; q.y0 = y;
        q.x1 = x + kDigitWidth; q.y1 = y;
        q.x2 = x + kDigitWidth; q.y2 = y + digits_.h;
        q.x3 = x; q.y3 = y + digits_.h;
        q.u0 = uL; q.v0 = 0.0f;
        q.u1 = uL + digitW; q.v1 = 0.0f;
        q.u2 = uL + digitW; q.v2 = 1.0f;
        q.u3 = uL; q.v3 = 1.0f;
        q.colour = {1.0f, 1.0f, 1.0f, 1.0f};
        q.texture = digits_.texture;
        q.palette = palette_;
        mdk_render_canvas_quad(renderer_, &q);
        x += kDigitWidth;
    }
}

void HudView::drawInventory(const HudState& state) {
    if (state.selected < (int32_t)state.slots.size() &&
        state.slots[state.selected].first != kSuperChainGun) {
        const float bx = kSelectionX + (float)state.selected * kSlotStep;
        fillRect(bx + 1.0f, kSelectionY + 1.0f, kSelectionW - 2.0f, kSelectionH - 2.0f,
                 Colour{0.0f, 0.0f, 0.0f, 0.5f});
        frameRect(bx, kSelectionY, kSelectionW, kSelectionH,
                  Colour{0.75f, 0.75f, 0.75f, 1.0f});
    }

    for (size_t i = 0; i < state.slots.size(); i++) {
        const int32_t item = state.slots[i].first;
        const int32_t count = state.slots[i].second;
        const int32_t frame = item - 1;
        if (frame < 0 || frame >= (int32_t)icons_.size()) {
            continue;
        }
        const Image& icon = icons_[frame];
        const float anchorX = kIconAnchorX + (float)i * kSlotStep;
        const float anchorY = kIconAnchorY;
        drawImage(icon, anchorX - (float)icon.hotspotX, anchorY - (float)icon.hotspotY,
                  Colour{1.0f, 1.0f, 1.0f, 1.0f});
        const int32_t shown = item == kSuperChainGun ? state.superChainGun : count;
        if (shown > 1) {
            drawNumber(shown, anchorX, anchorY - kCountRise);
        }
    }
}

void HudView::drawBar(int32_t health, int32_t max) {
    if (max <= 0) {
        return;
    }
    const float fill = clampf((float)health * kBarScale, 0.0f, kBarWidth);
    const float frame = clampf((float)max * kBarScale, 0.0f, kBarWidth);
    fillRect(0.0f, kBarTop, fill + 1.0f, kBarHeight, barFill_);
    frameRect(0.0f, kBarTop, frame + 1.0f, kBarHeight, barFrame_);
}

void HudView::fillRect(float x, float y, float w, float h, const Colour& colour) {
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
    mdk_render_canvas_quad(renderer_, &q);
}

void HudView::frameRect(float x, float y, float w, float h, const Colour& colour) {
    fillRect(x, y, w, 1.0f, colour);
    fillRect(x, y + h - 1.0f, w, 1.0f, colour);
    fillRect(x, y, 1.0f, h, colour);
    fillRect(x + w - 1.0f, y, 1.0f, h, colour);
}

} // namespace mdk

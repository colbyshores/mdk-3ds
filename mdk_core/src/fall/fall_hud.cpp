// fall_hud.cpp — see fall_hud.h. The C#'s FallHud.cs, method for method.

#include "fall/fall_hud.h"

#include <stdio.h>

#include "formats/bni.h"
#include "formats/font.h"
#include "formats/fti.h"
#include "formats/sprite_animation.h"
#include "formats/texture.h"
#include "game/kurt.h"

namespace mdk {
namespace fall {

namespace {

constexpr int32_t kBlinkMask = 31;
constexpr int32_t kBlinkOn = 16;
constexpr int32_t kLowHealth = 20;
constexpr int32_t kMaxNumber = 999;
constexpr float kDigitWidth = 8.0f;
constexpr float kPanelRight = 16.0f;
constexpr float kPanelBottom = 10.0f;
constexpr float kSlotStep = 48.0f;
constexpr float kIconAnchorX = 32.0f;
constexpr float kIconAnchorY = 328.0f;
constexpr float kCountRise = 12.0f;
constexpr float kViewCentreY = 180.0f;
// The skull grows with the red: full size at 256/255.
constexpr float kSkullGrowth = 255.0f / 256.0f;

} // namespace

bool FallHud::load(MdkRenderer renderer, const formats::Bni& bni, int32_t palette, const uint8_t* paletteRgba,
                   const formats::Fti& fti, const formats::Font& big, const formats::Font& small) {
    palette_ = palette;
    paletteRgba_ = paletteRgba;
    // HudView.Images: the panel, the skull and the digits, then PICKUPS' frames.
    renderer_ = renderer;
    loadImage(bni, "SC_STAT", &panel_);
    loadImage(bni, "SKULL", &skull_);
    loadImage(bni, "SNIP_TXT", &digits_);
    formats::SpriteAnimation pickups;
    if (bni.animation("PICKUPS", &pickups)) {
        icons_.reserve((size_t)pickups.frameCount());
        for (int32_t i = 0; i < pickups.frameCount(); i++) {
            formats::SpriteAnimation::Frame frame;
            Image icon;
            if (pickups.frame(i, &frame)) {
                icon.texture = mdk_render_create_index_texture(
                    renderer, frame.image.width(), frame.image.height() * frame.image.frameCount(),
                    frame.image.indices(), paletteRgba);
                icon.width = (float)frame.image.width();
                icon.height = (float)frame.image.height();
                icon.hotspotX = frame.hotspotX;
                icon.hotspotY = frame.hotspotY;
            }
            icons_.push_back(icon);
        }
    }

    // The fonts use the fall's palette and its effects (fall.gd).
    messages_.setTexts(fti);
    const bool fonts = big_.load(renderer, big, palette, paletteRgba) &&
                       small_.load(renderer, small, palette, paletteRgba);
    return fonts && panel_.texture >= 0 && digits_.texture >= 0;
}

void FallHud::loadImage(const formats::Bni& bni, const char* name, Image* out) {
    formats::Texture texture;
    if (!bni.image(name, &texture)) {
        return;
    }
    // Made with the palette's base colours, which the 3DS expands through (render.h).
    out->texture = mdk_render_create_index_texture(renderer_, texture.width(),
                                                   texture.height() * texture.frameCount(),
                                                   texture.indices(), paletteRgba_);
    out->width = (float)texture.width();
    out->height = (float)texture.height();
}

void FallHud::tick(int32_t ticks) {
    blink_ = (blink_ + ticks) & kBlinkMask;
}

void FallHud::drawImage(MdkRenderer renderer, const Image& image, float x, float y, float width,
                        float height) {
    if (image.texture < 0) {
        return;
    }
    CanvasQuad q;
    q.x0 = x;
    q.y0 = y;
    q.x1 = x + width;
    q.y1 = y;
    q.x2 = x + width;
    q.y2 = y + height;
    q.x3 = x;
    q.y3 = y + height;
    q.u0 = 0.0f;
    q.v0 = 0.0f;
    q.u1 = 1.0f;
    q.v1 = 0.0f;
    q.u2 = 1.0f;
    q.v2 = 1.0f;
    q.u3 = 0.0f;
    q.v3 = 1.0f;
    q.texture = image.texture;
    q.palette = palette_;
    mdk_render_canvas_quad(renderer, &q);
}

void FallHud::drawNumber(MdkRenderer renderer, int32_t value, float x0, float y) {
    // C# DrawNumber: at most 999, centred on x0, 8-pixel digits (0x420bd0).
    if (digits_.texture < 0) {
        return;
    }
    char text[16];
    snprintf(text, sizeof(text), "%d", (int)(value < kMaxNumber ? value : kMaxNumber));
    int32_t length = 0;
    while (text[length] != '\0') {
        length++;
    }
    float x = x0 - kDigitWidth / 2.0f * (float)length;
    const float unit = kDigitWidth / digits_.width;
    for (int32_t i = 0; i < length; i++) {
        const int32_t digit = text[i] - '0';
        if (digit < 0 || digit > 9) {
            x += kDigitWidth;
            continue;
        }
        const float u = (float)digit * unit;
        CanvasQuad q;
        q.x0 = x;
        q.y0 = y;
        q.x1 = x + kDigitWidth;
        q.y1 = y;
        q.x2 = x + kDigitWidth;
        q.y2 = y + digits_.height;
        q.x3 = x;
        q.y3 = y + digits_.height;
        q.u0 = u;
        q.v0 = 0.0f;
        q.u1 = u + unit;
        q.v1 = 0.0f;
        q.u2 = u + unit;
        q.v2 = 1.0f;
        q.u3 = u;
        q.v3 = 1.0f;
        q.texture = digits_.texture;
        q.palette = palette_;
        mdk_render_canvas_quad(renderer, &q);
        x += kDigitWidth;
    }
}

void FallHud::draw(MdkRenderer renderer, int32_t health, const Kurt& inventory, Life life, float red) {
    const float width = mdk_render_canvas_width(renderer);
    if (life == Dying) {
        float grow = red * kSkullGrowth;
        grow = grow < 1.0f ? grow : 1.0f;
        const float w = skull_.width * grow;
        const float h = skull_.height * grow;
        drawImage(renderer, skull_, width / 2.0f - w / 2.0f, kViewCentreY - h / 2.0f, w, h);
    }

    if (big_.ready() && small_.ready()) {
        messages_.draw(renderer, big_, small_, width);
    }

    // The health panel at the bottom right; the number blinks when health is low.
    const float panelX = width - (panel_.width + kPanelRight);
    const float panelY = kCanvasHeight - (panel_.height + kPanelBottom);
    drawImage(renderer, panel_, panelX, panelY, panel_.width, panel_.height);
    if (health > kLowHealth || blink_ < kBlinkOn) {
        drawNumber(renderer, health, panelX + (float)((int32_t)panel_.width >> 1),
                   panelY + (float)(((int32_t)panel_.height - (int32_t)digits_.height) >> 1));
    }

    // DrawInventory: every slot, its icon and its count (the super chain gun's time).
    for (int32_t i = 0; i < inventory.slotCount(); i++) {
        const Kurt::Slot& slot = inventory.slots()[i];
        const int32_t frame = (int32_t)slot.item - 1;
        if (frame < 0 || frame >= (int32_t)icons_.size()) {
            continue;
        }
        const Image& icon = icons_[(size_t)frame];
        const float anchorX = kIconAnchorX + (float)i * kSlotStep;
        const float anchorY = kIconAnchorY;
        drawImage(renderer, icon, anchorX - (float)icon.hotspotX, anchorY - (float)icon.hotspotY, icon.width,
                  icon.height);
        const int32_t shown = slot.item == Kurt::SuperChainGun ? inventory.superChainGun() : slot.count;
        if (shown > 1) {
            drawNumber(renderer, shown, anchorX, anchorY - kCountRise);
        }
    }
}

} // namespace fall
} // namespace mdk

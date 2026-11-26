// ============================================================================
// hud_view.cpp — the port of HudView.cs. See hud_view.h.
// ============================================================================

#include "game/hud_view.h"

#include <cstdio>
#include <cstring>

#include "formats/bni.h"
#include "formats/palette.h"
#include "formats/sprite_animation.h"
#include "formats/texture.h"
#include "game/font_view.h"
#include "game/mod_images.h"
#include "util/math_lut.h"

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
// The arena's colours in the palette (C# Arena.PaletteColors at 64).
const int32_t kArenaFirst = 64;
const int32_t kArenaLast = 175;

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

// ---- The mission timer's ring (upstream 235b1792; C# TimerRing, 0x420cb0, 0x420d88) ----
//
// The time gone, as an angle counter-clockwise from the right, stamps the empty
// panel's pixels (SC_BSTAT, its black ring) over the panel's from the centre
// outwards. Each half of the panel is a fan of rows from its centre row, the
// upper one for angles up to pi, the lower mirrored.
//
//        pi/2
//    +----+----+
//  pi+    o    +0   time gone 0 -> 2pi: empties anticlockwise from the right
//    +----+----+
//        3pi/2
//
// Cold: it runs once per degree of the town's time (a mission is minutes long).

const float kPi = 3.14159265f;
const float kTau = 6.28318531f;
// The split at pi (0x420d5a: 0x40490fb1, just below it, for the upper half).
const float kUpperEnd = 3.141582f;
const int32_t kFixed = 16;
// Slopes: near pi/2 a row goes straight up; near 0 or pi, a whole width a row.
const float kNearUpLow = 1.570482f;
const float kNearUpHigh = 1.571110f;
const float kNearZero = 3.14159e-5f;
const float kNearPi = 3.141561f;
// The ring is stamped again once the time gone has moved this far (a degree).
const float kTimerStep = kPi / 180.0f;

// x moved per row, 16.16 (truncated, like the original's FPU setting).
int32_t ringSlope(float angle, int32_t width) {
    if (angle >= kNearUpLow && angle <= kNearUpHigh) {
        return 0;
    }
    if (angle < kNearZero) {
        return width << kFixed;
    }
    if (angle > kNearPi) {
        return -(width << kFixed);
    }
    // The cotangent through the trig table (AGENTS.md R6: one path on both
    // targets; the C#'s MathF.Tan differs in the last bits, which the 16.16
    // truncation all but always hides).
    // A slope past a whole width a row ends the fan in one row either way, so it
    // is clamped there: the table's sine near 0 or pi cannot overflow the cast.
    const float sine = fastSin(angle);
    const float most = (float)(width << kFixed);
    if (sine == 0.0f) {
        return fastCos(angle) > 0.0f ? width << kFixed : -(width << kFixed);
    }
    const float slope = (float)(1 << kFixed) * fastCos(angle) / sine;
    return (int32_t)(slope > most ? most : (slope < -most ? -most : slope));
}

// The rows between angles `high` and `low` of one half (0x420d88): every other
// row from the centre, x moving by the cotangent per row.
void ringFan(uint8_t* panel, const uint8_t* empty, int32_t width, int32_t height, float high, float low) {
    int32_t row = -width;
    // The lower half: the angles mirrored, rows going down.
    if (high > kPi || low > kPi) {
        const float mirroredHigh = kTau - low;
        low = kTau - high;
        high = mirroredHigh;
        row = width;
    }
    const int32_t leftStep = ringSlope(high, width);
    const int32_t rightStep = ringSlope(low, width);
    const int32_t limit = width << kFixed;
    int32_t left = (width >> 1) << kFixed;
    int32_t right = left;
    int32_t at = (height >> 1) * width;
    for (int32_t y = 1; y < height; y += 2) {
        left = left + leftStep > 0 ? left + leftStep : 0;
        right += rightStep;
        if (left >= limit || right < 0) {
            return;
        }
        right = right < limit - 1 ? right : limit - 1;
        for (int32_t x = left >> kFixed; x <= right >> kFixed; x++) {
            const uint8_t pixel = empty[at + x];
            if (pixel != 0) {
                panel[at + x] = pixel;
            }
        }
        at += row;
    }
}

// Stamps the ring of `empty` into `panel` (indices of the same size) from angle
// `from` to `to` (0x420cb0). The C# wraps both into 0-2pi with a loop; here they
// are already in it (the time left is clamped), so they are clamped instead.
void ringWipe(uint8_t* panel, const uint8_t* empty, int32_t width, int32_t height, float from, float to) {
    from = clampf(from, 0.0f, kTau);
    to = clampf(to, 0.0f, kTau);
    if (from >= to) {
        return;
    }
    if (to <= kPi || from >= kPi) {
        ringFan(panel, empty, width, height, to, from);
        return;
    }
    ringFan(panel, empty, width, height, kUpperEnd, from);
    ringFan(panel, empty, width, height, to, kPi);
}

} // namespace

bool HudView::load(MdkRenderer renderer, const formats::Bni& sprites,
                   const formats::Palette& palette, ModImages* mods) {
    renderer_ = renderer;
    mods_ = mods;
    arenaColours_ = nullptr;
    arenaPalette_ = -1;
    palette_ = mdk_render_create_palette(renderer, palette.rgba());
    if (palette_ < 0) {
        return false;
    }

    modPanel_ = HdImage();
    loadImage(sprites, "SC_STAT", &panel_, palette, &modPanel_);
    modTimerPanel_ = modPanel_.rgba;
    levelRgba_.assign(palette.rgba(), palette.rgba() + 256 * 4);
    // The timer's two panels, full and empty (the same size: the ring is stamped
    // from one into the other); without SC_BSTAT the ring stays full.
    formats::Texture full;
    formats::Texture empty;
    if (sprites.image("SC_STAT", &full) && sprites.image("SC_BSTAT", &empty) &&
        full.width() == empty.width() && full.height() == empty.height()) {
        panelWidth_ = full.width();
        panelHeight_ = full.height();
        const size_t size = (size_t)panelWidth_ * (size_t)panelHeight_;
        fullPanel_.assign(full.indices(), full.indices() + size);
        emptyPanel_.assign(empty.indices(), empty.indices() + size);
        timerPanel_ = fullPanel_;
    }
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
            char name[32];
            std::snprintf(name, sizeof(name), "PICKUPS_%d", (int)i);
            // The 3DS HD pack's picture for this frame: with it, the HD file is the
            // picture and the indices are not expanded (render.h).
            std::string path;
            if (mods_ && mods_->compressed() && frame.image.frameCount() == 1 &&
                mods_->resolve(ModImages::Kind::Images, name, frame.image, palette, &path)) {
                icon.texture = mdk_render_create_hd_image(
                    renderer, frame.image.width(), frame.image.height(), frame.image.indices(),
                    palette.rgba(), path.c_str());
            } else {
                icon.texture = mdk_render_create_index_texture(
                    renderer, frame.image.width(), frame.image.height() * frame.image.frameCount(),
                    frame.image.indices(), palette.rgba());
                keepArenaIndices(&icon, frame.image.indices(), frame.image.width(),
                                 frame.image.height() * frame.image.frameCount());
            }
            // A mod's image for this pickup frame (CanvasImages.FrameName).
            if (mods_ && !mods_->compressed() && icon.texture >= 0 &&
                frame.image.frameCount() == 1) {
                HdImage image;
                if (mods_->image(name, frame.image, palette, &image)) {
                    mdk_render_replace_image(renderer, icon.texture, palette_, image.width,
                                             image.height, image.rgba.data());
                    icon.arenaIndices.clear();   // the mod's picture is drawn, not the indices
                }
            }
            icon.w = (float)frame.image.width();
            icon.h = (float)frame.image.height();
            icon.hotspotX = frame.hotspotX;
            icon.hotspotY = frame.hotspotY;
            icons_.push_back(icon);
        }
    }

    // C# HudView: `Sniper = new SniperOverlay(renderer, sprites, palette, _palette,
    // mods)`. A missing image leaves it unloaded (the C# throws): the HUD still
    // loads, and sniper mode keeps the HUD instead of its screen.
    sniper_.load(renderer, sprites, palette, palette_);

    // The bomber's sight (C# HudView's `new BomberOverlay(renderer, sprites, _palette,
    // big, palette, mods)`). It degrades: without its sprites there is no sight.
    std::string targetHd;
    std::string crossHd;
    BomberOverlay::hdPaths(sprites, palette, mods, &targetHd, &crossHd);
    if (bomber_.load(renderer, sprites, palette_, palette, targetHd.c_str(), crossHd.c_str()) &&
        mods != nullptr) {
        bomber_.replaceWithMods(renderer, sprites, palette, mods);
    }

    barFill_ = paletteColour(palette, kBarFill);
    barFrame_ = paletteColour(palette, kBarFrame);

    // The panel and the digits are required; the skull and the icons degrade.
    return panel_.texture >= 0 && digits_.texture >= 0;
}

void HudView::loadImage(const formats::Bni& sprites, const char* name, Image* out,
                        const formats::Palette& palette, HdImage* keep) {
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
    // The 3DS HD pack's picture: with it, the HD file is the picture and the
    // indices are not expanded (render.h, mdk_render_create_hd_image).
    std::string path;
    if (mods_ && mods_->compressed() && texture.frameCount() == 1 &&
        mods_->resolve(ModImages::Kind::Images, name, texture, palette, &path)) {
        out->texture = mdk_render_create_hd_image(renderer_, texture.width(), texture.height(),
                                                  texture.indices(), palette.rgba(),
                                                  path.c_str());
    } else {
        out->texture = mdk_render_create_index_texture(renderer_, texture.width(),
                                                       texture.height() * texture.frameCount(),
                                                       texture.indices(), palette.rgba());
        keepArenaIndices(out, texture.indices(), texture.width(), texture.height() * texture.frameCount());
    }
    // A mod's image for this picture (CanvasImages.Create): the enhanced
    // canvas draws it instead of the filtered index texture.
    if (mods_ && !mods_->compressed() && out->texture >= 0 && texture.frameCount() == 1) {
        HdImage image;
        if (mods_->image(name, texture, palette, &image)) {
            mdk_render_replace_image(renderer_, out->texture, palette_, image.width, image.height,
                                     image.rgba.data());
            out->arenaIndices.clear();   // the mod's picture is drawn, not the indices
            if (keep != nullptr && image.frames == 1) {
                *keep = image;
            }
        }
    }
    out->w = (float)texture.width();
    out->h = (float)texture.height();
}

void HudView::keepArenaIndices(Image* image, const uint8_t* indices, int32_t width, int32_t height) {
    const size_t size = (size_t)width * (size_t)height;
    for (size_t i = 0; i < size; i++) {
        if (indices[i] >= kArenaFirst && indices[i] <= kArenaLast) {
            image->arenaIndices.assign(indices, indices + size);
            image->indexWidth = width;
            image->indexHeight = height;
            return;
        }
    }
}

void HudView::remake(Image* image, const formats::Palette& palette, int32_t paletteId) {
    if (image->arenaIndices.empty() || image->texture < 0) {
        return;
    }
    // The panel is made from its stamped copy, so the timer's ring stays.
    const uint8_t* indices = image == &panel_ && !timerPanel_.empty() ? timerPanel_.data()
                                                                       : image->arenaIndices.data();
    const int32_t texture = mdk_render_create_index_texture(renderer_, image->indexWidth, image->indexHeight,
                                                            indices, palette.rgba());
    if (texture < 0) {
        return;   // the old picture stays, in the level's colours
    }
    mdk_render_release_texture(renderer_, image->texture);
    image->texture = texture;
    image->palette = paletteId;
}

void HudView::enter(const formats::Palette& arena) {
    if (&arena == arenaColours_ || renderer_ == nullptr) {
        return;
    }
    arenaColours_ = &arena;
    bool any = !panel_.arenaIndices.empty() || !skull_.arenaIndices.empty() || !digits_.arenaIndices.empty();
    for (size_t i = 0; i < icons_.size() && !any; i++) {
        any = !icons_[i].arenaIndices.empty();
    }
    if (!any) {
        return;
    }
    const int32_t paletteId = mdk_render_create_palette(renderer_, arena.rgba());
    if (paletteId < 0) {
        return;
    }
    remake(&panel_, arena, paletteId);
    remake(&skull_, arena, paletteId);
    remake(&digits_, arena, paletteId);
    for (size_t i = 0; i < icons_.size(); i++) {
        remake(&icons_[i], arena, paletteId);
    }
    if (arenaPalette_ >= 0) {
        mdk_render_release_palette(renderer_, arenaPalette_);
    }
    arenaPalette_ = paletteId;
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

    // Sniper mode's screen instead of the HUD (C# HudView.Draw): the health in
    // the frame's panel, where the normal view puts SC_STAT, then the messages.
    if (sniper_.visible()) {
        const Blink blink = state.health > kLowHealth || blink_ < kBlinkOn ? Blink::Shown
                                                                           : Blink::Hidden;
        const float panelAtX = SniperOverlay::kViewWidth - (panel_.w + kPanelRight);
        const float panelAtY = SniperOverlay::kViewHeight - (panel_.h + kPanelBottom);
        sniper_.draw(renderer_, canvasWidth, state.health, blink, panelAtX, panelAtY, panel_.w,
                     panel_.h);
        if (big_ != nullptr && small_ != nullptr && big_->ready() && small_->ready()) {
            messages_.draw(renderer_, *big_, *small_, canvasWidth);
        }
        return;
    }

    // The bomber's sight first (C# HudView.cs:164-168, 0x41e3c8), its count in the big
    // font once the fonts are ready.
    if (state.bomber && bomber_.ready()) {
        const FontView* big = big_ != nullptr && big_->ready() ? big_ : nullptr;
        bomber_.draw(renderer_, big, canvasWidth, state.bomberCursorX, state.bomberCursorY,
                     state.bomberBombs);
    }

    // The health panel at the bottom right; the number blinks when health is low.
    const float panelX = canvasWidth - (panel_.w + kPanelRight);
    const float panelY = kCanvasHeight - (panel_.h + kPanelBottom);
    updateTimer(state.timeLeft);
    drawImage(panel_, panelX, panelY, Colour{1.0f, 1.0f, 1.0f, 1.0f});
    if (state.health > kLowHealth || blink_ < kBlinkOn) {
        const float centreX = panelX + (float)((int32_t)panel_.w >> 1);
        const float numberY = panelY + (float)(((int32_t)panel_.h - (int32_t)digits_.h) >> 1);
        drawNumber(state.health, centreX, numberY);
    }

    drawInventory(state);
    drawBar(state.barHealth, state.barMax);

    // The messages, over the rest (the C# Draw's last call).
    if (big_ != nullptr && small_ != nullptr && big_->ready() && small_->ready()) {
        messages_.draw(renderer_, *big_, *small_, canvasWidth);
    }
}

void HudView::updateTimer(float timeLeft) {
    // C# UpdateTimer: the time gone stamped in once it moved a degree. Less gone
    // than stamped (a game loaded): the full ring again.
    if (timerPanel_.empty() || panel_.texture < 0) {
        return;
    }
    const float angle = (1.0f - timeLeft) * kTau;
    if (angle < timerAngle_) {
        timerPanel_ = fullPanel_;
        timerAngle_ = 0.0f;
        uploadTimer();
    }
    if (angle <= timerAngle_ + kTimerStep) {
        return;
    }
    ringWipe(timerPanel_.data(), emptyPanel_.data(), panelWidth_, panelHeight_, timerAngle_, angle);
    timerAngle_ = angle;
    uploadTimer();
}

void HudView::uploadTimer() {
    mdk_render_update_texture(renderer_, panel_.texture, panelWidth_, panelHeight_, timerPanel_.data());
    // A mod's panel (C# UploadTimer): each of its pixels shows the panel pixel
    // it covers, SC_BSTAT's colour where the ring was stamped.
    const int32_t modWidth = modPanel_.width;
    const int32_t modHeight = modPanel_.height;
    if (modWidth <= 0 || modHeight <= 0 || modPanel_.rgba.size() < (size_t)modWidth * (size_t)modHeight * 4) {
        return;
    }
    // The arena's colours once entered (upstream 6eb0963b), the level's before.
    const uint8_t* colours = arenaColours_ != nullptr ? arenaColours_->rgba() : levelRgba_.data();
    // The panel pixel under mod pixel (x, y) is (x * panelWidth_ / modWidth,
    // y * panelHeight_ / modHeight), stepped here without a divide per pixel
    // (AGENTS.md R11: the ARM11 has no integer divide): each coordinate keeps
    // its floor and remainder as x or y moves on by one.
    int32_t panelY = 0;
    int32_t yRemainder = 0;
    for (int32_t y = 0; y < modHeight; y++) {
        const int32_t row = panelY * panelWidth_;
        int32_t panelX = 0;
        int32_t xRemainder = 0;
        for (int32_t x = 0; x < modWidth; x++) {
            const int32_t at = row + panelX;
            const size_t pixel = ((size_t)y * (size_t)modWidth + (size_t)x) * 4;
            const uint8_t* from = timerPanel_[(size_t)at] == fullPanel_[(size_t)at]
                                      ? modPanel_.rgba.data() + pixel
                                      : colours + (size_t)timerPanel_[(size_t)at] * 4;
            memcpy(modTimerPanel_.data() + pixel, from, 4);
            xRemainder += panelWidth_;
            while (xRemainder >= modWidth) {
                xRemainder -= modWidth;
                panelX++;
            }
        }
        yRemainder += panelHeight_;
        while (yRemainder >= modHeight) {
            yRemainder -= modHeight;
            panelY++;
        }
    }
    mdk_render_replace_image(renderer_, panel_.texture, panel_.palette >= 0 ? panel_.palette : palette_,
                             modWidth, modHeight, modTimerPanel_.data());
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
    q.palette = image.palette >= 0 ? image.palette : palette_;
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
        q.palette = digits_.palette >= 0 ? digits_.palette : palette_;
        mdk_render_canvas_quad(renderer_, &q);
        x += kDigitWidth;
    }
}

void HudView::drawInventory(const HudState& state) {
    if (state.selected < state.slotCount &&
        state.slots[state.selected].first != kSuperChainGun) {
        const float bx = kSelectionX + (float)state.selected * kSlotStep;
        fillRect(bx + 1.0f, kSelectionY + 1.0f, kSelectionW - 2.0f, kSelectionH - 2.0f,
                 Colour{0.0f, 0.0f, 0.0f, 0.5f});
        frameRect(bx, kSelectionY, kSelectionW, kSelectionH,
                  Colour{0.75f, 0.75f, 0.75f, 1.0f});
    }

    for (int32_t i = 0; i < state.slotCount; i++) {
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

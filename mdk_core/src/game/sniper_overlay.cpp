// ============================================================================
// sniper_overlay.cpp — the port of src/Mdk.Game/Hud/SniperOverlay.cs. See
// sniper_overlay.h.
// ============================================================================

#include "game/sniper_overlay.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "formats/bni.h"
#include "formats/palette.h"
#include "formats/sniper_screen.h"
#include "formats/sprite_animation.h"
#include "formats/texture.h"

namespace mdk {

namespace {

// The C#'s constants (SniperOverlay.cs:45-90), in its order.
const float kCrosshairX = 299.0f;
const float kCrosshairY = 219.0f;
const float kZoomDigitsX = 564.0f;
const float kZoomDigitsY = 155.0f;
const float kGaugeX = 552.0f;
const float kGaugeY = 176.0f;
const int32_t kGaugeHeight = 88;
// The gauge's shown height follows the zoom by 3 pixels per frame.
const int32_t kGaugeSpeed = 3;
// The zoom in percent: (1 - zoom)^2 x 1.05194 (59% at 4x).
const float kZoomPercentFactor = 1.05194f;
const float kPercent = 100.0f;
const float kWeaponX = 112.0f;
const float kWeaponY = 304.0f;
const float kTypeIcons[6][2] = {{0, 256}, {0, 280}, {0, 300}, {4, 320}, {16, 336}, {32, 344}};
const float kTypeLabels[6][2] = {{12, 268}, {12, 288}, {12, 308}, {20, 320}, {24, 328}, {36, 336}};
const float kCountX = 64.0f;
const float kCountY = 315.0f;
const int32_t kTypes = 6;

// Round camera fills: after a hit, after a kill or an explosion; a miss shows
// SNIPERGA, a frame every 2 ticks.
const int32_t kHitFill = 0x3c;
const int32_t kKillFill = 0xf4;
const int32_t kEmptyFill = 0;
const float kMissFrameTicks = 2.0f;

// The air strike's iris (0x41ef90): with a target the scope reddens from its
// edge in 1 s, leaving a hole of radius 384 x c^2 (c from 1 to 0); closed, a
// darker ring contracts every second. View rows 80-359, x 108-492, centred on
// (300, 220).
const int32_t kStrikeType = 5;
const float kIrisCentreX = 300.0f;
const float kIrisCentreY = 220.0f;
const int32_t kIrisTop = 80;
const int32_t kIrisBottom = 360;
const float kIrisHalfWidth = 192.0f;
const float kIrisRadius = 384.0f;
const float kRingOuter = 392.0f;
const float kRingInner = 376.0f;
const Colour kIrisColour = {200.0f / 255.0f, 0.0f, 0.0f, 0x60 / 255.0f};
const Colour kRingColour = {200.0f / 255.0f, 0.0f, 0.0f, 0xC4 / 255.0f};
const Colour kClear = {0.0f, 0.0f, 0.0f, 0.0f};

const int32_t kDigitWidth = 8;
const int32_t kMaxNumber = 999;
const Colour kBlack = {0.0f, 0.0f, 0.0f, 1.0f};
const Colour kWhite = {1.0f, 1.0f, 1.0f, 1.0f};

// The images' names in TRAVSPRT.BNI (the frame and mask: SniperScreen's).
const char kCross[] = "CROSS";
const char kGaugeImage[] = "SNIP_RNG";
const char kWeaponImage[] = "SNIP_WEP";
const char kDigits[] = "SNIP_TXT";
const char kIcon[] = "SNIP_W";
const char kLabel[] = "SNIP_L";
const char kMiss[] = "SNIPERGA";

// The budget's tile: 63 texels in a 64-texel texture (sniper_overlay.h).
const int32_t kTile = 63;

int32_t clampInt(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }
float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// MathF.Round: to the nearest, halves to even (the default rounding mode).
int32_t roundEven(float v) { return (int32_t)std::nearbyint(v); }

} // namespace

const SniperOverlay::Rect SniperOverlay::kRoundViews[3] = {
    {72.0f, 10.0f, 140.0f, 70.0f}, {228.0f, 0.0f, 140.0f, 70.0f}, {384.0f, 10.0f, 140.0f, 70.0f}};

bool SniperOverlay::load(MdkRenderer renderer, const formats::Bni& sprites,
                         const formats::Palette& palette, int32_t paletteTexture) {
    // C# SniperOverlay(renderer, sprites, palette, paletteTexture, mods). The
    // mods' replacement images are not ported here (docs/OPEN_ITEMS.md).
    renderer_ = renderer;
    palette_ = paletteTexture;
    std::memcpy(colours_, palette.rgba(), sizeof(colours_));
    loaded_ = false;

    formats::Texture frame;
    formats::Texture mask;
    if (!formats::SniperScreen::frame(sprites, &frame) ||
        !formats::SniperScreen::mask(sprites, &mask)) {
        return false;
    }

    // C# CutView: the frame without the view's rectangle, but for its edge under
    // the mask -- filtered (the enhanced look), the frame's and the mask's edges
    // both fade there and would let the scene through as a light line.
    std::vector<uint8_t> cut(frame.indices(),
                             frame.indices() + (size_t)frame.width() * (size_t)frame.height());
    const int32_t left = (int32_t)kViewX;
    const int32_t top = (int32_t)kViewY;
    const int32_t viewW = formats::SniperScreen::kViewWidth;
    const int32_t viewH = formats::SniperScreen::kViewHeight;
    const uint8_t* maskIndices = mask.indices();
    for (int32_t y = 0; y < viewH; y++) {
        for (int32_t x = 0; x < viewW; x++) {
            const bool edge = x == 0 || y == 0 || x == viewW - 1 || y == viewH - 1;
            if (edge && maskIndices[y * mask.width() + x] != 0) {
                continue;
            }
            cut[(size_t)((top + y) * frame.width() + left + x)] = 0;
        }
    }
    if (!loadIndices(cut.data(), frame.width(), frame.height(), &frame_, palette, true) ||
        !loadIndices(maskIndices, mask.width(), mask.height(), &mask_, palette, true)) {
        return false;
    }

    formats::SpriteAnimation cross;
    formats::SpriteAnimation::Frame crossFrame;
    if (!sprites.animation(kCross, &cross) || !cross.frame(0, &crossFrame) ||
        !loadIndices(crossFrame.image.indices(), crossFrame.image.width(),
                     crossFrame.image.height(), &cross_, palette, false)) {
        return false;
    }
    cross_.hotspotX = (float)crossFrame.hotspotX;
    cross_.hotspotY = (float)crossFrame.hotspotY;

    if (!loadImage(sprites, kGaugeImage, &gauge_, palette) ||
        !loadImage(sprites, kWeaponImage, &weapon_, palette) ||
        !loadImage(sprites, kDigits, &digits_, palette)) {
        return false;
    }
    for (int32_t i = 1; i <= kTypes; i++) {
        char name[16];
        std::snprintf(name, sizeof(name), "%s%d", kIcon, (int)i);
        if (!loadImage(sprites, name, &icons_[i - 1], palette)) {
            return false;
        }
        std::snprintf(name, sizeof(name), "%s%d", kLabel, (int)i);
        if (!loadImage(sprites, name, &labels_[i - 1], palette)) {
            return false;
        }
    }

    formats::SpriteAnimation miss;
    if (!sprites.animation(kMiss, &miss) || miss.frameCount() <= 0) {
        return false;
    }
    miss_.clear();
    for (int32_t i = 0; i < miss.frameCount(); i++) {
        formats::SpriteAnimation::Frame f;
        Image image;
        if (!miss.frame(i, &f) || !loadIndices(f.image.indices(), f.image.width(),
                                               f.image.height(), &image, palette, false)) {
            return false;
        }
        miss_.push_back(image);
    }

    loaded_ = true;
    return true;
}

bool SniperOverlay::loadImage(const formats::Bni& sprites, const char* name, Image* out,
                              const formats::Palette& palette) {
    formats::Texture texture;
    if (!sprites.image(name, &texture)) {
        return false;
    }
    return loadIndices(texture.indices(), texture.width(), texture.height(), out, palette, false);
}

bool SniperOverlay::loadIndices(const uint8_t* indices, int32_t width, int32_t height, Image* out,
                                const formats::Palette& palette, bool fit) {
    // C# Load / CanvasImages.Create: an index texture through the HUD's palette.
    // The palette's bytes go in with the indices for the 3DS (hud_view.h says why).
    out->w = (float)width;
    out->h = (float)height;
    out->pieces.clear();
    out->texture = -1;
    if (indices == nullptr || width <= 0 || height <= 0) {
        return false;
    }
    if (!fit || (!budget.tiles && budget.shrink <= 1)) {
        out->texture =
            mdk_render_create_index_texture(renderer_, width, height, indices, palette.rgba());
        return out->texture >= 0;
    }

    // THE BUDGET (sniper_overlay.h): pieces of kTile x kTile kept texels, each
    // covering kTile * shrink screen pixels, the transparent ones not made.
    // The shrink is 1 or 2 (sniper_overlay.h), so its divisions are a shift.
    const int32_t shift = budget.shrink > 1 ? 1 : 0;
    const int32_t shrink = 1 << shift;
    const int32_t span = budget.tiles ? kTile * shrink : (width > height ? width : height);
    std::vector<uint8_t> texels;
    for (int32_t y0 = 0; y0 < height; y0 += span) {
        for (int32_t x0 = 0; x0 < width; x0 += span) {
            const int32_t w = width - x0 < span ? width - x0 : span;
            const int32_t h = height - y0 < span ? height - y0 : span;
            const int32_t tw = (w + shrink - 1) >> shift;
            const int32_t th = (h + shrink - 1) >> shift;
            texels.assign((size_t)tw * (size_t)th, 0);
            bool any = false;
            for (int32_t ty = 0; ty < th; ty++) {
                const uint8_t* row = indices + (size_t)(y0 + ty * shrink) * (size_t)width;
                for (int32_t tx = 0; tx < tw; tx++) {
                    const uint8_t index = row[x0 + tx * shrink];
                    texels[(size_t)ty * (size_t)tw + (size_t)tx] = index;
                    any = any || index != 0;
                }
            }
            if (!any) {
                continue;
            }
            Piece piece;
            piece.texture =
                mdk_render_create_index_texture(renderer_, tw, th, texels.data(), palette.rgba());
            if (piece.texture < 0) {
                return false;
            }
            piece.x = (float)x0;
            piece.y = (float)y0;
            piece.w = (float)(tw * shrink);
            piece.h = (float)(th * shrink);
            out->pieces.push_back(piece);
        }
    }
    return !out->pieces.empty();
}

void SniperOverlay::update(float delta, const SniperHud* hud) {
    // C# Update (SniperOverlay.cs:213-228).
    shown_ = hud != nullptr;
    if (hud == nullptr) {
        iris_ = 1.0f;
        pulse_ = 1.0f;
        return;
    }
    hud_ = *hud;

    updateIris(delta, *hud);
    const int32_t goal = roundEven((float)kGaugeHeight * zoomFraction(hud->zoom));
    gaugeShown_ = clampInt(goal, gaugeShown_ - kGaugeSpeed, gaugeShown_ + kGaugeSpeed);
}

float SniperOverlay::zoomFraction(float zoom) {
    // C# ZoomFraction: Math.Clamp((1 - zoom) * (1 - zoom) * 1.05194, 0, 1).
    return clampf((1.0f - zoom) * (1.0f - zoom) * kZoomPercentFactor, 0.0f, 1.0f);
}

int32_t SniperOverlay::zoomPercent(float zoom) {
    // C# Draw: (int)MathF.Round(Percent * fraction).
    return roundEven(kPercent * zoomFraction(zoom));
}

void SniperOverlay::updateIris(float delta, const SniperHud& hud) {
    // C# UpdateIris (SniperOverlay.cs:234-264): the iris closes over 1 s while the
    // strike is selected and has a target, and opens again otherwise; closed, it
    // pulses once a second.
    const bool strike = hud.selectedAmmo == kStrikeType;
    if (strike && hud.aim != StrikeAim::NotChecked) {
        target_ = hud.aim == StrikeAim::Target;
        if (target_) {
            iris_ = std::fmax(iris_ - delta, 0.0f);
        }
    }

    if (!strike || !target_) {
        iris_ += delta;
        if (iris_ >= 1.0f) {
            iris_ = 1.0f;
            pulse_ = 1.0f;
            return;
        }
    }

    if (iris_ == 0.0f || pulse_ != 1.0f) {
        pulse_ -= delta;
        if (pulse_ < 0.0f) {
            pulse_ = 1.0f;
        }
    }
}

void SniperOverlay::placement(float canvasWidth, float* scale, float* left) {
    // C# Placement: the canvas height over the screen's, centred.
    const float s = kCanvasHeight / kScreenHeight;
    *scale = s;
    *left = (canvasWidth - kScreenWidth * s) / 2.0f;
}

void SniperOverlay::draw(MdkRenderer renderer, float canvasWidth, int32_t health, Blink blink,
                         float panelX, float panelY, float panelW, float panelH) {
    // C# Draw (SniperOverlay.cs:276-313).
    if (!visible()) {
        return;
    }
    renderer_ = renderer;

    float scale = 0.0f;
    float left = 0.0f;
    placement(canvasWidth, &scale, &left);
    if (left > 0.0f) {
        quad(renderer, 0.0f, 0.0f, std::ceil(left), kCanvasHeight, 0.0f, 0.0f, 0.0f, 0.0f, -1, -1,
             kBlack);
        quad(renderer, std::floor(canvasWidth - left), 0.0f, std::ceil(left), kCanvasHeight, 0.0f,
             0.0f, 0.0f, 0.0f, -1, -1, kBlack);
    }

    const Screen screen = {renderer, palette_, scale, left};
    drawFrame(screen);
    drawCameras(screen, hud_);
    drawIris(screen);
    image(screen, mask_, kViewX, kViewY);
    image(screen, cross_, kViewX + kCrosshairX - cross_.hotspotX,
          kViewY + kCrosshairY - cross_.hotspotY);

    drawNumber(screen, zoomPercent(hud_.zoom), kViewX + kZoomDigitsX, kViewY + kZoomDigitsY);
    if (gaugeShown_ > 0) {
        const float gaugeTop = (float)(kGaugeHeight - gaugeShown_);
        part(screen, gauge_, 0.0f, gaugeTop, gauge_.w, (float)gaugeShown_, kViewX + kGaugeX,
             kViewY + kGaugeY + gaugeTop);
    }

    drawAmmo(screen, hud_);

    // Health in the frame's panel, where the normal view draws SC_STAT.
    if (blink == Blink::Shown) {
        drawNumber(screen, health, kViewX + panelX + (float)((int32_t)panelW >> 1),
                   kViewY + panelY + (float)(((int32_t)panelH - (int32_t)digits_.h) >> 1));
    }
}

void SniperOverlay::drawFrame(const Screen& screen) {
    // C# DrawFrame: black behind the frame (index 0 is transparent), then the frame.
    const float viewBottom = kViewY + kViewHeight;
    fill(screen, 0.0f, 0.0f, kScreenWidth, kViewY, kBlack);
    fill(screen, 0.0f, viewBottom, kScreenWidth, kScreenHeight - viewBottom, kBlack);
    fill(screen, 0.0f, kViewY, kViewX, kViewHeight, kBlack);
    fill(screen, kViewX + kViewWidth, kViewY, kScreenWidth - kViewX - kViewWidth, kViewHeight,
         kBlack);
    image(screen, frame_, 0.0f, 0.0f);
}

void SniperOverlay::drawCameras(const Screen& screen, const SniperHud& hud) {
    // C# DrawCameras: round cameras that don't watch a round: a colour, or the
    // miss animation.
    for (int32_t i = 0; i < 3 && i < hud.cameraCount; i++) {
        const scripts::SniperRounds::RoundCamera& camera = hud.cameras[i];
        const float x = kRoundViews[i].x + kViewX;
        const float y = kRoundViews[i].y + kViewY;
        const float w = kRoundViews[i].w;
        const float h = kRoundViews[i].h;
        int32_t fillIndex = kEmptyFill;
        switch (camera.shot) {
            case scripts::SniperRounds::ShotWatching:
                if (roundViewsDrawn_) {
                    continue;
                }
                // The 3DS draws no round-camera inset: the window is an empty one.
                break;
            case scripts::SniperRounds::ShotMiss: {
                fill(screen, x, y, w, h, kBlack);
                // (int)(Time / 2) % frames. The divisor is the animation's frame
                // count, so the remainder is the subtraction it is bounded by.
                int32_t index = (int32_t)(camera.time / kMissFrameTicks);
                const int32_t frames = (int32_t)miss_.size();
                index = index < 0 ? 0 : index;
                while (index >= frames) {
                    index -= frames;
                }
                const Image& frame = miss_[(size_t)index];
                image(screen, frame, x + w / 2.0f - frame.w / 2.0f, y + h / 2.0f - frame.h / 2.0f);
                continue;
            }
            case scripts::SniperRounds::ShotHit:
                fillIndex = kHitFill;
                break;
            case scripts::SniperRounds::ShotKill:
                fillIndex = kKillFill;
                break;
            default:
                fillIndex = kEmptyFill;
                break;
        }
        fill(screen, x, y, w, h, colour(fillIndex));
    }
}

Colour SniperOverlay::colour(int32_t index) const {
    // C# Colour: the palette's RGB over 255, opaque.
    Colour c;
    c.r = (float)colours_[index * 4] / 255.0f;
    c.g = (float)colours_[index * 4 + 1] / 255.0f;
    c.b = (float)colours_[index * 4 + 2] / 255.0f;
    c.a = 1.0f;
    return c;
}

void SniperOverlay::drawIris(const Screen& screen) {
    // C# DrawIris: the iris, row by row from the scope's edges inwards: red, the
    // darker pulse ring, red again, then the clear hole.
    if (iris_ >= 1.0f) {
        return;
    }

    struct Circle {
        float radius;
        Colour colour;
    };
    Circle circles[3];
    int32_t count = 0;
    const float hole = kIrisRadius * iris_ * iris_;
    const float outer = kRingOuter * pulse_ * pulse_;
    const float inner = kRingInner * pulse_ * pulse_;
    if (outer > hole && pulse_ != 1.0f) {
        circles[count++] = Circle{outer, kRingColour};
        if (inner > hole) {
            circles[count++] = Circle{inner, kIrisColour};
        }
    }
    circles[count++] = Circle{hole, kClear};

    for (int32_t y = kIrisTop; y < kIrisBottom; y++) {
        const float dy = (float)y - kIrisCentreY;
        Colour current = kIrisColour;
        float outside = kIrisHalfWidth;
        for (int32_t c = 0; c < count; c++) {
            const float r = circles[c].radius;
            const float edge =
                std::fmin(std::fabs(dy) < r ? std::sqrt(r * r - dy * dy) : 0.0f, outside);
            drawSpan(screen, y, edge, outside, current);
            outside = edge;
            current = circles[c].colour;
        }
        drawSpan(screen, y, 0.0f, outside, current);
    }
}

void SniperOverlay::drawSpan(const Screen& screen, int32_t y, float nearX, float farX,
                             const Colour& colour) {
    // C# DrawSpan: a row of the iris between two distances from its centre, on
    // both sides.
    if (colour.a == 0.0f || farX <= nearX) {
        return;
    }
    const float width = farX - nearX;
    fill(screen, kViewX + kIrisCentreX - farX, kViewY + (float)y, width, 1.0f, colour);
    fill(screen, kViewX + kIrisCentreX + nearX, kViewY + (float)y, width, 1.0f, colour);
}

void SniperOverlay::drawAmmo(const Screen& screen, const SniperHud& hud) {
    // C# DrawAmmo: SNIP_WEP, the icons of the types with rounds, the selected
    // type's label and count.
    image(screen, weapon_, kViewX + kWeaponX, kViewY + kWeaponY);
    for (int32_t i = 0; i < kTypes; i++) {
        if (i == 0 || hud.ammo[i - 1] > 0) {
            image(screen, icons_[i], kViewX + kTypeIcons[i][0], kViewY + kTypeIcons[i][1]);
        }
    }

    const int32_t selected = clampInt(hud.selectedAmmo, 0, kTypes - 1);
    image(screen, labels_[selected], kViewX + kTypeLabels[selected][0],
          kViewY + kTypeLabels[selected][1]);
    if (selected > 0) {
        drawNumber(screen, hud.ammo[selected - 1], kViewX + kCountX, kViewY + kCountY);
    }
}

void SniperOverlay::drawNumber(const Screen& screen, int32_t value, float atX, float atY) {
    // C# DrawNumber: a number (at most 999) centred on atX, 8-pixel digits.
    char text[8];
    std::snprintf(text, sizeof(text), "%d", (int)clampInt(value, 0, kMaxNumber));
    const int32_t length = (int32_t)std::strlen(text);
    float x = atX - (float)kDigitWidth / 2.0f * (float)length;
    for (int32_t i = 0; i < length; i++) {
        const float sx = (float)((text[i] - '0') * kDigitWidth);
        part(screen, digits_, sx, 0.0f, (float)kDigitWidth, digits_.h, x, atY);
        x += (float)kDigitWidth;
    }
}

void SniperOverlay::fill(const Screen& screen, float x, float y, float w, float h,
                         const Colour& c) {
    // Screen.Fill: FillRect on the canvas (a flat quad: the C#'s UVs are zero).
    quad(screen.renderer, screen.left + x * screen.scale, y * screen.scale, w * screen.scale,
         h * screen.scale, 0.0f, 0.0f, 0.0f, 0.0f, -1, -1, c);
}

void SniperOverlay::image(const Screen& screen, const Image& image, float atX, float atY) {
    // Screen.Image: the whole image at `at`. A tiled one is its pieces.
    if (image.pieces.empty()) {
        part(screen, image, 0.0f, 0.0f, image.w, image.h, atX, atY);
        return;
    }
    for (size_t i = 0; i < image.pieces.size(); i++) {
        const Piece& p = image.pieces[i];
        quad(screen.renderer, screen.left + (atX + p.x) * screen.scale, (atY + p.y) * screen.scale,
             p.w * screen.scale, p.h * screen.scale, 0.0f, 0.0f, 1.0f, 1.0f, p.texture,
             screen.palette, kWhite);
    }
}

void SniperOverlay::part(const Screen& screen, const Image& image, float sx, float sy, float sw,
                         float sh, float atX, float atY) {
    // Screen.Part: DrawImage of `source` at `at`, untinted.
    if (image.texture < 0 || image.w <= 0.0f || image.h <= 0.0f) {
        return;
    }
    quad(screen.renderer, screen.left + atX * screen.scale, atY * screen.scale, sw * screen.scale,
         sh * screen.scale, sx / image.w, sy / image.h, (sx + sw) / image.w, (sy + sh) / image.h,
         image.texture, screen.palette, kWhite);
}

void SniperOverlay::quad(MdkRenderer renderer, float x, float y, float w, float h, float u0,
                         float v0, float u1, float v1, int32_t texture, int32_t palette,
                         const Colour& colour) {
    CanvasQuad q;
    q.x0 = x;
    q.y0 = y;
    q.x1 = x + w;
    q.y1 = y;
    q.x2 = x + w;
    q.y2 = y + h;
    q.x3 = x;
    q.y3 = y + h;
    q.u0 = u0;
    q.v0 = v0;
    q.u1 = u1;
    q.v1 = v0;
    q.u2 = u1;
    q.v2 = v1;
    q.u3 = u0;
    q.v3 = v1;
    q.colour = colour;
    q.texture = texture;
    q.palette = texture >= 0 ? palette : -1;
    mdk_render_canvas_quad(renderer, &q);
}

} // namespace mdk

// ============================================================================
// bomber_overlay.cpp — the port of Hud/BomberOverlay.cs. See bomber_overlay.h.
// ============================================================================

#include "game/bomber_overlay.h"

#include <math.h>
#include <stdio.h>

#include "formats/bni.h"
#include "formats/palette.h"
#include "formats/sprite_animation.h"
#include "formats/texture.h"
#include "game/font_view.h"

namespace mdk {

namespace {

// C# BomberOverlay.cs:20-25 (the two sprites' names are the class's).
const float kViewCentreX = 300.0f;
const float kViewCentreY = 180.0f;
const float kCountRight = 472.0f;
const float kCountBaseline = 56.0f;

} // namespace

const char* const BomberOverlay::kTarget = "BOMBTARG";
const char* const BomberOverlay::kCross = "CROSS";

bool BomberOverlay::load(MdkRenderer renderer, const formats::Bni& sprites, int32_t palette,
                         const formats::Palette& colours) {
    palette_ = palette;
    const bool target = loadImage(renderer, sprites, kTarget, colours, &target_);
    const bool cross = loadImage(renderer, sprites, kCross, colours, &cross_);
    return target && cross;
}

bool BomberOverlay::loadImage(MdkRenderer renderer, const formats::Bni& sprites,
                              const char* animation, const formats::Palette& colours,
                              Image* out) {
    // C# BomberOverlay.Load (BomberOverlay.cs:47-56): the animation's frame 0, its size
    // and hotspot. The palette's bytes go in with the indices, as HudView's images do
    // (the 3DS backend expands them on the CPU; see hud_view.h).
    formats::SpriteAnimation frames;
    formats::SpriteAnimation::Frame frame;
    if (!sprites.animation(animation, &frames) || !frames.frame(0, &frame)) {
        return false;
    }
    out->texture = mdk_render_create_index_texture(
        renderer, frame.image.width(), frame.image.height() * frame.image.frameCount(),
        frame.image.indices(), colours.rgba());
    if (out->texture < 0) {
        return false;
    }
    out->w = (float)frame.image.width();
    out->h = (float)frame.image.height();
    out->hotspotX = (float)frame.hotspotX;
    out->hotspotY = (float)frame.hotspotY;
    return true;
}

void BomberOverlay::draw(MdkRenderer renderer, const FontView* font, float canvasWidth,
                         float cursorX, float cursorY, int32_t bombs) const {
    // C# BomberOverlay.Draw (BomberOverlay.cs:59-66): the 600×360 view centred on the
    // canvas; the target at its centre, the cross at the cursor rounded to the pixel
    // (MathF.Round: to even, rintf's default mode), the count right-aligned.
    const float originX = canvasWidth / 2.0f - kViewCentreX;
    const float originY = 0.0f;
    drawImage(renderer, target_, originX + kViewCentreX, originY + kViewCentreY);
    drawImage(renderer, cross_, originX + rintf(cursorX), originY + rintf(cursorY));
    if (font == nullptr) {
        return;
    }
    char text[16];
    const int length = snprintf(text, sizeof(text), "%d", (int)bombs);
    const uint8_t* bytes = (const uint8_t*)text;
    font->draw(renderer, bytes, length,
               originX + kCountRight - (float)font->width(bytes, length), originY + kCountBaseline,
               1.0f, Colour{1.0f, 1.0f, 1.0f, 1.0f});
}

void BomberOverlay::drawImage(MdkRenderer renderer, const Image& image, float atX,
                              float atY) const {
    // C# BomberOverlay.DrawImage (BomberOverlay.cs:69-74): an image with its hotspot at
    // `at`, the whole texture, untinted.
    if (image.texture < 0) {
        return;
    }
    const float x = atX - image.hotspotX;
    const float y = atY - image.hotspotY;
    CanvasQuad q;
    q.x0 = x; q.y0 = y;
    q.x1 = x + image.w; q.y1 = y;
    q.x2 = x + image.w; q.y2 = y + image.h;
    q.x3 = x; q.y3 = y + image.h;
    q.u0 = 0.0f; q.v0 = 0.0f;
    q.u1 = 1.0f; q.v1 = 0.0f;
    q.u2 = 1.0f; q.v2 = 1.0f;
    q.u3 = 0.0f; q.v3 = 1.0f;
    q.colour = Colour{1.0f, 1.0f, 1.0f, 1.0f};
    q.texture = image.texture;
    q.palette = palette_;
    mdk_render_canvas_quad(renderer, &q);
}

} // namespace mdk

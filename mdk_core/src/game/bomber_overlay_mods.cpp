// ============================================================================
// bomber_overlay_mods.cpp — BomberOverlay's mod images (the C# constructor's
// CanvasImages.Replace, BomberOverlay.cs:52-55). Apart from bomber_overlay.cpp so
// the goldens' binary links the overlay without ModImages. See bomber_overlay.h.
// ============================================================================

#include "game/bomber_overlay.h"

#include <string>

#include "formats/bni.h"
#include "formats/palette.h"
#include "formats/sprite_animation.h"
#include "formats/texture.h"
#include "game/mod_images.h"

namespace mdk {

void BomberOverlay::replaceWithMods(MdkRenderer renderer, const formats::Bni& sprites,
                                    const formats::Palette& colours, ModImages* mods) {
    replaceWithMod(renderer, sprites, kTarget, colours, mods, target_);
    replaceWithMod(renderer, sprites, kCross, colours, mods, cross_);
}

void BomberOverlay::replaceWithMod(MdkRenderer renderer, const formats::Bni& sprites,
                                   const char* animation, const formats::Palette& colours,
                                   ModImages* mods, const Image& image) {
    // CanvasImages.Replace: a mod's image of `<animation>_0` replaces the picture, as
    // HudView's own pictures are replaced (hud_view.cpp's loadImage).
    formats::SpriteAnimation frames;
    formats::SpriteAnimation::Frame frame;
    if (mods == nullptr || image.texture < 0 || !sprites.animation(animation, &frames) ||
        !frames.frame(0, &frame) || frame.image.frameCount() != 1) {
        return;
    }
    const std::string name = std::string(animation) + "_0";
    if (mods->compressed()) {
        // The 3DS HD pack: the picture's .t3x, imported by the backend.
        std::string path;
        if (mods->resolve(ModImages::Kind::Images, name, frame.image, colours, &path)) {
            mdk_render_replace_image_compressed(renderer, image.texture, path.c_str());
        }
        return;
    }
    HdImage hd;
    if (mods->image(name, frame.image, colours, &hd)) {
        mdk_render_replace_image(renderer, image.texture, palette_, hd.width, hd.height,
                                 hd.rgba.data());
    }
}

} // namespace mdk

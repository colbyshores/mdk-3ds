// ============================================================================
// surface_cache.cpp — see surface_cache.h.
// ============================================================================

#include "render/surface_cache.h"

namespace mdk {

namespace {

// A colour as a key: the four bytes packed. The +0.5f is the round-to-nearest
// the C#'s Vector4-to-byte conversion does; a truncation would darken every
// flat colour by up to one byte.
uint32_t packColour(const Colour& colour) {
    const uint32_t r = (uint32_t)(colour.r * 255.0f + 0.5f) & 0xFFu;
    const uint32_t g = (uint32_t)(colour.g * 255.0f + 0.5f) & 0xFFu;
    const uint32_t b = (uint32_t)(colour.b * 255.0f + 0.5f) & 0xFFu;
    const uint32_t a = (uint32_t)(colour.a * 255.0f + 0.5f) & 0xFFu;
    return r | (g << 8) | (b << 16) | (a << 24);
}

} // namespace

MdkPaletteId SurfaceCache::paletteId(const formats::Palette* palette) {
    if (!renderer_ || palette == nullptr) {
        return -1;
    }
    std::map<const formats::Palette*, MdkPaletteId>::const_iterator found = palettes_.find(palette);
    if (found != palettes_.end()) {
        return found->second;
    }
    // The 3DS keeps it to expand the textures through it; the Vulkan backend
    // binds it as a texture of its own and the material's palette id selects it.
    const MdkPaletteId id = mdk_render_create_palette(renderer_, palette->rgba());
    palettes_[palette] = id;
    return id;
}

MdkTextureId SurfaceCache::textureId(const formats::Texture* texture,
                                     const formats::Palette* palette) {
    if (!renderer_ || texture == nullptr || palette == nullptr) {
        return -1;
    }
    const std::pair<const formats::Texture*, const formats::Palette*> key(texture, palette);
    std::map<decltype(key), MdkTextureId>::const_iterator found = textures_.find(key);
    if (found != textures_.end()) {
        return found->second;
    }

    // The texture's height already includes the animation frames: the
    // original stacks them vertically.
    const MdkTextureId id = mdk_render_create_index_texture(
        renderer_, texture->width(), texture->height() * texture->frameCount(),
        texture->indices(), palette->rgba());
    textures_[key] = id;
    return id;
}

MdkTextureId SurfaceCache::flatColourId(const Colour& colour) {
    if (!renderer_) {
        return -1;
    }
    const uint32_t key = packColour(colour);
    std::map<uint32_t, MdkTextureId>::const_iterator found = flatColours_.find(key);
    if (found != flatColours_.end()) {
        return found->second;
    }
    // A one-pixel texture of the colour. The 3DS draws a flat surface with
    // it, because the PICA200's combiner cannot take a constant colour from
    // a vertex shader.
    uint8_t pixel[4];
    pixel[0] = (uint8_t)((key >> 0) & 0xFFu);
    pixel[1] = (uint8_t)((key >> 8) & 0xFFu);
    pixel[2] = (uint8_t)((key >> 16) & 0xFFu);
    pixel[3] = (uint8_t)((key >> 24) & 0xFFu);
    const MdkTextureId id = mdk_render_create_rgba_texture(renderer_, 1, 1, pixel);
    flatColours_[key] = id;
    return id;
}

MdkTextureId SurfaceCache::surfaceId(const Surface& surface, const formats::Palette* palette) {
    if (surface.texture == nullptr) {
        return flatColourId(surface.colour);
    }
    const MdkTextureId id = textureId(surface.texture, palette);
    // A Lit surface's mod replacement, asked once per (texture, palette)
    // pair (the C#'s MaterialResolver.Replace, at the first point here
    // where the GPU ids it names exist). The backend copies the bytes, so
    // the lookup's image need only live for this call.
    if (replacementLookup_ && surface.shading == Shading::Lit && id >= 0) {
        const std::pair<const formats::Texture*, const formats::Palette*> key(surface.texture,
                                                                               palette);
        if (replacementLooked_.find(key) == replacementLooked_.end()) {
            replacementLooked_[key] = true;
            ColourReplacement replacement;
            const bool looked = replacementLookup_(replacementUser_, surface, palette, &replacement);
            if (looked && replacement.compressedPath && replacement.compressedPath[0]) {
                mdk_render_replace_compressed(renderer_, id, replacement.compressedPath);
                replacementFound_++;
            } else if (looked && replacement.rgba && replacement.width > 0 &&
                       replacement.height > 0) {
                mdk_render_replace_colour(renderer_, id, paletteId(palette), surface.frameCount,
                                          replacement.width, replacement.height, replacement.rgba);
                replacementFound_++;
            }
        }
    }
    return id;
}

} // namespace mdk

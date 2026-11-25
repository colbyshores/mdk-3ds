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
        const MdkTextureId id = found->second;
        // A PACKED ORIGINAL the backend could not make from its file (surfaceId's
        // packed-original hit, unreadable or not this set's .t3x): it gets
        // its index pixels, the same fetch as a first make's below, and from
        // then on it is an ordinary index texture under the SAME id -- nothing
        // the caller holds changes. Pixels that cannot be had yet are asked for
        // again on the next call.
        if (id >= 0 && mdk_render_texture_needs_pixels(renderer_, id)) {
            const int32_t texels = texture->width() * texture->height() * texture->frameCount();
            const bool pixelsWereAway = texture->pixelsAway();
            const uint8_t* indices = texture->indices();
            if (indices == nullptr || texels <= 0 || texture->indexCount() < texels) {
                return id;
            }
            if (pixelsWereAway) {
                sourceStats_.mtoDemandReads++;
                sourceStats_.mtoDemandBytes += texels;
            }
            mdk_render_update_texture(renderer_, id, texture->width(),
                                      texture->height() * texture->frameCount(), indices);
            mdk_render_prepare_texture(renderer_, id);
        }
        return id;
    }

    // The pixels, which may have to be read back first (an arena's are streamed:
    // formats/texture.h). A texture whose pixels cannot be had -- the file
    // stopped reading -- is NOT cached as a failure and NOT handed to the
    // backend with a null pointer: the surface is skipped this frame and asked
    // for again, which costs nothing once the stream has given up on the arena.
    const int32_t texels = texture->width() * texture->height() * texture->frameCount();
    const bool pixelsWereAway = texture->pixelsAway();
    const uint8_t* indices = texture->indices();
    if (indices == nullptr || texels <= 0 || texture->indexCount() < texels) {
        return -1;
    }
    if (pixelsWereAway) {
        sourceStats_.mtoDemandReads++;
        sourceStats_.mtoDemandBytes += texels;
    }
    // The texture's height already includes the animation frames: the
    // original stacks them vertically.
    const MdkTextureId id = mdk_render_create_index_texture(
        renderer_, texture->width(), texture->height() * texture->frameCount(),
        indices, palette->rgba());
    textures_[key] = id;
    return id;
}

int32_t SurfaceCache::refresh(const formats::Texture* texture) {
    if (!renderer_ || texture == nullptr) {
        return 0;
    }
    const int32_t texels = texture->width() * texture->height() * texture->frameCount();
    const bool pixelsWereAway = texture->pixelsAway();
    const uint8_t* indices = texture->indices();
    if (indices == nullptr || texels <= 0 || texture->indexCount() < texels) {
        return 0;
    }
    if (pixelsWereAway) {
        sourceStats_.mtoDemandReads++;
        sourceStats_.mtoDemandBytes += texels;
    }
    int32_t updated = 0;
    typedef std::map<std::pair<const formats::Texture*, const formats::Palette*>, MdkTextureId>
        TextureMap;
    for (TextureMap::const_iterator it = textures_.begin(); it != textures_.end(); ++it) {
        if (it->first.first != texture || it->second < 0) {
            continue;
        }
        // The same stacked-frame size the texture was made with (textureId above).
        mdk_render_update_texture(renderer_, it->second, texture->width(),
                                  texture->height() * texture->frameCount(), indices);
        updated++;
    }
    return updated;
}

int32_t SurfaceCache::releasePalette(const formats::Palette* palette) {
    if (!renderer_ || palette == nullptr) {
        return 0;
    }
    int32_t released = 0;
    typedef std::map<std::pair<const formats::Texture*, const formats::Palette*>, MdkTextureId>
        TextureMap;
    for (TextureMap::iterator it = textures_.begin(); it != textures_.end();) {
        if (it->first.second == palette) {
            if (it->second >= 0) {
                mdk_render_release_texture(renderer_, it->second);
                released++;
            }
            textures_.erase(it++);
        } else {
            ++it;
        }
    }
    typedef std::map<std::pair<const formats::Texture*, const formats::Palette*>, bool> LookedMap;
    for (LookedMap::iterator it = replacementLooked_.begin(); it != replacementLooked_.end();) {
        if (it->first.second == palette) {
            replacementLooked_.erase(it++);
        } else {
            ++it;
        }
    }
    std::map<const formats::Palette*, MdkPaletteId>::iterator found = palettes_.find(palette);
    if (found != palettes_.end()) {
        if (found->second >= 0) {
            mdk_render_release_palette(renderer_, found->second);
        }
        palettes_.erase(found);
    }
    paletteReleases_++;
    return released;
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

MdkTextureId SurfaceCache::prepare(const Surface& surface, const formats::Palette* palette) {
    const MdkTextureId id = surfaceId(surface, palette);
    if (id >= 0) {
        mdk_render_prepare_texture(renderer_, id);
    }
    return id;
}

MdkTextureId SurfaceCache::createPacked(const Key& key, const Surface& surface,
                                        const formats::Palette* palette,
                                        const ColourReplacement& replacement) {
    if (palette == nullptr || !replacement.compressedPath || !replacement.compressedPath[0]) {
        return -1;
    }
    // A PAIR ALREADY MADE (its index texture, by a draw that asked nothing or
    // whose answer it did not take) keeps the texture it has: overwriting the
    // entry would leak that texture for the level's life.
    if (textures_.find(key) != textures_.end()) {
        return -1;
    }
    const MdkTextureId made = mdk_render_create_compressed_file(
        renderer_, surface.texture->width(),
        surface.texture->height() * surface.texture->frameCount(), replacement.compressedPath,
        replacement.hdPath, palette->rgba());
    if (made < 0) {
        return -1;
    }
    replacementLooked_[key] = true;
    textures_[key] = made;
    replacementFound_++;
    return made;
}

MdkTextureId SurfaceCache::requestPacked(const Surface& surface,
                                         const formats::Palette* palette) {
    if (!renderer_ || surface.texture == nullptr || palette == nullptr) {
        return -1;
    }
    const Key key(surface.texture, palette);
    std::map<Key, MdkTextureId>::const_iterator found = textures_.find(key);
    if (found != textures_.end()) {
        return found->second;
    }
    if (!replacementLookup_ || surface.shading == Shading::Original ||
        replacementLooked_.find(key) != replacementLooked_.end()) {
        return -1;
    }
    ColourReplacement replacement;
    if (!replacementLookup_(replacementUser_, surface, palette, &replacement)) {
        // A Lit miss is surfaceId's miss too; a Sprite's is its own (the header).
        if (surface.shading == Shading::Lit) {
            replacementLooked_[key] = true;
        }
        return -1;
    }
    return createPacked(key, surface, palette, replacement);
}

void SurfaceCache::prepareCached(const Surface& surface, const formats::Palette* palette) {
    if (!renderer_ || surface.texture == nullptr) {
        return;
    }
    std::map<Key, MdkTextureId>::const_iterator found =
        textures_.find(Key(surface.texture, palette));
    if (found != textures_.end() && found->second >= 0) {
        mdk_render_prepare_texture(renderer_, found->second);
    }
}

MdkTextureId SurfaceCache::surfaceId(const Surface& surface, const formats::Palette* palette) {
    if (surface.texture == nullptr) {
        return flatColourId(surface.colour);
    }
    // A SPRITE TAKES A PACKED ORIGINAL AND NOTHING ELSE (surface_cache.h). The
    // effects draw their sprites Sprite-shaded, so before this an explosion's
    // drops were palette-expanded whole in their first draw though the orig pack
    // holds them (measured on the Old 3DS level-7 walk, frame 650: SL_MED 32x960
    // 42.7 ms and SL_SMA 16x480 11.0 ms in one effects draw). requestPacked makes
    // and caches the pending id only for a packed-original answer; anything
    // else leaves the pair to textureId, which also hands a pack file the backend
    // could not make its index pixels.
    if (surface.shading == Shading::Sprite) {
        requestPacked(surface, palette);
        return textureId(surface.texture, palette);
    }
    // THE LOOKUP COMES FIRST, THE TEXTURE SECOND (the arena orig pack's whole
    // point). A Lit surface the pack holds as a .t3x of the ORIGINAL look is
    // ASKED FOR from that file (mdk_render_create_compressed_file: the backend
    // reads and imports it off the frame) and the ARM11 never expands its
    // palette -- the 600-1600 ms an arena's expansion costs is exactly the
    // hitch the pack exists to remove. Only a packed-original hit takes this
    // path, and a -1 (no compressed textures in this backend, or a pair already
    // made) falls through to the ordinary expansion. A file the
    // backend later cannot make gets its index pixels in textureId's hit path.
    // Asked once per pair, as before; the bytes the lookup hands back are
    // borrowed for the call only.
    const std::pair<const formats::Texture*, const formats::Palette*> key(surface.texture,
                                                                           palette);
    ColourReplacement replacement;
    bool lookedNow = false;
    if (replacementLookup_ && surface.shading == Shading::Lit &&
        replacementLooked_.find(key) == replacementLooked_.end()) {
        // NOT marked yet: a streamed arena whose pixels are not ready returns -1
        // from textureId below, and a replacement consumed on that frame would
        // never be attached (the pair would be marked asked with nothing landed).
        // The mark lands when the answer is a miss, or when the replacement is.
        lookedNow = replacementLookup_(replacementUser_, surface, palette, &replacement);
        if (!lookedNow) {
            replacementLooked_[key] = true;
        } else {
            const MdkTextureId made = createPacked(key, surface, palette, replacement);
            if (made >= 0) {
                return made;
            }
        }
    }
    const MdkTextureId id = textureId(surface.texture, palette);
    // The replacement that is NOT asked for up front: a decoded colour image
    // (the PC mods). The backend copies the bytes, so the lookup's image need
    // only live for this call. A packed original's answer for a pair already
    // made attaches nothing (the file IS the original; its HD copy is not taken).
    if (lookedNow && id >= 0) {
        const bool packed = replacement.compressedPath && replacement.compressedPath[0];
        if (!packed && replacement.rgba && replacement.width > 0 && replacement.height > 0) {
            mdk_render_replace_colour(renderer_, id, paletteId(palette), surface.frameCount,
                                      replacement.width, replacement.height, replacement.rgba);
            replacementFound_++;
        }
        replacementLooked_[key] = true;
    }
    return id;
}

} // namespace mdk

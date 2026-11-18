#pragma once

// ============================================================================
// surface_cache.h — a resolved surface's GPU objects.
//
// The C#'s MaterialResolver owns two caches: `_textures`, keyed by the format's
// Texture, and `_palettes`, keyed by the format's Palette, and every Material it
// hands the renderer names an id out of them (MaterialResolver.cs's TextureId /
// PaletteId). The C++ MaterialResolver deliberately carries no GPU handles
// (material_resolver.h says why), so the two caches live here instead, and
// anything that draws a surface asks this: the arena view and the object pass.
//
// WHY ONE CACHE AND NOT ONE PER VIEW. In the C# the arena view and the object
// view are built with the SAME resolver, so an arena and an object that use the
// same texture through the same palette are ONE GPU texture. Two caches would
// make two, and on the 3DS -- where an index texture is palette-expanded RGBA8
// padded to a power of two -- that is not a tidiness point, it is megabytes.
//
// WHY THE PALETTE IS PART OF THE TEXTURE KEY. The same index texture is drawn
// through DIFFERENT palettes in different arenas (the arena colours replace
// palette indices 64-175), and on the 3DS the lookup happens on the CPU when the
// texture is made, so the pixels depend on the palette. Keying by the texture
// alone would draw one arena through another's colours.
//
// The objects are released with the renderer, not here: a level's textures are
// made once and live as long as the level, exactly as in the C#, whose
// Renderer.Mark/Release scopes the level view never uses.
// ============================================================================

#include <stdint.h>

#include <map>
#include <utility>

#include "formats/palette.h"
#include "formats/texture.h"
#include "render/material_resolver.h"
#include "render/render.h"
#include "render/render_types.h"

namespace mdk {

// A whole colour image replacing a surface's palette expansion (a mod's
// replacement, game/mod_images.h's HdImage without the owning vector):
// frames of width x height stacked downwards, RGBA8 premultiplied. The
// bytes are borrowed for the call only; the backend copies them.
struct ColourReplacement {
    int32_t width = 0;
    int32_t height = 0;
    int32_t frames = 1;
    const uint8_t* rgba = nullptr;
    // OR a compressed file (the 3DS HD pack's .t3x), handed to
    // mdk_render_replace_compressed instead; borrowed for the call only.
    const char* compressedPath = nullptr;
};

// Asks for a Lit surface's colour replacement, or returns false. Called
// once per (texture, palette) pair, the first time the pair is drawn (the
// C#'s MaterialResolver.Replace runs as surfaces resolve; here the GPU ids
// only exist at this point, so this is where the replacement can land).
typedef bool (*ColourReplacementLookup)(void* user, const Surface& surface,
                                        const formats::Palette* palette,
                                        ColourReplacement* out);

class SurfaceCache {
public:
    SurfaceCache() = default;

    // The renderer the objects are made in is BORROWED and must outlive the
    // cache (the C#'s resolver holds its renderer the same way).
    explicit SurfaceCache(MdkRenderer renderer) : renderer_(renderer) {}

    MdkRenderer renderer() const { return renderer_; }

    // The GPU palette for a format palette (the C#'s PaletteId), made on first
    // use. Null for no palette, which is what a surface with no texture to read
    // through one needs.
    MdkPaletteId paletteId(const formats::Palette* palette);

    // The index texture for `texture` read through `palette` (the C#'s
    // TextureId), made on first use. The animation frames are stacked
    // vertically, so the height the backend gets already includes them.
    MdkTextureId textureId(const formats::Texture* texture, const formats::Palette* palette);

    // The GPU texture for a flat colour: a ONE-PIXEL texture of that colour.
    // The PICA200's combiner has no constant-colour source for a vertex
    // shader's output, so a flat colour has to come from a texture.
    MdkTextureId flatColourId(const Colour& colour);

    // The texture a draw of `surface` uses: its index texture through
    // `palette`, or the flat colour's one-pixel texture. This is the whole
    // mapping from a resolved surface to what a Material needs; a second copy
    // of it in a second view is how the two trees drift.
    MdkTextureId surfaceId(const Surface& surface, const formats::Palette* palette);

    // Release everything made through `palette`: its textures, their mod
    // replacements' bookkeeping, and the palette itself. An arena that is no
    // longer on the GPU gives its colours back this way (game/arena_views.h); a
    // later draw through the palette makes them again, replacements included.
    // Returns how many textures went.
    int32_t releasePalette(const formats::Palette* palette);

    // Whether anything has been made through `palette` (its palette id exists:
    // every draw asks for the palette's id beside its texture's).
    bool hasPalette(const formats::Palette* palette) const {
        return palettes_.find(palette) != palettes_.end();
    }

    // The colour-replacement lookup (the mods; null: none, the shipped
    // behaviour). Set once by the frontend before the first surface draws.
    void setColourReplacementLookup(ColourReplacementLookup lookup, void* user) {
        replacementLookup_ = lookup;
        replacementUser_ = user;
    }

    // (texture, palette) pairs asked about, and replacements found: the
    // C# Viewer's "textures X of Y" summary line.
    int32_t replacementLooked() const { return (int32_t)replacementLooked_.size(); }
    int32_t replacementFound() const { return replacementFound_; }

private:
    MdkRenderer renderer_ = nullptr;
    ColourReplacementLookup replacementLookup_ = nullptr;
    void* replacementUser_ = nullptr;
    std::map<std::pair<const formats::Texture*, const formats::Palette*>, bool> replacementLooked_;
    int32_t replacementFound_ = 0;

    // A map, not a vector: these are borrowed pointers into LevelData, and a
    // map's elements do not move when later entries are inserted.
    std::map<const formats::Palette*, MdkPaletteId> palettes_;
    std::map<std::pair<const formats::Texture*, const formats::Palette*>, MdkTextureId> textures_;
    std::map<uint32_t, MdkTextureId> flatColours_;
};

} // namespace mdk

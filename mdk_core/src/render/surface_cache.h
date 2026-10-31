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

private:
    MdkRenderer renderer_ = nullptr;

    // A map, not a vector: these are borrowed pointers into LevelData, and a
    // map's elements do not move when later entries are inserted.
    std::map<const formats::Palette*, MdkPaletteId> palettes_;
    std::map<std::pair<const formats::Texture*, const formats::Palette*>, MdkTextureId> textures_;
    std::map<uint32_t, MdkTextureId> flatColours_;
};

} // namespace mdk

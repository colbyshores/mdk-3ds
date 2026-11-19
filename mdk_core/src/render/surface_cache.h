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

// ---------------------------------------------------------------------------
// THE PREWARM: a surface's GPU texture made BEFORE the frame that first draws it.
//
// WHY. A surface's texture is made the first time a draw asks for it
// (surfaceId), which puts the backend's whole creation -- on the 3DS the palette
// expansion into tiled RGBA8, the filter preparation and the upload -- inside the
// frame that first shows it. Measured on the Old 3DS (level 8, 2026-11-19): the
// frames where things come into view take the draw build from ~8 ms to 20-40 ms,
// in clusters of 13-16 frames. The C# makes every texture at the level's load
// (Viewer.Preload: "a level's load: no frame creates any"); Forsaken pre-inits
// every model at load. The 3DS cannot hold every texture of a level at once (the
// arenas stream, the textures evict), so the prewarm makes what the frontend
// says will be drawn, within a byte budget the platform names, and stops.
//
// It makes EXACTLY what the draw would have made -- the same surfaceId call, so
// the same texture, the same palette, the same mod replacement lookup -- only
// earlier. A texture whose pixels are away (an arena still streaming) is NEVER
// made: asking for its indices would load them inside the frame, the stream's
// demand backstop, which is the stall the prewarm exists to remove.
//
// The answer for one surface.
enum class PrewarmResult {
    Made,     // the texture was made now
    Had,      // it was already made (or already tried: the draw path's own cache)
    Away,     // its pixels are not in memory: not made, ask again later
    NoBudget, // the call's make budget is spent: ask again later
    NoRoom,   // the platform said there is no room for it: ask again later
    Failed    // the backend refused it (the draw will try again, as it does now)
};

// Whether there is room for a texture costing `bytes` of GPU memory (the
// backend's own arithmetic: mdk_render_index_texture_bytes). The platform's
// answer, so that a prewarm never makes the backend EVICT: a texture made only
// to push out one that is being drawn would move the cost, not remove it.
typedef bool (*TextureRoom)(void* user, int32_t bytes);

// What one prewarm pass may spend. `makes` < 0: no bound on the count.
struct PrewarmBudget {
    int32_t makes = -1;
    TextureRoom room = nullptr;   // null: always room (the desktop)
    void* user = nullptr;
    // What the pass did, for the frontend's log.
    int32_t made = 0;
    int32_t madeBytes = 0;
    int32_t away = 0;
    int32_t noRoom = 0;
    int32_t failed = 0;

    bool spent() const { return makes == 0; }
};

// True when a prewarm walk should stop at this answer and resume at the same
// surface next time (the budget, the room, pixels that are not in yet); false
// when it may go on to the next surface (made, already made, refused). A pure
// rule, pinned by the format tests.
inline bool prewarmStops(PrewarmResult result) {
    return result == PrewarmResult::Away || result == PrewarmResult::NoBudget ||
           result == PrewarmResult::NoRoom;
}

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

    // Make `surface`'s texture through `palette` now, if the budget allows (see
    // THE PREWARM above). Exactly the draw's surfaceId + paletteId, earlier.
    PrewarmResult prewarm(const Surface& surface, const formats::Palette* palette,
                          PrewarmBudget* budget);

    // Whether the draw of `surface` through `palette` would find its texture
    // made (or already refused) -- i.e. would make nothing.
    bool has(const Surface& surface, const formats::Palette* palette) const;

    // The textures this cache has asked the backend to make, over its life
    // (index textures and flat colours; a palette release and remake counts
    // again). The first-use counter the frontends' profile lines read.
    int32_t texturesMade() const { return texturesMade_; }

private:
    int32_t texturesMade_ = 0;
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

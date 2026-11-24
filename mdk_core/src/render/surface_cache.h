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
    // The compressedPath is the ORIGINAL look at the original size (the arena
    // orig pack), not an upscale: surfaceId asks for the texture from the file
    // up front (mdk_render_create_compressed_file: read and made by the backend
    // off the frame) and the palette is never expanded -- unless the backend
    // cannot make the file, when textureId hands it the index pixels. False
    // for an HD-pack upscale, which keeps the expansion as its fallback and the
    // toggle's per-draw choice.
    bool compressedOriginal = false;
};

// Asks for a Lit surface's colour replacement, or returns false. Called
// once per (texture, palette) pair, the first time the pair is drawn (the
// C#'s MaterialResolver.Replace runs as surfaces resolve). It is asked
// BEFORE the texture is made, so a compressedOriginal hit can be made from
// its file instead of expanded; the replacement then lands on the id the
// surface ends up with (the file's, or the made expansion for the other
// replacement kinds).
//
// A Sprite surface asks it too (SurfaceCache::surfaceId) and takes ONLY a
// compressedOriginal answer. So the answer must be the pair's, never the
// shading's: a Sprite's packed original is the texture a later Lit draw of the
// same pair is handed, and it must be the one that Lit draw would have asked for.
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
    // vertically, so the height the backend gets already includes them. A
    // packed original the backend could not make from its file
    // (mdk_render_texture_needs_pixels) is handed its index pixels here, under
    // the same id.
    MdkTextureId textureId(const formats::Texture* texture, const formats::Palette* palette);

    // A texture's pixels changed (a bullet hole, scripts/bullet_holes.h): every GPU
    // texture made from it, through any palette, gets its pixels again (the C#'s
    // MaterialResolver.Refresh, subscribed to ScriptRuntime.TextureStamped at
    // Viewer.cs:263). A texture not on the GPU yet needs nothing: it is made from the
    // changed pixels when first drawn. Returns how many were updated.
    int32_t refresh(const formats::Texture* texture);

    // The GPU texture for a flat colour: a ONE-PIXEL texture of that colour.
    // The PICA200's combiner has no constant-colour source for a vertex
    // shader's output, so a flat colour has to come from a texture.
    MdkTextureId flatColourId(const Colour& colour);

    // The texture a draw of `surface` uses: its index texture through
    // `palette`, or the flat colour's one-pixel texture. This is the whole
    // mapping from a resolved surface to what a Material needs; a second copy
    // of it in a second view is how the two trees drift.
    //
    // A Lit surface's replacement is asked for here (the lookup above). A
    // SPRITE (an effect, an object's shine) takes the pair's packed original
    // and nothing else -- requestPacked, then the index texture as before: an
    // HD-pack upscale or a decoded image stays Lit-only (the C# replaces only
    // Lit materials), and an answer it does not take is left for the pair's
    // Lit draw to ask for again. An Original-shaded surface asks nothing.
    MdkTextureId surfaceId(const Surface& surface, const formats::Palette* palette);

    // surfaceId, plus asking the backend to have the copy ready before a
    // draw needs it (render.h mdk_render_prepare_texture): the pump's way
    // of saying a resident arena's large textures should not be re-made in
    // the frame that first draws them again after an eviction.
    MdkTextureId prepare(const Surface& surface, const formats::Palette* palette);

    // THE PUMP'S ASK FOR A HIDDEN SURFACE (LevelView::pumpSurfaces): a group the
    // scripts hid is not drawn, so its texture is not made -- but a surface the
    // orig pack holds costs nothing to ask for (a pending id, no I/O, no
    // expansion; the backend reads it off the frame). So: the replacement
    // lookup, and ONLY when its answer is a compressedOriginal, the pending id
    // made and cached exactly as surfaceId makes it (one helper, createPacked).
    // Any other answer makes nothing -- a hidden index texture would be a
    // palette expansion for a surface nobody may ever see -- and leaves the pair
    // to surfaceId: a miss is marked asked (surfaceId then makes the index
    // texture, as it would after its own miss); a hit of another kind is NOT
    // marked, so surfaceId asks again and attaches it to the texture it makes.
    //
    // ALSO A SPRITE'S WHOLE LOOKUP (surfaceId) and the effects warm's ask: a
    // Sprite surface comes through here too, an Original-shaded one never. A
    // Sprite's miss is NOT marked: the mark would stop the pair's Lit draw from
    // asking, and that draw may be named differently (a lookup can answer by
    // name) and find what the Sprite did not.
    // Returns the pair's id, or -1 when none was made.
    MdkTextureId requestPacked(const Surface& surface, const formats::Palette* palette);

    // The pair's texture asked to be ready (mdk_render_prepare_texture) only if
    // it is already made: nothing is looked up, nothing created. The pump's
    // re-ask for a packed original evicted while its arena sits resident (the
    // backend reads it again before the draw); a backend no-op for a resident
    // texture and for a small index one.
    void prepareCached(const Surface& surface, const formats::Palette* palette);

    // Release everything made through `palette`: its textures, their mod
    // replacements' bookkeeping, and the palette itself. An arena that is no
    // longer on the GPU gives its colours back this way (game/arena_views.h); a
    // later draw through the palette makes them again, replacements included.
    // Returns how many textures went.
    int32_t releasePalette(const formats::Palette* palette);

    // How many palettes releasePalette has released, ever: a caller that keeps
    // its own progress over this cache's entries (the frontend's object warm)
    // tells from a change that some of them are gone.
    int32_t paletteReleases() const { return paletteReleases_; }

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

    // Synchronous resource sources, for a profile frontend to attribute a
    // hitch instead of calling every first use a texture make. Counts are
    // cumulative; the byte count is index bytes returned by an MTO read.
    // (The packed originals' reads and makes are the backend's to count: on
    // the 3DS, render_c3d_stats.h's packImported / packSync.)
    struct SourceStats {
        int32_t mtoDemandReads = 0;
        int64_t mtoDemandBytes = 0;
    };
    const SourceStats& sourceStats() const { return sourceStats_; }

private:
    typedef std::pair<const formats::Texture*, const formats::Palette*> Key;

    // A compressedOriginal answer made into the pair's pending id
    // (mdk_render_create_compressed_file), cached and marked asked. -1, with
    // nothing marked, when the answer is not one or the backend has no
    // compressed textures. surfaceId's and requestPacked's one implementation.
    MdkTextureId createPacked(const Key& key, const Surface& surface,
                              const formats::Palette* palette,
                              const ColourReplacement& replacement);

    MdkRenderer renderer_ = nullptr;
    ColourReplacementLookup replacementLookup_ = nullptr;
    void* replacementUser_ = nullptr;
    std::map<std::pair<const formats::Texture*, const formats::Palette*>, bool> replacementLooked_;
    int32_t replacementFound_ = 0;
    int32_t paletteReleases_ = 0;
    SourceStats sourceStats_;

    // A map, not a vector: these are borrowed pointers into LevelData, and a
    // map's elements do not move when later entries are inserted.
    std::map<const formats::Palette*, MdkPaletteId> palettes_;
    std::map<std::pair<const formats::Texture*, const formats::Palette*>, MdkTextureId> textures_;
    std::map<uint32_t, MdkTextureId> flatColours_;
};

} // namespace mdk

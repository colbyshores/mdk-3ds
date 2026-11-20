#pragma once

// ============================================================================
// effects_view.h -- the flying pieces and sprite effects, laid out.
//
// A port of src/Mdk.Game/Objects/EffectsView.cs, the parts whose pools
// exist in this tree: the debris pieces (textured triangles batched by
// surface; sparks as flat palette-coloured triangles shaded by how much
// they face the camera) and the sprite effects (wounds, drops, bubbles,
// trails) as camera-facing quads showing one frame of their animated
// texture. Everything is rebuilt every frame into one vertex list with
// one queued draw per batch/quad; EffectsDraw (game/effects_draw.h)
// uploads and draws it, as ObjectDraw does for ObjectView's stream.
//
// After the sprites come the tornado's twisters' ribbons (Ribbon.Triangles,
// WMIBTEX): one queued draw per ribbon, at most 24 corners each, so a
// ribbon is ONE draw call on either backend.
//
// NOT HERE, because its pool is not ported yet: the spawn_box billboards
// (opcode 159). The oracle draws them between the sprites and the
// ribbons, so they join this build when that subsystem lands.
//
// The oracle's mesh holds 1<<16 vertices and a frame draws at most
// that many; the cap is kept exactly (quads that do not fit are
// dropped, a piece batch is truncated to whole triangles).
// ============================================================================

#include <stdint.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "game/object_view.h"
#include "render/material_resolver.h"
#include "render/render_types.h"
#include "scripts/twister.h"
#include "util/matrix.h"

namespace mdk {

class LevelData;

namespace scripts {
class EndLevel;
class ScriptRuntime;
} // namespace scripts

// The billboard basis of a view-projection matrix, as the oracle reads
// it out of the matrix itself: forward from the fourth column, right
// from the first, up = right x forward. Each is normalised.
void effectsBillboardBasis(const Mat4& viewProjection, Vec3* right, Vec3* up, Vec3* forward);

// The palette index a spark's face draws with: base + range * |facing|,
// rounded to nearest (ties to even, as MathF.Round) and clamped to the
// palette. A degenerate face has facing 0.
int32_t effectsSparkColourIndex(int32_t base, int32_t range, const Vec3& a, const Vec3& b,
                                const Vec3& c, const Vec3& forward);

// The six corners of a sprite quad: `right` and `up` are ONE TEXEL each,
// the texture is width x height texels, centred at `center`. UVs run
// (0,0) top-left to (1,1) bottom-right.
void effectsQuadVertices(const Vec3& center, const Vec3& right, const Vec3& up, int32_t width,
                         int32_t height, Vertex out[6]);

// A twister's ribbon as vertices (EffectsView.AddRibbon): Ribbon.Triangles'
// corners, their UVs from WMIBTEX's texels to 0-1 (times 1/width, 1/height,
// the texture being width x height). Returns the corner count, 0 while the
// ribbon has fewer than two positions.
int32_t effectsRibbonVertices(const scripts::Ribbon& ribbon, int32_t width, int32_t height,
                              Vertex out[scripts::Ribbon::kMaxCorners]);

// THE GLOW OF A SPRITE TEXTURE, for the 3DS's glow billboards (GlowQuad): of
// the opaque texels (index != 0) of `indices` through `paletteRgba`, the share
// whose LINEAR colour (sRGB^2.2, as the PC's post.frag reads it) passes the PC
// bloom's threshold (post_params.h kGlowThreshold) in any channel, and -- per
// channel, over those -- the mean of what the PC's bloom adds from a texel,
// max(linear - threshold, 0) (post.frag's shine before its gain). 0 and black
// when none pass. Cold: once per sprite texture.
void effectsGlowOf(const uint8_t* indices, int32_t texelCount, const uint8_t* paletteRgba,
                   float* glow, float colour[3]);

// The halo's size against its sprite (both axes) and how much of the measured
// colour it adds at the centre (the PC's kGlowIntensity, its bloom's gain).
static const float kGlowHaloScale = 2.0f;

// One queued draw of the effects frame: a slice of the frame's vertices
// with the surface and palette it draws through, and the animated
// texture's frame.
struct EffectsDrawItem {
    int32_t first = 0;
    int32_t count = 0;
    Surface surface;
    const formats::Palette* palette = nullptr;
    int32_t frame = 0;
};

// The frame's effects vertices and their draws. Kept between frames and
// reset at each build, so a frame after the first does not allocate.
struct EffectsFrame {
    std::vector<Vertex> vertices;
    std::vector<EffectsDrawItem> draws;
    // The bright sprite effects' halos (render_types.h GlowQuad).
    std::vector<GlowQuad> glows;

    void reset() {
        vertices.clear();
        draws.clear();
        glows.clear();
    }
};

class EffectsView {
public:
    // Vertices drawn at most per frame (the rest is dropped).
    static const int32_t kMaxVertices = 1 << 16;

    // The resolver and level are BORROWED. `sprites` is the shading the
    // sprite surfaces are given (Shading::Sprite in the enhanced look,
    // Shading::Original in the original look; EnhancedLook.Sprites).
    EffectsView(const MaterialResolver& resolver, LevelData* level, Shading sprites)
        : resolver_(&resolver), level_(level), sprites_(sprites) {}

    // Builds the frame: pieces first (batched by surface, in the order
    // their surfaces came), then one quad per sprite effect. `camera`
    // supplies the view-projection the billboards face.
    //
    // THE END OF A LEVEL'S PIECES (the C#'s LevelView.DrawPieces, Viewer.cs:703)
    // are laid out first, in the same batches: each torn triangle of the arena
    // (scripts::EndLevel::pieces), turned by its angle around its centre, through
    // the arena's own surface drawn both faces, batched by surface in the order the
    // pieces came. The C# draws them in their own pass right after the arena, before
    // the objects; here they share the effects pass (one upload, one draw a batch --
    // the 3DS's dynamic mesh is written once a frame, AGENTS.md P3), which is the same
    // picture for every opaque surface.
    //
    // The C# draws the pieces only for an arena that has a view, and only for the
    // triangles its view placed (Placements: not the clip/Disabled triangles, a drawn
    // surface). The arena is every level's MTO arena here, so the view's test is the
    // same as the triangle's own: a triangle whose surface does not resolve is skipped.
    void build(const scripts::ScriptRuntime& scripts, const Camera& camera, EffectsFrame* frame);

    // Drops the cached looks and sprite surfaces (a level change).
    void clear() {
        looks_.clear();
        spriteCache_.clear();
    }

private:
    struct SpriteEntry {
        bool present = false;
        Surface surface;
        const formats::Palette* palette = nullptr;
        // THE GLOW (effectsGlowOf): the share of the texture's opaque texels the
        // PC's bloom would catch, and their mean colour; 0 for none. Measured
        // once, when the entry is made and the texture's pixels are resident.
        float glow = 0.0f;
        float glowColour[3] = {0.0f, 0.0f, 0.0f};
        // Measured yet: a texture whose pixels were not resident when the entry
        // was made (arena streaming) is measured at a later build that finds
        // them, not cached as "no glow".
        bool glowMeasured = false;
    };

    // A piece batch: one surface's corners this frame. The batches keep
    // their vertex lists between frames (the oracle's _pieceVertices).
    struct Batch {
        Surface surface;
        const formats::Palette* palette = nullptr;
        std::vector<Vertex> vertices;
    };

    const ObjectLook& lookOf(const std::string& arena);
    void addEndPieces(const scripts::EndLevel& end);
    SpriteEntry& sprite(const std::string& arena, const std::string& texture);
    Batch& batchFor(const Surface& surface, const formats::Palette* palette);

    const MaterialResolver* resolver_;
    LevelData* level_;
    Shading sprites_;

    std::map<std::string, ObjectLook> looks_;
    std::map<std::pair<std::string, std::string>, SpriteEntry> spriteCache_;
    // The last build's sprite effects, the ones given a halo, and the ones whose
    // texture could not be measured yet (pixels not resident): a platform's log
    // reads them (spriteEffects / glowingEffects / unmeasuredGlows).
    int32_t lastSprites_ = 0;
    int32_t lastGlowing_ = 0;
    int32_t lastUnmeasured_ = 0;

public:
    int32_t spriteEffects() const { return lastSprites_; }
    int32_t glowingEffects() const { return lastGlowing_; }
    int32_t unmeasuredGlows() const { return lastUnmeasured_; }

private:

    std::vector<Batch> batches_;
    int32_t batchCount_ = 0;
};

} // namespace mdk

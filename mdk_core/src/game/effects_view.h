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
// NOT HERE, because their pools are not ported yet: the spawn_box
// billboards (opcode 159) and the twisters' ribbons (Items). The oracle
// draws them among the sprites, so they join this build when those
// subsystems land.
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
#include "util/matrix.h"

namespace mdk {

class LevelData;

namespace scripts {
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

    void reset() {
        vertices.clear();
        draws.clear();
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
    };

    // A piece batch: one surface's corners this frame. The batches keep
    // their vertex lists between frames (the oracle's _pieceVertices).
    struct Batch {
        Surface surface;
        const formats::Palette* palette = nullptr;
        std::vector<Vertex> vertices;
    };

    const ObjectLook& lookOf(const std::string& arena);
    const SpriteEntry& sprite(const std::string& arena, const std::string& texture);
    Batch& batchFor(const Surface& surface, const formats::Palette* palette);

    const MaterialResolver* resolver_;
    LevelData* level_;
    Shading sprites_;

    std::map<std::string, ObjectLook> looks_;
    std::map<std::pair<std::string, std::string>, SpriteEntry> spriteCache_;

    std::vector<Batch> batches_;
    int32_t batchCount_ = 0;
};

} // namespace mdk

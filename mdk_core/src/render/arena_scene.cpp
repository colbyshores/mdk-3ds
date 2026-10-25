// ============================================================================
// arena_scene.cpp — see arena_scene.h.
// ============================================================================

#include "render/arena_scene.h"

#include <map>

namespace mdk {

bool buildArenaScene(const formats::Arena* arena,
                     const MaterialResolver& resolver,
                     const formats::Palette& palette,
                     const std::vector<const formats::TextureArchive*>& archives,
                     ArenaScene* out) {
    if (!out) {
        return false;
    }
    out->clear();
    if (!arena) {
        return false;
    }

    const std::vector<int32_t>& indices = arena->triangleIndices();
    const std::vector<int32_t>& materials = arena->triangleMaterials();
    const std::vector<uint32_t>& flags = arena->triangleFlags();
    const std::vector<formats::Vec2f>& uvs = arena->triangleUvs();
    const std::vector<formats::Vec3f>& positions = arena->vertices();

    const int32_t triangleCount = arena->triangleCount();
    if (triangleCount <= 0 || (int32_t)indices.size() < triangleCount * 3) {
        return false;
    }

    out->vertices.reserve((size_t)triangleCount * 3);

    // The run of triangles currently being batched, and the material value
    // it is for. A new material value starts a new run.
    int32_t runMaterial = 0;
    bool haveRun = false;

    // The surfaces, by material value. A material value's surface is
    // resolved ONCE, because the resolution searches the archives and the
    // UV SCALE depends on the texture it found.
    //
    // THE UV SCALE MATTERS, and it is easy to miss: the arena's texture
    // coordinates are in TEXELS, and the C# normalises them by the
    // texture's size -- `new Vertex(position, triangleUvs[...] * new
    // Vector2(1f / texture.Width, 1f / texture.Height))`. Without the
    // normalisation the texture coordinates run over the whole texture
    // range instead of one frame of it, and the surfaces are covered in
    // garbage.
    std::map<int32_t, Surface> surfaces;
    std::map<int32_t, bool> drawn;
    std::map<int32_t, float> scaleX;
    std::map<int32_t, float> scaleY;

    for (int32_t t = 0; t < triangleCount; t++) {
        // The C# LevelView.Build skips a triangle the arena's clip flag marks
        // (the 1996 demo's not-drawn triangles) or the Disabled flag marks
        // (the retail arena activation's, LevelView.cs:294).
        if ((flags[(size_t)t] & (arena->clipFlag() | formats::Arena::kGroupDisabled)) != 0) {
            continue;
        }
        const int32_t triangleGroup = (int32_t)(flags[(size_t)t] >> 24);
        // A group whose stored state is Hidden is not drawn (LevelView.cs:126).
        // The state is read at BUILD time here; a group hidden mid-play needs a
        // scene rebuild to disappear (open rendering-integration item).
        if ((arena->groupState(triangleGroup) & formats::Arena::kGroupHidden) != 0) {
            continue;
        }
        // A stored group material replaces the triangle's own (C# Group.Material);
        // absence is a presence test, not a sentinel, because a negative material
        // is a valid flat palette colour.
        int32_t groupMaterial = 0;
        const int32_t material = arena->groupMaterial(triangleGroup, &groupMaterial)
                                     ? groupMaterial
                                     : materials[(size_t)t];

        if (surfaces.find(material) == surfaces.end()) {
            Surface surface;
            const bool isDrawn = resolver.resolve(material, arena->materials(), palette, archives, &surface);
            surfaces[material] = surface;
            drawn[material] = isDrawn;
            float sx = 0.0f;
            float sy = 0.0f;
            if (isDrawn && surface.texture) {
                sx = 1.0f / (float)surface.texture->width();
                sy = 1.0f / (float)surface.texture->height();
            }
            scaleX[material] = sx;
            scaleY[material] = sy;
        }
        const Surface& surface = surfaces[material];
        const float sx = scaleX[material];
        const float sy = scaleY[material];

        // The vertices. The colour is white: the original's Gouraud-blended
        // stream is this vertex's colour, and the world's surfaces are
        // unlit (the palette decides).
        const int32_t first = (int32_t)out->vertices.size();
        for (int32_t k = 0; k < 3; k++) {
            const int32_t index = indices[(size_t)t * 3 + (size_t)k];
            Vertex vertex;
            if (index >= 0 && index < (int32_t)positions.size()) {
                vertex.pos = Vec3{positions[(size_t)index].x,
                                  positions[(size_t)index].y,
                                  positions[(size_t)index].z};
            }
            const size_t uvAt = (size_t)t * 3 + (size_t)k;
            if (uvAt < uvs.size()) {
                vertex.uv = Vec2{uvs[uvAt].x * sx, uvs[uvAt].y * sy};
            }
            vertex.colour = kVertexWhite;
            out->vertices.push_back(vertex);
        }

        if (!haveRun || material != runMaterial) {
            // A new run. The previous one ends where this one starts.
            if (haveRun && !out->batches.empty()) {
                out->batches.back().count = first - out->batches.back().first;
            }
            if (!drawn[material]) {
                // Not drawn: NONE, PEN_ENV, RIPPLE, or a material name that
                // no archive has. The run is left empty, and the next
                // triangle starts a new one.
                haveRun = false;
                continue;
            }
            ArenaBatch batch;
            batch.first = first;
            batch.count = 0;
            batch.surface = surface;
            out->batches.push_back(batch);
            runMaterial = material;
            haveRun = true;
        }
    }

    if (haveRun && !out->batches.empty()) {
        out->batches.back().count = (int32_t)out->vertices.size() - out->batches.back().first;
    }

    // A batch with no triangles is a surface that is not drawn; drop it.
    std::vector<ArenaBatch> kept;
    kept.reserve(out->batches.size());
    for (size_t i = 0; i < out->batches.size(); i++) {
        if (out->batches[i].count > 0) {
            kept.push_back(out->batches[i]);
        }
    }
    out->batches.swap(kept);

    return !out->batches.empty();
}

} // namespace mdk

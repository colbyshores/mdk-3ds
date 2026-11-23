// ============================================================================
// effects_draw.cpp -- see effects_draw.h.
// ============================================================================

#include "game/effects_draw.h"

#include <string.h>

#include "render/surface_cache.h"

namespace mdk {

namespace {

const int32_t kInitialMeshCapacity = 4096;

} // namespace

bool EffectsDraw::ensure(MdkRenderer renderer, int32_t need) {
    if (mesh_ >= 0 && need <= capacity_) {
        failed_ = false;
        attemptedCapacity_ = 0;
        return true;
    }
    if (mesh_ >= 0) {
        mdk_render_release_mesh(renderer, mesh_);
        mesh_ = -1;
        capacity_ = 0;
    }
    int32_t capacity = kInitialMeshCapacity;
    while (capacity < need) {
        capacity *= 2;
    }
    attemptedCapacity_ = capacity;
    mesh_ = mdk_render_create_dynamic_mesh(renderer, capacity);
    if (mesh_ < 0) {
        failed_ = true;
        return false;
    }
    capacity_ = capacity;
    attemptedCapacity_ = 0;
    failed_ = false;
    return true;
}

void EffectsDraw::draw(MdkRenderer renderer, SurfaceCache* surfaces, const EffectsFrame& frame) {
    if (!renderer) {
        return;
    }
    // The halos first, every frame (none clears the last frame's).
    mdk_render_set_glow_quads(renderer, frame.glows.empty() ? nullptr : frame.glows.data(),
                              (int32_t)frame.glows.size());
    if (surfaces == nullptr || frame.draws.empty()) {
        return;
    }
    const int32_t count = (int32_t)frame.vertices.size();
    if (count == 0) {
        return;
    }
    if (!ensure(renderer, count)) {
        return;
    }
    mdk_render_update_mesh(renderer, mesh_, frame.vertices.data(), count);

    // Consecutive draws share their surface (the build batches by it):
    // the cache's answer for the same texture through the same palette
    // is the same id, so it is asked once per run of them, not per draw.
    const formats::Texture* lastTexture = nullptr;
    const formats::Palette* lastPalette = nullptr;
    MdkTextureId lastTextureId = -1;
    MdkPaletteId lastPaletteId = -1;
    for (size_t i = 0; i < frame.draws.size(); i++) {
        const EffectsDrawItem& queued = frame.draws[i];
        if (queued.count <= 0) {
            continue;
        }
        MeshDraw draw;
        draw.mesh = mesh_;
        draw.first_vertex = queued.first;
        draw.vertex_count = queued.count;
        draw.frame = queued.frame;
        draw.primitive = Primitive::Triangles;
        const Mat4 identity = Mat4::identity();
        memcpy(draw.world, identity.m, sizeof(draw.world));
        if (queued.surface.texture != nullptr && queued.surface.texture == lastTexture &&
            queued.palette == lastPalette) {
            draw.material.texture = lastTextureId;
            draw.material.palette = lastPaletteId;
        } else {
            draw.material.texture = surfaces->surfaceId(queued.surface, queued.palette);
            draw.material.palette = surfaces->paletteId(queued.palette);
            lastTexture = queued.surface.texture;
            lastPalette = queued.palette;
            lastTextureId = draw.material.texture;
            lastPaletteId = draw.material.palette;
        }
        draw.material.frame_count = queued.surface.frameCount;
        draw.material.pass = queued.surface.pass;
        draw.material.shading = queued.surface.shading;
        draw.material.row_shift = queued.surface.rowShift;
        // As in ObjectDraw: a flat surface's colour already rides its
        // one-pixel texture, so the material colour is white either way.
        draw.material.colour = Colour{1.0f, 1.0f, 1.0f, 1.0f};
        mdk_render_draw_mesh(renderer, &draw);
    }
}

} // namespace mdk

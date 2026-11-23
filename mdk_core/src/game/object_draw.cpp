// ============================================================================
// object_draw.cpp — see object_draw.h.
// ============================================================================

#include "game/object_draw.h"

#include <string.h>

#include "game/hd_key.h"

namespace mdk {

namespace {

// The stream mesh's first capacity, in vertices. The measured frames need 2559
// (level 3 tick 1) and 5616 (level 8's), so the first doubling covers a level 3
// frame and the second covers the largest measured one.
const int32_t kInitialStreamCapacity = 4096;

} // namespace

MdkTextureId ObjectDraw::imageTexture(MdkRenderer renderer, const HdImage* image) {
    std::map<const HdImage*, MdkTextureId>::const_iterator found = imageTextures_.find(image);
    if (found != imageTextures_.end()) {
        return found->second;
    }
    const MdkTextureId id =
        mdk_render_create_rgba_texture(renderer, image->width, image->height * image->frames,
                                       image->rgba.data());
    imageTextures_[image] = id;
    return id;
}

bool ObjectDraw::ensure(MdkRenderer renderer, int32_t need) {
    if (mesh_ >= 0 && need <= capacity_) {
        // A mesh exists and fits, so the pass is not in a refused state. Defensive
        // rather than load-bearing: a refusal leaves `mesh_` at -1, so the frames
        // after one always take the full path below and clear the flag there. It
        // is written because "failed_ describes the last attempt" is a promise the
        // early return would otherwise be free to break (review-11 F7).
        failed_ = false;
        attemptedCapacity_ = 0;
        return true;
    }
    if (mesh_ >= 0) {
        // The old mesh is released, not destroyed: it may still be an in-flight
        // resource, and each backend frees it at its own completion boundary
        // (the Vulkan backend retires it for two frames, the 3DS one defers the
        // linearFree to the next C3D_FrameBegin). So a frame that outgrows the
        // mesh briefly holds both buffers -- a bounded transient that happens a
        // handful of times, never in a steady state.
        mdk_render_release_mesh(renderer, mesh_);
        mesh_ = -1;
        capacity_ = 0;
    }
    // Geometric growth, so the recreation happens a handful of times at most and
    // never in a steady state. Doubling from kInitialStreamCapacity keeps the
    // capacity a power of two, which is also what the backends round a vertex
    // buffer up to anyway.
    int32_t capacity = kInitialStreamCapacity;
    while (capacity < need) {
        capacity *= 2;
    }
    // The request the backend is about to be given, recorded BEFORE the call:
    // a refusal returns before `capacity_` is assigned, so without this the
    // attempted size is not reachable by the caller at all and the frontend's
    // log line can only guess (review-11 F1).
    attemptedCapacity_ = capacity;
    mesh_ = mdk_render_create_dynamic_mesh(renderer, capacity);
    if (mesh_ < 0) {
        // Recorded, not printed: see allocationFailed(). The backend that refused
        // the allocation prints the memory half of this (renderer_c3d.cpp logs its
        // own linearAlloc failure); this half says that the refusal reached the
        // object pass, which is the difference between "a mesh failed" and "the
        // frame drew no objects, and here is why".
        failed_ = true;
        return false;
    }
    capacity_ = capacity;
    attemptedCapacity_ = 0;
    failed_ = false;
    return true;
}

void ObjectDraw::draw(MdkRenderer renderer, SurfaceCache* surfaces, const ObjectStream& stream) {
    if (!renderer || surfaces == nullptr || stream.draws.empty()) {
        return;
    }
    const int32_t count = (int32_t)stream.vertices.size();
    if (count == 0) {
        return;
    }
    if (!ensure(renderer, count)) {
        return;
    }

    // The whole frame's corners in one upload, exactly as the C#'s QueueStream
    // does before the frame is drawn. The draws below then read slices of it.
    mdk_render_update_mesh(renderer, mesh_, stream.vertices.data(), count);

    // Consecutive draws share their surface where the batching made
    // them: the cache's answer for the same texture through the same
    // palette is the same id (see EffectsDraw::draw).
    const formats::Texture* lastTexture = nullptr;
    const formats::Palette* lastPalette = nullptr;
    MdkTextureId lastTextureId = -1;
    MdkPaletteId lastPaletteId = -1;
    for (size_t i = 0; i < stream.draws.size(); i++) {
        const ObjectStreamDraw& queued = stream.draws[i];
        if (queued.corners <= 0) {
            continue;
        }

        MeshDraw draw;
        draw.mesh = mesh_;
        draw.first_vertex = queued.first;
        draw.vertex_count = queued.corners;
        draw.frame = queued.frame;
        draw.primitive = Primitive::Triangles;
        // Mat4 and MeshDraw::world are both row-major row-vector 4x4s.
        memcpy(draw.world, queued.world.m, sizeof(draw.world));

        if (queued.image != nullptr) {
            // A mod surface's own picture: its RGBA texture, no palette.
            draw.material.texture = imageTexture(renderer, queued.image);
            draw.material.palette = -1;
            lastTexture = nullptr;
            lastPalette = nullptr;
        } else if (queued.surface.texture != nullptr &&
                   queued.surface.texture == lastTexture && queued.palette == lastPalette) {
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
        // The colour is applied EXACTLY ONCE, as in LevelView::draw: a flat
        // surface's colour is already carried by its one-pixel texture, so the
        // material colour is white for both kinds of surface, on both targets.
        draw.material.colour = Colour{1.0f, 1.0f, 1.0f, 1.0f};

        mdk_render_draw_mesh(renderer, &draw);
    }
}

} // namespace mdk

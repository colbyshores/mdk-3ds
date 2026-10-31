// ============================================================================
// object_draw.cpp — see object_draw.h.
// ============================================================================

#include "game/object_draw.h"

#include <string.h>

namespace mdk {

namespace {

// The stream mesh's first capacity, in vertices. The measured frames need 2559
// (level 3 tick 1) and 5616 (level 8's), so the first doubling covers a level 3
// frame and the second covers the largest measured one.
const int32_t kInitialStreamCapacity = 4096;

} // namespace

bool ObjectDraw::ensure(MdkRenderer renderer, int32_t need) {
    if (mesh_ >= 0 && need <= capacity_) {
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
    mesh_ = mdk_render_create_dynamic_mesh(renderer, capacity);
    if (mesh_ < 0) {
        return false;
    }
    capacity_ = capacity;
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

        draw.material.texture = surfaces->surfaceId(queued.surface, queued.palette);
        draw.material.palette = surfaces->paletteId(queued.palette);
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

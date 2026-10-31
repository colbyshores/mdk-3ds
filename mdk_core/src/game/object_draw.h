#pragma once

// ============================================================================
// object_draw.h — the object pass's GPU half.
//
// `ObjectView::addObject` (game/object_view.h) fills a frame's `ObjectStream`:
// the posed corners and one queued draw per batch. This is what the C# does with
// them inside `Renderer`:
//
//   Renderer.QueueStream  -- the stream goes to ONE dynamic mesh, once per frame
//   Renderer.Draw(mesh, first, corners, material, frame, world)
//                          -- one draw per queued batch, at its offset
//
// WHY ONE MESH FOR THE WHOLE FRAME. The C# makes its stream buffer once, at
// renderer construction, and every object's corners are appended into it, so a
// frame's objects are one upload and N draws at offsets. A mesh per object would
// hang a GPU buffer off a script-created object's identity and fragment linear
// memory mid-play on the 3DS, which is the resource the 3DS has least of.
//
// WHY THE BUFFER IS NOT SIZED LIKE THE C#'S. The C# allocates its stream mesh at
// `Renderer.StreamVertices` (1 << 18 vertices, 5.2 MiB) and keeps it for the
// process. On the 3DS that is 5.2 MiB of LINEAR memory -- which libctru hands
// out of a heap measured in single megabytes, and a failed linearAlloc is a
// silently missing mesh, not an error. So the mesh here is made the first time a
// frame has corners to upload and grows geometrically to what the frames
// actually reach (measured on the oracle: 2559 corners at level 3 tick 1, 5616 at
// level 8's). The refusal at kObjectStreamCapacity is the C#'s and stays in
// ObjectView; this is only about how much memory is committed, and it commits
// what the frames use. Growth is rare and stops: a frame after the largest one
// does not allocate at all.
//
// WHAT IS NOT HERE: the pass ORDER, again the C#'s and not this file's. The C#
// buckets its queued commands by Pass and draws the buckets in order
// (Renderer.cs:1004, 1049-1057), and it does it inside `Renderer`, where the
// arena's batches and the objects' batches are one list. So the buckets are the
// backends' job -- render.h: "the material carries the pass, so the backend
// sorts and batches; the caller does not" -- and they are in render/draw_queue.h.
// This file only carries the pass through in the material, as the C#'s
// `Renderer.Draw` receives it.
// ============================================================================

#include <stdint.h>

#include "game/object_view.h"
#include "render/render.h"
#include "render/surface_cache.h"

namespace mdk {

// The frame's streamed corners, on the GPU, drawn.
class ObjectDraw {
public:
    ObjectDraw() = default;

    // Uploads the frame's stream once and issues its queued draws. Call it
    // between mdk_render_begin_frame and mdk_render_end_frame, after every
    // addObject for the frame, and before anything that draws over the scene.
    //
    // Does nothing when the frame has no corners. A frame whose mesh could not
    // be made draws no objects and does not fail: a missing object pass is a
    // missing picture, not a crash.
    //
    // HOT PATH (AGENTS.md 2): once per drawn batch per frame. It is one memcpy
    // of the stream and a Material per draw -- no float->int conversion (R2), no
    // float comparison (R3), no division (R11), no transcendental (R6), no
    // double (R4). The surface-to-texture lookups are map finds on pointers.
    void draw(MdkRenderer renderer, SurfaceCache* surfaces, const ObjectStream& stream);

    // The stream mesh's capacity in vertices, for the diagnostics.
    int32_t capacity() const { return capacity_; }

private:
    // Makes the mesh big enough for `need` vertices, replacing it when it is not.
    bool ensure(MdkRenderer renderer, int32_t need);

    MdkMeshId mesh_ = -1;
    int32_t capacity_ = 0;
};

} // namespace mdk

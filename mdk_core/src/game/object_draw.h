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
    // missing picture, not a crash. It is NOT silent — `allocationFailed()` says
    // so, and the frontend that reads it prints it. Only the 3DS frontend does
    // today (`main_3ds.cpp`); `mdk_linux/src/main.cpp` calls `draw` without
    // consulting the flag and the Vulkan backend's mesh refusal prints nothing
    // either, so a refused mesh on Linux is still a silent missing picture.
    // That half is recorded and NOT fixed: `mdk_linux/**` is outside `AGENTS.md`
    // §8's scope (review-11 F4).
    //
    // HOT PATH (AGENTS.md 2): once per drawn batch per frame. It is one memcpy
    // of the stream and a Material per draw -- no float->int conversion (R2), no
    // float comparison (R3), no division (R11), no transcendental (R6), no
    // double (R4). The surface-to-texture lookups are map finds on pointers.
    void draw(MdkRenderer renderer, SurfaceCache* surfaces, const ObjectStream& stream);

    // The stream mesh's capacity in vertices, for the diagnostics.
    int32_t capacity() const { return capacity_; }

    // True when the LAST attempt to make the mesh failed. The pass cannot report
    // the failure itself — `mdk_core` has no log to write to, and inventing one
    // here would put a frontend's stderr inside the shared game code — so it
    // records it and the frontend that owns the log prints it (review-10 F3).
    // Without this the only line that ever reported an allocation failure was
    // deleted by review-9 F6, and a level whose mesh genuinely fails to allocate
    // reads as nothing at all: no mesh line, no failure line, no objects.
    //
    // It describes the LAST attempt, not the whole run: `ensure` runs again on
    // every frame that has corners and no mesh, so a transient linear-pressure
    // failure clears by itself when a later frame succeeds — and `failed_` is
    // cleared on that success, including on the early return, or a recovered
    // pass would report a refusal forever.
    bool allocationFailed() const { return failed_; }

    // The capacity the LAST refused request asked the backend for, in vertices,
    // and 0 when nothing has been refused. `capacity()` cannot answer this, and not
    // because it might still hold the previous mesh: `ensure` releases that mesh and
    // zeroes `capacity_` BEFORE the request that may be refused (object_draw.cpp),
    // so after a refusal it is exactly 0 — the "no mesh yet" shape review-9 F6 took
    // out of the log because it is indistinguishable from a frame that never asked.
    // What the backend refused is the ROUNDED-UP power of two (`480` corners in a
    // frame asks for `4096`), and only this accessor carries it. A log line that
    // printed `capacity()` or the frame's corner count would report an allocation
    // that was never attempted and disagree with the backend's own byte count
    // (review-11 F1; the "or the OLD mesh's size" alternative this comment used to
    // offer is a state that cannot occur — review-12 R5).
    int32_t attemptedCapacity() const { return attemptedCapacity_; }

private:
    // Makes the mesh big enough for `need` vertices, replacing it when it is not.
    bool ensure(MdkRenderer renderer, int32_t need);

    MdkMeshId mesh_ = -1;
    int32_t capacity_ = 0;
    int32_t attemptedCapacity_ = 0;
    bool failed_ = false;
};

} // namespace mdk

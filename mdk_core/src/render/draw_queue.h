#pragma once

// ============================================================================
// draw_queue.h — the frame's queued mesh draws, in the oracle's ORDER.
//
// `render.h` says the material carries the pass and "the backend sorts and
// batches; the caller does not". This is the sorting, and it is shared because
// the ORDER is the oracle's, not a backend's preference:
//
//     Renderer.cs:227   private static readonly Pass[] Passes = Enum.GetValues<Pass>();
//     Renderer.cs:1000  foreach (var order in Passes) DrawPass(commands, pass, _commands, order, view);
//     Renderer.cs:1049  foreach (ref readonly var command in ...) if (command.Material.Pass == order) DrawOne(...);
//
// The oracle collects every command of the frame into ONE list and then walks
// that list once per pass, drawing the commands of that pass in the order they
// were queued. So every `Solid` batch of the frame is drawn before any
// `DoubleSided`, all of those before any `Mirror`, then `Blended`, `OnTop` and
// `Overlay`. `Pass` in render_types.h is the same enum in the same order, so
// the bucket index IS the pass value.
//
// WHY THIS MATTERS AND IS NOT COSMETIC. The original has no depth sort of
// transparent geometry: glass is drawn after the opaque world because it is in
// a later bucket, and a mirror shows what the opaque world had already drawn.
// Recording draws as they arrive instead -- which is what both backends did --
// puts an object's `Blended` batch wherever its script happened to be created,
// in front of arena glass that had not been drawn yet. Non-`Solid` batches are
// in 10 of level 3's 17 arenas, so this is not a corner case.
//
// WHY IT IS A QUEUE AND NOT A SORT. The oracle does not sort by anything; it
// filters, and filtering preserves queue order. Sorting by pass with any
// tie-breaking (depth, texture, age) would be a different, invented order.
//
// WHAT THE QUEUE DOES NOT DO: it does not batch by texture, and it does not
// merge anything. The C# does not either -- `DrawOne` binds a pipeline and
// issues one draw per command (Renderer.cs:1060-1110).
// ============================================================================

#include <stdint.h>
#include <vector>

#include "render/render_types.h"

namespace mdk {

// The number of buckets, which is the number of values `Pass` has.
const int32_t kDrawQueuePassCount = 6;

// The frame's queued draws, bucketed by pass.
//
// One instance lives in each backend's renderer. The backend queues in
// `mdk_render_draw_mesh` and flushes at the end of the 3D scene -- before the
// 2D canvas, before an inset's viewport changes, and at the frame's end -- so
// the scene's buckets are always drawn before anything that draws over them.
class DrawQueue {
public:
    DrawQueue();

    // Queue one draw. The record is copied: the caller's `MeshDraw` is a
    // temporary.
    void add(const MeshDraw& draw);

    // Forget the frame's draws. Capacity is KEPT: the queue is reused every
    // frame and per-frame growth is what AGENTS.md 10 forbids in a hot path.
    void reset();

    bool empty() const { return total_ == 0; }
    int32_t total() const { return total_; }

    // The draws queued for one pass, in queue order. `pass` is a `Pass` value
    // as an integer; an out-of-range index gives an empty bucket.
    const std::vector<MeshDraw>& bucket(int32_t pass) const;

private:
    std::vector<MeshDraw> buckets_[kDrawQueuePassCount];
    int32_t total_ = 0;
};

} // namespace mdk

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

// The number of buckets, which is the number of values `Pass` has. Derived from
// the enum's own sentinel rather than hand-counted. With a literal 6 here a
// seventh pass would still have been accepted by the enum, and `add()` below
// clamps an out-of-range bucket to 0 rather than dropping the draw -- so what
// the literal would have done is fold the new pass's draws INTO the `Solid`
// bucket: drawn in the wrong order, with the opaque blend and depth state, and
// with no diagnostic anywhere. Deriving the count gives a new pass its own
// bucket, and the `static_assert`s in `pica_matrix_tests.cpp` are what turn
// "every pass is handled" into a build failure (review-4 F1; review-5 F3 for
// this comment's earlier claim that the draw is rejected or voided, which
// `add()` deliberately never does).
const int32_t kDrawQueuePassCount = static_cast<int32_t>(Pass::Count);

// The frame's queued draws, bucketed by pass.
//
// One instance lives in each backend's renderer, and the backend queues in
// `mdk_render_draw_mesh`. WHERE it submits is the backend's own business and the
// two backends differ, so this header deliberately names no flush point (the
// sentence it used to carry -- "flushes before the 2D canvas, before an inset's
// viewport changes, and at the frame's end" -- is true of Vulkan and FALSE of
// Citro3D, where a mid-frame flush is a black screen; review-2 D and review-3
// F6). `render.h` states the seam for both backends and is the file a caller
// reads: a caller may NOT assume a submission at any seam except
// `mdk_render_end_frame`.
//
// What is shared, and is the reason this class is shared code, is the ORDER:
// every `Solid`, then `DoubleSided`, `Mirror`, `Blended`, `OnTop`, `Overlay`,
// each bucket in queue order, because that order is the oracle's and not a
// backend's.
class DrawQueue {
public:
    DrawQueue();

    // Queue one draw. The record is copied: the caller's `MeshDraw` is a
    // temporary.
    void add(const MeshDraw& draw);

    // Forget the frame's draws. Capacity is KEPT: the queue is reused every
    // frame and per-frame growth is what AGENTS.md 10 forbids in a hot path.
    void reset();

    // Capacity up front: `solid` draws for `Solid`, `other` for each other
    // pass (never less than the constructor's). A backend whose queue lives
    // for the whole run calls it once at its creation, so a bucket's first
    // growth is not made in the middle of a level, where the block would
    // stay for good among the level's own (mdk_3ds 02_lifecycle.inc).
    void reserve(int32_t solid, int32_t other);

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

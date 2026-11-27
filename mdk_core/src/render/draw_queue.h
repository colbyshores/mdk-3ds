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
//
// THE CAMERA'S ARENA OVER THE OTHER ONE (upstream e1eb36bb, the original's
// 0x41df35). While the other drawn arena may lie in front of the camera's
// (game/arena_views.cpp), its batches and objects are marked `back` and go into
// their own set of buckets; the camera arena's batches it may hide are marked
// `cover` and stay in theirs (render_types.h MeshDraw). A backend then draws the
// frame so that the camera's arena shows wherever its opaque (Solid,
// DoubleSided) cover draws are drawn, and the depth buffer decides everything
// else: the original's picture at full depth precision, where upstream split the
// depth range into two bands. The two backends get there two ways, to one picture:
//
//   Vulkan (12_draw.inc; no stencil in its depth buffer)
//     1. every back bucket, in pass order
//     2. each opaque cover draw again, its depth only, at the far plane: the
//        other arena's depth under it gives way
//     3. every bucket, in pass order, as before
//   Citro3D (06_queue.inc; DEPTH24_STENCIL8, and no draw added on the Old 3DS)
//     1. the opaque cover draws, marking the stencil where they draw
//     2. every back bucket, in pass order, only where the stencil is unmarked
//     3. every bucket, in pass order, without the cover draws of step 1
//
// Without back draws only step 3 runs, and the frame is drawn as before. The
// marks are made for the frame's camera: a sniper round camera's inset replays
// them, and from its eye the box argument does not hold (both backends alike).
// Not like upstream: the other arena's effects (sparks, debris, ropes) are not
// marked, and Kurt and the camera arena's objects are depth-tested against the
// other arena's geometry as before (upstream draws them over it).
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

    bool empty() const { return total_ == 0; }
    int32_t total() const { return total_; }

    // The draws queued for one pass, in queue order. `pass` is a `Pass` value
    // as an integer; an out-of-range index gives an empty bucket. A `back` draw
    // is not here: it is in backBucket.
    const std::vector<MeshDraw>& bucket(int32_t pass) const;

    // The `back` draws of one pass, and whether there are any (the steps above
    // run only then).
    const std::vector<MeshDraw>& backBucket(int32_t pass) const;
    bool hasBack() const { return backTotal_ > 0; }

private:
    std::vector<MeshDraw> buckets_[kDrawQueuePassCount];
    std::vector<MeshDraw> backBuckets_[kDrawQueuePassCount];
    int32_t total_ = 0;
    int32_t backTotal_ = 0;
};

} // namespace mdk

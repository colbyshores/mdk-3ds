#pragma once

// ============================================================================
// bsp.h — collision against one arena's BSP, as the original does it.
//
// A 1:1 port of src/Mdk.Game/Collision/Bsp.cs (godot-mdk docs/bsp.md): a box
// swept with sliding (bsp_sweep_box 0x4093e0) and a segment (0x421680,
// 0x421708). Triangles flagged 0x20 (NotSolid) are skipped.
//
//   node: plane n·p + d ── front list (faces d >= 0), back list (faces d < 0)
//         ├─ negative child (d < 0)
//         └─ positive child (d >= 0)          -1 = empty leaf
//
// The sweep keeps its state in members exactly as the C# does; it is used one
// arena at a time, single-threaded, within a step.
// ============================================================================

#include <stdint.h>

#include "formats/arena.h"
#include "game/vec_util.h"

namespace mdk {

class Bsp {
public:
    static const uint32_t kNotSolid = 0x20;
    static const int kNone = -1;

    // Which planes and faces a segment test looks at.
    enum SegmentMode {
        kSegmentAny,   // every plane, both faces (0x421680)
        kSegmentFloor, // planes with |nz| >= 0.5, up-facing triangles only (0x421708)
    };

    // Whether a segment test stops at the triangles that only stop Kurt
    // (Arena.ClipFlag): the scripts' rays pass them.
    enum Clip { kClipStops, kClipPassesThrough };

    explicit Bsp(const formats::Arena* arena) : arena_(arena) {}

    const formats::Arena* arena() const { return arena_; }

    // Sweeps a box of half extents h from a to b (centres) in up to iterations
    // + 1 passes, sliding along faces met obliquely enough that
    // (D·n)^2 <= slideK·|D|^2. Returns the triangle of the last pass's contact
    // (kNone when it ended free) and writes where the box ends and the node.
    //
    // firstNode/firstTriangle (optional) report the FIRST contact across all
    // passes -- the C# Bsp.SweepBox's contacts callback's first invocation, which
    // ObjectMotion.Sweep keeps as its first-contact normal. The returned node is
    // the STOPPING pass's contact (what Kurt's move wants); a slide that ends free
    // returns kNone there even though an earlier pass hit, so footing uses the
    // first contact instead. Kurt passes neither and is unaffected.
    int sweepBox(Vec a, Vec b, Vec h, int iterations, float slideK, Vec* end, int* node,
                 int* firstNode = nullptr, int* firstTriangle = nullptr);

    // The nearest triangle a segment crosses (front-to-back walk 0x421470), or
    // kNone. Writes the hit point.
    int segment(Vec a, Vec b, SegmentMode mode, Vec* point, Clip clip = kClipStops);

    // The plane of the node whose list holds the triangle.
    const formats::BspNode& planeOf(int triangle) const;

private:
    void sweepNode(int index);
    int sides(float distance) const;
    void testPlane(int index, const formats::BspNode& n, float dA, float r);
    void slide(const formats::BspNode& n, float t, float den, float r);
    int testTriangles(Vec p, const formats::BspNode& n, int first, int count);
    int walk(int index, Vec a, Vec b, SegmentMode mode, Vec* point);
    void crossingTriangle(const formats::BspNode& n, Vec a, Vec b, float dA, SegmentMode mode,
                          int* triangle, Vec* point);
    int pointTriangle(Vec x, const formats::BspNode& n, int first, int count) const;
    int nodeOf(int triangle) const;

    const formats::Arena* arena_;

    // Sweep state (the original keeps it in globals 0x4d4e30-0x4d4eb4).
    Vec _a, _b, _d, _d0, _h, _p, _s;
    float _margin = 0.0f, _slideLimit = 0.0f, _tBest = 0.0f;
    bool _canSlide = false, _slide = false;
    // The C# fields default to 0 (a valid triangle index), not None; sweepBox
    // resets them each pass, so this only shows for the unreachable
    // iterations < 0 on a fresh Bsp. Match the oracle exactly.
    int _hitTriangle = 0, _hitNode = 0;
    uint32_t _passed = kNotSolid;
};

} // namespace mdk

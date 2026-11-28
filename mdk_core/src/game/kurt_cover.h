#pragma once

// ============================================================================
// kurt_cover.h — the surfaces Kurt's sprite is drawn over.
//
// A FORK DIVERGENCE FROM UPSTREAM (docs/DIVERGENCES.md D8, "Kurt's sprite is
// hidden only by surfaces between the camera and him"; owner, 2026-11-27).
// Upstream's C++ (origin/main src/game/kurt/KurtSprite.cpp FloorDepth) and its
// C# before it are what this departs from.
//
// THE ORIGINAL HAS NO DEPTH BUFFER (docs/original-engine.md, Rendering): the
// arena comes back to front out of a walk of its BSP tree and an object is drawn
// in its place in that walk, so the surfaces of the space Kurt stands in -- the
// floor under him, the walls he walks along, the edge of a step he came down --
// are drawn BEFORE him and never cut his sprite. Upstream (and this port, until
// this change) depth-tests the sprite at his feet and pulls it in front of
// floors at most half a unit above them (FloorDepth, FloorBelow = 0.5): a wall
// he walks along hid his gun arm, a ledge his legs (docs/WORK_LEDGER.md
// 2026-11-27, fix/kurt-sprite-depth).
//
// THE RULE, per drawn arena triangle, decided by the triangle's own BSP plane
// (a triangle lies on its node's plane, formats/arena.h): the sprite is drawn
// over the triangle when the plane leaves the camera and Kurt's ordering point
// on the SAME side -- the BSP order, node by node, puts such a face before the
// side of it both stand on. A face whose plane has the camera on one side and
// Kurt on the other stands between them; it is NOT marked, and hides him
// wherever the depth test says it is in front, as before. The ordering point is
// the centre of his walking collision box (feet + 3.0: ArenaSpace's kWalkBox.z
// 2.5 + kWalkLift 0.5), never his feet, so the floor he stands on and a step of
// less than half his height are on his side; a ledge taller than that, a pillar,
// another room's wall are not.
//
// WHY NOT THE WHOLE TREE WALK. The full painter's order decides a face by the
// FIRST splitting plane that separates it from Kurt, and the arenas' splitting
// planes run through the whole subtree: one of them often passes between Kurt
// and the wall he walks along, with the camera on the wall's side, and that
// wall then covers him in the model of the original too (measured over the
// 36 scripted walks and 3000 random starts, L3-L8: 1701 of the 20036 frames his
// own space cut would still be cut). The owner's requirement is that the
// surfaces of his own space never cut him; the face's own plane is the part of
// the walk that says so.
//
// WHAT THIS CLASS DOES. Per frame it answers the triangles of the drawn arenas
// that (1) are drawn (not skipped, group not hidden, front-facing for the Solid
// pass), (2) lie in the pyramid from the eye(s) through the sprite's rectangle,
// cut at the sprite's depth, and (3) pass the rule. The backends draw them into
// the stencil (render_types.h KurtPass::Mark) and the sprite over the marked
// pixels. The search is a box tree over the arena scene's own triangles (the
// positions the renderer draws), built the first time an arena is drawn with
// Kurt, a slice a frame, and kept while it stays drawn.
// ============================================================================

#include <stdint.h>

#include <vector>

#include "formats/arena.h"
#include "render/arena_scene.h"
#include "render/render_types.h"

namespace mdk {

class KurtCover {
public:
    // A drawn arena: its format (the groups' state; null: no group is hidden)
    // and the scene its view draws.
    struct Source {
        const formats::Arena* arena = nullptr;
        const ArenaScene* scene = nullptr;
    };

    // What the sprite looks like from the camera this frame.
    struct View {
        Vec3 eye{};
        Vec3 forward{};
        // Half the stereo eyes' distance: the pyramid is widened to hold both
        // eyes' (collect).
        float spread = 0.0f;
        // The sprite's rectangle in the world, at its depth: four corners in
        // order around it.
        Vec3 corners[4];
        // Kurt's ordering point (feet + kOrderHeight).
        Vec3 order{};
    };

    // A run of collected triangles of one depth layer (arena_scene.h
    // ArenaBatch::depthLayer), glass or not: their vertices are
    // out[first .. first+count).
    struct Run {
        bool glass = false;
        int32_t layer = 0;
        int32_t first = 0;
        int32_t count = 0;
    };

    struct Stats {
        int32_t triangles = 0;   // collected last frame
        int32_t dropped = 0;     // left out last frame: over the capacity
        int32_t tested = 0;      // triangles tested last frame
        int32_t builds = 0;      // box trees begun since start
        int32_t buildWork = 0;   // their build's triangles touched since start
    };

    // The ordering point's height over his feet: the centre of the walking
    // collision box (game/arena_space.cpp kWalkBox.z + kWalkLift).
    static constexpr float kOrderHeight = 3.0f;

    // Collect the triangles of `sources` Kurt is drawn over, at most
    // `maxTriangles`, into `out` (3 vertices each), grouped into runs by depth
    // layer (at most `maxRuns`). Returns the number of runs written. Box trees
    // of arenas not in `sources` are let go.
    int32_t collect(const Source* sources, int32_t sourceCount, const View& view, Vertex* out,
                    int32_t maxTriangles, Run* runs, int32_t maxRuns);

    // Drop every box tree (a level change).
    void clear() { trees_.clear(); }

    const Stats& stats() const { return stats_; }

private:
    // THE BOX TREE: the scene's triangles in Morton order of their centres,
    // eight to a leaf, under a complete binary tree of boxes (node 1 the root,
    // node i's children 2i and 2i+1, leaf j at node `leafBase` + j). Built in a
    // time linear in the triangles, a slice a frame (step): the median-split
    // tree it replaced cost ~8 ms in one frame for LEVEL3's HMO_1 on the Old 3DS.
    struct Node {
        float lo[3];
        float hi[3];
    };
    enum class Phase : uint8_t { Codes, Count, Scatter, Leaves, Inner, Done };
    struct Tree {
        const formats::Arena* arena = nullptr;
        const ArenaScene* scene = nullptr;
        const Vertex* data = nullptr;
        size_t vertices = 0;
        size_t batches = 0;
        std::vector<Node> nodes;         // 2 * leafBase; node 0 unused
        std::vector<int32_t> order;      // scene triangle indices, leaf by leaf
        int32_t triangles = 0;
        int32_t leaves = 0;
        int32_t leafBase = 1;            // a power of two >= leaves
        // The build in progress: the Morton codes and the radix sort's other
        // buffer (let go when done), the phase, its cursor and radix pass.
        std::vector<uint32_t> codes;
        std::vector<int32_t> spare;
        int32_t counts[1024] = {};
        float origin[3] = {0.0f, 0.0f, 0.0f};
        float scaleToGrid[3] = {0.0f, 0.0f, 0.0f};
        Phase phase = Phase::Codes;
        int32_t cursor = 0;
        int32_t pass = 0;
        bool ready = false;
        bool used = false;
    };
    // The pyramid: four sides (0-3), the sprite's depth (4), the eye's (5);
    // inside is n.p + d >= 0.
    struct Planes {
        float n[6][3];
        float d[6];
    };
    struct Candidate {
        int32_t triangle = 0;
        int32_t layer = 0;
        bool glass = false;
        const Tree* tree = nullptr;
    };

    static void begin(Tree* tree);
    bool step(Tree* tree, int32_t budget);
    static uint32_t mortonOf(const Tree& tree, const Vec3& centreSum);
    static int32_t clipToSides(const Planes& planes, const Vec3& a, const Vec3& b, const Vec3& c,
                               Vec3* out);
    void search(const Tree& tree, const Planes& planes, const View& view, int32_t maxTriangles);

    std::vector<Tree> trees_;
    std::vector<Candidate> found_;
    std::vector<int32_t> stack_;
    Stats stats_;
};

} // namespace mdk

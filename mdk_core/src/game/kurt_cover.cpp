// ============================================================================
// kurt_cover.cpp — the surfaces Kurt's sprite is drawn over. See kurt_cover.h
// (and docs/DIVERGENCES.md D8 for why this port departs from upstream here).
// ============================================================================

#include "game/kurt_cover.h"

#include <math.h>
#include <string.h>


namespace mdk {

namespace {

// The most corners a triangle cut to the pyramid keeps (KurtCover::clipToSides).
const int32_t kClipCorners = 12;

// A leaf holds at most this many triangles (1 << kLeafShift).
const int32_t kLeafShift = 3;
const int32_t kLeafTriangles = 1 << kLeafShift;

// The box tree's build, a slice a frame: this many steps (a triangle coded or
// sorted, a leaf triangle boxed, an inner box) -- KurtCover::step.
const int32_t kBuildBudget = 3000;

// The sprite's rectangle is grown by this share of its size and this many world
// units before the pyramid is made: a triangle that only touches its edge is in.
const float kGrowShare = 0.05f;
const float kGrowUnits = 0.1f;

// The pyramid is cut this far beyond the sprite's depth (the feet's rows are
// pulled nearer, never farther), and this near the eye.
const float kFarMargin = 0.05f;
const float kNear = 0.01f;

inline float dot3(const float* n, const Vec3& p) {
    return n[0] * p.x + n[1] * p.y + n[2] * p.z;
}

inline Vec3 sub(const Vec3& a, const Vec3& b) {
    return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}

inline Vec3 crossV(const Vec3& a, const Vec3& b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline float dotV(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

// The planes of the pyramid from `apex` through the rectangle `q` (inside: the
// rectangle's centre), and the depth window (measured from `eye`).
void pyramid(const Vec3& apex, const Vec3* q, const Vec3& centre, const Vec3& eye,
             const Vec3& forward, float farDepth, float out[6][3], float d[6]) {
    for (int32_t i = 0; i < 4; i++) {
        Vec3 n = crossV(sub(q[i], apex), sub(q[(i + 1) % 4], apex));
        if (dotV(n, sub(centre, apex)) < 0.0f) {
            n = Vec3{-n.x, -n.y, -n.z};
        }
        out[i][0] = n.x;
        out[i][1] = n.y;
        out[i][2] = n.z;
        d[i] = -dotV(n, apex);
    }
    // Beyond the sprite: depth <= farDepth.
    out[4][0] = -forward.x;
    out[4][1] = -forward.y;
    out[4][2] = -forward.z;
    d[4] = dotV(forward, eye) + farDepth;
    // In front of the eye: depth >= kNear.
    out[5][0] = forward.x;
    out[5][1] = forward.y;
    out[5][2] = forward.z;
    d[5] = -(dotV(forward, eye) + kNear);
}

// Whether the box is wholly outside one of the six planes.
bool boxOutside(const float n[6][3], const float d[6], const float* lo, const float* hi) {
    for (int32_t i = 0; i < 6; i++) {
        const Vec3 far{n[i][0] >= 0.0f ? hi[0] : lo[0], n[i][1] >= 0.0f ? hi[1] : lo[1],
                       n[i][2] >= 0.0f ? hi[2] : lo[2]};
        if (dot3(n[i], far) + d[i] < 0.0f) {
            return true;
        }
    }
    return false;
}

// Whether the triangle is wholly outside one of the six planes.
bool triangleOutside(const float n[6][3], const float d[6], const Vec3& a, const Vec3& b,
                     const Vec3& c) {
    for (int32_t i = 0; i < 6; i++) {
        if (dot3(n[i], a) + d[i] < 0.0f && dot3(n[i], b) + d[i] < 0.0f &&
            dot3(n[i], c) + d[i] < 0.0f) {
            return true;
        }
    }
    return false;
}

} // namespace

int32_t KurtCover::clipToSides(const Planes& planes, const Vec3& a, const Vec3& b, const Vec3& c,
                               Vec3* out) {
    // Sutherland-Hodgman against the four side planes (0-3): 3 + 4 corners for a
    // convex cut; rounding on a sliver can add more, so a pass keeps at most
    // kClipCorners (out holds as many).
    Vec3 bufA[kClipCorners];
    Vec3 bufB[kClipCorners];
    Vec3* in = bufA;
    Vec3* next = bufB;
    int32_t n = 3;
    in[0] = a;
    in[1] = b;
    in[2] = c;
    /* @vfp-exempt R3 — Sutherland-Hodgman is a sequence of float side tests: at most four planes of a handful of corners per marked triangle. measured inside Kurt's draw (~360 us a frame, Old 3DS). Verified 2026-11-27. */
    for (int32_t p = 0; p < 4 && n > 0; p++) {
        const float* nn = planes.n[p];
        const float d = planes.d[p];
        int32_t m = 0;
        for (int32_t i = 0; i < n; i++) {
            const Vec3& s = in[i];
            const Vec3& e = in[i + 1 == n ? 0 : i + 1];
            const float ds = dot3(nn, s) + d;
            const float de = dot3(nn, e) + d;
            if (ds >= 0.0f && m < kClipCorners) {
                next[m++] = s;
            }
            if ((ds >= 0.0f) != (de >= 0.0f) && m < kClipCorners) {
                const float t = ds / (ds - de);
                next[m++] = Vec3{s.x + (e.x - s.x) * t, s.y + (e.y - s.y) * t,
                                 s.z + (e.z - s.z) * t};
            }
        }
        Vec3* swap = in;
        in = next;
        next = swap;
        n = m;
    }
    for (int32_t i = 0; i < n; i++) {
        out[i] = in[i];
    }
    return n;
}

void KurtCover::begin(Tree* tree) {
    const int32_t triangles = (int32_t)(tree->scene->vertices.size() / 3);
    tree->triangles = triangles;
    tree->leaves = (triangles + kLeafTriangles - 1) >> kLeafShift;
    tree->leafBase = 1;
    while (tree->leafBase < tree->leaves) {
        tree->leafBase *= 2;
    }
    tree->nodes.assign((size_t)tree->leafBase * 2, Node());
    tree->order.assign((size_t)triangles, 0);
    tree->spare.assign((size_t)triangles, 0);
    tree->codes.assign((size_t)triangles, 0u);
    tree->phase = triangles > 0 ? Phase::Codes : Phase::Done;
    tree->cursor = 0;
    tree->pass = 0;
    tree->ready = triangles == 0;
    // The grid the centres are coded on: the scene's box (its batches' boxes),
    // 1024 cells a side.
    float lo[3] = {1e30f, 1e30f, 1e30f};
    float hi[3] = {-1e30f, -1e30f, -1e30f};
    const std::vector<ArenaBatch>& batches = tree->scene->batches;
    for (size_t b = 0; b < batches.size(); b++) {
        for (int32_t j = 0; j < 3; j++) {
            lo[j] = batches[b].boundsMin[j] < lo[j] ? batches[b].boundsMin[j] : lo[j];
            hi[j] = batches[b].boundsMax[j] > hi[j] ? batches[b].boundsMax[j] : hi[j];
        }
    }
    for (int32_t j = 0; j < 3; j++) {
        const float extent = hi[j] > lo[j] ? hi[j] - lo[j] : 1.0f;
        tree->origin[j] = lo[j] * 3.0f;              // the codes take the corners' SUM
        tree->scaleToGrid[j] = 1023.0f / (extent * 3.0f);
    }
}

uint32_t KurtCover::mortonOf(const Tree& tree, const Vec3& sum) {
    const float xyz[3] = {sum.x, sum.y, sum.z};
    uint32_t code = 0;
    uint32_t cell[3];
    for (int32_t j = 0; j < 3; j++) {
        /* @vfp-exempt R2 — a Morton cell is an integer by definition: one conversion per centre axis while a box tree is built (a few thousand a frame, a slice at a time), never in play once built. measured Old 3DS: first slice ~0.7 ms over the baseline frame. Verified 2026-11-27. */
        float g = (xyz[j] - tree.origin[j]) * tree.scaleToGrid[j];
        g = g < 0.0f ? 0.0f : (g > 1023.0f ? 1023.0f : g);
        cell[j] = (uint32_t)g;
    }
    for (int32_t bit = 9; bit >= 0; bit--) {
        for (int32_t j = 0; j < 3; j++) {
            code = (code << 1) | ((cell[j] >> bit) & 1u);
        }
    }
    return code;
}

bool KurtCover::step(Tree* tree, int32_t budget) {
    const std::vector<Vertex>& v = tree->scene->vertices;
    const int32_t n = tree->triangles;
    int32_t work = 0;
    while (tree->phase != Phase::Done && work < budget) {
        switch (tree->phase) {
        case Phase::Codes: {
            // Each triangle's Morton code; the order starts as the scene's.
            const int32_t t = tree->cursor++;
            const Vec3& a = v[(size_t)t * 3].pos;
            const Vec3& b = v[(size_t)t * 3 + 1].pos;
            const Vec3& c = v[(size_t)t * 3 + 2].pos;
            tree->codes[(size_t)t] = mortonOf(*tree, Vec3{a.x + b.x + c.x, a.y + b.y + c.y,
                                                          a.z + b.z + c.z});
            tree->order[(size_t)t] = t;
            work++;
            if (tree->cursor == n) {
                tree->phase = Phase::Count;
                tree->cursor = 0;
                tree->pass = 0;
                memset(tree->counts, 0, sizeof(tree->counts));
            }
            break;
        }
        case Phase::Count: {
            // A radix pass of ten bits (three passes): the histogram ...
            const uint32_t code = tree->codes[(size_t)tree->order[(size_t)tree->cursor++]];
            tree->counts[(code >> (tree->pass * 10)) & 1023u]++;
            work++;
            if (tree->cursor == n) {
                int32_t at = 0;
                for (int32_t k = 0; k < 1024; k++) {
                    const int32_t c = tree->counts[k];
                    tree->counts[k] = at;
                    at += c;
                }
                tree->phase = Phase::Scatter;
                tree->cursor = 0;
            }
            break;
        }
        case Phase::Scatter: {
            // ... and the stable scatter into the other buffer.
            const int32_t t = tree->order[(size_t)tree->cursor++];
            const uint32_t digit = (tree->codes[(size_t)t] >> (tree->pass * 10)) & 1023u;
            tree->spare[(size_t)tree->counts[digit]++] = t;
            work++;
            if (tree->cursor == n) {
                tree->order.swap(tree->spare);
                tree->cursor = 0;
                tree->pass++;
                if (tree->pass < 3) {
                    tree->phase = Phase::Count;
                    memset(tree->counts, 0, sizeof(tree->counts));
                } else {
                    tree->phase = Phase::Leaves;
                }
            }
            break;
        }
        case Phase::Leaves: {
            // A leaf's box: its eight triangles' corners.
            const int32_t leaf = tree->cursor++;
            Node& node = tree->nodes[(size_t)(tree->leafBase + leaf)];
            for (int32_t j = 0; j < 3; j++) {
                node.lo[j] = 1e30f;
                node.hi[j] = -1e30f;
            }
            const int32_t first = leaf * kLeafTriangles;
            const int32_t last = first + kLeafTriangles < n ? first + kLeafTriangles : n;
            for (int32_t i = first; i < last; i++) {
                const int32_t t = tree->order[(size_t)i];
                for (int32_t k = 0; k < 3; k++) {
                    const Vec3& q = v[(size_t)t * 3 + (size_t)k].pos;
                    const float xyz[3] = {q.x, q.y, q.z};
                    for (int32_t j = 0; j < 3; j++) {
                        node.lo[j] = xyz[j] < node.lo[j] ? xyz[j] : node.lo[j];
                        node.hi[j] = xyz[j] > node.hi[j] ? xyz[j] : node.hi[j];
                    }
                }
            }
            work += last - first;
            if (tree->cursor == tree->leaves) {
                // The leaves past the last are empty boxes (lo > hi): never in.
                for (int32_t k = tree->leaves; k < tree->leafBase; k++) {
                    for (int32_t j = 0; j < 3; j++) {
                        tree->nodes[(size_t)(tree->leafBase + k)].lo[j] = 1e30f;
                        tree->nodes[(size_t)(tree->leafBase + k)].hi[j] = -1e30f;
                    }
                }
                // One leaf is the whole tree (node 1); otherwise the inner boxes.
                tree->phase = tree->leafBase > 1 ? Phase::Inner : Phase::Done;
                tree->cursor = tree->leafBase - 1;
            }
            break;
        }
        case Phase::Inner: {
            // An inner box: its two children's, from the bottom up.
            const int32_t i = tree->cursor--;
            Node& node = tree->nodes[(size_t)i];
            const Node& l = tree->nodes[(size_t)(2 * i)];
            const Node& r = tree->nodes[(size_t)(2 * i + 1)];
            for (int32_t j = 0; j < 3; j++) {
                node.lo[j] = l.lo[j] < r.lo[j] ? l.lo[j] : r.lo[j];
                node.hi[j] = l.hi[j] > r.hi[j] ? l.hi[j] : r.hi[j];
            }
            work++;
            if (tree->cursor == 0) {
                tree->phase = Phase::Done;
            }
            break;
        }
        case Phase::Done:
            break;
        }
    }
    stats_.buildWork += work;
    if (tree->phase == Phase::Done && !tree->ready) {
        std::vector<uint32_t>().swap(tree->codes);
        std::vector<int32_t>().swap(tree->spare);
        tree->ready = true;
    }
    return tree->ready;
}

void KurtCover::search(const Tree& tree, const Planes& planes, const View& view,
                       int32_t maxTriangles) {
    if (tree.triangles == 0) {
        return;
    }
    const std::vector<Vertex>& v = tree.scene->vertices;
    const std::vector<ArenaBatch>& batches = tree.scene->batches;
    stack_.clear();
    stack_.push_back(1);
    while (!stack_.empty()) {
        const int32_t index = stack_.back();
        stack_.pop_back();
        const Node& node = tree.nodes[(size_t)index];
        /* @vfp-exempt R3 — a frustum cull and a plane-side test ARE float predicates (as 03_shadows.inc's sun cull): the box, the triangle and the rule's camera/Kurt side tests decide what is marked. measured Old 3DS: Kurt's draw 51 -> ~360 us a frame with ~48 triangles tested. Verified 2026-11-27. */
        if (node.lo[0] > node.hi[0] || boxOutside(planes.n, planes.d, node.lo, node.hi)) {
            continue;   // empty, or wholly outside the pyramid
        }
        if (index < tree.leafBase) {
            stack_.push_back(2 * index);
            stack_.push_back(2 * index + 1);
            continue;
        }
        const int32_t first = (index - tree.leafBase) * kLeafTriangles;
        const int32_t last =
            first + kLeafTriangles < tree.triangles ? first + kLeafTriangles : tree.triangles;
        for (int32_t i = first; i < last; i++) {
            const int32_t t = tree.order[(size_t)i];
            // Its batch: the runs are in vertex order, so a binary search over the
            // arena's ~20 batches (no per-triangle table kept).
            const int32_t vertex = t * 3;
            size_t lo = 0;
            size_t hi = batches.size();
            while (hi - lo > 1) {
                const size_t mid = (lo + hi) / 2;
                if (batches[mid].first <= vertex) {
                    lo = mid;
                } else {
                    hi = mid;
                }
            }
            if (batches.empty() || vertex < batches[lo].first ||
                vertex >= batches[lo].first + batches[lo].count) {
                continue;
            }
            const ArenaBatch& batch = batches[lo];
            // arenaBatchVisible: a group the scripts hid is not drawn.
            if (tree.arena != nullptr &&
                (tree.arena->groupState(batch.group) & formats::Arena::kGroupHidden) != 0) {
                continue;
            }
            const Vec3& a = v[(size_t)t * 3].pos;
            const Vec3& bb = v[(size_t)t * 3 + 1].pos;
            const Vec3& c = v[(size_t)t * 3 + 2].pos;
            stats_.tested++;
            if (triangleOutside(planes.n, planes.d, a, bb, c)) {
                continue;
            }
            // The triangle's plane. Its winding normal points AWAY from the side
            // it is drawn from (a front face has the eye on its negative side:
            // the renderers cull the other winding, the Solid pass only).
            const Vec3 n = crossV(sub(bb, a), sub(c, a));
            const float sideEye = dotV(n, sub(view.eye, a));
            const float sideKurt = dotV(n, sub(view.order, a));
            if (sideEye == 0.0f) {
                continue;   // edge-on: draws nothing
            }
            if (batch.surface.pass == Pass::Solid && sideEye > 0.0f) {
                continue;   // a back face: culled, not in the depth buffer
            }
            // THE RULE (kurt_cover.h): the camera and Kurt on the same side of
            // the plane -- drawn before him, so he is drawn over it.
            if ((sideEye > 0.0f) != (sideKurt > 0.0f)) {
                continue;
            }
            if ((int32_t)found_.size() >= maxTriangles) {
                stats_.dropped++;
                continue;
            }
            Candidate cand;
            cand.triangle = t;
            cand.layer = batch.depthLayer;
            cand.glass = batch.surface.pass == Pass::Blended;
            cand.tree = &tree;
            found_.push_back(cand);
        }
    }
}

int32_t KurtCover::collect(const Source* sources, int32_t sourceCount, const View& view,
                           Vertex* out, int32_t maxTriangles, Run* runs, int32_t maxRuns) {
    stats_.triangles = 0;
    stats_.dropped = 0;
    stats_.tested = 0;
    found_.clear();
    if (found_.capacity() < (size_t)maxTriangles) {
        found_.reserve((size_t)maxTriangles);
    }
    for (size_t i = 0; i < trees_.size(); i++) {
        trees_[i].used = false;
    }
    if (out == nullptr || runs == nullptr || maxTriangles <= 0 || maxRuns <= 0) {
        return 0;
    }

    // THE PYRAMID. One for both stereo eyes: its apex is drawn back along the
    // view, and the rectangle grown by the eyes' half distance, far enough that
    // each eye's own pyramid lies inside it from the eye to the sprite (an eye
    // `spread` to the side of the apex line, a rectangle at least `halfWidth`
    // wide: back by spread * depth / halfWidth).
    Vec3 centre{0.0f, 0.0f, 0.0f};
    for (int32_t i = 0; i < 4; i++) {
        centre.x += view.corners[i].x * 0.25f;
        centre.y += view.corners[i].y * 0.25f;
        centre.z += view.corners[i].z * 0.25f;
    }
    Vec3 grown[4];
    float farDepth = 0.0f;
    float halfWidth = 1e30f;
    const float spread = view.spread > 0.0f ? view.spread : 0.0f;
    for (int32_t i = 0; i < 4; i++) {
        const float depth = dotV(sub(view.corners[i], view.eye), view.forward);
        farDepth = depth > farDepth ? depth : farDepth;
        // The narrowest the rectangle is around its centre, across the view.
        const Vec3 edge = sub(view.corners[(i + 1) % 4], view.corners[i]);
        const float edgeLen = sqrtf(dotV(edge, edge));
        halfWidth = edgeLen * 0.5f < halfWidth ? edgeLen * 0.5f : halfWidth;
    }
    // Scaled about its centre so that EVERY side moves out by at least the
    // margin and the eyes' half distance (the narrowest half-width moves least).
    const float grow = kGrowUnits + spread;
    const float scaleUp = 1.0f + kGrowShare + (halfWidth > 0.0f ? grow / halfWidth : 0.0f);
    for (int32_t i = 0; i < 4; i++) {
        const Vec3 off = sub(view.corners[i], centre);
        grown[i] = Vec3{centre.x + off.x * scaleUp, centre.y + off.y * scaleUp,
                        centre.z + off.z * scaleUp};
    }
    farDepth += kFarMargin;
    const float back = (spread > 0.0f && halfWidth > 0.0f) ? spread * farDepth / halfWidth : 0.0f;
    const Vec3 apex{view.eye.x - view.forward.x * back, view.eye.y - view.forward.y * back,
                    view.eye.z - view.forward.z * back};
    Planes planes;
    pyramid(apex, grown, centre, view.eye, view.forward, farDepth, planes.n, planes.d);

    for (int32_t s = 0; s < sourceCount; s++) {
        const Source& src = sources[s];
        if (src.scene == nullptr || src.scene->vertices.empty()) {
            continue;
        }
        Tree* tree = nullptr;
        for (size_t i = 0; i < trees_.size(); i++) {
            Tree& t = trees_[i];
            if (t.arena == src.arena && t.scene == src.scene && t.data == src.scene->vertices.data() &&
                t.vertices == src.scene->vertices.size() && t.batches == src.scene->batches.size()) {
                tree = &t;
                break;
            }
        }
        if (tree == nullptr) {
            // Begun the first time the arena is drawn with Kurt (or after its view
            // was rebuilt), and built a slice a frame (step). Until it is done the
            // arena marks nothing (Kurt is depth-tested against it).
            Tree fresh;
            fresh.arena = src.arena;
            fresh.scene = src.scene;
            fresh.data = src.scene->vertices.data();
            fresh.vertices = src.scene->vertices.size();
            fresh.batches = src.scene->batches.size();
            trees_.push_back(fresh);
            tree = &trees_.back();
            begin(tree);
            stats_.builds++;
        }
        tree->used = true;
        if (!tree->ready) {
            step(tree, kBuildBudget);
        }
    }
    // Let go of the arenas no longer drawn (before searching: `found_` keeps
    // pointers into trees_).
    for (size_t i = trees_.size(); i > 0; i--) {
        if (!trees_[i - 1].used) {
            trees_.erase(trees_.begin() + (ptrdiff_t)(i - 1));
        }
    }
    for (size_t i = 0; i < trees_.size(); i++) {
        if (trees_[i].ready) {
            search(trees_[i], planes, view, maxTriangles);
        }
    }

    // Group by glass and depth layer, write the runs. An insertion sort: the
    // triangles are a handful (std::stable_sort would allocate every frame),
    // and it is stable, so a layer's triangles keep the order they were found.
    for (size_t i = 1; i < found_.size(); i++) {
        const Candidate c = found_[i];
        const int32_t key = (c.glass ? 1 << 16 : 0) + c.layer;
        size_t j = i;
        while (j > 0 && ((found_[j - 1].glass ? 1 << 16 : 0) + found_[j - 1].layer) > key) {
            found_[j] = found_[j - 1];
            j--;
        }
        found_[j] = c;
    }
    int32_t runCount = 0;
    int32_t written = 0;
    for (size_t i = 0; i < found_.size(); i++) {
        const Candidate& c = found_[i];
        if (runCount == 0 || runs[runCount - 1].layer != c.layer ||
            runs[runCount - 1].glass != c.glass) {
            if (runCount == maxRuns) {
                stats_.dropped += (int32_t)(found_.size() - i);
                break;
            }
            runs[runCount].glass = c.glass;
            runs[runCount].layer = c.layer;
            runs[runCount].first = written * 3;
            runs[runCount].count = 0;
            runCount++;
        }
        // THE FILL: the triangle cut to the pyramid's four sides, so the mark
        // costs the pixels around the sprite and not a wall's whole screen area
        // (measured on the Old 3DS: whole triangles cost ~2 ms of GPU a frame).
        // The cut points lie on the triangle's own plane, so the depth the mark
        // is tested at is the triangle's.
        const std::vector<Vertex>& v = c.tree->scene->vertices;
        Vec3 poly[kClipCorners];
        int32_t count = clipToSides(planes, v[(size_t)c.triangle * 3].pos,
                                    v[(size_t)c.triangle * 3 + 1].pos,
                                    v[(size_t)c.triangle * 3 + 2].pos, poly);
        for (int32_t k = 1; k + 1 < count; k++) {
            if (written >= maxTriangles) {
                stats_.dropped++;
                break;
            }
            const Vec3 fan[3] = {poly[0], poly[k], poly[k + 1]};
            for (int32_t j = 0; j < 3; j++) {
                Vertex vertex;
                vertex.pos = fan[j];
                vertex.uv = Vec2{0.0f, 0.0f};
                vertex.colour = kVertexWhite;
                out[written * 3 + j] = vertex;
            }
            runs[runCount - 1].count += 3;
            written++;
        }
    }
    // A run its triangles were all cut away from (or dropped) draws nothing.
    int32_t kept = 0;
    for (int32_t i = 0; i < runCount; i++) {
        if (runs[i].count > 0) {
            runs[kept++] = runs[i];
        }
    }
    runCount = kept;
    stats_.triangles = written;
    return runCount;
}

} // namespace mdk

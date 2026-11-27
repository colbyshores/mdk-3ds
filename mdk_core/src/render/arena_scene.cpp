// ============================================================================
// arena_scene.cpp — see arena_scene.h.
// ============================================================================

#include "render/arena_scene.h"

#include <math.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <tuple>

namespace mdk {

namespace {

// The C# Layers.cs constants.
const int32_t kHighestLayer = 3;
const float kPlaneSteps = 64.0f;
const float kMinOverlap = 1.0e-3f;
const int32_t kTriangleCorners = 3;

formats::Vec3f subtract(const formats::Vec3f& a, const formats::Vec3f& b) {
    return formats::Vec3f{a.x - b.x, a.y - b.y, a.z - b.z};
}

formats::Vec3f cross(const formats::Vec3f& a, const formats::Vec3f& b) {
    return formats::Vec3f{a.y * b.z - a.z * b.y,
                          a.z * b.x - a.x * b.z,
                          a.x * b.y - a.y * b.x};
}

float dot(const formats::Vec3f& a, const formats::Vec3f& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

float length(const formats::Vec3f& v) {
    return sqrtf(dot(v, v));
}

// Vector3.Normalize, except that a zero vector returns zero here where the C#
// divides by zero. Only a corrupt arena reaches that: the plane normal is a
// unit vector, and the axis below is chosen so it is never parallel to it.
formats::Vec3f normalize(const formats::Vec3f& v) {
    const float len = length(v);
    if (len <= 0.0f) {
        return formats::Vec3f{0.0f, 0.0f, 0.0f};
    }
    return formats::Vec3f{v.x / len, v.y / len, v.z / len};
}

formats::Vec3f divide(const formats::Vec3f& v, float scalar) {
    return formats::Vec3f{v.x / scalar, v.y / scalar, v.z / scalar};
}

// MathF.Round's default midpoint rule is to even. The plane keys use it to
// put nearly coplanar triangles together.
//
// The saturating guard is for a corrupt arena: a vertex far enough out of
// float range makes the plane distance overflow. On .NET 10 the (int) cast
// saturates (NaN -> 0, a positive overflow -> INT32_MAX, a negative overflow
// -> INT32_MIN), whereas C++ float-to-int outside the representable range is
// undefined behaviour. Return those values explicitly instead.
//
// The negative bound is inclusive: -2147483648 is exactly INT32_MIN, and
// the C# casts it unchanged. The positive bound is 2147483520, the largest
// float below 2^31; that value is representable, while the next float up
// overflows and saturates. The guard changes nothing for real data.
int32_t roundToEven(float value) {
    if (!(value >= -2147483648.0f)) {
        return value < 0.0f ? INT32_MIN : 0;
    }
    if (value > 2147483520.0f) {
        return INT32_MAX;
    }
    const float lower = std::floor(value);
    const float fraction = value - lower;
    if (fraction > 0.5f) {
        return (int32_t)(lower + 1.0f);
    }
    if (fraction < 0.5f) {
        return (int32_t)lower;
    }
    const int64_t whole = (int64_t)lower;
    return (int32_t)(whole + (whole & 1 ? 1 : 0));
}

int32_t planeStep(float value) {
    return roundToEven(value * kPlaneSteps);
}

formats::Vec3f corner(const std::vector<formats::Vec3f>& vertices,
                      const std::vector<int32_t>& triangleIndices,
                      int32_t triangle, int32_t at) {
    const int32_t index = triangleIndices[(size_t)triangle * kTriangleCorners + (size_t)at];
    return vertices[(size_t)index];
}

struct Vec2f {
    float x = 0.0f;
    float y = 0.0f;
};

Vec2f subtract(const Vec2f& a, const Vec2f& b) {
    return Vec2f{a.x - b.x, a.y - b.y};
}

float cross(const Vec2f& a, const Vec2f& b) {
    return a.x * b.y - a.y * b.x;
}

// MathF.Sign. A NaN returns 0 here where the C# returns NaN; the winding of a
// degenerate clip triangle decides nothing either way in a real arena.
float sign(float value) {
    if (value > 0.0f) {
        return 1.0f;
    }
    if (value < 0.0f) {
        return -1.0f;
    }
    return 0.0f;
}

// A clipped polygon in fixed storage. overlap() runs once per pair of
// triangles in a plane, quadratic in the plane's size, and the depth build is
// sliced across play frames (ArenaSceneBuilder::step): with a std::vector per
// clip it was four heap allocations and frees a pair, plus a runtime `%` (a
// software divide on the ARM11, AGENTS.md R11) per corner -- measured on the
// Old 3DS, a 600-pair depth step was ~20 ms of a corridor-crossing frame.
// The bound: a clip keeps each corner at most once and adds at most one
// crossing per edge of the polygon, so it at most doubles the count, and a
// triangle is clipped three times: 3 -> 6 -> 12 -> 24. (A convex clip of a
// triangle never passes 6; the capacity is the loop's bound, so no input --
// a NaN corner included -- can overrun it.)
const size_t kClipMaxCorners = 24;

struct ClipPolygon {
    Vec2f corners[kClipMaxCorners];
    size_t count = 0;
};

// The part of a polygon on the inner side of the edge from a to b (the C#
// ClipBy), into `kept`. The same arithmetic, in the same order, as the C#.
void clipBy(const ClipPolygon& polygon, const Vec2f& a, const Vec2f& b, float winding,
            ClipPolygon* kept) {
    kept->count = 0;
    for (size_t k = 0; k < polygon.count; k++) {
        const Vec2f& p = polygon.corners[k];
        const Vec2f& q = polygon.corners[k + 1 < polygon.count ? k + 1 : 0];
        const float sideP = winding * cross(subtract(b, a), subtract(p, a));
        const float sideQ = winding * cross(subtract(b, a), subtract(q, a));
        if (sideP >= 0.0f) {
            kept->corners[kept->count++] = p;
        }
        if ((sideP >= 0.0f) != (sideQ >= 0.0f)) {
            const float at = sideP / (sideP - sideQ);
            kept->corners[kept->count++] = Vec2f{p.x + (q.x - p.x) * at, p.y + (q.y - p.y) * at};
        }
    }
}

// The area two triangles in one plane share. One is clipped by the other's
// edges in the plane's own 2D coordinates.
float overlap(const std::vector<formats::Vec3f>& vertices,
              const std::vector<int32_t>& triangleIndices,
              int32_t triangle, int32_t other, const formats::Vec3f& normal) {
    const formats::Vec3f origin = corner(vertices, triangleIndices, triangle, 0);
    const formats::Vec3f axis = std::fabs(normal.x) < 0.9f
                                     ? formats::Vec3f{1.0f, 0.0f, 0.0f}
                                     : formats::Vec3f{0.0f, 1.0f, 0.0f};
    const formats::Vec3f u = normalize(cross(normal, axis));
    const formats::Vec3f v = cross(normal, u);

    auto flat = [&](const formats::Vec3f& point) {
        const formats::Vec3f relative = subtract(point, origin);
        return Vec2f{dot(relative, u), dot(relative, v)};
    };

    // Two buffers, clipped back and forth.
    ClipPolygon buffers[2];
    buffers[0].corners[0] = flat(origin);
    buffers[0].corners[1] = flat(corner(vertices, triangleIndices, triangle, 1));
    buffers[0].corners[2] = flat(corner(vertices, triangleIndices, triangle, 2));
    buffers[0].count = 3;
    int32_t current = 0;

    const Vec2f clip0 = flat(corner(vertices, triangleIndices, other, 0));
    const Vec2f clip1 = flat(corner(vertices, triangleIndices, other, 1));
    const Vec2f clip2 = flat(corner(vertices, triangleIndices, other, 2));
    const float winding = sign(cross(subtract(clip1, clip0), subtract(clip2, clip0)));

    for (int32_t edge = 0; edge < kTriangleCorners && buffers[current].count > 0; edge++) {
        const Vec2f& a = edge == 0 ? clip0 : (edge == 1 ? clip1 : clip2);
        const Vec2f& b = edge == 0 ? clip1 : (edge == 1 ? clip2 : clip0);
        clipBy(buffers[current], a, b, winding, &buffers[1 - current]);
        current = 1 - current;
    }

    const ClipPolygon& polygon = buffers[current];
    float twice = 0.0f;
    for (size_t k = 0; k < polygon.count; k++) {
        twice += cross(polygon.corners[k], polygon.corners[k + 1 < polygon.count ? k + 1 : 0]);
    }
    return std::fabs(twice) / 2.0f;
}

} // namespace

bool DepthLayersBuilder::begin(const std::vector<formats::Vec3f>* vertices,
                               const std::vector<int32_t>* triangleIndices,
                               std::vector<int32_t>* layers) {
    planes_.clear();
    assignValid_ = false;
    nextTriangle_ = 0;
    planeSorted_ = false;
    assignI_ = 1;
    assignJ_ = 0;
    if (!vertices || !triangleIndices || !layers ||
        triangleIndices->size() % (size_t)kTriangleCorners != 0) {
        phase_ = Phase::Failed;
        return false;
    }
    const int32_t triangleCount = (int32_t)(triangleIndices->size() / (size_t)kTriangleCorners);
    layers->assign((size_t)triangleCount, 0);
    for (int32_t index : *triangleIndices) {
        if (index < 0 || index >= (int32_t)vertices->size()) {
            layers->clear();
            phase_ = Phase::Failed;
            return false;
        }
    }
    vertices_ = vertices;
    triangleIndices_ = triangleIndices;
    layers_ = layers;
    phase_ = Phase::Collect;
    return true;
}

bool DepthLayersBuilder::step(int32_t budget) {
    if (phase_ == Phase::Done) {
        return true;
    }
    if (phase_ != Phase::Collect && phase_ != Phase::Assign) {
        return false;
    }
    const int32_t triangleCount =
        (int32_t)(triangleIndices_->size() / (size_t)kTriangleCorners);
    int32_t spent = 0;
    if (phase_ == Phase::Collect) {
        // The triangles of each plane, keyed by a rounded normal and plane
        // distance. The key puts nearly coplanar triangles together.
        // A collected triangle is TWO units of the budget: on the Old 3DS one
        // costs about twice an overlap test below (L7 walk, DANT_2's build:
        // 600 triangles ~7 ms a step against 600 tests ~4 ms).
        while (nextTriangle_ < triangleCount && spent < budget) {
            const int32_t triangle = nextTriangle_++;
            spent += 2;
            const formats::Vec3f a = corner(*vertices_, *triangleIndices_, triangle, 0);
            const formats::Vec3f b = corner(*vertices_, *triangleIndices_, triangle, 1);
            const formats::Vec3f c = corner(*vertices_, *triangleIndices_, triangle, 2);
            const formats::Vec3f crossed = cross(subtract(c, a), subtract(b, a));
            const float area = length(crossed);
            // The C# drops a zero area. This also drops a NaN one, which only a
            // corrupt arena produces, and dropping it is not a fidelity choice:
            // a NaN would make the sort below compare inconsistently, and an
            // inconsistent comparator is undefined behaviour in C++ where the
            // C#'s merge sort merely orders it arbitrarily.
            if (!(area > 0.0f)) {
                continue;
            }
            Entry entry;
            entry.triangle = triangle;
            entry.area = area;
            entry.normal = divide(crossed, area);
            const PlaneKey key(planeStep(entry.normal.x), planeStep(entry.normal.y),
                               planeStep(entry.normal.z), planeStep(dot(entry.normal, a)));
            planes_[key].push_back(entry);
        }
        if (nextTriangle_ < triangleCount) {
            return false;
        }
        assignValid_ = !planes_.empty();
        if (assignValid_) {
            assignKey_ = planes_.begin()->first;
        }
        phase_ = Phase::Assign;
    }
    while (assignValid_ && spent < budget) {
        std::vector<Entry>& plane = planes_.find(assignKey_)->second;
        if (!planeSorted_) {
            std::sort(plane.begin(), plane.end(), [](const Entry& a, const Entry& b) {
                if (a.area != b.area) {
                    return a.area > b.area;
                }
                return a.triangle < b.triangle;
            });
            planeSorted_ = true;
            assignI_ = 1;
            assignJ_ = 0;
        }
        // Each smaller triangle goes one layer above the highest bigger one
        // it overlaps (the C# Layers.Stack).
        while (assignI_ < plane.size() && spent < budget) {
            const int32_t triangle = plane[assignI_].triangle;
            while (assignJ_ < assignI_ && spent < budget) {
                const int32_t under = plane[assignJ_].triangle;
                spent++;
                if ((*layers_)[(size_t)under] >= (*layers_)[(size_t)triangle] &&
                    overlap(*vertices_, *triangleIndices_, triangle, under,
                            plane[assignI_].normal) > kMinOverlap) {
                    (*layers_)[(size_t)triangle] =
                        std::min((*layers_)[(size_t)under] + 1, kHighestLayer);
                }
                assignJ_++;
            }
            if (assignJ_ >= assignI_) {
                assignI_++;
                assignJ_ = 0;
            }
        }
        if (assignI_ >= plane.size()) {
            std::map<PlaneKey, std::vector<Entry>>::iterator next = planes_.find(assignKey_);
            ++next;
            assignValid_ = next != planes_.end();
            if (assignValid_) {
                assignKey_ = next->first;
            }
            planeSorted_ = false;
        }
    }
    if (!assignValid_) {
        phase_ = Phase::Done;
        return true;
    }
    return false;
}

bool computeArenaDepthLayers(const std::vector<formats::Vec3f>& vertices,
                             const std::vector<int32_t>& triangleIndices,
                             std::vector<int32_t>* layers) {
    DepthLayersBuilder builder;
    if (!builder.begin(&vertices, &triangleIndices, layers)) {
        return false;
    }
    while (!builder.step(1 << 30)) {
        if (builder.failed()) {
            break;
        }
    }
    return !builder.failed();
}

bool arenaSceneSkips(const formats::Arena* arena, int32_t triangle) {
    return (arena->triangleFlags()[(size_t)triangle] &
            (arena->clipFlag() | formats::Arena::kGroupDisabled)) != 0;
}

// The emission order of a scene's kept triangles: a stable sort by
// (group, material value, depth layer) -- the C#'s sort by surface and
// layer (arena_scene.h), so triangles that share a batch's whole state are
// contiguous and the run builder below merges them into one batch. Stable,
// so triangles equal on the whole key keep their original order: a
// coplanar tie between them draws as it did unsorted. Shared by the build
// and the placements, so the two can never disagree about where a
// triangle landed.
struct TriangleOrder {
    int32_t triangle = 0;
    int32_t group = 0;
    int32_t material = 0;
    int32_t depthLayer = 0;
};

static std::vector<int32_t> sortedTriangleOrder(const formats::Arena* arena,
                                                const std::vector<int32_t>& depthLayers) {
    const std::vector<int32_t>& materials = arena->triangleMaterials();
    const std::vector<uint32_t>& flags = arena->triangleFlags();
    std::vector<TriangleOrder> kept;
    kept.reserve((size_t)arena->triangleCount());
    for (int32_t t = 0; t < arena->triangleCount(); t++) {
        if (arenaSceneSkips(arena, t)) {
            continue;
        }
        TriangleOrder entry;
        entry.triangle = t;
        entry.group = (int32_t)(flags[(size_t)t] >> 24);
        int32_t groupMaterial = 0;
        entry.material = arena->groupMaterial(entry.group, &groupMaterial)
                             ? groupMaterial
                             : materials[(size_t)t];
        entry.depthLayer = (size_t)t < depthLayers.size() ? depthLayers[(size_t)t] : 0;
        kept.push_back(entry);
    }
    std::stable_sort(kept.begin(), kept.end(), [](const TriangleOrder& a, const TriangleOrder& b) {
        if (a.group != b.group) {
            return a.group < b.group;
        }
        if (a.material != b.material) {
            return a.material < b.material;
        }
        return a.depthLayer < b.depthLayer;
    });
    std::vector<int32_t> order;
    order.reserve(kept.size());
    for (size_t i = 0; i < kept.size(); i++) {
        order.push_back(kept[i].triangle);
    }
    return order;
}

bool arenaSceneTriangleFirsts(const formats::Arena* arena, std::vector<int32_t>* out) {
    if (arena == nullptr || out == nullptr) {
        return false;
    }
    // buildArenaScene pushes three vertices for every triangle it does not
    // skip, in the sorted order above, so a triangle's first vertex is three
    // times its position in that order.
    std::vector<int32_t> depthLayers;
    if (!computeArenaDepthLayers(arena->vertices(), arena->triangleIndices(), &depthLayers)) {
        return false;
    }
    const std::vector<int32_t> order = sortedTriangleOrder(arena, depthLayers);
    out->assign((size_t)arena->triangleCount(), -1);
    for (size_t i = 0; i < order.size(); i++) {
        (*out)[(size_t)order[i]] = (int32_t)i * 3;
    }
    return true;
}

bool ArenaSceneBuilder::begin(const formats::Arena* arena, const MaterialResolver* resolver,
                              const formats::Palette* palette,
                              const std::vector<const formats::TextureArchive*>& archives) {
    failure_ = Failure::None;
    scene_.clear();
    surfaces_.clear();
    drawn_.clear();
    scaleX_.clear();
    scaleY_.clear();
    depthLayers_.clear();
    order_.clear();
    oi_ = 0;
    haveRun_ = false;
    if (!arena || !resolver || !palette) {
        failure_ = Failure::InvalidData;
        phase_ = Phase::Failed;
        return false;
    }
    const std::vector<int32_t>& indices = arena->triangleIndices();
    const int32_t triangleCount = arena->triangleCount();
    if (triangleCount <= 0 || (int32_t)indices.size() < triangleCount * 3) {
        failure_ = Failure::InvalidData;
        phase_ = Phase::Failed;
        return false;
    }
    arena_ = arena;
    resolver_ = resolver;
    palette_ = palette;
    archives_ = archives;
    // The vertex reserve is NOT taken here: two builds can be started in
    // one sync (the pair), and the buffer is only needed when the Loop
    // phase fills it -- the step that gets there reserves it then, so a
    // build still in its depth phase holds no vertex buffer at all.
    if (!depth_.begin(&arena->vertices(), &arena->triangleIndices(), &depthLayers_)) {
        failure_ = Failure::InvalidData;
        phase_ = Phase::Failed;
        return false;
    }
    phase_ = Phase::Setup;
    return true;
}

bool ArenaSceneBuilder::step(int32_t maxTriangles) {
    if (phase_ == Phase::Done) {
        return true;
    }
    if (phase_ == Phase::Failed || phase_ == Phase::Idle) {
        return false;
    }
    const std::vector<int32_t>& indices = arena_->triangleIndices();
    const std::vector<int32_t>& materials = arena_->triangleMaterials();
    const std::vector<uint32_t>& flags = arena_->triangleFlags();
    const std::vector<formats::Vec2f>& uvs = arena_->triangleUvs();
    const std::vector<formats::Vec3f>& positions = arena_->vertices();
    if (phase_ == Phase::Setup) {
        // The triangles' depth layers: the C# Levels.Of, computed once per arena
        // (the C# keeps them in the ArenaView). A detail lying in the plane of a
        // bigger surface is a layer above it, and the renderer pulls it towards
        // the eye so the two do not fight for the same depth. Sliced like the
        // loop below: the per-plane stacking is quadratic in a plane's size
        // and was the first sync's whole cost when it ran unsliced. Measured on
        // the Old 3DS (L7 walk, DANT_2's build at the corridor crossing): 600
        // units are ~4 ms of overlap tests (DepthLayersBuilder counts a
        // collected triangle as two units, its measured relative cost).
        const int32_t kDepthWorkPerStep = 600;
        if (!depth_.step(kDepthWorkPerStep)) {
            if (depth_.failed()) {
                failure_ = Failure::InvalidData;
                phase_ = Phase::Failed;
            }
            return false;
        }
        // Emitted in the sorted order (see sortedTriangleOrder): same-state
        // triangles are contiguous, so the runs below are the merged batches.
        order_ = sortedTriangleOrder(arena_, depthLayers_);
        scene_.vertices.reserve((size_t)arena_->triangleCount() * 3);
        phase_ = Phase::Loop;
        return false;
    }
    int32_t spent = 0;
    while (oi_ < order_.size() && spent < maxTriangles) {
        const int32_t t = order_[oi_];
        spent++;
        oi_++;
        // The C# LevelView.Build skips a triangle the arena's clip flag marks
        // (the 1996 demo's not-drawn triangles) or the Disabled flag marks
        // (the retail arena activation's, LevelView.cs:294).
        if (arenaSceneSkips(arena_, t)) {
            continue;
        }
        const int32_t triangleGroup = (int32_t)(flags[(size_t)t] >> 24);
        // A group's Hidden state is NOT checked here: the C# LevelView builds
        // every group's mesh and checks Hidden at DRAW time (LevelView.cs:126),
        // so a group the scripts hide mid-play disappears with no rebuild. The
        // build only drops the clip/disabled triangles (above); hidden groups'
        // geometry stays in the buffer and arenaBatchVisible skips its draw.
        // A stored group material replaces the triangle's own (C# Group.Material);
        // absence is a presence test, not a sentinel, because a negative material
        // is a valid flat palette colour.
        int32_t groupMaterial = 0;
        const int32_t material = arena_->groupMaterial(triangleGroup, &groupMaterial)
                                     ? groupMaterial
                                     : materials[(size_t)t];

        if (surfaces_.find(material) == surfaces_.end()) {
            Surface surface;
            const bool isDrawn = resolver_->resolve(material, arena_->materials(), *palette_, archives_, &surface);
            surfaces_[material] = surface;
            drawn_[material] = isDrawn;
            float sx = 0.0f;
            float sy = 0.0f;
            if (isDrawn && surface.texture) {
                sx = 1.0f / (float)surface.texture->width();
                sy = 1.0f / (float)surface.texture->height();
            }
            scaleX_[material] = sx;
            scaleY_[material] = sy;
        }
        const Surface& surface = surfaces_[material];
        const float sx = scaleX_[material];
        const float sy = scaleY_[material];
        const int32_t depthLayer = (size_t)t < depthLayers_.size() ? depthLayers_[(size_t)t] : 0;

        // The vertices. The colour is white: the original's Gouraud-blended
        // stream is this vertex's colour, and the world's surfaces are
        // unlit (the palette decides).
        const int32_t first = (int32_t)scene_.vertices.size();
        for (int32_t k = 0; k < 3; k++) {
            const int32_t index = indices[(size_t)t * 3 + (size_t)k];
            Vertex vertex;
            if (index >= 0 && index < (int32_t)positions.size()) {
                vertex.pos = Vec3{positions[(size_t)index].x,
                                  positions[(size_t)index].y,
                                  positions[(size_t)index].z};
            }
            const size_t uvAt = (size_t)t * 3 + (size_t)k;
            if (uvAt < uvs.size()) {
                vertex.uv = Vec2{uvs[uvAt].x * sx, uvs[uvAt].y * sy};
            }
            vertex.colour = kVertexWhite;
            scene_.vertices.push_back(vertex);
        }

        // This triangle's box, from the three vertices just appended. The run's
        // box accumulates these; the backend culls per batch against it.
        float tmin[3], tmax[3];
        for (int k = 0; k < 3; k++) {
            const Vec3& p = scene_.vertices[(size_t)first + (size_t)k].pos;
            const float c[3] = {p.x, p.y, p.z};
            for (int j = 0; j < 3; j++) {
                if (k == 0) {
                    tmin[j] = c[j];
                    tmax[j] = c[j];
                } else {
                    if (c[j] < tmin[j]) tmin[j] = c[j];
                    if (c[j] > tmax[j]) tmax[j] = c[j];
                }
            }
        }

        if (!haveRun_ || material != runMaterial_ || triangleGroup != runGroup_ ||
            depthLayer != runDepthLayer_) {
            // A new run. The previous one ends where this one starts.
            if (haveRun_ && !scene_.batches.empty()) {
                scene_.batches.back().count = first - scene_.batches.back().first;
                for (int j = 0; j < 3; j++) {
                    scene_.batches.back().boundsMin[j] = runMin_[j];
                    scene_.batches.back().boundsMax[j] = runMax_[j];
                }
            }
            if (!drawn_[material]) {
                // Not drawn: NONE, PEN_ENV, RIPPLE, or a material name that
                // no archive has. The run is left empty, and the next
                // triangle starts a new one.
                haveRun_ = false;
                continue;
            }
            ArenaBatch batch;
            batch.first = first;
            batch.count = 0;
            batch.surface = surface;
            batch.group = triangleGroup;
            batch.depthLayer = depthLayer;
            scene_.batches.push_back(batch);
            for (int j = 0; j < 3; j++) {
                runMin_[j] = tmin[j];
                runMax_[j] = tmax[j];
            }
            runMaterial_ = material;
            runGroup_ = triangleGroup;
            runDepthLayer_ = depthLayer;
            haveRun_ = true;
        } else {
            for (int j = 0; j < 3; j++) {
                if (tmin[j] < runMin_[j]) runMin_[j] = tmin[j];
                if (tmax[j] > runMax_[j]) runMax_[j] = tmax[j];
            }
        }
    }
    if (oi_ >= order_.size()) {
        finish();
        return phase_ == Phase::Done;
    }
    return false;
}

void ArenaSceneBuilder::finish() {
    if (haveRun_ && !scene_.batches.empty()) {
        scene_.batches.back().count = (int32_t)scene_.vertices.size() - scene_.batches.back().first;
        for (int j = 0; j < 3; j++) {
            scene_.batches.back().boundsMin[j] = runMin_[j];
            scene_.batches.back().boundsMax[j] = runMax_[j];
        }
    }

    // A batch with no triangles is a surface that is not drawn; drop it.
    std::vector<ArenaBatch> kept;
    kept.reserve(scene_.batches.size());
    for (size_t i = 0; i < scene_.batches.size(); i++) {
        if (scene_.batches[i].count > 0) {
            kept.push_back(scene_.batches[i]);
        }
    }
    scene_.batches.swap(kept);

    failure_ = scene_.batches.empty() ? Failure::EmptyScene : Failure::None;
    phase_ = scene_.batches.empty() ? Phase::Failed : Phase::Done;
}

bool buildArenaScene(const formats::Arena* arena,
                     const MaterialResolver& resolver,
                     const formats::Palette& palette,
                     const std::vector<const formats::TextureArchive*>& archives,
                     ArenaScene* out) {
    if (!out) {
        return false;
    }
    out->clear();
    ArenaSceneBuilder builder;
    if (!builder.begin(arena, &resolver, &palette, archives)) {
        return false;
    }
    while (!builder.step(1 << 30)) {
        if (builder.failed()) {
            break;
        }
    }
    if (builder.failed()) {
        out->clear();
        return false;
    }
    out->vertices.swap(builder.scene().vertices);
    out->batches.swap(builder.scene().batches);
    return true;
}

bool arenaBatchVisible(const formats::Arena* arena, const ArenaBatch& batch) {
    if (!arena) {
        return false;
    }
    // The C# LevelView skips a group whose stored state is Hidden (LevelView.cs:126).
    // Group 0 has no stored state (groupState returns 0) and is always drawn.
    return (arena->groupState(batch.group) & formats::Arena::kGroupHidden) == 0;
}

bool boxesCut(const Vec3& aMin, const Vec3& aMax, const Vec3& bMin, const Vec3& bMax) {
    return aMin.x < bMax.x && bMin.x < aMax.x && aMin.y < bMax.y && bMin.y < aMax.y &&
           aMin.z < bMax.z && bMin.z < aMax.z;
}

void boxWithEye(const Vec3& min, const Vec3& max, const Vec3& eye, float margin, Vec3* outMin,
                Vec3* outMax) {
    *outMin = Vec3{fminf(min.x, eye.x - margin), fminf(min.y, eye.y - margin),
                   fminf(min.z, eye.z - margin)};
    *outMax = Vec3{fmaxf(max.x, eye.x + margin), fmaxf(max.y, eye.y + margin),
                   fmaxf(max.z, eye.z + margin)};
}

} // namespace mdk

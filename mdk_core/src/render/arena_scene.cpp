// ============================================================================
// arena_scene.cpp — see arena_scene.h.
// ============================================================================

#include "render/arena_scene.h"

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

struct PlaneEntry {
    int32_t triangle = 0;
    float area = 0.0f;
    formats::Vec3f normal = {0.0f, 0.0f, 0.0f};
};

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

// The part of a polygon on the inner side of the edge from a to b (the C#
// ClipBy).
std::vector<Vec2f> clipBy(const std::vector<Vec2f>& polygon, const Vec2f& a, const Vec2f& b,
                          float winding) {
    std::vector<Vec2f> kept;
    kept.reserve(polygon.size() + 1);
    for (size_t k = 0; k < polygon.size(); k++) {
        const Vec2f& p = polygon[k];
        const Vec2f& q = polygon[(k + 1) % polygon.size()];
        const float sideP = winding * cross(subtract(b, a), subtract(p, a));
        const float sideQ = winding * cross(subtract(b, a), subtract(q, a));
        if (sideP >= 0.0f) {
            kept.push_back(p);
        }
        if ((sideP >= 0.0f) != (sideQ >= 0.0f)) {
            const float at = sideP / (sideP - sideQ);
            kept.push_back(Vec2f{p.x + (q.x - p.x) * at, p.y + (q.y - p.y) * at});
        }
    }
    return kept;
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

    std::vector<Vec2f> polygon;
    polygon.push_back(flat(origin));
    polygon.push_back(flat(corner(vertices, triangleIndices, triangle, 1)));
    polygon.push_back(flat(corner(vertices, triangleIndices, triangle, 2)));

    const Vec2f clip0 = flat(corner(vertices, triangleIndices, other, 0));
    const Vec2f clip1 = flat(corner(vertices, triangleIndices, other, 1));
    const Vec2f clip2 = flat(corner(vertices, triangleIndices, other, 2));
    const float winding = sign(cross(subtract(clip1, clip0), subtract(clip2, clip0)));

    for (int32_t edge = 0; edge < kTriangleCorners && !polygon.empty(); edge++) {
        const Vec2f& a = edge == 0 ? clip0 : (edge == 1 ? clip1 : clip2);
        const Vec2f& b = edge == 0 ? clip1 : (edge == 1 ? clip2 : clip0);
        polygon = clipBy(polygon, a, b, winding);
    }

    float twice = 0.0f;
    for (size_t k = 0; k < polygon.size(); k++) {
        twice += cross(polygon[k], polygon[(k + 1) % polygon.size()]);
    }
    return std::fabs(twice) / 2.0f;
}

} // namespace

bool computeArenaDepthLayers(const std::vector<formats::Vec3f>& vertices,
                             const std::vector<int32_t>& triangleIndices,
                             std::vector<int32_t>* layers) {
    if (!layers || triangleIndices.size() % (size_t)kTriangleCorners != 0) {
        return false;
    }

    const int32_t triangleCount = (int32_t)(triangleIndices.size() / (size_t)kTriangleCorners);
    layers->assign((size_t)triangleCount, 0);
    for (int32_t index : triangleIndices) {
        if (index < 0 || index >= (int32_t)vertices.size()) {
            layers->clear();
            return false;
        }
    }

    // The triangles of each plane, keyed by a rounded normal and plane
    // distance. The key puts nearly coplanar triangles together.
    typedef std::tuple<int32_t, int32_t, int32_t, int32_t> PlaneKey;
    std::map<PlaneKey, std::vector<PlaneEntry>> planes;
    for (int32_t triangle = 0; triangle < triangleCount; triangle++) {
        const formats::Vec3f a = corner(vertices, triangleIndices, triangle, 0);
        const formats::Vec3f b = corner(vertices, triangleIndices, triangle, 1);
        const formats::Vec3f c = corner(vertices, triangleIndices, triangle, 2);
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

        PlaneEntry entry;
        entry.triangle = triangle;
        entry.area = area;
        entry.normal = divide(crossed, area);
        const PlaneKey key(planeStep(entry.normal.x), planeStep(entry.normal.y),
                           planeStep(entry.normal.z), planeStep(dot(entry.normal, a)));
        planes[key].push_back(entry);
    }

    for (std::map<PlaneKey, std::vector<PlaneEntry>>::iterator it = planes.begin();
         it != planes.end(); ++it) {
        std::vector<PlaneEntry>& plane = it->second;
        std::sort(plane.begin(), plane.end(), [](const PlaneEntry& a, const PlaneEntry& b) {
            if (a.area != b.area) {
                return a.area > b.area;
            }
            return a.triangle < b.triangle;
        });

        // Each smaller triangle goes one layer above the highest bigger one
        // it overlaps (the C# Layers.Stack).
        for (size_t i = 1; i < plane.size(); i++) {
            const int32_t triangle = plane[i].triangle;
            for (size_t j = 0; j < i; j++) {
                const int32_t under = plane[j].triangle;
                if ((*layers)[(size_t)under] >= (*layers)[(size_t)triangle] &&
                    overlap(vertices, triangleIndices, triangle, under, plane[i].normal) > kMinOverlap) {
                    (*layers)[(size_t)triangle] =
                        std::min((*layers)[(size_t)under] + 1, kHighestLayer);
                }
            }
        }
    }
    return true;
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
    if (!arena) {
        return false;
    }

    const std::vector<int32_t>& indices = arena->triangleIndices();
    const std::vector<int32_t>& materials = arena->triangleMaterials();
    const std::vector<uint32_t>& flags = arena->triangleFlags();
    const std::vector<formats::Vec2f>& uvs = arena->triangleUvs();
    const std::vector<formats::Vec3f>& positions = arena->vertices();

    const int32_t triangleCount = arena->triangleCount();
    if (triangleCount <= 0 || (int32_t)indices.size() < triangleCount * 3) {
        return false;
    }

    out->vertices.reserve((size_t)triangleCount * 3);

    // The triangles' depth layers: the C# Levels.Of, computed once per arena
    // (the C# keeps them in the ArenaView). A detail lying in the plane of a
    // bigger surface is a layer above it, and the renderer pulls it towards
    // the eye so the two do not fight for the same depth.
    std::vector<int32_t> depthLayers;
    if (!computeArenaDepthLayers(positions, indices, &depthLayers)) {
        return false;
    }

    // The run of triangles currently being batched: its material value and its
    // group. A new material value OR a new group starts a new run, so a batch
    // never spans two groups and the drawing can skip a hidden group per batch.
    int32_t runMaterial = 0;
    int32_t runGroup = 0;
    int32_t runDepthLayer = 0;
    bool haveRun = false;

    // The surfaces, by material value. A material value's surface is
    // resolved ONCE, because the resolution searches the archives and the
    // UV SCALE depends on the texture it found.
    //
    // THE UV SCALE MATTERS, and it is easy to miss: the arena's texture
    // coordinates are in TEXELS, and the C# normalises them by the
    // texture's size -- `new Vertex(position, triangleUvs[...] * new
    // Vector2(1f / texture.Width, 1f / texture.Height))`. Without the
    // normalisation the texture coordinates run over the whole texture
    // range instead of one frame of it, and the surfaces are covered in
    // garbage.
    std::map<int32_t, Surface> surfaces;
    std::map<int32_t, bool> drawn;
    std::map<int32_t, float> scaleX;
    std::map<int32_t, float> scaleY;

    for (int32_t t = 0; t < triangleCount; t++) {
        // The C# LevelView.Build skips a triangle the arena's clip flag marks
        // (the 1996 demo's not-drawn triangles) or the Disabled flag marks
        // (the retail arena activation's, LevelView.cs:294).
        if ((flags[(size_t)t] & (arena->clipFlag() | formats::Arena::kGroupDisabled)) != 0) {
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
        const int32_t material = arena->groupMaterial(triangleGroup, &groupMaterial)
                                     ? groupMaterial
                                     : materials[(size_t)t];

        if (surfaces.find(material) == surfaces.end()) {
            Surface surface;
            const bool isDrawn = resolver.resolve(material, arena->materials(), palette, archives, &surface);
            surfaces[material] = surface;
            drawn[material] = isDrawn;
            float sx = 0.0f;
            float sy = 0.0f;
            if (isDrawn && surface.texture) {
                sx = 1.0f / (float)surface.texture->width();
                sy = 1.0f / (float)surface.texture->height();
            }
            scaleX[material] = sx;
            scaleY[material] = sy;
        }
        const Surface& surface = surfaces[material];
        const float sx = scaleX[material];
        const float sy = scaleY[material];
        const int32_t depthLayer = (size_t)t < depthLayers.size() ? depthLayers[(size_t)t] : 0;

        // The vertices. The colour is white: the original's Gouraud-blended
        // stream is this vertex's colour, and the world's surfaces are
        // unlit (the palette decides).
        const int32_t first = (int32_t)out->vertices.size();
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
            out->vertices.push_back(vertex);
        }

        if (!haveRun || material != runMaterial || triangleGroup != runGroup ||
            depthLayer != runDepthLayer) {
            // A new run. The previous one ends where this one starts.
            if (haveRun && !out->batches.empty()) {
                out->batches.back().count = first - out->batches.back().first;
            }
            if (!drawn[material]) {
                // Not drawn: NONE, PEN_ENV, RIPPLE, or a material name that
                // no archive has. The run is left empty, and the next
                // triangle starts a new one.
                haveRun = false;
                continue;
            }
            ArenaBatch batch;
            batch.first = first;
            batch.count = 0;
            batch.surface = surface;
            batch.group = triangleGroup;
            batch.depthLayer = depthLayer;
            out->batches.push_back(batch);
            runMaterial = material;
            runGroup = triangleGroup;
            runDepthLayer = depthLayer;
            haveRun = true;
        }
    }

    if (haveRun && !out->batches.empty()) {
        out->batches.back().count = (int32_t)out->vertices.size() - out->batches.back().first;
    }

    // A batch with no triangles is a surface that is not drawn; drop it.
    std::vector<ArenaBatch> kept;
    kept.reserve(out->batches.size());
    for (size_t i = 0; i < out->batches.size(); i++) {
        if (out->batches[i].count > 0) {
            kept.push_back(out->batches[i]);
        }
    }
    out->batches.swap(kept);

    return !out->batches.empty();
}

bool arenaBatchVisible(const formats::Arena* arena, const ArenaBatch& batch) {
    if (!arena) {
        return false;
    }
    // The C# LevelView skips a group whose stored state is Hidden (LevelView.cs:126).
    // Group 0 has no stored state (groupState returns 0) and is always drawn.
    return (arena->groupState(batch.group) & formats::Arena::kGroupHidden) == 0;
}

} // namespace mdk

// ============================================================================
// sun_baker.cpp -- see sun_baker.h. HOST ONLY.
//
// THE STEPS:
//   1. Sides. A Solid receiver's front, a DoubleSided receiver's front and back
//      (the shader turns the normal to the camera, so each side has a light of
//      its own). Each side's F_lit / 2 and F_shadow / 2 from its arena's rig
//      (sunBakeLight); a side the sun cannot reach (a covered arena, a face
//      turned from the sun) has one colour and casts no rays.
//   2. Charts. Grown from the largest unassigned side across shared corner
//      positions while a neighbour's normal stays within the cone (default: one
//      plane) and its rig is the seed's. Projected on the chart's plane, turned
//      to the projected edge direction of least area, a one-texel gutter, the
//      rectangle rounded up to the ETC1 block.
//   3. Probe. One ray a texel at the aimed-for density, stopping at the first
//      disagreement: a chart the sun reaches everywhere or nowhere is one colour
//      per side, and every side of one colour shares a 4x4 COLOUR BLOCK. Only
//      the charts a shadow edge crosses get texels of their own.
//   4. Pack. The blocks along the top, then shelves of charts, tallest first,
//      into the smallest power-of-two atlas (<= 1024 a side) within the byte
//      budget of the format; over it, every chart coarsens by 12% and it is
//      packed again. A chart over chartMostSide texels coarsens on its own.
//      Everything on 4-texel boundaries: no ETC1 block mixes two of them.
//   5. Sample. Each texel's raysPerAxis^2 sub-points inside one of the chart's
//      sides cast a ray to the sun from ON that triangle (pushed normalOffset
//      along its normal) against the casters that can block this sun; the texel
//      is the mean of its sub-points' F_lit / F_shadow. A texel no sub-point
//      landed in takes the mean of its neighbours (dilation).
//   6. The depth map: the sun's view of the same casters over the whole level
//      (first hit along the light), for the objects' one lookup a frame.
// SUNBAKE_DEBUG=1 in the environment prints the twelve largest atlas charts (a
// tuning aid for the density settings; nothing reads it otherwise).
// ============================================================================

#include "sun_baker.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <unordered_map>

namespace mdk {

namespace {

struct V3 {
    float x, y, z;
};

V3 v3(const float p[3]) { return V3{p[0], p[1], p[2]}; }
V3 operator+(V3 a, V3 b) { return V3{a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return V3{a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return V3{a.x * s, a.y * s, a.z * s}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) {
    return V3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float length(V3 a) { return sqrtf(dot(a, a)); }
V3 normalized(V3 a) {
    const float l = length(a);
    return l > 0.0f ? a * (1.0f / l) : V3{0.0f, 0.0f, 0.0f};
}

// The outward (front) normal, unnormalised: -cross(p1 - p0, p2 - p0).
V3 frontCross(const BakeTriangle& t) {
    const V3 a = v3(t.p[0]);
    const V3 c = cross(v3(t.p[1]) - a, v3(t.p[2]) - a);
    return V3{-c.x, -c.y, -c.z};
}

// Whether a caster can block a sun whose light travels along `travel`: a
// DoubleSided one always, a Solid one when its front faces the sun.
bool blocksThisSun(const BakeTriangle& t, V3 toSun) {
    if (!t.caster) {
        return false;
    }
    const V3 n = frontCross(t);
    if (dot(n, n) <= 0.0f) {
        return false;
    }
    return t.doubleSided || dot(n, toSun) > 0.0f;
}

// ---------------------------------------------------------------------------
// The casters' BVH: median split on the longest centroid axis, four a leaf.
// ---------------------------------------------------------------------------
struct BvhNode {
    float lo[3];
    float hi[3];
    int32_t left = -1;    // inner: the left child (right = left + 1); leaf: -1
    int32_t first = 0;    // leaf: the first triangle in `order`
    int32_t count = 0;
};

struct Caster {
    V3 p0, e1, e2;
    V3 centre;
};

class Bvh {
public:
    void build(const std::vector<Caster>& casters) {
        casters_ = &casters;
        order_.resize(casters.size());
        for (size_t i = 0; i < casters.size(); i++) {
            order_[i] = (int32_t)i;
        }
        nodes_.clear();
        nodes_.reserve(casters.size() * 2 + 1);
        if (!casters.empty()) {
            nodes_.push_back(BvhNode());
            split(0, 0, (int32_t)casters.size());
        }
    }

    // The nearest hit in (tMin, tMax) along origin + t * dir, or -1.
    float nearest(V3 origin, V3 dir, float tMin, float tMax, bool any) const {
        if (nodes_.empty()) {
            return -1.0f;
        }
        const V3 inv{1.0f / (dir.x != 0.0f ? dir.x : 1e-30f), 1.0f / (dir.y != 0.0f ? dir.y : 1e-30f),
                     1.0f / (dir.z != 0.0f ? dir.z : 1e-30f)};
        int32_t stack[128];
        int32_t top = 0;
        stack[top++] = 0;
        float best = tMax;
        bool hit = false;
        while (top > 0) {
            const BvhNode& node = nodes_[(size_t)stack[--top]];
            if (!boxHit(node, origin, inv, tMin, best)) {
                continue;
            }
            if (node.left < 0) {
                for (int32_t i = 0; i < node.count; i++) {
                    const Caster& c = (*casters_)[(size_t)order_[(size_t)(node.first + i)]];
                    const float t = triangleHit(c, origin, dir);
                    if (t > tMin && t < best) {
                        best = t;
                        hit = true;
                        if (any) {
                            return best;
                        }
                    }
                }
            } else if (top + 2 <= 128) {
                stack[top++] = node.left;
                stack[top++] = node.left + 1;
            }
        }
        return hit ? best : -1.0f;
    }

private:
    static bool boxHit(const BvhNode& n, V3 o, V3 inv, float tMin, float tMax) {
        float t0 = (n.lo[0] - o.x) * inv.x, t1 = (n.hi[0] - o.x) * inv.x;
        float near = std::min(t0, t1), far = std::max(t0, t1);
        t0 = (n.lo[1] - o.y) * inv.y;
        t1 = (n.hi[1] - o.y) * inv.y;
        near = std::max(near, std::min(t0, t1));
        far = std::min(far, std::max(t0, t1));
        t0 = (n.lo[2] - o.z) * inv.z;
        t1 = (n.hi[2] - o.z) * inv.z;
        near = std::max(near, std::min(t0, t1));
        far = std::min(far, std::max(t0, t1));
        return far >= near && far > tMin && near < tMax;
    }

    // Moller-Trumbore, both faces (the caller already kept only the casters that
    // block this sun from the side the ray comes from).
    static float triangleHit(const Caster& c, V3 o, V3 d) {
        const V3 p = cross(d, c.e2);
        const float det = dot(c.e1, p);
        if (fabsf(det) < 1e-12f) {
            return -1.0f;
        }
        const float inv = 1.0f / det;
        const V3 s = o - c.p0;
        const float u = dot(s, p) * inv;
        if (u < 0.0f || u > 1.0f) {
            return -1.0f;
        }
        const V3 q = cross(s, c.e1);
        const float v = dot(d, q) * inv;
        if (v < 0.0f || u + v > 1.0f) {
            return -1.0f;
        }
        return dot(c.e2, q) * inv;
    }

    void bounds(int32_t first, int32_t count, BvhNode* node, float clo[3], float chi[3]) const {
        for (int k = 0; k < 3; k++) {
            node->lo[k] = clo[k] = 1e30f;
            node->hi[k] = chi[k] = -1e30f;
        }
        for (int32_t i = first; i < first + count; i++) {
            const Caster& c = (*casters_)[(size_t)order_[(size_t)i]];
            const V3 pts[3] = {c.p0, c.p0 + c.e1, c.p0 + c.e2};
            for (int j = 0; j < 3; j++) {
                const float xyz[3] = {pts[j].x, pts[j].y, pts[j].z};
                for (int k = 0; k < 3; k++) {
                    node->lo[k] = std::min(node->lo[k], xyz[k]);
                    node->hi[k] = std::max(node->hi[k], xyz[k]);
                }
            }
            const float cc[3] = {c.centre.x, c.centre.y, c.centre.z};
            for (int k = 0; k < 3; k++) {
                clo[k] = std::min(clo[k], cc[k]);
                chi[k] = std::max(chi[k], cc[k]);
            }
        }
        for (int k = 0; k < 3; k++) {   // a little slack for the box test's rounding
            node->lo[k] -= 1e-3f;
            node->hi[k] += 1e-3f;
        }
    }

    void split(int32_t index, int32_t first, int32_t count) {
        float clo[3], chi[3];
        BvhNode node;
        bounds(first, count, &node, clo, chi);
        node.first = first;
        node.count = count;
        if (count <= 4) {
            nodes_[(size_t)index] = node;
            return;
        }
        int axis = 0;
        for (int k = 1; k < 3; k++) {
            if (chi[k] - clo[k] > chi[axis] - clo[axis]) {
                axis = k;
            }
        }
        const std::vector<Caster>& cs = *casters_;
        const int32_t mid = first + count / 2;
        std::nth_element(order_.begin() + first, order_.begin() + mid, order_.begin() + first + count,
                         [&cs, axis](int32_t a, int32_t b) {
                             const float ca = axis == 0 ? cs[(size_t)a].centre.x
                                              : axis == 1 ? cs[(size_t)a].centre.y
                                                          : cs[(size_t)a].centre.z;
                             const float cb = axis == 0 ? cs[(size_t)b].centre.x
                                              : axis == 1 ? cs[(size_t)b].centre.y
                                                          : cs[(size_t)b].centre.z;
                             return ca < cb || (ca == cb && a < b);
                         });
        node.left = (int32_t)nodes_.size();
        nodes_[(size_t)index] = node;
        nodes_.push_back(BvhNode());
        nodes_.push_back(BvhNode());
        split(node.left, first, mid - first);
        split(node.left + 1, mid, first + count - mid);
    }

    const std::vector<Caster>* casters_ = nullptr;
    std::vector<int32_t> order_;
    std::vector<BvhNode> nodes_;
};

void collectCasters(const std::vector<BakeTriangle>& triangles, V3 toSun,
                    std::vector<Caster>* out) {
    out->clear();
    for (size_t i = 0; i < triangles.size(); i++) {
        const BakeTriangle& t = triangles[i];
        if (!blocksThisSun(t, toSun)) {
            continue;
        }
        Caster c;
        c.p0 = v3(t.p[0]);
        c.e1 = v3(t.p[1]) - c.p0;
        c.e2 = v3(t.p[2]) - c.p0;
        c.centre = c.p0 + (c.e1 + c.e2) * (1.0f / 3.0f);
        out->push_back(c);
    }
}

// ---------------------------------------------------------------------------
// The sides and their charts.
// ---------------------------------------------------------------------------
struct Side {
    int32_t triangle;   // index into the input
    bool back;          // the back of a DoubleSided triangle (corners p0, p2, p1)
    V3 normal;          // the side's unit normal (the one the shader would light)
    float area;
    int32_t rig;
    bool sunMayReach;   // an open arena's side facing the sun: rays decide
    float lit[3];       // F / 2 with the sun, and without it
    float shadow[3];
    float ratio;        // F_shadow / F_lit, the mean of the three channels' (<= 1)
    uint32_t colour;    // lit as bytes R G B A in memory: the vertex colour
};

struct Chart {
    std::vector<int32_t> members;   // indices into the side list
    V3 normal;
    V3 axisU, axisV;
    float uMin = 0.0f, uMax = 0.0f, vMin = 0.0f, vMax = 0.0f;
    // Packing.
    float texel = 0.0f;
    int32_t width = 0, height = 0;
    int32_t atX = 0, atY = 0;
};

const int32_t kPad = 1;
const int32_t kAtlasMostSide = 1024;
const int32_t kProbeMostSide = 4096;

int32_t roundUpToBlock(int32_t v) {
    return (v + kSunBakeBlock - 1) & ~(kSunBakeBlock - 1);
}

void sizeChart(Chart* c, float texel, int32_t mostSide) {
    // A chart is never wider than `mostSide` texels with its gutters: a huge
    // floor coarsens on its own instead of the level. The rectangle is rounded
    // up to the ETC1 block (the extra is gutter, filled by the dilation).
    const float most = (float)(mostSide - 2 * kPad - kSunBakeBlock);
    float t = texel;
    const float du = c->uMax - c->uMin;
    const float dv = c->vMax - c->vMin;
    if (du / t > most) {
        t = du / most;
    }
    if (dv / t > most) {
        t = dv / most;
    }
    c->texel = t;
    const int32_t nu = std::max(1, (int32_t)ceilf(du / t - 1e-4f));
    const int32_t nv = std::max(1, (int32_t)ceilf(dv / t - 1e-4f));
    c->width = roundUpToBlock(nu + 2 * kPad);
    c->height = roundUpToBlock(nv + 2 * kPad);
}

// Shelf packing at a given width, after `blocks` 4x4 colour blocks laid along
// the top rows; the height used, or -1 when something is wider than the atlas.
// Every position is a multiple of 4 (the widths and heights are).
int32_t packShelves(std::vector<Chart>& charts, const std::vector<int32_t>& order, int32_t width,
                    int32_t blocks, bool place) {
    if (width < kSunBakeBlock) {
        return -1;
    }
    const int32_t perRow = width / kSunBakeBlock;
    const int32_t blockRows = (blocks + perRow - 1) / perRow;
    int32_t x = 0;
    int32_t shelfY = blockRows * kSunBakeBlock;
    int32_t shelfH = 0;
    for (size_t k = 0; k < order.size(); k++) {
        Chart& c = charts[(size_t)order[k]];
        if (c.width > width) {
            return -1;
        }
        if (x + c.width > width) {
            shelfY += shelfH;
            x = 0;
            shelfH = 0;
        }
        if (place) {
            c.atX = x;
            c.atY = shelfY;
        }
        x += c.width;
        shelfH = std::max(shelfH, c.height);
    }
    return shelfY + shelfH;
}

int32_t nextPow2(int32_t v) {
    int32_t p = 8;
    while (p < v) {
        p *= 2;
    }
    return p;
}

// Edge function sign test in chart texel space, either winding, edges included.
bool insideTriangle(const float tx[3], const float ty[3], float x, float y) {
    const float d0 = (tx[1] - tx[0]) * (y - ty[0]) - (ty[1] - ty[0]) * (x - tx[0]);
    const float d1 = (tx[2] - tx[1]) * (y - ty[1]) - (ty[2] - ty[1]) * (x - tx[1]);
    const float d2 = (tx[0] - tx[2]) * (y - ty[2]) - (ty[0] - ty[2]) * (x - tx[2]);
    const bool neg = d0 < 0.0f || d1 < 0.0f || d2 < 0.0f;
    const bool pos = d0 > 0.0f || d1 > 0.0f || d2 > 0.0f;
    return !(neg && pos);
}

struct ChartSampling {
    int64_t rays = 0;
    int64_t shadowed = 0;
    int32_t known = 0;
    bool sawLit = false;
    bool sawShadow = false;
};

// A chart's texels: raysPerAxis^2 sub-points a texel, each one that falls inside
// one of the chart's sides cast to the sun from ON that triangle; the sub-point's
// shade is 1 where the sun got through and its side's F_shadow / F_lit where it
// did not, and the texel's shade their mean (`grey`, one float a texel: m = 1 -
// A * (1 - r)). With `dilate` every texel no sub-point landed in takes its
// neighbours' mean, so a vertex near the chart's edge never filters in a value
// from off it. `stopWhenMixed` ends at the first lit and shadowed pair (the
// probe's question is only "uniform or not").
void sampleChart(const Chart& chart, const std::vector<BakeTriangle>& triangles,
                 const std::vector<Side>& sides, const Bvh& bvh, V3 toSun, int32_t rays,
                 float offset, bool stopWhenMixed, bool dilate, std::vector<float>* grey,
                 ChartSampling* stats) {
    const int32_t w = chart.width;
    const int32_t h = chart.height;
    const float inv = 1.0f / chart.texel;
    std::vector<uint8_t> covered((size_t)w * h * (size_t)(rays * rays), 0);
    std::vector<float> sum((size_t)w * h, 0.0f);
    std::vector<int32_t> sampleCount((size_t)w * h, 0);
    for (size_t m = 0; m < chart.members.size(); m++) {
        const Side& s = sides[(size_t)chart.members[m]];
        const BakeTriangle& t = triangles[(size_t)s.triangle];
        float tx[3], ty[3];
        for (int k = 0; k < 3; k++) {
            const V3 p = v3(t.p[k]);
            tx[k] = (dot(p, chart.axisU) - chart.uMin) * inv + (float)kPad;
            ty[k] = (dot(p, chart.axisV) - chart.vMin) * inv + (float)kPad;
        }
        const float twice = (tx[1] - tx[0]) * (ty[2] - ty[0]) - (ty[1] - ty[0]) * (tx[2] - tx[0]);
        if (fabsf(twice) < 1e-9f) {
            continue;   // edge-on in this chart's projection: its neighbours cover it
        }
        const float invArea = 1.0f / twice;
        const int32_t x0 = std::max(0, (int32_t)floorf(std::min(tx[0], std::min(tx[1], tx[2]))));
        const int32_t x1 = std::min(w - 1, (int32_t)floorf(std::max(tx[0], std::max(tx[1], tx[2]))));
        const int32_t y0 = std::max(0, (int32_t)floorf(std::min(ty[0], std::min(ty[1], ty[2]))));
        const int32_t y1 = std::min(h - 1, (int32_t)floorf(std::max(ty[0], std::max(ty[1], ty[2]))));
        for (int32_t y = y0; y <= y1; y++) {
            for (int32_t x = x0; x <= x1; x++) {
                for (int32_t sy = 0; sy < rays; sy++) {
                    for (int32_t sx = 0; sx < rays; sx++) {
                        const size_t bit =
                            ((size_t)y * w + x) * (size_t)(rays * rays) + (size_t)(sy * rays + sx);
                        if (covered[bit]) {
                            continue;
                        }
                        const float px = (float)x + ((float)sx + 0.5f) / (float)rays;
                        const float py = (float)y + ((float)sy + 0.5f) / (float)rays;
                        if (!insideTriangle(tx, ty, px, py)) {
                            continue;
                        }
                        covered[bit] = 1;
                        bool blocked = false;
                        if (s.sunMayReach) {
                            const float w1 = ((px - tx[0]) * (ty[2] - ty[0]) -
                                              (py - ty[0]) * (tx[2] - tx[0])) * invArea;
                            const float w2 = ((tx[1] - tx[0]) * (py - ty[0]) -
                                              (ty[1] - ty[0]) * (px - tx[0])) * invArea;
                            const V3 point = v3(t.p[0]) * (1.0f - w1 - w2) + v3(t.p[1]) * w1 +
                                             v3(t.p[2]) * w2;
                            blocked = bvh.nearest(point + s.normal * offset, toSun, 1e-3f, 1e30f,
                                                  true) >= 0.0f;
                            stats->rays++;
                            stats->shadowed += blocked ? 1 : 0;
                        }
                        stats->sawShadow = stats->sawShadow || blocked;
                        stats->sawLit = stats->sawLit || !blocked;
                        const size_t at = (size_t)y * w + x;
                        sum[at] += blocked ? s.ratio : 1.0f;
                        sampleCount[at]++;
                        if (stopWhenMixed && stats->sawLit && stats->sawShadow) {
                            return;
                        }
                    }
                }
            }
        }
    }
    grey->assign((size_t)w * h, -1.0f);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        if (sampleCount[i] > 0) {
            (*grey)[i] = sum[i] / (float)sampleCount[i];
            stats->known++;
        }
    }
    if (stats->known == 0) {
        // A sliver no sub-point landed in: its sides' centres decide.
        float fill = 0.0f;
        int32_t n = 0;
        for (size_t m = 0; m < chart.members.size(); m++) {
            const Side& s = sides[(size_t)chart.members[m]];
            const BakeTriangle& t = triangles[(size_t)s.triangle];
            const V3 centre = (v3(t.p[0]) + v3(t.p[1]) + v3(t.p[2])) * (1.0f / 3.0f);
            bool blocked = false;
            if (s.sunMayReach) {
                blocked = bvh.nearest(centre + s.normal * offset, toSun, 1e-3f, 1e30f, true) >= 0.0f;
                stats->rays++;
            }
            stats->sawShadow = stats->sawShadow || blocked;
            stats->sawLit = stats->sawLit || !blocked;
            fill += blocked ? s.ratio : 1.0f;
            n++;
        }
        for (size_t i = 0; i < (size_t)w * h; i++) {
            (*grey)[i] = n ? fill / (float)n : 1.0f;
        }
        return;
    }
    if (!dilate) {
        return;
    }
    for (int pass = 0; pass < w + h; pass++) {
        std::vector<float> next = *grey;
        bool changed = false;
        for (int32_t y = 0; y < h; y++) {
            for (int32_t x = 0; x < w; x++) {
                if ((*grey)[(size_t)y * w + x] >= 0.0f) {
                    continue;
                }
                float acc = 0.0f;
                int32_t n = 0;
                for (int32_t dy = -1; dy <= 1; dy++) {
                    for (int32_t dx = -1; dx <= 1; dx++) {
                        const int32_t nx = x + dx, ny = y + dy;
                        if ((dx || dy) && nx >= 0 && ny >= 0 && nx < w && ny < h &&
                            (*grey)[(size_t)ny * w + nx] >= 0.0f) {
                            acc += (*grey)[(size_t)ny * w + nx];
                            n++;
                        }
                    }
                }
                if (n > 0) {
                    next[(size_t)y * w + x] = acc / (float)n;
                    changed = true;
                }
            }
        }
        grey->swap(next);
        if (!changed) {
            break;
        }
    }
    for (size_t i = 0; i < grey->size(); i++) {
        if ((*grey)[i] < 0.0f) {
            (*grey)[i] = 1.0f;
        }
    }
}

uint8_t toByte(float v) {
    const float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return (uint8_t)(c * 255.0f + 0.5f);
}

} // namespace

size_t sunBakeAtlasBytes(SunBakeFormat format, int32_t w, int32_t h) {
    const size_t texels = (size_t)w * (size_t)h;
    return format == kSunBakeL4 ? texels / 2u : texels;
}

void sunBakeLight(const BakeRig& rig, const float normal[3], const float sunDirection[3],
                  bool sunReaches, float outHalf[3]) {
    const float t = normal[2] * 0.5f + 0.5f;
    float facing = -(sunDirection[0] * normal[0] + sunDirection[1] * normal[1] +
                     sunDirection[2] * normal[2]);
    facing = facing > 0.0f ? facing : 0.0f;
    for (int c = 0; c < 3; c++) {
        float light = rig.ground[c] + (rig.sky[c] - rig.ground[c]) * t;
        if (sunReaches) {
            light += rig.sun[c] * facing;
        }
        light *= rig.exposure;
        light = light > 0.0f ? light : 0.0f;
        const float f = 0.5f * powf(light, 0.45454545f);
        outHalf[c] = f < 1.0f ? f : 1.0f;
    }
}

bool sunBlockedAt(const std::vector<BakeTriangle>& triangles, const float sunDirection[3],
                  const float point[3]) {
    const V3 toSun = normalized(V3{-sunDirection[0], -sunDirection[1], -sunDirection[2]});
    std::vector<Caster> casters;
    collectCasters(triangles, toSun, &casters);
    Bvh bvh;
    bvh.build(casters);
    return bvh.nearest(v3(point), toSun, 1e-3f, 1e30f, true) >= 0.0f;
}

void sunBlockedAtMany(const std::vector<BakeTriangle>& triangles, const float sunDirection[3],
                      const std::vector<float>& points, std::vector<uint8_t>* blocked) {
    const V3 toSun = normalized(V3{-sunDirection[0], -sunDirection[1], -sunDirection[2]});
    std::vector<Caster> casters;
    collectCasters(triangles, toSun, &casters);
    Bvh bvh;
    bvh.build(casters);
    blocked->assign(points.size() / 3, 0);
    for (size_t i = 0; i + 2 < points.size(); i += 3) {
        const V3 p{points[i], points[i + 1], points[i + 2]};
        (*blocked)[i / 3] = bvh.nearest(p, toSun, 1e-3f, 1e30f, true) >= 0.0f ? 1 : 0;
    }
}

bool bakeSunShadows(const std::vector<BakeTriangle>& triangles, const BakeSettings& settings,
                    SunBakeImage* out, BakeReport* report) {
    if (!out) {
        return false;
    }
    BakeReport local;
    BakeReport& rep = report ? *report : local;
    rep = BakeReport();
    const V3 travel = normalized(v3(settings.sunDirection));
    const V3 toSun = travel * -1.0f;
    if (dot(travel, travel) <= 0.0f) {
        return false;
    }
    std::vector<BakeRig> rigs = settings.rigs;
    if (rigs.empty()) {
        BakeRig neutral;
        for (int c = 0; c < 3; c++) {
            neutral.sky[c] = neutral.ground[c] = 1.0f;
        }
        rigs.push_back(neutral);
    }

    // 1. The sides: a Solid receiver's front, a DoubleSided one's front and back.
    std::vector<Side> sides;
    for (size_t i = 0; i < triangles.size(); i++) {
        const BakeTriangle& t = triangles[i];
        rep.receivers += t.receiver ? 1 : 0;
        rep.casters += t.caster ? 1 : 0;
        if (!t.receiver) {
            continue;
        }
        const V3 n = frontCross(t);
        const float twiceArea = length(n);
        if (twiceArea <= 1e-6f) {
            continue;
        }
        const int32_t rig = t.rig >= 0 && (size_t)t.rig < rigs.size() ? t.rig : 0;
        for (int back = 0; back < (t.doubleSided ? 2 : 1); back++) {
            Side s;
            s.triangle = (int32_t)i;
            s.back = back != 0;
            s.normal = n * ((back ? -1.0f : 1.0f) / twiceArea);
            s.area = 0.5f * twiceArea;
            s.rig = rig;
            const BakeRig& r = rigs[(size_t)rig];
            const bool anySun = r.sun[0] > 0.0f || r.sun[1] > 0.0f || r.sun[2] > 0.0f;
            s.sunMayReach = r.shadows && anySun && dot(s.normal, toSun) > 1e-4f;
            const float normal[3] = {s.normal.x, s.normal.y, s.normal.z};
            sunBakeLight(r, normal, settings.sunDirection, true, s.lit);
            // Not shadowed (a covered arena, a side turned from the sun): the
            // shader's light is the sunlit formula as it stands.
            sunBakeLight(r, normal, settings.sunDirection, !s.sunMayReach, s.shadow);
            // The shadow's multiplier: the mean of the three channels' F_shadow /
            // F_lit (the real-time path mixes per channel; the report says what
            // the mean costs), and the vertex colour.
            float ratio = 0.0f;
            for (int c = 0; c < 3; c++) {
                ratio += s.lit[c] > 1e-6f ? s.shadow[c] / s.lit[c] : 1.0f;
            }
            s.ratio = std::min(1.0f, std::max(0.0f, ratio / 3.0f));
            s.colour = (uint32_t)toByte(s.lit[0]) | (uint32_t)toByte(s.lit[1]) << 8 |
                       (uint32_t)toByte(s.lit[2]) << 16 | 0xFF000000u;
            if (s.sunMayReach) {
                // What a fully shadowed texel shows against the exact per-channel
                // F_shadow, in F / 2 bytes: the mean ratio, and it rounded to the
                // L4 and L8 steps.
                const float q4 = roundf(s.ratio * 15.0f) / 15.0f;
                const float q8 = roundf(s.ratio * 255.0f) / 255.0f;
                for (int c = 0; c < 3; c++) {
                    const float lit = (float)toByte(s.lit[c]);
                    const float exact = s.shadow[c] * 255.0f;
                    rep.ratioError = std::max(rep.ratioError, fabsf(lit * s.ratio - exact));
                    rep.ratioErrorL4 = std::max(rep.ratioErrorL4, fabsf(lit * q4 - exact));
                    rep.ratioErrorL8 = std::max(rep.ratioErrorL8, fabsf(lit * q8 - exact));
                }
            }
            rep.sunSides += s.sunMayReach ? 1 : 0;
            sides.push_back(s);
        }
    }
    rep.sides = (int32_t)sides.size();
    if (sides.empty()) {
        return false;
    }

    // 2. Charts: grown from the largest unassigned side across shared corner
    // positions (their bits) while a neighbour's normal stays within the cone of
    // the seed's and its rig is the seed's: one side of one planar polygon.
    std::vector<std::vector<int32_t>> neighbours(sides.size());
    {
        std::unordered_map<uint64_t, std::vector<int32_t>> corners;
        for (size_t i = 0; i < sides.size(); i++) {
            const BakeTriangle& t = triangles[(size_t)sides[i].triangle];
            for (int k = 0; k < 3; k++) {
                uint32_t bits[3];
                memcpy(bits, t.p[k], 12);
                const uint64_t key = ((uint64_t)bits[0] * 0x9E3779B97F4A7C15ull) ^
                                     ((uint64_t)bits[1] * 0xC2B2AE3D27D4EB4Full) ^
                                     ((uint64_t)bits[2] * 0x165667B19E3779F9ull);
                std::vector<int32_t>& list = corners[key];
                for (size_t m = 0; m < list.size(); m++) {
                    const int32_t j = list[m];
                    const BakeTriangle& u = triangles[(size_t)sides[(size_t)j].triangle];
                    bool shares = false;
                    for (int q = 0; q < 3 && !shares; q++) {
                        shares = memcmp(u.p[q], t.p[k], 12) == 0;
                    }
                    if (shares && j != (int32_t)i) {   // (not a hash collision)
                        neighbours[i].push_back(j);
                        neighbours[(size_t)j].push_back((int32_t)i);
                    }
                }
                list.push_back((int32_t)i);
            }
        }
        for (size_t i = 0; i < neighbours.size(); i++) {
            std::sort(neighbours[i].begin(), neighbours[i].end());
            neighbours[i].erase(std::unique(neighbours[i].begin(), neighbours[i].end()),
                                neighbours[i].end());
        }
    }
    std::vector<Chart> charts;
    std::vector<int32_t> chartOfSide(sides.size(), -1);
    {
        std::vector<int32_t> seeds(sides.size());
        for (size_t i = 0; i < seeds.size(); i++) {
            seeds[i] = (int32_t)i;
        }
        std::sort(seeds.begin(), seeds.end(), [&sides](int32_t a, int32_t b) {
            if (sides[(size_t)a].area != sides[(size_t)b].area) {
                return sides[(size_t)a].area > sides[(size_t)b].area;
            }
            return a < b;
        });
        std::vector<int32_t> queue;
        for (size_t s = 0; s < seeds.size(); s++) {
            const int32_t seed = seeds[s];
            if (chartOfSide[(size_t)seed] >= 0) {
                continue;
            }
            const int32_t id = (int32_t)charts.size();
            charts.push_back(Chart());
            const V3 cone = sides[(size_t)seed].normal;
            const int32_t rig = sides[(size_t)seed].rig;
            queue.clear();
            queue.push_back(seed);
            chartOfSide[(size_t)seed] = id;
            for (size_t head = 0; head < queue.size(); head++) {
                const int32_t i = queue[head];
                charts[(size_t)id].members.push_back(i);
                for (size_t m = 0; m < neighbours[(size_t)i].size(); m++) {
                    const int32_t j = neighbours[(size_t)i][m];
                    if (chartOfSide[(size_t)j] < 0 && sides[(size_t)j].rig == rig &&
                        dot(sides[(size_t)j].normal, cone) > settings.chartCone) {
                        chartOfSide[(size_t)j] = id;
                        queue.push_back(j);
                    }
                }
            }
        }
    }
    for (size_t c = 0; c < charts.size(); c++) {
        Chart& chart = charts[c];
        V3 n{0.0f, 0.0f, 0.0f};
        for (size_t m = 0; m < chart.members.size(); m++) {
            const Side& s = sides[(size_t)chart.members[m]];
            n = n + s.normal * s.area;
        }
        chart.normal = normalized(n);
        const V3 helper = fabsf(chart.normal.x) < 0.9f ? V3{1.0f, 0.0f, 0.0f} : V3{0.0f, 1.0f, 0.0f};
        const V3 baseU = normalized(cross(helper, chart.normal));
        const V3 baseV = cross(chart.normal, baseU);
        // THE ORIENTATION: the chart's rectangle is turned to the projected edge
        // direction that gives the least area. Candidates: the longest edges,
        // deduplicated by angle.
        std::vector<std::pair<float, V3>> edges;
        for (size_t m = 0; m < chart.members.size(); m++) {
            const BakeTriangle& t = triangles[(size_t)sides[(size_t)chart.members[m]].triangle];
            for (int k = 0; k < 3; k++) {
                const V3 e = v3(t.p[(k + 1) % 3]) - v3(t.p[k]);
                const float eu = dot(e, baseU), ev = dot(e, baseV);
                const float len = sqrtf(eu * eu + ev * ev);
                if (len > 1e-3f) {
                    edges.push_back(std::make_pair(-len, V3{eu / len, ev / len, 0.0f}));
                }
            }
        }
        std::stable_sort(edges.begin(), edges.end(),
                         [](const std::pair<float, V3>& a, const std::pair<float, V3>& b) {
                             return a.first < b.first;
                         });
        float bestArea = 1e38f;
        chart.axisU = baseU;
        chart.axisV = baseV;
        int32_t tried = 0;
        for (size_t e = 0; e < edges.size() && tried < 48; e++) {
            const float cu = edges[e].second.x, cv = edges[e].second.y;
            bool seen = false;
            for (size_t f = 0; f < e && !seen; f++) {
                const float du = edges[f].second.x, dv = edges[f].second.y;
                const float along = fabsf(cu * du + cv * dv), across = fabsf(cu * dv - cv * du);
                seen = along > 0.9999f || across > 0.9999f;
            }
            if (seen) {
                continue;
            }
            tried++;
            const V3 u = baseU * cu + baseV * cv;
            const V3 v = cross(chart.normal, u);
            float ulo = 1e30f, uhi = -1e30f, vlo = 1e30f, vhi = -1e30f;
            for (size_t m = 0; m < chart.members.size(); m++) {
                const BakeTriangle& t = triangles[(size_t)sides[(size_t)chart.members[m]].triangle];
                for (int k = 0; k < 3; k++) {
                    const float pu = dot(v3(t.p[k]), u), pv = dot(v3(t.p[k]), v);
                    ulo = std::min(ulo, pu);
                    uhi = std::max(uhi, pu);
                    vlo = std::min(vlo, pv);
                    vhi = std::max(vhi, pv);
                }
            }
            const float area = (uhi - ulo + 1.0f) * (vhi - vlo + 1.0f);
            if (area < bestArea * 0.999f) {
                bestArea = area;
                chart.axisU = u;
                chart.axisV = v;
            }
        }
        chart.uMin = chart.vMin = 1e30f;
        chart.uMax = chart.vMax = -1e30f;
        for (size_t m = 0; m < chart.members.size(); m++) {
            const BakeTriangle& t = triangles[(size_t)sides[(size_t)chart.members[m]].triangle];
            for (int k = 0; k < 3; k++) {
                const float u = dot(v3(t.p[k]), chart.axisU);
                const float v = dot(v3(t.p[k]), chart.axisV);
                chart.uMin = std::min(chart.uMin, u);
                chart.uMax = std::max(chart.uMax, u);
                chart.vMin = std::min(chart.vMin, v);
                chart.vMax = std::max(chart.vMax, v);
            }
        }
    }

    std::vector<Caster> casters;
    collectCasters(triangles, toSun, &casters);
    rep.effectiveCasters = (int32_t)casters.size();
    Bvh bvh;
    bvh.build(casters);
    const int32_t rays = settings.raysPerAxis >= 1 ? settings.raysPerAxis : 1;
    const float offset = settings.normalOffset;

    // 3. Probe: a chart the sun may reach is sampled with one ray a texel at the
    // aimed-for density, stopping at the first disagreement. A chart the sun
    // reaches everywhere, or that it cannot reach at all, is m = 1 (the white
    // block); one it reaches nowhere is ONE SHADE PER SIDE (its ratio), a 4x4
    // grey block shared by every side of that shade. Only the charts a shadow
    // edge crosses get texels of their own. (A shadow narrower
    // than a texel of the aimed-for density on an otherwise uniform chart is
    // lost; the atlas could not have held it either.)
    std::vector<int32_t> mixed;
    std::vector<int8_t> sideState(sides.size(), 0);   // 1 all lit, 2 all shadow, 3 mixed
    for (size_t c = 0; c < charts.size(); c++) {
        bool mayReach = false;
        for (size_t m = 0; m < charts[c].members.size() && !mayReach; m++) {
            mayReach = sides[(size_t)charts[c].members[m]].sunMayReach;
        }
        int8_t state = 1;
        if (mayReach) {
            Chart probe = charts[c];
            sizeChart(&probe, settings.texelSize > 0.01f ? settings.texelSize : 0.01f,
                      kProbeMostSide);
            std::vector<float> grey;
            ChartSampling stats;
            sampleChart(probe, triangles, sides, bvh, toSun, 1, offset, true, false, &grey, &stats);
            rep.probeRays += stats.rays;
            state = stats.sawLit && stats.sawShadow ? 3 : (stats.sawShadow ? 2 : 1);
        }
        if (state == 3) {
            mixed.push_back((int32_t)c);
        }
        for (size_t m = 0; m < charts[c].members.size(); m++) {
            sideState[(size_t)charts[c].members[m]] = state;
        }
    }

    // The grey blocks: block 0 is WHITE (m = 1: the sun everywhere, or a side it
    // cannot reach), then one per distinct shade of a side the sun reaches
    // nowhere (its ratio, a byte), in side order (deterministic).
    std::vector<uint32_t> blockColours;
    blockColours.push_back(255u);
    std::unordered_map<uint32_t, int32_t> blockOfColour;
    blockOfColour[255u] = 0;
    std::vector<int32_t> blockOfSide(sides.size(), -1);
    for (size_t i = 0; i < sides.size(); i++) {
        if (sideState[i] == 3) {
            continue;
        }
        const uint32_t key = sideState[i] == 2 ? (uint32_t)toByte(sides[i].ratio) : 255u;
        std::unordered_map<uint32_t, int32_t>::const_iterator found = blockOfColour.find(key);
        if (found == blockOfColour.end()) {
            blockOfColour[key] = (int32_t)blockColours.size();
            blockOfSide[i] = (int32_t)blockColours.size();
            blockColours.push_back(key);
        } else {
            blockOfSide[i] = found->second;
        }
        rep.uniformSides++;
    }
    const int32_t blocks = (int32_t)blockColours.size();
    rep.colourBlocks = blocks;
    if (blocks + (int32_t)mixed.size() > 65535) {
        return false;
    }

    // 4. Pack the blocks and the mixed charts, coarsening until the atlas fits.
    float texel = settings.texelSize > 0.01f ? settings.texelSize : 0.01f;
    const int32_t chartMost =
        std::min(kAtlasMostSide, std::max(4 * kSunBakeBlock, (int32_t)settings.chartMostSide));
    int32_t atlasW = 0, atlasH = 0;
    std::vector<int32_t> order(mixed);
    for (int step = 0; step < 60 && atlasW == 0; step++) {
        for (size_t k = 0; k < mixed.size(); k++) {
            sizeChart(&charts[(size_t)mixed[k]], texel, chartMost);
        }
        std::sort(order.begin(), order.end(), [&charts](int32_t a, int32_t b) {
            const Chart& ca = charts[(size_t)a];
            const Chart& cb = charts[(size_t)b];
            if (ca.height != cb.height) return ca.height > cb.height;
            if (ca.width != cb.width) return ca.width > cb.width;
            return a < b;
        });
        int64_t bestArea = -1;
        for (int32_t w = 64; w <= 1024; w *= 2) {
            const int32_t used = packShelves(charts, order, w, blocks, false);
            if (used < 0) {
                continue;
            }
            const int32_t h = nextPow2(used);
            if (h > 1024) {
                continue;
            }
            const int64_t area = (int64_t)w * h;
            if (sunBakeAtlasBytes(kSunBakeL4, w, h) > settings.budgetBytes) {
                continue;
            }
            // The smallest; at a tie the squarer (then the narrower).
            const int32_t skew = abs((int32_t)log2f((float)w) - (int32_t)log2f((float)h));
            const int32_t bestSkew =
                atlasW ? abs((int32_t)log2f((float)atlasW) - (int32_t)log2f((float)atlasH)) : 99;
            if (bestArea < 0 || area < bestArea || (area == bestArea && skew < bestSkew)) {
                bestArea = area;
                atlasW = w;
                atlasH = h;
            }
        }
        if (atlasW == 0) {
            texel *= 1.12f;
            rep.densitySteps++;
        }
    }
    if (atlasW == 0) {
        return false;
    }
    packShelves(charts, order, atlasW, blocks, true);
    if (getenv("SUNBAKE_DEBUG")) {
        std::vector<int32_t> big(order);
        std::sort(big.begin(), big.end(), [&charts](int32_t a, int32_t b) {
            return (int64_t)charts[(size_t)a].width * charts[(size_t)a].height >
                   (int64_t)charts[(size_t)b].width * charts[(size_t)b].height;
        });
        for (size_t k = 0; k < big.size() && k < 12; k++) {
            const Chart& c = charts[(size_t)big[k]];
            fprintf(stderr, "  chart %d: %dx%d texels, %.0fx%.0f units, %d sides, n=(%.2f,%.2f,%.2f)\n",
                    (int)big[k], (int)c.width, (int)c.height, (double)(c.uMax - c.uMin),
                    (double)(c.vMax - c.vMin), (int)c.members.size(), (double)c.normal.x,
                    (double)c.normal.y, (double)c.normal.z);
        }
    }
    rep.charts = (int32_t)mixed.size();
    rep.atlasWidth = atlasW;
    rep.atlasHeight = atlasH;
    rep.texelSize = texel;
    rep.atlasBytes = sunBakeAtlasBytes(settings.format, atlasW, atlasH);
    rep.atlasBytesL4 = sunBakeAtlasBytes(kSunBakeL4, atlasW, atlasH);
    rep.atlasBytesL8 = sunBakeAtlasBytes(kSunBakeL8, atlasW, atlasH);

    // 5. The atlas: the blocks (constant mappings to their centres), then each
    // mixed chart's texels. The file's charts are the blocks first, then the
    // mixed charts.
    out->format = settings.format;
    out->atlasWidth = atlasW;
    out->atlasHeight = atlasH;
    out->texelSize = texel;
    out->sunDirection[0] = settings.sunDirection[0];
    out->sunDirection[1] = settings.sunDirection[1];
    out->sunDirection[2] = settings.sunDirection[2];
    out->grey.assign((size_t)atlasW * (size_t)atlasH, 255);
    out->charts.assign((size_t)blocks + mixed.size(), SunBakeChart());
    const int32_t perRow = atlasW / kSunBakeBlock;
    for (int32_t b = 0; b < blocks; b++) {
        const int32_t bx = (b % perRow) * kSunBakeBlock;
        const int32_t by = (b / perRow) * kSunBakeBlock;
        for (int32_t y = 0; y < kSunBakeBlock; y++) {
            for (int32_t x = 0; x < kSunBakeBlock; x++) {
                out->grey[(size_t)(by + y) * (size_t)atlasW + (size_t)(bx + x)] =
                    (uint8_t)blockColours[(size_t)b];
            }
        }
        out->charts[(size_t)b].x[3] = (float)bx + 0.5f * (float)kSunBakeBlock;
        out->charts[(size_t)b].y[3] = (float)by + 0.5f * (float)kSunBakeBlock;
    }
    std::vector<int32_t> fileChart(charts.size(), -1);
    for (size_t k = 0; k < mixed.size(); k++) {
        const Chart& chart = charts[(size_t)mixed[k]];
        fileChart[(size_t)mixed[k]] = blocks + (int32_t)k;
        // The mapping: x = (dot(p, U) - uMin) / texel + pad + atX.
        SunBakeChart& map = out->charts[(size_t)blocks + k];
        const float inv = 1.0f / chart.texel;
        map.x[0] = chart.axisU.x * inv;
        map.x[1] = chart.axisU.y * inv;
        map.x[2] = chart.axisU.z * inv;
        map.x[3] = (float)(kPad + chart.atX) - chart.uMin * inv;
        map.y[0] = chart.axisV.x * inv;
        map.y[1] = chart.axisV.y * inv;
        map.y[2] = chart.axisV.z * inv;
        map.y[3] = (float)(kPad + chart.atY) - chart.vMin * inv;

        std::vector<float> grey;
        ChartSampling stats;
        sampleChart(chart, triangles, sides, bvh, toSun, rays, offset, false, true, &grey, &stats);
        rep.rays += stats.rays;
        rep.shadowedSamples += stats.shadowed;
        rep.chartTexels += (int64_t)chart.width * chart.height;
        for (int32_t y = 0; y < chart.height; y++) {
            for (int32_t x = 0; x < chart.width; x++) {
                out->grey[(size_t)(chart.atY + y) * (size_t)atlasW + (size_t)(chart.atX + x)] =
                    toByte(grey[(size_t)y * chart.width + x]);
            }
        }
    }

    // The keys: every side, by its corners' bits in its own order (the back:
    // p0, p2, p1), with its chart and its vertex colour; a duplicate (the same
    // three positions twice) keeps the first.
    {
        std::vector<std::pair<uint64_t, int32_t>> keyed;
        keyed.reserve(sides.size());
        for (size_t i = 0; i < sides.size(); i++) {
            const BakeTriangle& t = triangles[(size_t)sides[i].triangle];
            const uint64_t key = sides[i].back ? sunBakeTriangleKey(t.p[0], t.p[2], t.p[1])
                                               : sunBakeTriangleKey(t.p[0], t.p[1], t.p[2]);
            keyed.push_back(std::make_pair(key, (int32_t)i));
        }
        std::sort(keyed.begin(), keyed.end());
        out->keys.clear();
        out->keyCharts.clear();
        out->keyColours.clear();
        for (size_t i = 0; i < keyed.size(); i++) {
            if (!out->keys.empty() && out->keys.back() == keyed[i].first) {
                continue;
            }
            const size_t side = (size_t)keyed[i].second;
            const int32_t chart = sideState[side] == 3 ? fileChart[(size_t)chartOfSide[side]]
                                                       : blockOfSide[side];
            out->keys.push_back(keyed[i].first);
            out->keyCharts.push_back((uint16_t)chart);
            out->keyColours.push_back(sides[side].colour);
        }
    }

    // 6. The depth map over every effective caster, in the sun's own frame:
    // X and Y across the light, Z along it (depth grows away from the sun).
    {
        const V3 helper = fabsf(travel.z) < 0.9f ? V3{0.0f, 0.0f, 1.0f} : V3{1.0f, 0.0f, 0.0f};
        const V3 axisX = normalized(cross(helper, travel));
        const V3 axisY = cross(travel, axisX);
        float xlo = 1e30f, xhi = -1e30f, ylo = 1e30f, yhi = -1e30f, zlo = 1e30f, zhi = -1e30f;
        for (size_t i = 0; i < casters.size(); i++) {
            const V3 pts[3] = {casters[i].p0, casters[i].p0 + casters[i].e1, casters[i].p0 + casters[i].e2};
            for (int k = 0; k < 3; k++) {
                xlo = std::min(xlo, dot(pts[k], axisX));
                xhi = std::max(xhi, dot(pts[k], axisX));
                ylo = std::min(ylo, dot(pts[k], axisY));
                yhi = std::max(yhi, dot(pts[k], axisY));
                zlo = std::min(zlo, dot(pts[k], travel));
                zhi = std::max(zhi, dot(pts[k], travel));
            }
        }
        if (!casters.empty()) {
            const int32_t most = settings.depthMaxSide > 8 ? settings.depthMaxSide : 8;
            float cell = settings.depthCell > 0.5f ? settings.depthCell : 0.5f;
            cell = std::max(cell, std::max(xhi - xlo, yhi - ylo) / (float)(most - 2));
            const int32_t dw = std::min(most, (int32_t)ceilf((xhi - xlo) / cell) + 2);
            const int32_t dh = std::min(most, (int32_t)ceilf((yhi - ylo) / cell) + 2);
            const float x0 = xlo - cell;   // one cell of margin each side
            const float y0 = ylo - cell;
            const float zStart = zlo - 1.0f;
            const float zRange = (zhi + 1.0f) - zStart;
            const float zScale = 65534.0f / zRange;
            out->depthWidth = dw;
            out->depthHeight = dh;
            out->depth.assign((size_t)dw * (size_t)dh, 0xFFFFu);
            const float a[12] = {axisX.x / cell, axisX.y / cell, axisX.z / cell, -x0 / cell,
                                 axisY.x / cell, axisY.y / cell, axisY.z / cell, -y0 / cell,
                                 travel.x * zScale, travel.y * zScale, travel.z * zScale,
                                 -zStart * zScale};
            memcpy(out->depthAxes, a, sizeof(a));
            for (int32_t y = 0; y < dh; y++) {
                for (int32_t x = 0; x < dw; x++) {
                    const float sx = x0 + ((float)x + 0.5f) * cell;
                    const float sy = y0 + ((float)y + 0.5f) * cell;
                    const V3 origin = axisX * sx + axisY * sy + travel * zStart;
                    const float t = bvh.nearest(origin, travel, 0.0f, zRange + 1.0f, false);
                    if (t >= 0.0f) {
                        const float q = t * zScale;
                        out->depth[(size_t)y * dw + x] =
                            (uint16_t)std::min(65534.0f, std::max(0.0f, q + 0.5f));
                    }
                }
            }
            rep.depthWidth = dw;
            rep.depthHeight = dh;
            rep.depthCell = cell;
        } else {
            out->depthWidth = 0;
            out->depthHeight = 0;
            out->depth.clear();
        }
    }
    out->colourBlocks = (uint32_t)blocks;
    out->sides = (uint32_t)sides.size();
    out->casterTriangles = (uint32_t)rep.effectiveCasters;
    out->receiverTriangles = (uint32_t)rep.receivers;
    return true;
}

} // namespace mdk

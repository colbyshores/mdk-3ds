// ============================================================================
// bsp.cpp — the BSP box sweep and segment. See bsp.h. A 1:1 port of Bsp.cs.
// ============================================================================

#include "game/bsp.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace mdk {

namespace {

// Beyond this the pass found nothing (5000 at the start, free under 100).
const float kNoHit = 5000.0f;
const float kFreeTime = 100.0f;
const float kMarginFactor = 2.0f;
// Faces flatter than this are climbed by a horizontal slide; steeper are walls.
const float kSteepNz = 0.75f;
const float kPushOut = 0.01f;
// Coordinate planes the box-triangle test projects on: normal components at
// least this.
const float kProjectionMin = 0.1f;
const float kFloorNz = 0.5f;

Vec nodeNormal(const formats::BspNode& n) {
    return Vec{n.normal[0], n.normal[1], n.normal[2]};
}

// Within e of the plane: moved along the normal to exactly +/-e on its side.
// axes == xy keeps Z; otherwise the push is applied in all three.
Vec pushOut3(Vec p, const formats::BspNode& n, float e, bool xy) {
    float d = n.distance(p);
    if (std::fabs(d) > e) {
        return p;
    }
    float k = d >= 0.0f ? e - d : -d - e;
    Vec push = vScale(nodeNormal(n), k);
    if (xy) {
        return Vec{p.x + push.x, p.y + push.y, p.z};
    }
    return vAdd(p, push);
}

// Exact overlap of the rectangle +/- half with a triangle: outcodes, then the
// triangle's v range within the strip |u| <= half.u.
bool rectOverlaps(float q0x, float q0y, float q1x, float q1y, float q2x, float q2y, float hx,
                  float hy) {
    auto code = [&](float x, float y) {
        return (x < -hx ? 1 : 0) | (x > hx ? 2 : 0) | (y < -hy ? 4 : 0) | (y > hy ? 8 : 0);
    };
    int c0 = code(q0x, q0y), c1 = code(q1x, q1y), c2 = code(q2x, q2y);
    if (c0 == 0 || c1 == 0 || c2 == 0) {
        return true;
    }
    if ((c0 & c1 & c2) != 0) {
        return false;
    }
    float low = FLT_MAX;
    float high = -FLT_MAX;
    auto add = [&](float v) {
        if (v < low) {
            low = v;
        }
        if (v > high) {
            high = v;
        }
    };
    float px[3] = {q0x, q1x, q2x};
    float py[3] = {q0y, q1y, q2y};
    for (int i = 0; i < 3; i++) {
        int j = (i + 1) % 3;
        if (std::fabs(px[i]) <= hx) {
            add(py[i]);
        }
        // Edge crossings of u = -half.u and u = +half.u.
        const float us[2] = {-hx, hx};
        for (int u = 0; u < 2; u++) {
            if ((px[i] - us[u]) * (px[j] - us[u]) < 0.0f) {
                add(py[i] + (py[j] - py[i]) * (us[u] - px[i]) / (px[j] - px[i]));
            }
        }
    }
    return low <= high && low <= hy && high >= -hy;
}

// Box (centred at the origin) against a triangle, projected on each coordinate
// plane the node's normal isn't almost parallel to (0x409de0).
bool boxOverlaps(Vec v0, Vec v1, Vec v2, Vec h, Vec normal) {
    for (int k = 0; k < 3; k++) {
        if (std::fabs(vComp(normal, k)) < kProjectionMin) {
            continue;
        }
        int a, b;
        if (k == 0) {
            a = 1;
            b = 2;
        } else if (k == 1) {
            a = 2;
            b = 0;
        } else {
            a = 0;
            b = 1;
        }
        if (!rectOverlaps(vComp(v0, a), vComp(v0, b), vComp(v1, a), vComp(v1, b), vComp(v2, a),
                          vComp(v2, b), vComp(h, a), vComp(h, b))) {
            return false;
        }
    }
    return true;
}

} // namespace

int Bsp::sweepBox(Vec a, Vec b, Vec h, int iterations, float slideK, Vec* end, int* node,
                  int* firstNode, int* firstTriangle) {
    *node = kNone;
    *end = a;
    if (firstNode != nullptr) {
        *firstNode = kNone;
    }
    if (firstTriangle != nullptr) {
        *firstTriangle = kNone;
    }
    if (arena_->nodes().empty() || arena_->vertices().empty()) {
        return kNone;
    }

    _a = a;
    _b = b;
    _h = h;
    _d0 = vSub(b, a);
    _slideLimit = slideK * vLenSq(_d0);
    _margin = kMarginFactor * (std::fabs(h.x) + std::fabs(h.y) + std::fabs(h.z));
    _slide = true;
    bool firstRecorded = false;
    while (iterations >= 0 && _slide) {
        _canSlide = iterations != 0;
        _slide = false;
        _hitTriangle = kNone;
        _tBest = kNoHit;
        _d = vSub(_b, _a);
        sweepNode(0);
        if (_slide) {
            // The C# fires its contacts callback here (Final=false); the first such
            // contact is the first contact. Record it once.
            if (!firstRecorded) {
                if (firstNode != nullptr) {
                    *firstNode = _hitNode;
                }
                if (firstTriangle != nullptr) {
                    *firstTriangle = _hitTriangle;
                }
                firstRecorded = true;
            }
            // Slid: the next pass goes from the contact to the slid end.
            _a = _p;
            _b = _s;
        } else if (_hitTriangle != kNone) {
            // The C# fires its callback here too (Final=true).
            if (!firstRecorded) {
                if (firstNode != nullptr) {
                    *firstNode = _hitNode;
                }
                if (firstTriangle != nullptr) {
                    *firstTriangle = _hitTriangle;
                }
                firstRecorded = true;
            }
        }
        iterations--;
    }

    if (_tBest >= kFreeTime) {
        *end = _b;
        return kNone;
    }
    *end = _p;
    *node = _hitNode;
    return _hitTriangle;
}

// One node and what lies behind it (0x409680). Children on the start's side(s)
// first.
void Bsp::sweepNode(int index) {
    const auto& nodes = arena_->nodes();
    while (true) {
        const formats::BspNode& n = nodes[index];
        Vec normal = nodeNormal(n);
        float r = std::fabs(_h.x * normal.x) + std::fabs(_h.y * normal.y) + std::fabs(_h.z * normal.z);
        float dA = n.distance(_a);
        float dB = n.distance(_b);
        if (dA >= -_margin && n.positive >= 0) {
            sweepNode(n.positive);
        }
        if (dA <= _margin && n.negative >= 0) {
            sweepNode(n.negative);
        }

        int sA = sides(dA);
        int sB = sides(dB);
        bool towards = (dA < 0.0f && dA <= dB) || (dA >= 0.0f && dB <= dA);
        if ((sA | sB) == 3 && towards) {
            testPlane(index, n, dA, r);
        }

        // Go on only into the sides the end reaches and the start doesn't.
        int fresh = sB & (sA ^ sB);
        int next = (fresh & 1) != 0 ? n.positive : ((fresh & 2) != 0 ? n.negative : kNone);
        if (next < 0) {
            return;
        }
        index = next;
    }
}

// Bands within the margin of a plane: 1 not far behind, 2 not far in front.
int Bsp::sides(float distance) const {
    return (distance >= -_margin ? 1 : 0) | (distance <= _margin ? 2 : 0);
}

// Where the box touches the plane, the triangles there facing the start, and
// the slide.
void Bsp::testPlane(int index, const formats::BspNode& n, float dA, float r) {
    Vec normal = nodeNormal(n);
    float offset = dA >= 0.0f ? std::min(dA, r) : std::max(dA, -r);
    float den = vDot(_d, normal);
    if (den == 0.0f) {
        return;
    }
    float t = -(dA - offset) / den;
    if (t > _tBest || t > 1.0f) {
        return;
    }
    // Moving away though dB <= dA: rounding of a tiny move far from the origin.
    if (t < 0.0f) {
        return;
    }

    _p = vAdd(_a, vScale(_d, t));
    int first, count;
    if (dA < 0.0f) {
        first = n.backFirst;
        count = n.backCount;
    } else {
        first = n.frontFirst;
        count = n.frontCount;
    }
    int triangle = testTriangles(_p, n, first, count);
    if (triangle == kNone) {
        // Retry where the centre crosses the plane; the contact point stays (a
        // quirk of the original).
        float t2 = std::min(-dA / den, 1.0f);
        triangle = testTriangles(vAdd(_a, vScale(_d, t2)), n, first, count);
        if (triangle != kNone) {
            t = t2;
        }
    }
    if (triangle == kNone) {
        return;
    }

    _hitTriangle = triangle;
    _hitNode = index;
    if (_canSlide) {
        slide(n, t, den, r);
    }
    _tBest = t;
}

// Not too head-on: the end moves along the face. Walls and steep up-slopes
// slide in XY only; floors and down-facing faces are projected in 3D. Too
// head-on: it stops at the contact.
void Bsp::slide(const formats::BspNode& n, float t, float den, float r) {
    Vec normal = nodeNormal(n);
    float along = vDot(_d0, normal);
    if (along * along > _slideLimit) {
        _slide = false;
        _s = _p;
        return;
    }

    float s = (1.0f - t) * den;
    float nz = normal.z;
    _slide = true;
    _s = Vec{_b.x - s * normal.x, _b.y - s * normal.y, _b.z};
    bool steep = (nz >= 0.0f && nz < kSteepNz && den <= 0.0f) ||
                 (nz > -kSteepNz && nz <= 0.0f && den >= 0.0f);
    if (steep) {
        _s = pushOut3(_s, n, r + kPushOut, true);
        return;
    }
    _s.z = _b.z - s * nz;
    _s = pushOut3(_s, n, r + kPushOut, false);
}

// The first solid triangle of a node list the box at p overlaps (0x409c40).
int Bsp::testTriangles(Vec p, const formats::BspNode& n, int first, int count) {
    const auto& V = arena_->vertices();
    const auto& I = arena_->triangleIndices();
    const auto& flags = arena_->triangleFlags();
    Vec normal = nodeNormal(n);
    for (int t = first; t < first + count; t++) {
        if ((flags[t] & kNotSolid) != 0) {
            continue;
        }
        Vec v0 = vSub(V[I[t * 3]], p);
        Vec v1 = vSub(V[I[t * 3 + 1]], p);
        Vec v2 = vSub(V[I[t * 3 + 2]], p);
        if (boxOverlaps(v0, v1, v2, _h, normal)) {
            return t;
        }
    }
    return kNone;
}

int Bsp::segment(Vec a, Vec b, SegmentMode mode, Vec* point, Clip clip) {
    *point = b;
    _passed = (clip == kClipPassesThrough) ? (kNotSolid | arena_->clipFlag()) : kNotSolid;
    if (arena_->nodes().empty()) {
        return kNone;
    }
    Vec hit;
    int triangle = walk(0, a, b, mode, &hit);
    if (triangle != kNone) {
        *point = hit;
    }
    return triangle;
}

const formats::BspNode& Bsp::planeOf(int triangle) const { return arena_->nodes()[nodeOf(triangle)]; }

// The node whose front or back list holds the triangle.
int Bsp::nodeOf(int triangle) const {
    const auto& nodes = arena_->nodes();
    for (size_t i = 0; i < nodes.size(); i++) {
        const formats::BspNode& n = nodes[i];
        if ((triangle >= n.frontFirst && triangle < n.frontFirst + n.frontCount) ||
            (triangle >= n.backFirst && triangle < n.backFirst + n.backCount)) {
            return (int)i;
        }
    }
    return 0;
}

// The nearest triangle a segment crosses (front-to-back walk 0x421470).
int Bsp::walk(int index, Vec a, Vec b, SegmentMode mode, Vec* point) {
    const auto& nodes = arena_->nodes();
    while (true) {
        const formats::BspNode& n = nodes[index];
        float dA = n.distance(a);
        float dB = n.distance(b);
        int near = dA < 0.0f ? n.negative : n.positive;
        if (near >= 0) {
            Vec p;
            int tri = walk(near, a, b, mode, &p);
            if (tri != kNone) {
                *point = p;
                return tri;
            }
        }

        // No strict crossing: the far side isn't visited.
        if (dA * dB >= 0.0f) {
            return kNone;
        }

        int tri;
        Vec pt;
        crossingTriangle(n, a, b, dA, mode, &tri, &pt);
        if (tri != kNone) {
            *point = pt;
            return tri;
        }

        int far = dA < 0.0f ? n.positive : n.negative;
        if (far < 0) {
            return kNone;
        }
        index = far;
    }
}

void Bsp::crossingTriangle(const formats::BspNode& n, Vec a, Vec b, float dA, SegmentMode mode,
                           int* triangle, Vec* point) {
    Vec normal = nodeNormal(n);
    float nz = normal.z;
    if (mode == kSegmentFloor && std::fabs(nz) < kFloorNz) {
        *triangle = kNone;
        return;
    }
    Vec d = vSub(b, a);
    float dot = vDot(d, normal);
    Vec x = vAdd(a, vScale(d, dot == 0.0f ? 1.0f : -dA / dot));
    *point = x;
    if (mode == kSegmentFloor) {
        int first, count;
        if (nz >= kFloorNz) {
            first = n.frontFirst;
            count = n.frontCount;
        } else {
            first = n.backFirst;
            count = n.backCount;
        }
        *triangle = pointTriangle(x, n, first, count);
        return;
    }
    int front = pointTriangle(x, n, n.frontFirst, n.frontCount);
    *triangle = front != kNone ? front : pointTriangle(x, n, n.backFirst, n.backCount);
}

// The first solid triangle of a list holding a point of its plane (0x421350,
// 0x42de60): crossings of a ray along +u, with the normal's largest axis
// dropped.
int Bsp::pointTriangle(Vec x, const formats::BspNode& n, int first, int count) const {
    const auto& V = arena_->vertices();
    const auto& I = arena_->triangleIndices();
    const auto& flags = arena_->triangleFlags();
    Vec abs = vAbs(nodeNormal(n));
    int ua, va;
    if (abs.x >= abs.y && abs.x >= abs.z) {
        ua = 1;
        va = 2;
    } else if (abs.y >= abs.z) {
        ua = 0;
        va = 2;
    } else {
        ua = 0;
        va = 1;
    }
    for (int t = first; t < first + count; t++) {
        if ((flags[t] & _passed) != 0) {
            continue;
        }
        bool inside = false;
        for (int k = 0; k < 3; k++) {
            Vec p = vSub(V[I[t * 3 + k]], x);
            Vec q = vSub(V[I[t * 3 + (k + 1) % 3]], x);
            bool pAbove = vComp(p, va) >= 0.0f;
            bool qAbove = vComp(q, va) >= 0.0f;
            if (pAbove == qAbove) {
                continue;
            }
            // Where the edge crosses v = 0, on the +u side?
            float u = vComp(p, ua) + (vComp(q, ua) - vComp(p, ua)) * (-vComp(p, va)) /
                      (vComp(q, va) - vComp(p, va));
            if (u > 0.0f) {
                inside = !inside;
            }
        }
        if (inside) {
            return t;
        }
    }
    return kNone;
}

} // namespace mdk

#pragma once

// ============================================================================
// vec_util.h — free helpers over the formats' Vec3f.
//
// The collision code is a faithful port of the C#, which uses System.Numerics
// Vector3 with operator overloads and component indexing. The formats' Vec3f
// is plain data with no members (vector_types.h), so the arithmetic lives here
// as free functions. No operators and no templates: house style (AGENTS.md 10).
// ============================================================================

#include <cmath>

#include "formats/vector_types.h"

namespace mdk {

using Vec = formats::Vec3f;

inline Vec vAdd(Vec a, Vec b) { return Vec{a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec vSub(Vec a, Vec b) { return Vec{a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec vScale(Vec a, float s) { return Vec{a.x * s, a.y * s, a.z * s}; }
inline Vec vNeg(Vec a) { return Vec{-a.x, -a.y, -a.z}; }
inline float vDot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float vLenSq(Vec a) { return vDot(a, a); }
inline float vDistSq(Vec a, Vec b) { return vLenSq(vSub(a, b)); }
inline Vec vAbs(Vec a) { return Vec{std::fabs(a.x), std::fabs(a.y), std::fabs(a.z)}; }

// The C# Vector3 component indexer (v[0], v[1], v[2]).
inline float vComp(const Vec& v, int i) { return i == 0 ? v.x : (i == 1 ? v.y : v.z); }
inline void vSetComp(Vec& v, int i, float value) {
    if (i == 0) {
        v.x = value;
    } else if (i == 1) {
        v.y = value;
    } else {
        v.z = value;
    }
}

} // namespace mdk

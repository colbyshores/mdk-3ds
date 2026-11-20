#pragma once

// ============================================================================
// sun_baker.h -- the BUILD-TIME half of the 3DS's baked light
// (mdk_3ds/src/render/sun_bake.h has the file and the runtime's half).
//
// HOST ONLY: compiled by mdk_3ds/tools/sun_bake/Makefile into sun_bake_level
// (one level at a time, from mdk_core's own arena build) and by the host tests
// (tests/scripts/sun_bake_tests.cpp, from hand-built scenes). Never part of the
// 3DS build.
//
// THE INPUT is a triangle soup in mesh corner order, each triangle marked:
//
//   receiver     lit by the shader at runtime: a Lit triangle of the Solid or
//                DoubleSided pass (pica::castsSunShadow) -- the draws the
//                real-time path sends through the lit geometry shader;
//   caster       blocks the sun: a receiver of the STATIC world drawn at level
//                start whose texture has no holes -- the real-time caster
//                pass's set (castShadows: castsSunShadow, no holes);
//   doubleSided  its pass is DoubleSided: it blocks the sun from either side and
//                is seen from both, so BOTH its sides are baked (the shader
//                turns the normal to the camera). A Solid triangle blocks only
//                when its FRONT faces the sun (castShadows culls Solid casters as
//                the eyes do) and only its front is ever seen;
//   rig          its arena's light (BakeSettings::rigs): enhancedLighting.
//
// THE FRONT of a triangle is the side its OUTWARD normal points to,
// -cross(p1 - p0, p2 - p0): MDK winds a floor's corners clockwise seen from
// above (game/level_light.cpp faceOf).
//
// THE OUTPUT is a SunBakeImage (sun_bake.h): the charts and grey blocks, the
// keys with each side's vertex colour (F_lit / 2), the grey atlas of the shadow's
// multiplier m (tex3ds makes the L4 .t3x from it), and the depth map for the
// moving objects. Deterministic: the same input and settings give
// the same bytes (no threads, no hash order).
// ============================================================================

#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "render/sun_bake.h"

namespace mdk {

// An arena's light, as the frame's rig carries it (render_types.h Lighting).
struct BakeRig {
    float sky[3] = {0.0f, 0.0f, 0.0f};
    float ground[3] = {0.0f, 0.0f, 0.0f};
    float sun[3] = {0.0f, 0.0f, 0.0f};
    float exposure = 1.0f;
    bool shadows = false;   // Lighting::shadows (an open arena)
};

struct BakeTriangle {
    float p[3][3];
    bool receiver = false;
    bool caster = false;
    bool doubleSided = false;
    int32_t rig = 0;
};

struct BakeSettings {
    // Where the sun SHINES (mdk::sunDirection(): the light travels along it).
    float sunDirection[3] = {0.0f, 0.0f, -1.0f};
    std::vector<BakeRig> rigs;   // indexed by BakeTriangle::rig (empty: one neutral rig)
    // The density aimed for, in world units a texel; coarsened (x1.12 a step)
    // until the atlas fits the budget and the PICA's 1024 x 1024.
    float texelSize = 2.0f;
    SunBakeFormat format = kSunBakeL4;
    // The atlas's texel budget, in L4 bytes (512 x 1024 texels): the atlas has
    // the same size in either format (L8 is twice the bytes).
    size_t budgetBytes = 256u * 1024u;
    // The most texels a chart may span a side (gutters included): a chart
    // larger than that at the level's density takes coarser texels of its own,
    // so a few huge planes do not coarsen the whole level.
    int32_t chartMostSide = 256;
    // How far a chart's sides may turn from its seed's normal (a cosine; 0.99 is
    // 8 degrees). Every sub-sample still takes its own triangle's flat light; only
    // a texel straddling two facets of one chart blends them.
    float chartCone = 0.99f;
    // Rays a texel per axis (2: four rays, the soft edge of the real-time path's
    // 2x2-filtered compare).
    int32_t raysPerAxis = 2;
    // The receiver's point is pushed this far along its sun-facing normal before
    // its ray leaves (no acne: the ray is exact, so this is the PC's own 2048-map
    // offset, not the 3DS map's coarse 4 units).
    float normalOffset = 0.5f;
    // The moving objects' depth map: world units a cell, and the most cells a side.
    float depthCell = 8.0f;
    int32_t depthMaxSide = 256;
};

struct BakeReport {
    int32_t receivers = 0;        // receiver triangles in the input
    int32_t sides = 0;            // ... their baked sides (two for a DoubleSided one)
    int32_t sunSides = 0;         // sides the sun may reach (rays cast for them)
    int32_t casters = 0;          // caster triangles in the input
    int32_t effectiveCasters = 0; // ... that can block this sun (the BVH's)
    int32_t uniformSides = 0;     // sides of one shade (a grey block)
    int32_t colourBlocks = 0;     // distinct shades, one 4x4 grey block each
    int32_t charts = 0;           // charts with a shadow edge (texels of their own)
    int32_t atlasWidth = 0;
    int32_t atlasHeight = 0;
    float texelSize = 0.0f;       // the density the budget allowed
    int64_t chartTexels = 0;      // texels the charts use (gutters included)
    int64_t probeRays = 0;
    int64_t rays = 0;             // lightmap rays cast
    int64_t shadowedSamples = 0;  // ... that hit a caster
    int32_t depthWidth = 0;
    int32_t depthHeight = 0;
    float depthCell = 0.0f;
    size_t atlasBytes = 0;        // in the settings' format
    size_t atlasBytesL4 = 0;
    size_t atlasBytesL8 = 0;
    // A fully shadowed texel's colour (vertex colour * m) against the exact
    // per-channel F_shadow, the worst over the sun-facing sides, in F / 2 bytes:
    // with the exact mean ratio, and with it rounded to L4's and L8's steps.
    float ratioError = 0.0f;
    float ratioErrorL4 = 0.0f;
    float ratioErrorL8 = 0.0f;
    int32_t densitySteps = 0;     // coarsenings the budget took
};

// Bake. False when the input has nothing to bake or the atlas cannot fit even
// at the coarsest density.
bool bakeSunShadows(const std::vector<BakeTriangle>& triangles, const BakeSettings& settings,
                    SunBakeImage* out, BakeReport* report);

// THE SHADER'S LIGHT for one side (mdk_c3d.g.pica, line for line in float):
// t = n.z / 2 + 1/2; light = mix(ground, sky, t) + sun * max(dot(-sunDir, n), 0)
// (the sun term only when `sunReaches`); F = (exposure * light)^(1/2.2); the
// output is F / 2 clamped at 1 -- what the shader emits as the vertex colour.
void sunBakeLight(const BakeRig& rig, const float normal[3], const float sunDirection[3],
                  bool sunReaches, float outHalf[3]);

// Whether the sun is blocked at a world point by the given casters, with the
// bake's own ray rule: the tests' ground truth.
bool sunBlockedAt(const std::vector<BakeTriangle>& triangles, const float sunDirection[3],
                  const float point[3]);

// The same for many points at once (xyz triples): one BVH for all of them.
void sunBlockedAtMany(const std::vector<BakeTriangle>& triangles, const float sunDirection[3],
                      const std::vector<float>& points, std::vector<uint8_t>* blocked);

// The atlas bytes of a w x h atlas in a format.
size_t sunBakeAtlasBytes(SunBakeFormat format, int32_t w, int32_t h);

} // namespace mdk

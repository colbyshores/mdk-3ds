#pragma once

// ============================================================================
// sun_bake.h -- the 3DS's BAKED LIGHT of the static world: one bake per level
// (romfs:/sunbake/levelN.sbk + levelN.t3x), made at BUILD TIME by the host
// baker (mdk_3ds/tools/sun_bake/, run from stage_romfs.sh), read at level load
// (EnhancedToggles::lightBaked).
//
// WHY IT CAN BE BAKED. Everything the lit geometry shader (mdk_c3d.g.pica,
// mdk_litshadow.g.pica) computes for a STATIC triangle is fixed for the level:
// its flat normal, the sun (ONE constant for the game: game/level_light.cpp
// sunDirection()), the arena's rig (sky, ground, sun colour, exposure:
// enhancedLighting, per arena) and the static world's shadow on itself. So it is
// baked in two parts, and the static world is drawn with NO geometry shader
// (mdk_lightmap.v.pica; 06_draw.inc, the baked draw):
//
//   THE LIGHT, per triangle side, into a VERTEX COLOUR -- the shader's own
//   F_lit / 2 = min(to_srgb(exposure * (mix(ground, sky, n.z/2 + 1/2) + sun *
//   facing)) / 2, 1), the same value on the side's three corners (the meshes
//   are non-indexed triangle lists), so the light is the shader's EXACTLY (to
//   the byte), with no fetch: surface * colour * 2, the lit program's light
//   stage with the vertex colour where the geometry shader's output was.
//
//   THE SHADOW, into a single-channel GREY LIGHTMAP (L4 by default), a texel
//   m = 1 - A * (1 - r): A the fraction of the sun the static casters block
//   there (supersampled rays), r the side's F_shadow / F_lit (the mean of its
//   three channels' ratios: the real-time path mixes per channel,
//   mix(F_lit, F_shadow, A); the baker reports the colour error of the mean).
//   The combiner's stage 0 multiplies the surface by it. With the sky shadows
//   off it is not sampled at all.
//
// TWO-SIDED surfaces have one light per SIDE (the shader turned the normal to
// the camera): each side has its own key (the back side's is the triangle's
// corners in reverse: p0, p2, p1), its own colour and its own chart, and the
// backend draws a DoubleSided triangle twice, once culled to each side.
//
// WHAT IS IN A .sbk (little-endian, every section 16-byte aligned):
//
//   header      SunBakeHeader, below.
//   charts      SunBakeChart[chartCount]: a chart is one planar, connected
//               piece of one side of a Lit surface with its own rectangle of
//               the atlas, or (a constant mapping) one of the atlas's 4x4 GREY
//               BLOCKS, which every side of one shade samples -- the sides the
//               sun reaches everywhere (m = 1: block 0), or nowhere, need no
//               texels of their own. The mapping takes a WORLD position to the
//               atlas's texel coordinates, so the runtime makes a vertex's
//               lightmap uv from its position.
//   keys        uint64[keyCount], ascending: sunBakeTriangleKey of every baked
//               side (its three positions' bits, in that side's corner order).
//   key charts  uint16[keyCount]: the chart of each key.
//   key colours uint32[keyCount]: each side's F_lit / 2 as bytes R, G, B, 255
//               (memory order), the vertex colour.
//   depth       uint16[depthWidth * depthHeight], row-major: the sun's view of
//               the casters over the whole level (first hit along the light,
//               quantised by depthAxes' third row; 0xFFFF = nothing), for the
//               MOVING objects, which are not baked: one lookup per object
//               position per frame (lightAt).
//
// THE ATLAS is the .t3x beside it (tex3ds -f l4, or l8): atlasWidth x
// atlasHeight, its top row at t = 1, as tex3ds writes. The top-left 4x4 block
// is WHITE (m = 1, no shadow), and is what a triangle with no key samples (its
// vertex colour is then the neutral 1/2: the surface unlit).
//
// WHY TRIANGLES ARE KEYED BY THEIR POSITIONS. The runtime meets the arena
// meshes one at a time, as the arena stream builds them (game/arena_views.h),
// and a script's group_set_texture rebuilds one (LevelView::refresh). A key
// made from the positions' own bits -- copied from the .MTO by
// buildArenaScene on both machines, never computed -- finds a triangle
// whatever the mesh's order or which groups were disabled when it was built.
//
// THE BAKER IS THE FORMAT'S OTHER HALF (mdk_3ds/tools/sun_bake/sun_baker.cpp
// writes with writeSunBake below): change one, change both. The host tests
// (tests/scripts/sun_bake_tests.cpp) round-trip a bake through SunBake::parse.
//
// COLD except lightAt (an object draw's, once per world position per frame)
// and meshSides (once per mesh creation). No citro3d here: host-testable.
// ============================================================================

#include <stddef.h>
#include <stdint.h>

#include <vector>

namespace mdk {

static const uint32_t kSunBakeVersion = 3;
static const uint32_t kSunBakeHeaderBytes = 136;   // sizeof(SunBakeHeader)

// The atlas's store, in the .t3x (tex3ds -f).
enum SunBakeFormat {
    kSunBakeL4 = 0,   // 4 bits a texel
    kSunBakeL8 = 1,   // 8 bits a texel
};

// The neutral vertex colour of a side the bake does not know: F / 2 = 1/2.
static const uint32_t kSunBakeNeutralColour = 0xFF808080u;

// The per-vertex SIDE DATA a baked mesh carries beside its five floats (its own
// vertex buffer, stride kSunBakeSideStride): the lightmap uv as two GPU_SHORTs
// (uv * kSunBakeUvOne), then the colour as four GPU_UNSIGNED_BYTEs (R, G, B, A:
// F_lit / 2 * 255; the PICA does not normalise integer attributes, so the
// vertex shader scales by 1 / 255).
static const int32_t kSunBakeSideStride = 8;

// The atlas's blocks and charts sit on this grid (an ETC1 block).
static const int32_t kSunBakeBlock = 4;

// The lightmap uv attribute's fixed point: GPU_SHORT, uv * kSunBakeUvOne
// (mdk_lightmap.v.pica multiplies by 1 / kSunBakeUvOne).
static const float kSunBakeUvOne = 32768.0f;

struct SunBakeHeader {
    char magic[4];             // "SUNB"
    uint32_t version;          // kSunBakeVersion
    uint32_t headerBytes;      // kSunBakeHeaderBytes
    uint32_t fileBytes;        // the whole .sbk
    uint32_t format;           // SunBakeFormat of the .t3x beside it
    uint16_t atlasWidth;       // powers of two, 8..1024
    uint16_t atlasHeight;
    float texelSize;           // world units a texel (the level's density)
    float sunDirection[3];     // the sun the bake was made for (where it shines)
    uint32_t chartCount;
    uint32_t keyCount;
    uint32_t chartsOffset;
    uint32_t keysOffset;
    uint32_t keyChartsOffset;
    uint32_t colourBlocks;     // statistics: the 4x4 grey blocks (the first charts)
    uint32_t sides;            // statistics: the Lit sides baked
    uint16_t depthWidth;
    uint16_t depthHeight;
    // texel x = dot(p, row0.xyz) + row0.w, texel y = dot(p, row1) + row1.w,
    // quantised depth along the light = dot(p, row2) + row2.w.
    float depthAxes[12];
    uint32_t depthOffset;
    uint32_t casterTriangles;  // statistics, for the log
    uint32_t receiverTriangles;
    uint32_t keyColoursOffset;
};

// One chart's mapping: atlas texel x = dot(p, x.xyz) + x.w (columns), atlas
// texel y = dot(p, y.xyz) + y.w (rows from the image's top). A colour block's
// mapping is constant: its axes are 0.
struct SunBakeChart {
    float x[4];
    float y[4];
};

// The key of a mesh triangle's side: FNV-1a 64 over its three positions' 36
// bytes, in the side's corner order (the front: p0 p1 p2; the back: p0 p2 p1).
uint64_t sunBakeTriangleKey(const float p0[3], const float p1[3], const float p2[3]);

// A parsed bake: views into the caller's bytes (which must outlive it).
class SunBake {
public:
    SunBake() = default;

    // False (and `why` set) when the bytes are not a whole, valid .sbk.
    bool parse(const uint8_t* bytes, size_t size, const char** why);

    bool valid() const { return header_ != nullptr; }
    const SunBakeHeader& header() const { return *header_; }

    // The chart of a key, or -1.
    int32_t chartOf(uint64_t key) const;

    // The lightmap uv of a world position in `chart` (or the neutral block for
    // -1), in the PICA's convention (s left to right, t = 1 at the image's top
    // row), as the attribute's GPU_SHORT pair.
    void uvOf(int32_t chart, const float p[3], int16_t out[2]) const;

    // The colour (F_lit / 2, bytes R G B A in memory) of a key, with its chart;
    // false (and the neutral colour, chart -1) for an unknown key.
    bool sideOf(uint64_t key, int32_t* chart, uint32_t* colour) const;

    // The side data of a triangle mesh's FRONT sides (`back` false) or BACK
    // sides (true): `count` vertices, three a triangle, `stride` floats from one
    // position to the next; kSunBakeSideStride bytes a vertex into `out` (the
    // lightmap uv, then the colour). Returns how many triangles had a key.
    int32_t meshSides(const float* positions, int32_t count, int32_t stride, bool back,
                      uint8_t* out) const;

    // How much of the sun reaches the world point `p` by the level's static
    // casters (0 = none, 1 = all), 2x2-filtered over the depth map, the point
    // first moved `bias` world units towards the sun. 1 when there is no map.
    float lightAt(const float p[3], float bias) const;

private:
    const SunBakeHeader* header_ = nullptr;
    const SunBakeChart* charts_ = nullptr;
    const uint8_t* keys_ = nullptr;        // uint64, read with memcpy
    const uint16_t* keyCharts_ = nullptr;
    const uint8_t* keyColours_ = nullptr;   // uint32, read with memcpy
    const uint16_t* depth_ = nullptr;
};

// What the baker hands the writer (and the atlas it hands tex3ds).
struct SunBakeImage {
    SunBakeFormat format = kSunBakeL4;
    int32_t atlasWidth = 0;
    int32_t atlasHeight = 0;
    float texelSize = 0.0f;
    float sunDirection[3] = {0.0f, 0.0f, 0.0f};
    std::vector<SunBakeChart> charts;
    std::vector<uint64_t> keys;          // ascending, unique
    std::vector<uint16_t> keyCharts;
    std::vector<uint32_t> keyColours;    // F_lit / 2, bytes R G B A in memory
    std::vector<uint8_t> grey;           // atlasWidth * atlasHeight, m * 255, rows
                                         // from the image's top (the .t3x)
    int32_t depthWidth = 0;
    int32_t depthHeight = 0;
    float depthAxes[12] = {};
    std::vector<uint16_t> depth;
    uint32_t colourBlocks = 0;            // grey blocks
    uint32_t sides = 0;
    uint32_t casterTriangles = 0;
    uint32_t receiverTriangles = 0;
};

// The .sbk's bytes for an image (the atlas goes to tex3ds separately). False
// when the image is inconsistent.
bool writeSunBake(const SunBakeImage& image, std::vector<uint8_t>* out);

} // namespace mdk

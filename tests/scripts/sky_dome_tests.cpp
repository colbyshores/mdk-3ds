// ============================================================================
// sky_dome_tests.cpp -- see sky_dome_tests.h.
// ============================================================================

#include "sky_dome_tests.h"

#include <math.h>
#include <stdio.h>

#include <vector>

#include "formats/texture.h"
#include "render/sky_dome.h"

namespace mdk {
namespace test {

namespace {

int g_failures = 0;

// THE ORACLE'S MAPPING, panorama.hlsli's panorama_position, in libm: the
// reference the dome (which uses the shared trig LUT) is measured against.
void skyPanoramaPosition(const Vec3& direction, float wrapWidth, float horizonRow, float offset,
                         float* column, float* row) {
    const float ppr = wrapWidth / 6.28318530718f;
    float z = direction.z;
    if (z > 1.0f) z = 1.0f;
    if (z < -1.0f) z = -1.0f;
    const float azimuth = atan2f(direction.x, direction.y);
    const float elevation = asinf(z);
    *column = fmodf(fmodf(azimuth * ppr + offset, wrapWidth) + wrapWidth, wrapWidth);
    *row = horizonRow - elevation * ppr;
}

void check(bool ok, const char* what) {
    if (!ok) {
        g_failures++;
        printf("  FAIL sky_dome: %s\n", what);
    }
}

// A level-shaped sky: the shipped values (wrap 1800, horizon 240, height 360,
// offset 300), a texture as wide as the wrap, some index-0 texels.
formats::Texture makeSky(int32_t width, int32_t height) {
    formats::Texture sky;
    sky.setWidth(width);
    sky.setHeight(height);
    sky.setFrameCount(1);
    std::vector<uint8_t> px((size_t)width * (size_t)height);
    for (size_t i = 0; i < px.size(); i++) {
        px[i] = (uint8_t)((i * 7u) % 256u);   // includes 0 every 256 texels
    }
    sky.setIndices(px.data(), (int32_t)px.size());
    return sky;
}

} // namespace

int runSkyDomeTests() {
    g_failures = 0;
    const int32_t wrap = 1800, horizon = 240, height = 360, offset = 300;
    formats::Texture sky = makeSky(wrap, height);
    uint8_t palette[256 * 4];
    for (int32_t e = 0; e < 256; e++) {
        palette[e * 4 + 0] = (uint8_t)e;
        palette[e * 4 + 1] = (uint8_t)(255 - e);
        palette[e * 4 + 2] = (uint8_t)(e / 2);
        palette[e * 4 + 3] = 255;
    }
    palette[200 * 4 + 0] = 0;   // entry 200 repeats entry 0's colour exactly
    palette[200 * 4 + 1] = 255;
    palette[200 * 4 + 2] = 0;
    SkyDome dome;
    check(buildSkyDome(sky, palette, wrap, horizon, offset, 7, 9, &dome), "builds");

    // (1) Slices: their TEXTURE at most 1024 wide, the drawn columns contiguous
    // and covering [0, wrap) once, each texture carrying one neighbour column on
    // each side (wrapped) so a bilinear tap at a slice edge reads the panorama.
    int32_t next = 0;
    for (const SkySlice& s : dome.slices) {
        check(s.width > 0 && s.storedWidth == s.width + 2, "slice stores its two neighbours");
        check(s.storedWidth <= kSkySliceMax, "slice texture fits the PICA's 1024");
        check(s.firstColumn == next, "slices are contiguous");
        check((int32_t)s.indices.size() == s.storedWidth * height,
              "slice holds storedWidth*height indices");
        bool neighbours = true;
        for (int32_t y = 0; y < height; y++) {
            const int32_t before = (s.firstColumn - 1 + wrap) % wrap;
            const int32_t after = (s.firstColumn + s.width) % wrap;
            const uint8_t* row = s.indices.data() + (size_t)y * (size_t)s.storedWidth;
            const uint8_t* src = sky.indices() + (size_t)y * (size_t)sky.width();
            auto remapped = [&](uint8_t i) { return i == 0 ? (uint8_t)dome.remapTarget : i; };
            if (row[0] != remapped(src[before]) ||
                row[s.storedWidth - 1] != remapped(src[after]) ||
                row[1] != remapped(src[s.firstColumn])) {
                neighbours = false;
            }
        }
        check(neighbours, "stored edge columns are the panorama's wrapped neighbours");
        next += s.width;
    }
    check(next == wrap, "slices cover the whole wrap");
    check(dome.slices.size() == 2, "1800 columns need two slices");

    // (2) Every band vertex maps, through the ORACLE's formula, to the column and
    // row its uv names (within 0.05 of a panorama cell; float trig at 10000).
    float worstColumn = 0.0f, worstRow = 0.0f;
    for (const SkySlice& s : dome.slices) {
        for (int32_t i = s.first; i < s.first + s.count; i++) {
            const Vertex& v = dome.vertices[(size_t)i];
            // The shader normalises the direction before asin (sky.hlsl ps_main).
            const float len = sqrtf(v.pos.x * v.pos.x + v.pos.y * v.pos.y + v.pos.z * v.pos.z);
            const Vec3 dir{v.pos.x / len, v.pos.y / len, v.pos.z / len};
            float column = 0.0f, row = 0.0f;
            skyPanoramaPosition(dir, (float)wrap, (float)horizon, (float)offset, &column, &row);
            const float wantColumn = (float)s.firstColumn - 1.0f + v.uv.x * (float)s.storedWidth;
            const float wantRow = v.uv.y * (float)height;
            float dc = fabsf(column - fmodf(wantColumn, (float)wrap));
            if (dc > (float)wrap / 2.0f) dc = (float)wrap - dc;   // the seam at 0/1800
            const float dr = fabsf(row - wantRow);
            if (dc > worstColumn) worstColumn = dc;
            if (dr > worstRow) worstRow = dr;
        }
    }
    check(worstColumn < 0.05f, "every band vertex lands on its column (oracle formula)");
    check(worstRow < 0.05f, "every band vertex lands on its row (oracle formula)");

    // (3) The radius is the dome's, every vertex.
    bool radiusOk = true;
    for (const Vertex& v : dome.vertices) {
        const float r = sqrtf(v.pos.x * v.pos.x + v.pos.y * v.pos.y + v.pos.z * v.pos.z);
        if (fabsf(r - kSkyRadius) > 1.0f) radiusOk = false;
    }
    check(radiusOk, "every vertex sits on the dome's sphere");

    // (4) Index 0 is remapped, to the exact repeat when the palette has one.
    check(dome.remapTarget == 200 && dome.remapDistance == 0, "index 0 maps to its exact repeat");
    bool noZero = true;
    for (const SkySlice& s : dome.slices) {
        for (uint8_t i : s.indices) {
            if (i == 0) noZero = false;
        }
    }
    check(noZero, "no slice keeps an index 0 (it would be discarded)");
    check(dome.remappedTexels > 0, "the remap is counted");

    // (5) Caps exist (horizon 240 and height 360 leave both poles uncovered) and
    // take the named palette entries' colours.
    check(dome.topCount > 0 && dome.bottomCount > 0, "both caps are built");
    check(dome.topRgb[0] == palette[7 * 4] && dome.bottomRgb[1] == palette[9 * 4 + 1],
          "caps take the top/bottom palette entries");

    // (6) Refusals: a texture narrower than the wrap, and no palette.
    formats::Texture narrow = makeSky(1000, height);
    SkyDome refused;
    check(!buildSkyDome(narrow, palette, wrap, horizon, offset, 0, 0, &refused), "refuses a texture narrower than the wrap");
    check(!buildSkyDome(sky, nullptr, wrap, horizon, offset, 0, 0, &refused), "refuses no palette");

    printf("  sky_dome: %zu vertices, %zu slices, worst column %.4f, worst row %.4f, remapped %d\n",
           dome.vertices.size(), dome.slices.size(), worstColumn, worstRow, dome.remappedTexels);
    return g_failures;
}

} // namespace test
} // namespace mdk

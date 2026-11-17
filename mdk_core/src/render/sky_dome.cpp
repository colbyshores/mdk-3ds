// ============================================================================
// sky_dome.cpp -- see sky_dome.h. Cold: built once per level load.
// ============================================================================

#include "render/sky_dome.h"

#include <math.h>

#include "formats/texture.h"
#include "util/math_lut.h"

namespace mdk {

namespace {

const float kTau = 6.28318530718f;
const float kHalfPi = 1.57079632679f;

// The direction the oracle maps to (column, row): the inverse of
// panorama_position. Elevation is clamped to the poles. Trig through the
// shared LUT (AGENTS.md R6: presentation/mesh trig is fastSin/fastCos on both
// targets); the host test measures the result against the oracle's libm
// formula and holds it to a small fraction of a panorama cell.
Vec3 skyDirection(float column, float row, float ppr, float horizonRow, float offset) {
    const float azimuth = (column - offset) / ppr;
    float elevation = (horizonRow - row) / ppr;
    if (elevation > kHalfPi) elevation = kHalfPi;
    if (elevation < -kHalfPi) elevation = -kHalfPi;
    const float c = fastCos(elevation);
    return Vec3{fastSin(azimuth) * c, fastCos(azimuth) * c, fastSin(elevation)};
}

Vertex skyVertex(const Vec3& direction, float u, float v) {
    Vertex out;
    out.pos = Vec3{direction.x * kSkyRadius, direction.y * kSkyRadius, direction.z * kSkyRadius};
    out.uv = Vec2{u, v};
    out.colour = kVertexWhite;
    return out;
}

// Two triangles for the cell (a, b, c, d) in (column, row) order:
// a = (c0, r0), b = (c1, r0), c = (c1, r1), d = (c0, r1).
void pushCell(std::vector<Vertex>* out, const Vertex& a, const Vertex& b, const Vertex& c,
              const Vertex& d) {
    out->push_back(a);
    out->push_back(b);
    out->push_back(c);
    out->push_back(a);
    out->push_back(c);
    out->push_back(d);
}

// Evenly spaced stops from `from` to `to` inclusive, no step wider than `step`.
void stops(float from, float to, float step, std::vector<float>* out) {
    out->clear();
    const float span = to - from;
    int32_t n = (int32_t)ceilf(span / step);
    if (n < 1) n = 1;
    for (int32_t i = 0; i <= n; i++) {
        out->push_back(from + span * (float)i / (float)n);
    }
}

// One cap: rings from the band's edge elevation to a pole, flat-coloured.
void pushCap(std::vector<Vertex>* out, float wrapWidth, float ppr, float offset, float edgeRow,
             float horizonRow, bool top) {
    const float edgeElevation = (horizonRow - edgeRow) / ppr;
    const float pole = top ? kHalfPi : -kHalfPi;
    if (top ? edgeElevation >= kHalfPi : edgeElevation <= -kHalfPi) {
        return;
    }
    std::vector<float> columns;
    stops(0.0f, wrapWidth, (float)kSkyColumnStep, &columns);
    for (int32_t k = 0; k < kSkyCapRings; k++) {
        const float e0 = edgeElevation + (pole - edgeElevation) * (float)k / (float)kSkyCapRings;
        const float e1 = edgeElevation + (pole - edgeElevation) * (float)(k + 1) / (float)kSkyCapRings;
        // A ring's rows, expressed back in the oracle's row space.
        const float r0 = horizonRow - e0 * ppr;
        const float r1 = horizonRow - e1 * ppr;
        for (size_t i = 0; i + 1 < columns.size(); i++) {
            const Vertex a = skyVertex(skyDirection(columns[i], r0, ppr, horizonRow, offset), 0.5f, 0.5f);
            const Vertex b = skyVertex(skyDirection(columns[i + 1], r0, ppr, horizonRow, offset), 0.5f, 0.5f);
            const Vertex c = skyVertex(skyDirection(columns[i + 1], r1, ppr, horizonRow, offset), 0.5f, 0.5f);
            const Vertex d = skyVertex(skyDirection(columns[i], r1, ppr, horizonRow, offset), 0.5f, 0.5f);
            pushCell(out, a, b, c, d);
        }
    }
}

} // namespace

bool buildSkyDome(const formats::Texture& sky, const uint8_t* paletteRgba, int32_t wrapWidth,
                  int32_t horizonRow, int32_t offset, int32_t topColour, int32_t bottomColour,
                  SkyDome* out) {
    *out = SkyDome();
    const int32_t height = sky.height();
    if (paletteRgba == nullptr || sky.indices() == nullptr || height <= 0 ||
        wrapWidth < kSkyColumnStep || sky.width() < wrapWidth) {
        return false;
    }
    out->height = height;

    // Index 0's nearest opaque stand-in (sky_dome.h: the sky has no discard).
    int32_t target = 1;
    int32_t best = -1;
    for (int32_t e = 1; e < 256; e++) {
        const int32_t dr = (int32_t)paletteRgba[e * 4 + 0] - (int32_t)paletteRgba[0];
        const int32_t dg = (int32_t)paletteRgba[e * 4 + 1] - (int32_t)paletteRgba[1];
        const int32_t db = (int32_t)paletteRgba[e * 4 + 2] - (int32_t)paletteRgba[2];
        const int32_t d = dr * dr + dg * dg + db * db;
        if (best < 0 || d < best) {
            best = d;
            target = e;
        }
    }
    out->remapTarget = target;
    out->remapDistance = best;

    const float ppr = (float)wrapWidth / kTau;
    const float horizon = (float)horizonRow;
    const float off = (float)offset;
    std::vector<float> columns;
    std::vector<float> rows;
    stops(0.0f, (float)height, (float)kSkyRowStep, &rows);

    for (int32_t first = 0; first < wrapWidth; first += kSkySliceMax) {
        SkySlice slice;
        slice.firstColumn = first;
        slice.width = (wrapWidth - first) < kSkySliceMax ? (wrapWidth - first) : kSkySliceMax;
        slice.indices.resize((size_t)slice.width * (size_t)height);
        for (int32_t y = 0; y < height; y++) {
            const uint8_t* src = sky.indices() + (size_t)y * (size_t)sky.width() + (size_t)first;
            uint8_t* dst = slice.indices.data() + (size_t)y * (size_t)slice.width;
            for (int32_t x = 0; x < slice.width; x++) {
                uint8_t index = src[x];
                if (index == 0) {
                    index = (uint8_t)target;
                    out->remappedTexels++;
                }
                dst[x] = index;
            }
        }
        slice.first = (int32_t)out->vertices.size();
        stops((float)first, (float)(first + slice.width), (float)kSkyColumnStep, &columns);
        const float width = (float)slice.width;
        for (size_t j = 0; j + 1 < rows.size(); j++) {
            for (size_t i = 0; i + 1 < columns.size(); i++) {
                const float c0 = columns[i], c1 = columns[i + 1];
                const float r0 = rows[j], r1 = rows[j + 1];
                const float u0 = (c0 - (float)first) / width, u1 = (c1 - (float)first) / width;
                const float v0 = r0 / (float)height, v1 = r1 / (float)height;
                pushCell(&out->vertices,
                         skyVertex(skyDirection(c0, r0, ppr, horizon, off), u0, v0),
                         skyVertex(skyDirection(c1, r0, ppr, horizon, off), u1, v0),
                         skyVertex(skyDirection(c1, r1, ppr, horizon, off), u1, v1),
                         skyVertex(skyDirection(c0, r1, ppr, horizon, off), u0, v1));
            }
        }
        slice.count = (int32_t)out->vertices.size() - slice.first;
        out->slices.push_back(std::move(slice));
    }

    out->topFirst = (int32_t)out->vertices.size();
    pushCap(&out->vertices, (float)wrapWidth, ppr, off, 0.0f, horizon, true);
    out->topCount = (int32_t)out->vertices.size() - out->topFirst;
    out->bottomFirst = (int32_t)out->vertices.size();
    pushCap(&out->vertices, (float)wrapWidth, ppr, off, (float)height, horizon, false);
    out->bottomCount = (int32_t)out->vertices.size() - out->bottomFirst;

    const int32_t top = topColour & 0xFF;
    const int32_t bottom = bottomColour & 0xFF;
    for (int32_t k = 0; k < 3; k++) {
        out->topRgb[k] = paletteRgba[top * 4 + k];
        out->bottomRgb[k] = paletteRgba[bottom * 4 + k];
    }
    return true;
}

} // namespace mdk

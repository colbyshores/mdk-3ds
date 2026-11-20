// ============================================================================
// sun_bake.cpp -- see sun_bake.h.
// ============================================================================

#include "render/sun_bake.h"

#include <math.h>
#include <string.h>

namespace mdk {

static_assert(sizeof(SunBakeHeader) == kSunBakeHeaderBytes, "the header is the file's layout");
static_assert(sizeof(SunBakeChart) == 32, "a chart is eight floats");

namespace {

const uint32_t kAlign = 16;

uint32_t alignUp(uint32_t value) {
    return (value + (kAlign - 1u)) & ~(kAlign - 1u);
}

bool isPowerOfTwoSide(uint32_t side) {
    return side >= 8u && side <= 1024u && (side & (side - 1u)) == 0u;
}

// A section [offset, offset + bytes) inside a file of `size` bytes, aligned.
bool sectionFits(uint32_t offset, uint64_t bytes, size_t size) {
    return (offset % kAlign) == 0u && (uint64_t)offset + bytes <= (uint64_t)size;
}

float dot3(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// Every value finite and inside +-1e30 (a NaN fails both compares): a chart or
// an axis that is not would make uvOf's or lightAt's float-to-int conversion
// undefined. Load time only.
bool allFinite(const float* values, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (!(values[i] > -1.0e30f && values[i] < 1.0e30f)) {
            return false;
        }
    }
    return true;
}

// floor() for the texel lookups, by truncation and a fixup: no libm, and the
// values are texel coordinates well inside int range (clamped by the caller).
int32_t floorToInt(float value) {
    int32_t i = (int32_t)value;
    if ((float)i > value) {
        i--;
    }
    return i;
}

} // namespace

uint64_t sunBakeTriangleKey(const float p0[3], const float p1[3], const float p2[3]) {
    uint8_t bytes[36];
    memcpy(bytes, p0, 12);
    memcpy(bytes + 12, p1, 12);
    memcpy(bytes + 24, p2, 12);
    uint64_t hash = 14695981039346656037ull;
    for (int i = 0; i < 36; i++) {
        hash ^= (uint64_t)bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

bool SunBake::parse(const uint8_t* bytes, size_t size, const char** why) {
    header_ = nullptr;
    const char* reason = nullptr;
    if (!bytes || size < kSunBakeHeaderBytes || ((uintptr_t)bytes & 3u) != 0u) {
        reason = "short or misaligned";
    } else {
        const SunBakeHeader* h = reinterpret_cast<const SunBakeHeader*>(bytes);
        if (memcmp(h->magic, "SUNB", 4) != 0) {
            reason = "not a bake";
        } else if (h->version != kSunBakeVersion || h->headerBytes != kSunBakeHeaderBytes) {
            reason = "another version";
        } else if (h->fileBytes != size) {
            reason = "truncated";
        } else if ((h->format != kSunBakeL4 && h->format != kSunBakeL8) ||
                   !isPowerOfTwoSide(h->atlasWidth) || !isPowerOfTwoSide(h->atlasHeight)) {
            reason = "bad atlas";
        } else if (h->chartCount > 65535u ||
                   !sectionFits(h->chartsOffset, (uint64_t)h->chartCount * sizeof(SunBakeChart),
                                size)) {
            reason = "bad charts";
        } else if (!sectionFits(h->keysOffset, (uint64_t)h->keyCount * 8u, size) ||
                   !sectionFits(h->keyChartsOffset, (uint64_t)h->keyCount * 2u, size) ||
                   !sectionFits(h->keyColoursOffset, (uint64_t)h->keyCount * 4u, size)) {
            reason = "bad keys";
        } else if (!sectionFits(h->depthOffset,
                                (uint64_t)h->depthWidth * h->depthHeight * 2u, size)) {
            reason = "bad depth map";
        } else if (!allFinite(h->depthAxes, 12) || !allFinite(h->sunDirection, 3) ||
                   !allFinite(reinterpret_cast<const float*>(bytes + h->chartsOffset),
                              (size_t)h->chartCount * 8u)) {
            reason = "a mapping is not finite";
        } else {
            const uint16_t* keyCharts =
                reinterpret_cast<const uint16_t*>(bytes + h->keyChartsOffset);
            for (uint32_t i = 0; i < h->keyCount && !reason; i++) {
                if (keyCharts[i] >= h->chartCount) {
                    reason = "a key names no chart";
                }
            }
            if (!reason) {
                header_ = h;
                charts_ = reinterpret_cast<const SunBakeChart*>(bytes + h->chartsOffset);
                keys_ = bytes + h->keysOffset;
                keyCharts_ = keyCharts;
                keyColours_ = bytes + h->keyColoursOffset;
                depth_ = reinterpret_cast<const uint16_t*>(bytes + h->depthOffset);
            }
        }
    }
    if (why) {
        *why = reason;
    }
    return header_ != nullptr;
}

int32_t SunBake::chartOf(uint64_t key) const {
    int32_t chart = -1;
    uint32_t colour = 0;
    sideOf(key, &chart, &colour);
    return chart;
}

bool SunBake::sideOf(uint64_t key, int32_t* chart, uint32_t* colour) const {
    *chart = -1;
    *colour = kSunBakeNeutralColour;
    if (!header_) {
        return false;
    }
    uint32_t lo = 0;
    uint32_t hi = header_->keyCount;
    while (lo < hi) {
        const uint32_t mid = lo + ((hi - lo) >> 1);
        uint64_t at;
        memcpy(&at, keys_ + (size_t)mid * 8u, 8);
        if (at < key) {
            lo = mid + 1u;
        } else if (at > key) {
            hi = mid;
        } else {
            *chart = (int32_t)keyCharts_[mid];
            memcpy(colour, keyColours_ + (size_t)mid * 4u, 4);
            return true;
        }
    }
    return false;
}

void SunBake::uvOf(int32_t chart, const float p[3], int16_t out[2]) const {
    out[0] = 0;
    out[1] = 0;
    if (!header_) {
        return;
    }
    // The white block's centre: a bilinear tap there reads its four middle texels.
    float x = 0.5f * (float)kSunBakeBlock;
    float y = 0.5f * (float)kSunBakeBlock;
    if (chart >= 0 && (uint32_t)chart < header_->chartCount) {
        const SunBakeChart& c = charts_[chart];
        x = dot3(c.x, p) + c.x[3];
        y = dot3(c.y, p) + c.y[3];
    }
    // tex3ds puts the image's top row at t = 1.
    const float s = x / (float)header_->atlasWidth;
    const float t = 1.0f - y / (float)header_->atlasHeight;
    const float values[2] = {s, t};
    for (int i = 0; i < 2; i++) {
        float v = values[i] * kSunBakeUvOne + 0.5f;
        if (v < 0.0f) {
            v = 0.0f;
        }
        if (v > 32767.0f) {
            v = 32767.0f;
        }
        out[i] = (int16_t)(int32_t)v;
    }
}

int32_t SunBake::meshSides(const float* positions, int32_t count, int32_t stride, bool back,
                           uint8_t* out) const {
    if (!header_ || !positions || !out || count <= 0 || stride < 3) {
        return 0;
    }
    int32_t keyed = 0;
    const int32_t triangles = count / 3;
    for (int32_t v = 0; v < count; v++) {
        const int32_t t = v / 3;
        int32_t chart = -1;
        uint32_t colour = kSunBakeNeutralColour;
        if (t < triangles) {
            const float* p0 = positions + (size_t)(t * 3) * (size_t)stride;
            const float* p1 = p0 + stride;
            const float* p2 = p1 + stride;
            if (v % 3 == 0) {
                // Once a triangle: the key, the chart, the colour.
                if (sideOf(back ? sunBakeTriangleKey(p0, p2, p1) : sunBakeTriangleKey(p0, p1, p2),
                           &chart, &colour)) {
                    keyed++;
                }
                for (int k = 0; k < 3; k++) {
                    uint8_t* at = out + (size_t)(v + k) * (size_t)kSunBakeSideStride;
                    int16_t uv[2];
                    uvOf(chart, p0 + (size_t)k * (size_t)stride, uv);
                    memcpy(at, uv, 4);
                    memcpy(at + 4, &colour, 4);
                }
            }
        } else {
            // A trailing partial triangle (never in an arena mesh): the white
            // block and the neutral colour.
            uint8_t* at = out + (size_t)v * (size_t)kSunBakeSideStride;
            int16_t uv[2];
            uvOf(-1, positions + (size_t)v * (size_t)stride, uv);
            memcpy(at, uv, 4);
            memcpy(at + 4, &colour, 4);
        }
    }
    return keyed;
}

float SunBake::lightAt(const float p[3], float bias) const {
    if (!header_ || header_->depthWidth == 0 || header_->depthHeight == 0) {
        return 1.0f;
    }
    const float* a = header_->depthAxes;
    const float row2[3] = {a[8], a[9], a[10]};
    // The depth row is the light's direction times the quantisation, so moving
    // the point `bias` units towards the sun lowers its depth by bias * |row2|
    // and leaves x and y (perpendicular to the light) where they are.
    // (sqrtf is the VFP's own vsqrt under -fno-math-errno: AGENTS.md R7.)
    const float k = sqrtf(dot3(row2, row2));
    const float tx = dot3(a, p) + a[3] - 0.5f;
    const float ty = dot3(a + 4, p) + a[7] - 0.5f;
    const float depth = dot3(row2, p) + a[11] - bias * k;
    const int32_t w = (int32_t)header_->depthWidth;
    const int32_t h = (int32_t)header_->depthHeight;
    // Outside the map is lit (the baker's map covers every caster).
    if (!(tx > -1.0f && ty > -1.0f && tx < (float)w && ty < (float)h)) {
        return 1.0f;
    }
    const int32_t x0 = floorToInt(tx);
    const int32_t y0 = floorToInt(ty);
    const float fx = tx - (float)x0;
    const float fy = ty - (float)y0;
    float light = 0.0f;
    for (int32_t j = 0; j < 2; j++) {
        for (int32_t i = 0; i < 2; i++) {
            const int32_t x = x0 + i;
            const int32_t y = y0 + j;
            float lit = 1.0f;
            if (x >= 0 && y >= 0 && x < w && y < h) {
                const uint16_t stored = depth_[(size_t)y * (size_t)w + (size_t)x];
                lit = (stored == 0xFFFFu || depth <= (float)stored) ? 1.0f : 0.0f;
            }
            const float weight = (i ? fx : 1.0f - fx) * (j ? fy : 1.0f - fy);
            light += lit * weight;
        }
    }
    return light;
}

bool writeSunBake(const SunBakeImage& image, std::vector<uint8_t>* out) {
    if (!out || !isPowerOfTwoSide((uint32_t)image.atlasWidth) ||
        !isPowerOfTwoSide((uint32_t)image.atlasHeight) ||
        image.grey.size() != (size_t)image.atlasWidth * (size_t)image.atlasHeight ||
        image.keys.size() != image.keyCharts.size() ||
        image.keys.size() != image.keyColours.size() || image.charts.size() > 65535u ||
        image.depth.size() != (size_t)image.depthWidth * (size_t)image.depthHeight ||
        image.depthWidth < 0 || image.depthHeight < 0 || image.depthWidth > 65535 ||
        image.depthHeight > 65535) {
        return false;
    }
    for (size_t i = 0; i < image.keys.size(); i++) {
        if ((i > 0 && image.keys[i] <= image.keys[i - 1]) ||
            image.keyCharts[i] >= image.charts.size()) {
            return false;
        }
    }
    SunBakeHeader h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, "SUNB", 4);
    h.version = kSunBakeVersion;
    h.headerBytes = kSunBakeHeaderBytes;
    h.format = (uint32_t)image.format;
    h.atlasWidth = (uint16_t)image.atlasWidth;
    h.atlasHeight = (uint16_t)image.atlasHeight;
    h.texelSize = image.texelSize;
    memcpy(h.sunDirection, image.sunDirection, sizeof(h.sunDirection));
    h.chartCount = (uint32_t)image.charts.size();
    h.keyCount = (uint32_t)image.keys.size();
    h.colourBlocks = image.colourBlocks;
    h.sides = image.sides;
    h.depthWidth = (uint16_t)image.depthWidth;
    h.depthHeight = (uint16_t)image.depthHeight;
    memcpy(h.depthAxes, image.depthAxes, sizeof(h.depthAxes));
    h.casterTriangles = image.casterTriangles;
    h.receiverTriangles = image.receiverTriangles;

    uint32_t at = alignUp(kSunBakeHeaderBytes);
    h.chartsOffset = at;
    at = alignUp(at + h.chartCount * (uint32_t)sizeof(SunBakeChart));
    h.keysOffset = at;
    at = alignUp(at + h.keyCount * 8u);
    h.keyChartsOffset = at;
    at = alignUp(at + h.keyCount * 2u);
    h.keyColoursOffset = at;
    at = alignUp(at + h.keyCount * 4u);
    h.depthOffset = at;
    at = alignUp(at + (uint32_t)image.depth.size() * 2u);
    h.fileBytes = at;

    out->assign(at, 0);
    uint8_t* file = out->data();
    memcpy(file, &h, sizeof(h));
    if (!image.charts.empty()) {
        memcpy(file + h.chartsOffset, image.charts.data(), image.charts.size() * sizeof(SunBakeChart));
    }
    if (!image.keys.empty()) {
        memcpy(file + h.keysOffset, image.keys.data(), image.keys.size() * 8u);
        memcpy(file + h.keyChartsOffset, image.keyCharts.data(), image.keyCharts.size() * 2u);
        memcpy(file + h.keyColoursOffset, image.keyColours.data(), image.keyColours.size() * 4u);
    }
    if (!image.depth.empty()) {
        memcpy(file + h.depthOffset, image.depth.data(), image.depth.size() * 2u);
    }
    return true;
}

} // namespace mdk

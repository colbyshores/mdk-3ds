// ============================================================================
// mirror_cube.cpp -- see mirror_cube.h. Cold: built once per level load.
// ============================================================================

#include "render/mirror_cube.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "formats/arena.h"
#include "formats/texture.h"
#include "render/material_resolver.h"
#include "util/math_lut.h"

namespace mdk {

namespace {

const float kTau = 6.28318530718f;

} // namespace

int32_t mirrorRowShifts(const std::vector<const formats::Arena*>& arenas, int32_t* shifts) {
    // Which of the 21 mirror values occur, then their shifts in ascending order
    // (the shift falls as the value rises, so the values are walked downwards).
    bool seen[kMirrorRowShiftsMax] = {};
    for (size_t a = 0; a < arenas.size(); a++) {
        if (!arenas[a]) {
            continue;
        }
        const std::vector<int32_t>& materials = arenas[a]->triangleMaterials();
        const std::vector<std::string>& names = arenas[a]->materials();
        const size_t pen = strlen(MaterialResolver::kPenPrefix);
        for (size_t t = 0; t < materials.size(); t++) {
            // A negative material is the palette colour -value (formats/arena.h);
            // a named one reaches a colour as PEN_n (MaterialResolver::resolve).
            // The specials 990-1010 are the mirrors. (An ARCHIVE colour by name
            // is the resolver's third way to a colour; no retail arena reaches a
            // mirror through one, and a draw that did would take the nearest cube.)
            int32_t colour = -1;
            if (materials[t] < 0) {
                colour = -materials[t];
            } else if ((size_t)materials[t] < names.size()) {
                const std::string& name = names[(size_t)materials[t]];
                if (name.size() > pen && name.compare(0, pen, MaterialResolver::kPenPrefix) == 0 &&
                    name.find_first_not_of("0123456789", pen) == std::string::npos) {
                    colour = atoi(name.c_str() + pen);
                }
            }
            if (colour >= MaterialResolver::kMirrorFirst && colour <= MaterialResolver::kMirrorLast) {
                seen[colour - MaterialResolver::kMirrorFirst] = true;
            }
        }
    }
    int32_t count = 0;
    for (int32_t i = kMirrorRowShiftsMax - 1; i >= 0; i--) {
        if (seen[i]) {
            // MaterialResolver's own expression, so the shift a draw carries
            // (Material::row_shift) is this one exactly.
            const int32_t value = MaterialResolver::kMirrorFirst + i;
            const float shift =
                (MaterialResolver::kMirrorMiddle - value) * MaterialResolver::kMirrorRowsPerValue;
            shifts[count++] = (int32_t)shift;
        }
    }
    return count;
}

Vec3 mirrorCubeDirection(int32_t face, int32_t x, int32_t y, int32_t size) {
    // The texel centre's (s, t) on the face, as -1..1: s across the columns,
    // t = 1 at the first memory row (the PICA's t runs from the last row).
    const float sc = 2.0f * ((float)x + 0.5f) / (float)size - 1.0f;
    const float tc = 1.0f - 2.0f * ((float)y + 0.5f) / (float)size;
    // The inverse of the lookup's (major axis, sc, tc) per face -- OpenGL's
    // table: +X sc = -z tc = -y; -X sc = z tc = -y; +Y sc = x tc = z;
    // -Y sc = x tc = -z; +Z sc = x tc = -y; -Z sc = -x tc = -y.
    switch (face) {
    case 0: return Vec3{1.0f, -tc, -sc};
    case 1: return Vec3{-1.0f, -tc, sc};
    case 2: return Vec3{sc, 1.0f, tc};
    case 3: return Vec3{sc, -1.0f, -tc};
    case 4: return Vec3{sc, -tc, 1.0f};
    default: return Vec3{-sc, -tc, -1.0f};
    }
}

void mirrorCubeCells(int32_t face, int32_t size, int32_t wrapWidth, int32_t horizonRow,
                     int32_t offset, int32_t height, int16_t* columns, int16_t* rows) {
    const float ppr = (float)wrapWidth / kTau;
    const float wrap = (float)wrapWidth;
    const float rowsHigh = (float)height;
    for (int32_t y = 0; y < size; y++) {
        for (int32_t x = 0; x < size; x++) {
            const Vec3 d = mirrorCubeDirection(face, x, y, size);
            // panorama.hlsli's panorama_position, through the shared trig table
            // (AGENTS.md R6, as render/sky_dome.cpp): the azimuth from +Y
            // towards +X, and the elevation as atan2(z, |xy|) -- asin of the
            // normalised z, without the normalisation.
            const float azimuth = fastAtan2(d.x, d.y);
            const float elevation = fastAtan2(d.z, sqrtf(d.x * d.x + d.y * d.y));
            // The oracle's own wrap, fmod(fmod(a, w) + w, w).
            const float column = fmodf(fmodf(azimuth * ppr + (float)offset, wrap) + wrap, wrap);
            int32_t c = (int32_t)column;
            if (c >= wrapWidth) c = wrapWidth - 1;   // the float edge of the last column
            if (c < 0) c = 0;
            // The row, floored, then wrapped into [0, height) by the oracle's own
            // fmod(fmod(r, H) + H, H) -- in float, as mirror.hlsl does it, and
            // exact: the row is a whole number far below 2^24.
            const float row = floorf((float)horizonRow - elevation * ppr);
            int32_t r = (int32_t)fmodf(fmodf(row, rowsHigh) + rowsHigh, rowsHigh);
            if (r >= height) r = height - 1;
            if (r < 0) r = 0;
            const size_t at = (size_t)y * (size_t)size + (size_t)x;
            columns[at] = (int16_t)c;
            rows[at] = (int16_t)r;
        }
    }
}

bool mirrorCubeFace(const formats::Texture& panorama, const int16_t* columns,
                    const int16_t* rows, int32_t count, int32_t rowShift, uint8_t* out) {
    const uint8_t* indices = panorama.indices();
    const int32_t width = panorama.width();
    const int32_t height = panorama.height();
    if (!indices || width <= 0 || height <= 0) {
        return false;
    }
    // mirror.hlsl: fmod(fmod(row + shift, H) + H, H), a true modulo. The rows
    // arrive wrapped into [0, H) (mirrorCubeCells) and a shift is at most 80
    // rows either way (MIRRLOW...MIRRHIGH: (1000 - value) * 8), well inside one
    // height, so the shift wraps with one addition and each sum is in [0, 2H):
    // one subtraction makes it exact, with no divide (the bounded wrap of
    // AGENTS.md R11). A shift of a whole height or more is refused, not wrapped.
    if (rowShift <= -height || rowShift >= height) {
        return false;
    }
    const int32_t shift = rowShift < 0 ? rowShift + height : rowShift;
    for (int32_t i = 0; i < count; i++) {
        int32_t row = (int32_t)rows[i];
        if (row < 0 || row >= height) {
            row = 0;   // not a mirrorCubeCells row: refused to index outside the panorama
        }
        row += shift;
        if (row >= height) {
            row -= height;
        }
        int32_t column = columns[i];
        if (column >= width) {
            column = width - 1;
        }
        out[i] = indices[(size_t)row * (size_t)width + (size_t)column];
    }
    return true;
}

} // namespace mdk

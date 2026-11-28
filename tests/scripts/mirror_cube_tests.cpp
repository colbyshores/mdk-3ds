// ============================================================================
// mirror_cube_tests.cpp -- see mirror_cube_tests.h.
// ============================================================================

#include "mirror_cube_tests.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "formats/arena.h"
#include "formats/mdk_data.h"
#include "formats/mto.h"
#include "formats/texture.h"
#include "game/level_data.h"
#include "render/material_resolver.h"
#include "render/mirror_cube.h"
#include "render/pica_matrix.h"
#include "util/math_lut.h"

namespace mdk {
namespace test {

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        g_failures++;
        printf("  FAIL mirror_cube: %s\n", what);
    }
}

// THE PICA'S CUBE LOOKUP, written out from OpenGL's table (the convention the
// header names), independently of mirrorCubeDirection: the major axis picks
// the face, (sc, tc) per face, s = (sc/|ma| + 1)/2, t = (tc/|ma| + 1)/2 -- and
// the PICA's t = 1 is a texture's FIRST memory row (pica_matrix.h,
// sceneUvFromTop), so the memory row is (1 - t) * size.
void picaCubeLookup(double rx, double ry, double rz, int32_t size, int32_t* face, int32_t* x,
                    int32_t* y) {
    const double ax = fabs(rx), ay = fabs(ry), az = fabs(rz);
    double sc = 0.0, tc = 0.0, ma = 1.0;
    if (ax >= ay && ax >= az) {
        ma = ax;
        if (rx > 0.0) { *face = 0; sc = -rz; tc = -ry; }   // +X
        else          { *face = 1; sc = rz;  tc = -ry; }   // -X
    } else if (ay >= az) {
        ma = ay;
        if (ry > 0.0) { *face = 2; sc = rx; tc = rz; }     // +Y
        else          { *face = 3; sc = rx; tc = -rz; }    // -Y
    } else {
        ma = az;
        if (rz > 0.0) { *face = 4; sc = rx;  tc = -ry; }   // +Z
        else          { *face = 5; sc = -rx; tc = -ry; }   // -Z
    }
    const double s = (sc / ma + 1.0) * 0.5;
    const double t = (tc / ma + 1.0) * 0.5;
    *x = (int32_t)floor(s * (double)size);
    *y = (int32_t)floor((1.0 - t) * (double)size);
}

// THE ORACLE'S CELL (mirror.hlsl + panorama.hlsli), in libm double: the
// column and the floored row a direction shows, before the row shift.
void oracleCell(double dx, double dy, double dz, double wrap, double horizon, double offset,
                int32_t* column, int32_t* row) {
    const double len = sqrt(dx * dx + dy * dy + dz * dz);
    dx /= len;
    dy /= len;
    dz /= len;
    const double ppr = wrap / 6.283185307179586;
    const double azimuth = atan2(dx, dy);
    const double elevation = asin(dz < -1.0 ? -1.0 : dz > 1.0 ? 1.0 : dz);
    const double c = fmod(fmod(azimuth * ppr + offset, wrap) + wrap, wrap);
    *column = (int32_t)c;
    *row = (int32_t)floor(horizon - elevation * ppr);
}

void testFaceConvention() {
    // Every texel of a small cube and of the shipped size.
    const int32_t sizes[2] = {16, kMirrorCubeSize};
    bool ok = true;
    for (int32_t k = 0; k < 2 && ok; k++) {
        const int32_t size = sizes[k];
        for (int32_t face = 0; face < kMirrorCubeFaces && ok; face++) {
            for (int32_t y = 0; y < size && ok; y++) {
                for (int32_t x = 0; x < size && ok; x++) {
                    const Vec3 d = mirrorCubeDirection(face, x, y, size);
                    int32_t f = -1, lx = -1, ly = -1;
                    picaCubeLookup(d.x, d.y, d.z, size, &f, &lx, &ly);
                    if (f != face || lx != x || ly != y) {
                        printf("  face %d texel (%d,%d) of %d looks up as face %d (%d,%d)\n",
                               (int)face, (int)x, (int)y, (int)size, (int)f, (int)lx, (int)ly);
                        ok = false;
                    }
                }
            }
        }
    }
    check(ok, "every texel's direction looks up as that texel (the PICA's cube convention)");
    // The axes, spelled out: +Z is up in MDK, so the +Z face is what a mirror
    // ceiling would show and -Z what a floor shows.
    const Vec3 up = mirrorCubeDirection(4, kMirrorCubeSize / 2, kMirrorCubeSize / 2,
                                        kMirrorCubeSize);
    const Vec3 down = mirrorCubeDirection(5, kMirrorCubeSize / 2, kMirrorCubeSize / 2,
                                          kMirrorCubeSize);
    check(up.z > 0.99f && down.z < -0.99f, "faces 4 and 5 are +Z and -Z");
}

void testCellsAgainstTheOracle() {
    // LEVEL6's panorama (wrap 900, horizon 300, offset 255) and LEVEL3's
    // (1800, 240, 300).
    struct Case {
        int32_t wrap, horizon, offset;
    };
    const Case cases[2] = {{900, 300, 255}, {1800, 240, 300}};
    const int32_t height = 360;   // every retail panorama's
    const int32_t size = kMirrorCubeSize;
    const int32_t texels = size * size;
    std::vector<int16_t> columns((size_t)texels), rows((size_t)texels);
    for (int32_t c = 0; c < 2; c++) {
        int64_t exact = 0, total = 0, far = 0;
        for (int32_t face = 0; face < kMirrorCubeFaces; face++) {
            mirrorCubeCells(face, size, cases[c].wrap, cases[c].horizon, cases[c].offset,
                            height, columns.data(), rows.data());
            for (int32_t y = 0; y < size; y++) {
                for (int32_t x = 0; x < size; x++) {
                    const Vec3 d = mirrorCubeDirection(face, x, y, size);
                    int32_t wantColumn = 0, wantRow = 0;
                    oracleCell(d.x, d.y, d.z, cases[c].wrap, cases[c].horizon, cases[c].offset,
                               &wantColumn, &wantRow);
                    const size_t at = (size_t)y * (size_t)size + (size_t)x;
                    int32_t dc = columns[at] - wantColumn;
                    if (dc < 0) dc = -dc;
                    if (dc > cases[c].wrap / 2) dc = cases[c].wrap - dc;   // the seam at 0
                    // The oracle's row wrapped as mirror.hlsl wraps it.
                    const int32_t wantWrapped = ((wantRow % height) + height) % height;
                    int32_t dr = rows[at] - wantWrapped;
                    if (dr < 0) dr = -dr;
                    if (dr > height / 2) dr = height - dr;   // the wrap seam
                    if (rows[at] < 0 || rows[at] >= height) far++;
                    total++;
                    if (dc == 0 && dr == 0) exact++;
                    if (dc > 1 || dr > 1) far++;
                    if (columns[at] < 0 || columns[at] >= cases[c].wrap) far++;
                }
            }
        }
        check(far == 0, "every texel's cell is within one of the oracle's");
        check(exact * 1000 >= total * 995, "at least 99.5% of the cells are the oracle's exactly");
        printf("  mirror_cube: wrap %d, %lld of %lld cells exact\n", (int)cases[c].wrap,
               (long long)exact, (long long)total);
    }
}

void testShiftAndWrap() {
    const int32_t width = 904, height = 360;
    formats::Texture pano;
    pano.setWidth(width);
    pano.setHeight(height);
    pano.setFrameCount(1);
    std::vector<uint8_t> px((size_t)width * (size_t)height);
    for (int32_t r = 0; r < height; r++) {
        for (int32_t c = 0; c < width; c++) {
            px[(size_t)r * (size_t)width + (size_t)c] = (uint8_t)((r * 3 + c * 7) & 255);
        }
    }
    pano.setIndices(px.data(), (int32_t)px.size());
    // Rows as mirrorCubeCells makes them: already wrapped into [0, H).
    const int16_t rows[7] = {355, 0, 75, 359, 1, 5, 165};
    const int16_t columns[7] = {0, 17, 899, 450, 3, 600, 255};
    const int32_t shifts[5] = {-80, -40, 0, 40, 80};
    bool ok = true;
    for (int32_t s = 0; s < 5; s++) {
        uint8_t out[7];
        check(mirrorCubeFace(pano, columns, rows, 7, shifts[s], out), "a readable panorama is read");
        for (int32_t i = 0; i < 7; i++) {
            // mirror.hlsl: fmod(fmod(row + shift, H) + H, H).
            const double wrapped = fmod(fmod((double)rows[i] + shifts[s], (double)height) + height,
                                        (double)height);
            const int32_t row = (int32_t)wrapped;
            if (out[i] != px[(size_t)row * (size_t)width + (size_t)columns[i]]) {
                ok = false;
            }
        }
    }
    check(ok, "the row shift and the wrap are mirror.hlsl's");
    formats::Texture empty;
    uint8_t sink[1];
    check(!mirrorCubeFace(empty, columns, rows, 1, 0, sink), "an unreadable panorama is refused");
    check(!mirrorCubeFace(pano, columns, rows, 1, height, sink) &&
              !mirrorCubeFace(pano, columns, rows, 1, -height, sink),
          "a shift of a whole height is refused");
}

void testDrawFindsItsCube() {
    // The cubes a level makes, in mirrorRowShifts' ascending order.
    const int32_t made[5] = {-80, -40, 0, 40, 80};
    bool ok = true;
    for (int32_t value = MaterialResolver::kMirrorFirst; value <= MaterialResolver::kMirrorLast;
         value++) {
        // The material's row shift, exactly as MaterialResolver writes it.
        const float rowShift =
            (MaterialResolver::kMirrorMiddle - value) * MaterialResolver::kMirrorRowsPerValue;
        const int32_t slot = pica::mirrorCubeFor(made, 5, rowShift);
        const bool listed = (value % 5) == 0;   // 990, 995, 1000, 1005, 1010
        int32_t off = slot < 0 ? 1000 : made[slot] - (int32_t)rowShift;
        if (off < 0) off = -off;
        if (listed ? off != 0 : off > 20) ok = false;
    }
    check(ok, "a mirror draw samples the cube of its own row shift, else the nearest made one");
    // A model's value no arena uses (LEVEL6's XF: 1009, -72 rows): the -80 cube.
    check(pica::mirrorCubeFor(made, 5, -72.0f) == 0, "-72 rows takes the -80 cube");
    check(pica::mirrorCubeFor(made, 5, 32.0f) == 3, "+32 rows takes the +40 cube");
    check(pica::mirrorCubeFor(made, 0, 0.0f) == -1, "no cubes, no slot");
}

void testFaceExpansion() {
    // 16x16, two tiles a row: the layout is streamTexelsRgb565's (no zeros, so
    // the two agree everywhere), and index 0 is palette[0]'s colour, not black.
    const int32_t size = 16;
    uint8_t palette[256 * 4];
    for (int32_t e = 0; e < 256; e++) {
        palette[e * 4 + 0] = (uint8_t)e;
        palette[e * 4 + 1] = (uint8_t)(255 - e);
        palette[e * 4 + 2] = (uint8_t)(e * 3);
        palette[e * 4 + 3] = 255;
    }
    palette[0] = 255;   // entry 0: (255, 255, 0), plainly not black
    std::vector<uint8_t> indices((size_t)(size * size));
    for (size_t i = 0; i < indices.size(); i++) {
        indices[i] = (uint8_t)(1 + (i * 13) % 255);
    }
    std::vector<uint8_t> a((size_t)(size * size * 2)), b((size_t)(size * size * 2));
    pica::mirrorFaceTexelsRgb565(indices.data(), size, palette, a.data());
    pica::streamTexelsRgb565(indices.data(), size, size, palette, size, b.data());
    check(a == b, "a face is tiled as every other RGB565 texture");
    indices[0] = 0;
    pica::mirrorFaceTexelsRgb565(indices.data(), size, palette, a.data());
    const uint16_t word = (uint16_t)(a[0] | (a[1] << 8));   // texel (0,0) is slot 0
    check(word == pica::rgb565Word(255, 255, 0), "index 0 is palette[0]'s colour (no discard)");
}

// THE BAKED FILE'S HEADER: written, accepted as itself, refused for each field
// that differs (render/mirror_cube.h, THE FILE).
void testBakedHeader() {
    MirrorBakedHeader h;
    h.version = kMirrorBakedVersion;
    h.size = kMirrorBakedSize;
    h.format = kMirrorBakedEtc1;
    h.rowShift = -40;
    h.faceBytes = kMirrorBakedFaceBytes;
    h.stamp = 0x1234567890abcdefull;
    uint8_t bytes[kMirrorBakedHeaderBytes];
    mirrorBakedHeaderWrite(h, bytes);
    const char* why = "unset";
    check(mirrorBakedHeaderAccepts(bytes, sizeof(bytes), -40, h.stamp, &why) && !why,
          "a baked header is accepted by the shift and stamp it was written with");
    check(!mirrorBakedHeaderAccepts(bytes, sizeof(bytes), 40, h.stamp, &why) && why,
          "another row shift is refused");
    check(!mirrorBakedHeaderAccepts(bytes, sizeof(bytes), -40, h.stamp ^ 1u, &why) && why,
          "a stale stamp is refused");
    check(!mirrorBakedHeaderAccepts(bytes, 63, -40, h.stamp, &why), "a short header is refused");
    uint8_t other[kMirrorBakedHeaderBytes];
    memcpy(other, bytes, sizeof(other));
    other[8] = (uint8_t)(kMirrorBakedVersion + 1);
    check(!mirrorBakedHeaderAccepts(other, sizeof(other), -40, h.stamp, &why),
          "another version is refused");
    memcpy(other, bytes, sizeof(other));
    other[12] = 0;   // the face size 512 (0x200, little-endian) made 256 (0x100)
    other[13] = 1;
    check(!mirrorBakedHeaderAccepts(other, sizeof(other), -40, h.stamp, &why),
          "another face size is refused");
    memcpy(other, bytes, sizeof(other));
    other[0] = 'X';
    check(!mirrorBakedHeaderAccepts(other, sizeof(other), -40, h.stamp, &why),
          "another magic is refused");
    char path[64];
    mirrorBakedPath("romfs:/orig", 6, -80, path, sizeof(path));
    check(strcmp(path, "romfs:/orig/mirror/level6_-80.bin") == 0, "the baked cube's path");
}

// AN ETC1 BLOCK as the PICA holds it (tex3ds' layout: the standard block's 64
// bits stored little-endian), decoded to RGB for texel (x, y) of the block,
// y = 0 the block's first row.
void etc1Texel(const uint8_t* block, int32_t x, int32_t y, uint8_t* rgb) {
    uint64_t v = 0;
    for (int32_t i = 7; i >= 0; i--) {
        v = (v << 8) | block[i];
    }
    static const int32_t kTables[8][2] = {{2, 8},   {5, 17},  {9, 29},  {13, 42},
                                          {18, 60}, {24, 80}, {33, 106}, {47, 183}};
    const bool diff = ((v >> 33) & 1u) != 0;
    const bool flip = ((v >> 32) & 1u) != 0;
    const int32_t sub = flip ? (y >= 2 ? 1 : 0) : (x >= 2 ? 1 : 0);
    int32_t base[3];
    for (int32_t c = 0; c < 3; c++) {
        const int32_t shift = 59 - c * 8;   // R at 63..59 / 63..60, G at 55.., B at 47..
        if (diff) {
            const int32_t a = (int32_t)((v >> shift) & 31u);
            int32_t d = (int32_t)((v >> (shift - 3)) & 7u);
            if (d >= 4) d -= 8;
            const int32_t five = sub == 0 ? a : a + d;
            base[c] = (five << 3) | (five >> 2);
        } else {
            const int32_t four = (int32_t)((v >> (sub == 0 ? shift + 1 : shift - 3)) & 15u);
            base[c] = (four << 4) | four;
        }
    }
    const int32_t table = (int32_t)((v >> (sub == 0 ? 37 : 34)) & 7u);
    const int32_t bit = x * 4 + y;
    const int32_t msb = (int32_t)((v >> (bit + 16)) & 1u);
    const int32_t lsb = (int32_t)((v >> bit) & 1u);
    const int32_t magnitude = kTables[table][lsb];
    const int32_t delta = msb ? -magnitude : magnitude;
    for (int32_t c = 0; c < 3; c++) {
        const int32_t value = base[c] + delta;
        rgb[c] = (uint8_t)(value < 0 ? 0 : value > 255 ? 255 : value);
    }
}

// A face's texel (x, y) -- y counted from the first memory row -- of the PICA's
// tiled ETC1: 8x8 tiles row by row, four 4x4 blocks a tile in Z order.
void etc1FaceTexel(const uint8_t* face, int32_t size, int32_t x, int32_t y, uint8_t* rgb) {
    const int32_t tile = (y >> 3) * (size >> 3) + (x >> 3);
    const int32_t block = ((y >> 2) & 1) * 2 + ((x >> 2) & 1);
    etc1Texel(face + ((size_t)tile * 4u + (size_t)block) * 8u, x & 3, y & 3, rgb);
}

} // namespace

// THE BAKED CUBES THE BUILD MADE (build-arena-etc1/mirror, when this worktree has
// built the 3DS RomFS): every texel of every face, decoded as the PICA reads
// ETC1, against the palette colour of the cell the runtime formula gives it at
// kMirrorBakedSize -- within ETC1's loss on average, and much nearer than the
// same face read upside down (the orientation is what a wrong layout breaks).
int runMirrorBakedTests(const formats::MdkData& data, bool* skipped) {
    g_failures = 0;
    *skipped = false;
    mathLutInit();
    testBakedHeader();
    const char* root = "../../build-arena-etc1/mirror";
    char path[128];
    snprintf(path, sizeof(path), "%s/level6_0.bin", root);
    FILE* probe = fopen(path, "rb");
    if (!probe) {
        printf("  mirror baked cubes: %s not built (stage the 3DS RomFS first)\n", path);
        *skipped = true;
        return g_failures;
    }
    fclose(probe);
    LevelData level;
    if (!LevelData::load(&level, &data, 6)) {
        check(false, "LEVEL6 loads");
        return g_failures;
    }
    const formats::Dti& d = level.dti();
    const formats::Texture& panorama = d.mirrorSky();
    const uint8_t* pal = d.palette().rgba();
    const uint64_t stamp = mirrorPanoramaStamp(panorama, pal, d.skyWrapWidth(),
                                               d.skyHorizonRow(), d.skyOffset());
    int32_t shifts[kMirrorRowShiftsMax];
    const int32_t count = mirrorRowShifts(level.arenas(), shifts);
    const int32_t size = kMirrorBakedSize;
    const size_t texels = (size_t)size * (size_t)size;
    std::vector<int16_t> columns(texels);
    std::vector<int16_t> rows(texels);
    std::vector<uint8_t> face(texels);
    for (int32_t i = 0; i < count; i++) {
        snprintf(path, sizeof(path), "%s/level6_%d.bin", root, (int)shifts[i]);
        FILE* file = fopen(path, "rb");
        std::vector<uint8_t> bytes;
        if (file) {
            uint8_t chunk[65536];
            size_t got = 0;
            while ((got = fread(chunk, 1, sizeof(chunk), file)) > 0) {
                bytes.insert(bytes.end(), chunk, chunk + got);
            }
            fclose(file);
        }
        const char* why = nullptr;
        const bool whole = bytes.size() == (size_t)kMirrorBakedHeaderBytes +
                                               6u * (size_t)kMirrorBakedFaceBytes;
        check(whole && mirrorBakedHeaderAccepts(bytes.data(), bytes.size(), shifts[i], stamp,
                                                &why),
              "LEVEL6's baked cube is whole and stamped with this data's panorama");
        if (!whole) {
            continue;
        }
        for (int32_t f = 0; f < kMirrorCubeFaces; f++) {
            mirrorCubeCells(f, size, d.skyWrapWidth(), d.skyHorizonRow(), d.skyOffset(),
                            panorama.height(), columns.data(), rows.data());
            mirrorCubeFace(panorama, columns.data(), rows.data(), (int32_t)texels, shifts[i],
                           face.data());
            const uint8_t* blocks = bytes.data() + kMirrorBakedHeaderBytes +
                                    (size_t)f * (size_t)kMirrorBakedFaceBytes;
            double error = 0.0;
            double flipped = 0.0;
            for (int32_t y = 0; y < size; y++) {
                for (int32_t x = 0; x < size; x++) {
                    uint8_t rgb[3];
                    etc1FaceTexel(blocks, size, x, y, rgb);
                    const uint8_t* want = pal + face[(size_t)y * (size_t)size + (size_t)x] * 4;
                    const uint8_t* upside =
                        pal + face[(size_t)(size - 1 - y) * (size_t)size + (size_t)x] * 4;
                    for (int32_t c = 0; c < 3; c++) {
                        error += fabs((double)rgb[c] - (double)want[c]);
                        flipped += fabs((double)rgb[c] - (double)upside[c]);
                    }
                }
            }
            error /= (double)texels * 3.0;
            flipped /= (double)texels * 3.0;
            if (i == 0 || f == 0) {
                printf("  mirror baked: LEVEL6 shift %d face %d: mean |error| %.2f "
                       "(read upside down %.2f)\n", (int)shifts[i], (int)f, error, flipped);
            }
            check(error < 8.0, "a baked face decodes to the runtime formula's colours "
                               "within ETC1's loss (mean |error| under 8 of 255)");
            check(error * 2.0 < flipped, "a baked face is the right way up (twice as near as "
                                         "the same face upside down)");
        }
    }
    return g_failures;
}

int runMirrorCubeTests() {
    g_failures = 0;
    mathLutInit();
    testFaceConvention();
    testCellsAgainstTheOracle();
    testShiftAndWrap();
    testDrawFindsItsCube();
    testFaceExpansion();
    return g_failures;
}

int runMirrorCubeDataTests(const formats::MdkData& data) {
    g_failures = 0;
    struct Level {
        int32_t number;
        int32_t count;
        int32_t shifts[5];
    };
    // The census of the retail arenas (2026-11-27): LEVEL6's OLYM_5/6/7 carry
    // all five of MIRRLOW...MIRRHIGH's used values; LEVEL7 has no mirror.
    const Level levels[2] = {{6, 5, {-80, -40, 0, 40, 80}}, {7, 0, {0, 0, 0, 0, 0}}};
    for (int32_t l = 0; l < 2; l++) {
        char path[64];
        snprintf(path, sizeof(path), "TRAVERSE/LEVEL%d/LEVEL%dO.MTO", (int)levels[l].number,
                 (int)levels[l].number);
        std::vector<uint8_t> bytes;
        if (!data.read(path, &bytes)) {
            check(false, "the level's arenas read");
            continue;
        }
        formats::Mto mto = formats::Mto::parse(bytes.data(), (int32_t)bytes.size());
        std::vector<const formats::Arena*> arenas;
        for (const std::string& name : mto.arenaNames()) {
            if (const formats::Arena* a = mto.getArena(name)) {
                arenas.push_back(a);
            }
        }
        int32_t shifts[kMirrorRowShiftsMax];
        const int32_t count = mirrorRowShifts(arenas, shifts);
        bool same = count == levels[l].count;
        for (int32_t i = 0; same && i < count; i++) {
            same = shifts[i] == levels[l].shifts[i];
        }
        check(same, levels[l].number == 6 ? "LEVEL6's mirror row shifts are -80,-40,0,40,80"
                                          : "LEVEL7 has no mirror");
    }
    return g_failures;
}

} // namespace test
} // namespace mdk

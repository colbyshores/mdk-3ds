// ============================================================================
// sun_bake_tests.cpp -- see sun_bake_tests.h.
// ============================================================================

#include "sun_bake_tests.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "render/sun_bake.h"
#include "sun_baker.h"

namespace mdk {
namespace test {

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        g_failures++;
        printf("  FAIL sun_bake: %s\n", what);
    }
}

struct P3 {
    float x, y, z;
};

// A quad as two triangles whose FRONT (the outward normal, -cross(p1 - p0,
// p2 - p0): game/level_light.cpp faceOf) points along `outward`.
void quad(std::vector<BakeTriangle>* out, P3 a, P3 b, P3 c, P3 d, P3 outward, bool caster,
          bool doubleSided, int32_t rig = 0) {
    const P3 corners[2][3] = {{a, b, c}, {a, c, d}};
    for (int t = 0; t < 2; t++) {
        P3 p0 = corners[t][0], p1 = corners[t][1], p2 = corners[t][2];
        const P3 e1{p1.x - p0.x, p1.y - p0.y, p1.z - p0.z};
        const P3 e2{p2.x - p0.x, p2.y - p0.y, p2.z - p0.z};
        const P3 front{-(e1.y * e2.z - e1.z * e2.y), -(e1.z * e2.x - e1.x * e2.z),
                       -(e1.x * e2.y - e1.y * e2.x)};
        if (front.x * outward.x + front.y * outward.y + front.z * outward.z < 0.0f) {
            const P3 swap = p1;
            p1 = p2;
            p2 = swap;
        }
        BakeTriangle tri;
        const P3 ps[3] = {p0, p1, p2};
        for (int k = 0; k < 3; k++) {
            tri.p[k][0] = ps[k].x;
            tri.p[k][1] = ps[k].y;
            tri.p[k][2] = ps[k].z;
        }
        tri.receiver = true;
        tri.caster = caster;
        tri.doubleSided = doubleSided;
        tri.rig = rig;
        out->push_back(tri);
    }
}

// The floor: 100 x 100 at z = 0, facing up.
void addFloor(std::vector<BakeTriangle>* out, int32_t rig = 0) {
    quad(out, P3{-50, -50, 0}, P3{50, -50, 0}, P3{50, 50, 0}, P3{-50, 50, 0}, P3{0, 0, 1}, true,
         false, rig);
}

// A closed box (Solid, outward faces) over [lo, hi].
void addBox(std::vector<BakeTriangle>* out, P3 lo, P3 hi) {
    const P3 c[8] = {{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z},
                     {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}};
    quad(out, c[0], c[1], c[2], c[3], P3{0, 0, -1}, true, false);   // bottom
    quad(out, c[4], c[5], c[6], c[7], P3{0, 0, 1}, true, false);    // top
    quad(out, c[0], c[1], c[5], c[4], P3{0, -1, 0}, true, false);
    quad(out, c[1], c[2], c[6], c[5], P3{1, 0, 0}, true, false);
    quad(out, c[2], c[3], c[7], c[6], P3{0, 1, 0}, true, false);
    quad(out, c[3], c[0], c[4], c[7], P3{-1, 0, 0}, true, false);
}

// A rig with distinct channels and a sun, as an open arena's.
BakeRig openRig() {
    BakeRig r;
    const float sky[3] = {0.55f, 0.6f, 0.75f};
    const float ground[3] = {0.25f, 0.2f, 0.15f};
    const float sun[3] = {0.9f, 0.8f, 0.6f};
    memcpy(r.sky, sky, sizeof(sky));
    memcpy(r.ground, ground, sizeof(ground));
    memcpy(r.sun, sun, sizeof(sun));
    r.exposure = 1.2f;
    r.shadows = true;
    return r;
}

BakeSettings downSun() {
    BakeSettings s;
    s.sunDirection[0] = 0.0f;
    s.sunDirection[1] = 0.0f;
    s.sunDirection[2] = -1.0f;   // straight down
    s.texelSize = 1.0f;
    s.depthCell = 1.0f;   // the objects' map at a unit a cell (the game's is 8+)
    s.rigs.push_back(openRig());
    return s;
}

// THE SHADER, rewritten here from mdk_c3d.g.pica (not from the baker): t =
// n.z * 0.5 + 0.5; light = ground + (sky - ground) * t + sun * max(dot(-sunDir,
// n), 0) (the sun term only where it reaches); * exposure; F = pow(light,
// 1/2.2) (lg2, * 0.45454545, ex2); emitted as min(F * 0.5, 1). In 0..255.
void shaderLight(const BakeRig& rig, P3 n, const float sun[3], bool reaches, float out[3]) {
    const float t = 0.5f + 0.5f * n.z;
    const float facing = -(sun[0] * n.x + sun[1] * n.y + sun[2] * n.z);
    for (int c = 0; c < 3; c++) {
        float light = rig.ground[c] + (rig.sky[c] - rig.ground[c]) * t;
        if (reaches && facing > 0.0f) {
            light += rig.sun[c] * facing;
        }
        light *= rig.exposure;
        if (light < 0.0f) {
            light = 0.0f;
        }
        const float half = 0.5f * exp2f(0.45454545f * log2f(light));
        out[c] = 255.0f * (half < 1.0f ? half : 1.0f);
    }
}

// The light the RUNTIME would make at a world point on a side, in F / 2 bytes:
// the side's key -> its vertex colour and its chart -> the uv (sun_bake.cpp uvOf,
// t from the top as tex3ds stores it) -> the nearest texel's m; colour * m.
bool colourAt(const SunBake& bake, const SunBakeImage& image, const BakeTriangle& t, bool back,
              P3 p, float out[3]) {
    int32_t chart = -1;
    uint32_t colour = 0;
    if (!bake.sideOf(back ? sunBakeTriangleKey(t.p[0], t.p[2], t.p[1])
                          : sunBakeTriangleKey(t.p[0], t.p[1], t.p[2]),
                     &chart, &colour)) {
        return false;
    }
    const float at[3] = {p.x, p.y, p.z};
    int16_t uv[2];
    bake.uvOf(chart, at, uv);
    const float x = (float)uv[0] / kSunBakeUvOne * (float)image.atlasWidth;
    const float y = (1.0f - (float)uv[1] / kSunBakeUvOne) * (float)image.atlasHeight;
    const int32_t tx = (int32_t)floorf(x), ty = (int32_t)floorf(y);
    if (tx < 0 || ty < 0 || tx >= image.atlasWidth || ty >= image.atlasHeight) {
        return false;
    }
    const float m = (float)image.grey[(size_t)ty * (size_t)image.atlasWidth + (size_t)tx] / 255.0f;
    for (int c = 0; c < 3; c++) {
        out[c] = (float)((colour >> (8 * c)) & 0xFFu) * m;
    }
    return true;
}

// What the bake makes of a full shadow: F_lit times the mean of the channels'
// F_shadow / F_lit (sun_bake.h: the grey map is one channel).
void meanShadow(const float lit[3], const float shadow[3], float out[3]) {
    float ratio = 0.0f;
    for (int c = 0; c < 3; c++) {
        ratio += lit[c] > 0.0f ? shadow[c] / lit[c] : 1.0f;
    }
    ratio /= 3.0f;
    for (int c = 0; c < 3; c++) {
        out[c] = lit[c] * ratio;
    }
}

bool near(const float a[3], const float b[3], float tolerance) {
    for (int c = 0; c < 3; c++) {
        if (fabsf(a[c] - b[c]) > tolerance) {
            return false;
        }
    }
    return true;
}

// The floor triangle that holds (x, y): the floor is the first two triangles
// (triangle 0 is the half below the diagonal y = x).
const BakeTriangle& floorAt(const std::vector<BakeTriangle>& scene, float x, float y) {
    return y <= x ? scene[0] : scene[1];
}

bool bake(const std::vector<BakeTriangle>& scene, const BakeSettings& settings,
          SunBakeImage* image, std::vector<uint8_t>* bytes, SunBake* reader) {
    BakeReport report;
    if (!bakeSunShadows(scene, settings, image, &report)) {
        return false;
    }
    if (!writeSunBake(*image, bytes)) {
        return false;
    }
    const char* why = nullptr;
    return reader->parse(bytes->data(), bytes->size(), &why);
}

const P3 kUp{0, 0, 1};

} // namespace

int runSunBakeTests() {
    g_failures = 0;
    // (Rounding: the baker stores round(F/2 * 255) and m to a byte, the shader's
    // own float24 arithmetic and the host's powf differ below a byte.)
    const float kByte = 1.5f;

    // 1. A BOX OVER A FLOOR, the sun straight down: the floor under the box is
    // F_shadow, the rest F_lit -- the shader's own formula, to the byte.
    {
        std::vector<BakeTriangle> scene;
        addFloor(&scene);
        addBox(&scene, P3{-5, -5, 20}, P3{5, 5, 30});
        const BakeSettings s = downSun();
        SunBakeImage image;
        std::vector<uint8_t> bytes;
        SunBake reader;
        check(bake(scene, s, &image, &bytes, &reader), "box over floor: bakes and parses");
        float lit[3], exactShadow[3], shadow[3], got[3];
        shaderLight(s.rigs[0], kUp, s.sunDirection, true, lit);
        shaderLight(s.rigs[0], kUp, s.sunDirection, false, exactShadow);
        meanShadow(lit, exactShadow, shadow);
        // (The mean ratio against the exact per-channel F_shadow is the baker's
        // report, per level: this rig is deliberately more saturated than any
        // arena's, so its own gap says nothing about the game.)
        check(colourAt(reader, image, floorAt(scene, 0, 0), false, P3{0, 0, 0}, got) &&
                  near(got, shadow, kByte),
              "the floor under the box's middle is F_lit times the shadow's ratio");
        check(colourAt(reader, image, floorAt(scene, 3.5f, -3.5f), false, P3{3.5f, -3.5f, 0}, got) &&
                  near(got, shadow, kByte),
              "the floor under the box's corner is F_lit times the shadow's ratio");
        check(colourAt(reader, image, floorAt(scene, 30, 30), false, P3{30, 30, 0}, got) &&
                  near(got, lit, kByte),
              "the floor away from the box is F_lit (the vertex colour, m = 1)");
        check(colourAt(reader, image, floorAt(scene, 8, -8), false, P3{8, -8, 0}, got) &&
                  near(got, lit, kByte),
              "the floor just past the box's edge is F_lit");
        // The box's top: lit, flat: its vertex colour, the white block.
        float top[3];
        check(colourAt(reader, image, scene[4], false, P3{0, 0, 30}, top) && near(top, lit, kByte),
              "the box's top is F_lit (an up face in the sun)");
        // A side, edge-on to a straight-down sun: the hemisphere at t = 0.5.
        float side[3], wantSide[3];
        shaderLight(s.rigs[0], P3{0, -1, 0}, s.sunDirection, true, wantSide);
        check(colourAt(reader, image, scene[6], false, P3{0, -5, 25}, side) &&
                  near(side, wantSide, kByte),
              "a side face is the shader's hemisphere light");
        // The bottom faces down: away from the sun, F_shadow of a down normal.
        float bottom[3], wantBottom[3];
        shaderLight(s.rigs[0], P3{0, 0, -1}, s.sunDirection, true, wantBottom);
        check(colourAt(reader, image, scene[2], false, P3{0, 0, 20}, bottom) &&
                  near(bottom, wantBottom, kByte),
              "a face turned from the sun is the shader's light without the sun");
        // The floor has a shadow edge: a chart of its own, past the blocks.
        const int32_t floorChart =
            reader.chartOf(sunBakeTriangleKey(scene[0].p[0], scene[0].p[1], scene[0].p[2]));
        check(floorChart >= (int32_t)reader.header().colourBlocks,
              "the floor is an atlas chart (a shadow edge crosses it)");
        const int32_t topChart =
            reader.chartOf(sunBakeTriangleKey(scene[4].p[0], scene[4].p[1], scene[4].p[2]));
        check(topChart == 0, "an all-lit side samples the white block");
        // Every block sampled at its centre.
        bool aligned = true;
        for (uint32_t b = 0; b < reader.header().colourBlocks; b++) {
            int16_t uv[2];
            const float any[3] = {1.0f, 2.0f, 3.0f};
            reader.uvOf((int32_t)b, any, uv);
            const float x = (float)uv[0] / kSunBakeUvOne * (float)image.atlasWidth;
            aligned = aligned && fabsf(fmodf(x - 2.0f, 4.0f)) < 0.05f;
        }
        check(aligned, "every grey block is sampled at its 4x4 block's centre");
        const float shaded[3] = {0.0f, 0.0f, 0.5f};
        const float sunny[3] = {30.0f, 30.0f, 0.5f};
        check(sunBlockedAt(scene, s.sunDirection, shaded) &&
                  !sunBlockedAt(scene, s.sunDirection, sunny),
              "the ground-truth ray agrees");

        // THE OBJECTS' DEPTH MAP: a point on the floor under the box is dark, one
        // in the open is lit, one above the box is lit.
        const float under[3] = {0.0f, 0.0f, 0.0f};
        const float open[3] = {30.0f, -30.0f, 0.0f};
        const float above[3] = {0.0f, 0.0f, 40.0f};
        const float far[3] = {5000.0f, 0.0f, 0.0f};
        check(reader.lightAt(under, 2.0f) < 0.01f, "an object under the box is shadowed");
        check(reader.lightAt(open, 2.0f) > 0.99f, "an object in the open is lit");
        check(reader.lightAt(above, 2.0f) > 0.99f, "an object over the box is lit");
        check(reader.lightAt(far, 2.0f) == 1.0f, "an object off the map is lit");
    }

    // 2. THE TILTED SUN: the shadow is displaced along the light.
    {
        std::vector<BakeTriangle> scene;
        addFloor(&scene);
        addBox(&scene, P3{-5, -5, 20}, P3{5, 5, 30});
        BakeSettings s = downSun();
        s.sunDirection[0] = 0.4472136f;   // (+0.5, 0, -1) normalised
        s.sunDirection[1] = 0.0f;
        s.sunDirection[2] = -0.8944272f;
        SunBakeImage image;
        std::vector<uint8_t> bytes;
        SunBake reader;
        check(bake(scene, s, &image, &bytes, &reader), "tilted sun: bakes");
        float lit[3], exactShadow[3], shadow[3], got[3];
        shaderLight(s.rigs[0], kUp, s.sunDirection, true, lit);
        shaderLight(s.rigs[0], kUp, s.sunDirection, false, exactShadow);
        meanShadow(lit, exactShadow, shadow);
        // A box bottom at z 20 shadows x in [5, 20] at z 0.
        check(colourAt(reader, image, floorAt(scene, 12, 0), false, P3{12, 0, 0}, got) &&
                  near(got, shadow, kByte),
              "tilted: the floor along the light is in the shadow");
        check(colourAt(reader, image, floorAt(scene, -3, 0), false, P3{-3, 0, 0}, got) &&
                  near(got, lit, kByte),
              "tilted: the floor straight under the box is F_lit (with the tilted sun's facing)");
    }

    // 3. ONE-SIDED AGAINST TWO-SIDED CASTERS: a panel over the floor whose front
    // faces down blocks nothing as a Solid caster and blocks as a DoubleSided
    // one; front up, it blocks as Solid too.
    {
        const P3 a{-5, -5, 20}, b{5, -5, 20}, c{5, 5, 20}, d{-5, 5, 20};
        for (int variant = 0; variant < 3; variant++) {
            std::vector<BakeTriangle> scene;
            addFloor(&scene);
            quad(&scene, a, b, c, d, variant == 2 ? P3{0, 0, 1} : P3{0, 0, -1}, true, variant == 1);
            const BakeSettings s = downSun();
            SunBakeImage image;
            std::vector<uint8_t> bytes;
            SunBake reader;
            float lit[3], exactShadow[3], shadow[3], got[3];
            shaderLight(s.rigs[0], kUp, s.sunDirection, true, lit);
            shaderLight(s.rigs[0], kUp, s.sunDirection, false, exactShadow);
            meanShadow(lit, exactShadow, shadow);
            const bool ok = bake(scene, s, &image, &bytes, &reader) &&
                            colourAt(reader, image, floorAt(scene, 0, 0), false, P3{0, 0, 0}, got);
            if (variant == 0) {
                check(ok && near(got, lit, kByte), "a Solid panel with its back to the sun casts nothing");
            } else if (variant == 1) {
                check(ok && near(got, shadow, kByte), "a DoubleSided panel casts with either side up");
            } else {
                check(ok && near(got, shadow, kByte), "a Solid panel with its front to the sun casts");
            }
        }
    }

    // 4. A TWO-SIDED RECEIVER has both sides, keyed by their own windings, each
    // lit by its own normal (the shader turned the normal to the camera); a
    // one-sided one has no back key.
    {
        std::vector<BakeTriangle> scene;
        addFloor(&scene);
        const size_t wall = scene.size();
        quad(&scene, P3{-40, 20, 0}, P3{-20, 20, 0}, P3{-20, 20, 10}, P3{-40, 20, 10}, P3{0, -1, 0},
             true, true);
        BakeSettings s = downSun();
        s.sunDirection[0] = 0.0f;
        s.sunDirection[1] = 0.5f;   // shining towards +y: the wall's -y side faces the sun
        s.sunDirection[2] = -0.8660254f;
        SunBakeImage image;
        std::vector<uint8_t> bytes;
        SunBake reader;
        check(bake(scene, s, &image, &bytes, &reader), "two-sided: bakes");
        float front[3], back[3], wantFront[3], wantBack[3];
        shaderLight(s.rigs[0], P3{0, -1, 0}, s.sunDirection, true, wantFront);
        shaderLight(s.rigs[0], P3{0, 1, 0}, s.sunDirection, true, wantBack);
        check(colourAt(reader, image, scene[wall], false, P3{-30, 20, 5}, front) &&
                  near(front, wantFront, kByte),
              "the outward side: its normal's light, with the sun");
        check(colourAt(reader, image, scene[wall], true, P3{-30, 20, 5}, back) &&
                  near(back, wantBack, kByte),
              "the inward side: the reversed normal's light, no sun");
        check(reader.chartOf(sunBakeTriangleKey(scene[0].p[0], scene[0].p[2], scene[0].p[1])) < 0,
              "a one-sided floor has no back key");
    }

    // 5. A COVERED ARENA (its rig: shadows off, no sun) has no shadow under the
    // box, and colours of a kind share one block.
    {
        std::vector<BakeTriangle> scene;
        addFloor(&scene, 1);
        addBox(&scene, P3{-5, -5, 20}, P3{5, 5, 30});
        BakeSettings s = downSun();
        BakeRig covered = openRig();
        covered.shadows = false;
        covered.sun[0] = covered.sun[1] = covered.sun[2] = 0.0f;
        s.rigs.push_back(covered);
        SunBakeImage image;
        std::vector<uint8_t> bytes;
        SunBake reader;
        check(bake(scene, s, &image, &bytes, &reader), "covered: bakes");
        float want[3], got[3];
        shaderLight(covered, kUp, s.sunDirection, true, want);
        check(colourAt(reader, image, floorAt(scene, 0, 0), false, P3{0, 0, 0}, got) &&
                  near(got, want, kByte),
              "a covered arena's floor is its hemisphere light, under the box too");
        const int32_t c0 =
            reader.chartOf(sunBakeTriangleKey(scene[0].p[0], scene[0].p[1], scene[0].p[2]));
        const int32_t c1 =
            reader.chartOf(sunBakeTriangleKey(scene[1].p[0], scene[1].p[1], scene[1].p[2]));
        check(c0 == 0 && c1 == 0, "no shadow, the white block");
    }

    // 6. DETERMINISM: the same scene and settings, the same bytes and atlas.
    {
        std::vector<BakeTriangle> scene;
        addFloor(&scene);
        addBox(&scene, P3{-5, -5, 20}, P3{5, 5, 30});
        addBox(&scene, P3{20, 10, 5}, P3{30, 14, 40});
        BakeSettings s = downSun();
        s.sunDirection[0] = -0.33f;
        s.sunDirection[1] = 0.47f;
        s.sunDirection[2] = -0.82f;
        SunBakeImage one, two;
        std::vector<uint8_t> first, second;
        SunBake r1, r2;
        check(bake(scene, s, &one, &first, &r1) && bake(scene, s, &two, &second, &r2) &&
                  first == second && one.grey == two.grey,
              "the same input bakes the same bytes");
    }

    // 7. THE FILE: the neutral block, the refusals, a whole mesh's uvs.
    {
        std::vector<BakeTriangle> scene;
        addFloor(&scene);
        addBox(&scene, P3{-5, -5, 20}, P3{5, 5, 30});
        SunBakeImage image;
        std::vector<uint8_t> bytes;
        SunBake reader;
        check(bake(scene, downSun(), &image, &bytes, &reader), "file: bakes");
        const SunBakeHeader& h = reader.header();
        check(h.format == kSunBakeL4 && h.fileBytes == bytes.size() &&
                  image.grey.size() == (size_t)image.atlasWidth * image.atlasHeight,
              "an L4 bake, the atlas beside the file");
        check(image.grey[0] == 255, "the white block (m = 1) at the top left");
        int32_t none = 0;
        uint32_t neutral = 0;
        check(!reader.sideOf(12345u, &none, &neutral) && none < 0 &&
                  neutral == kSunBakeNeutralColour,
              "an unknown side: no chart, the neutral colour (F / 2 = 1/2)");
        int16_t uv[2];
        const float anywhere[3] = {123.0f, -4.0f, 9.0f};
        reader.uvOf(-1, anywhere, uv);
        check(fabsf((float)uv[0] / kSunBakeUvOne * (float)h.atlasWidth - 2.0f) < 0.01f &&
                  fabsf((1.0f - (float)uv[1] / kSunBakeUvOne) * (float)h.atlasHeight - 2.0f) < 0.01f,
              "no key: the white block's centre, t from the image's top");
        SunBake refused;
        const char* why = nullptr;
        check(!refused.parse(bytes.data(), bytes.size() - 16, &why), "a truncated file is refused");
        std::vector<uint8_t> bad(bytes);
        bad[0] = 'X';
        check(!refused.parse(bad.data(), bad.size(), &why), "a wrong magic is refused");
        if (h.chartCount > 1) {
            bad = bytes;
            const float nan = nanf("");
            memcpy(bad.data() + h.chartsOffset + sizeof(SunBakeChart) + 4, &nan, 4);
            check(!refused.parse(bad.data(), bad.size(), &why),
                  "a chart mapping that is not finite is refused");
        }
        if (h.keyCount > 0) {
            bad = bytes;
            const uint16_t wild = 0xFFFF;
            memcpy(bad.data() + h.keyChartsOffset, &wild, 2);
            check(!refused.parse(bad.data(), bad.size(), &why), "a key naming no chart is refused");
        }
        // A whole mesh's uvs at mdk::Vertex's stride (six floats).
        std::vector<float> mesh;
        for (size_t i = 0; i < scene.size(); i++) {
            for (int k = 0; k < 3; k++) {
                mesh.push_back(scene[i].p[k][0]);
                mesh.push_back(scene[i].p[k][1]);
                mesh.push_back(scene[i].p[k][2]);
                mesh.push_back(0.0f);
                mesh.push_back(0.0f);
                mesh.push_back(0.0f);
            }
        }
        std::vector<uint8_t> data(scene.size() * 3 * (size_t)kSunBakeSideStride);
        check(reader.meshSides(mesh.data(), (int32_t)scene.size() * 3, 6, false, data.data()) ==
                  (int32_t)scene.size(),
              "meshSides: every Solid triangle's front is keyed");
        check(reader.meshSides(mesh.data(), (int32_t)scene.size() * 3, 6, true, data.data()) == 0,
              "meshSides: no Solid triangle has a back");
        reader.meshSides(mesh.data(), (int32_t)scene.size() * 3, 6, false, data.data());
        int32_t chart = -1;
        uint32_t colour = 0;
        reader.sideOf(sunBakeTriangleKey(scene[0].p[0], scene[0].p[1], scene[0].p[2]), &chart,
                      &colour);
        int16_t direct[2];
        reader.uvOf(chart, scene[0].p[1], direct);
        int16_t stored[2];
        uint32_t storedColour = 0;
        memcpy(stored, &data[(size_t)kSunBakeSideStride], 4);
        memcpy(&storedColour, &data[(size_t)kSunBakeSideStride + 4], 4);
        check(stored[0] == direct[0] && stored[1] == direct[1] && storedColour == colour,
              "meshSides: corner by corner, uvOf's uv and the side's colour");
        uint32_t corner0 = 0, corner2 = 0;
        memcpy(&corner0, &data[4], 4);
        memcpy(&corner2, &data[2 * (size_t)kSunBakeSideStride + 4], 4);
        check(corner0 == colour && corner2 == colour, "one colour on a side's three corners");
    }

    // 8. THE BAKER'S FORMULA IS THE SHADER'S: sunBakeLight against the rewrite
    // above over a sweep of normals, with and without the sun.
    {
        const BakeSettings s = downSun();
        const float sun[3] = {-0.33f, 0.47f, -0.82f};
        float worst = 0.0f;
        for (int i = 0; i < 64; i++) {
            const float a = (float)i * 0.39f, b = (float)i * 0.23f - 1.5f;
            const P3 n{cosf(b) * cosf(a), cosf(b) * sinf(a), sinf(b)};
            const float nn[3] = {n.x, n.y, n.z};
            for (int reach = 0; reach < 2; reach++) {
                float baked[3], want[3];
                sunBakeLight(s.rigs[0], nn, sun, reach != 0, baked);
                shaderLight(s.rigs[0], n, sun, reach != 0, want);
                for (int c = 0; c < 3; c++) {
                    worst = fmaxf(worst, fabsf(baked[c] * 255.0f - want[c]));
                }
            }
        }
        check(worst < 0.05f, "sunBakeLight is the shader's formula");
    }

    return g_failures;
}

} // namespace test
} // namespace mdk

// ============================================================================
// sun_bake_level.cpp -- bake a level's static light for the 3DS
// (EnhancedToggles::lightBaked; mdk_3ds/src/render/sun_bake.h).
//
//   sun_bake_level <out dir> [level ...] [--l8] [--texel <units>]
//                  [--budget <bytes>] [--chart-max <texels>] [--cone <cos>]
//                  [--topdown <prefix>]
//
// (levels 3..8 by default) writes <out>/level<N>.sbk (charts, keys with each
// side's vertex colour, the objects' depth map) and <out>/level<N>.grey (the
// shadow's multiplier atlas, W x H bytes of m, rows from the top: sun_bake.sh
// hands it to tex3ds for the L4 .t3x), and prints one line a level plus its
// fidelity check.
// The data is the player's own copy, found the way the game finds it
// (MDK_DATA_DIR, ../MDK, ...).
//
// THE GEOMETRY IS THE RUNTIME'S OWN. Every arena of the level is built by
// buildArenaScene with the enhanced look's resolver (MaterialResolver with
// Shading::Lit, LevelView's own call), so the triangles, their corner order and
// their positions' bits are the ones mdk_render_create_mesh receives on the 3DS
// (sun_bake.h: the keys). Each batch is then classified as the real-time path
// classifies a draw:
//   receiver   pica::castsSunShadow(pass, shading, Triangles)
//   caster     receiver, the batch drawn at level start (arenaBatchVisible), and
//              its texture without holes (pica::textureSourceHasHoles over the
//              index source: castShadows leaves those out)
//   two-sided  the DoubleSided pass
//   rig        enhancedLighting(dti, arena): the rig the frame carries while
//              Kurt is in that arena (main_3ds.cpp, LevelLight.Update)
// Every arena of the level casts on every other: the real-time path casts what
// the frame draws (Kurt's arena and the active second one), and the arenas of a
// level are one space.
//
// THE FIDELITY CHECK: every baked side's centroid is looked up as the runtime
// does it (key -> colour and chart -> uv, t flipped -> the atlas's m,
// bilinearly), and colour * m is compared with the shader's own light there
// (sunBakeLight: F_lit where a ray from the centroid reaches the sun, the exact
// per-channel F_shadow where it does not). Unshadowed sides must match to the
// byte's rounding; a shadowed side carries the mean ratio's error (reported by
// the baker, per channel); sides on a chart with a shadow edge are reported
// apart (a centroid can sit on the edge's soft texels).
//
// HOST ONLY (mdk_3ds/tools/sun_bake/Makefile), never part of the 3DS build.
// ============================================================================

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <algorithm>
#include <string>
#include <vector>

#include "formats/arena.h"
#include "formats/file_system.h"
#include "formats/mdk_data.h"
#include "formats/texture.h"
#include "game/level_data.h"
#include "game/level_light.h"
#include "render/arena_scene.h"
#include "render/material_resolver.h"
#include "render/pica_matrix.h"
#include "render/sun_bake.h"
#include "sun_baker.h"
#include "test_support.h"

namespace {

bool writeFile(const std::string& path, const uint8_t* bytes, size_t size) {
    const std::string staged = path + ".tmp";
    FILE* file = fopen(staged.c_str(), "wb");
    if (!file) {
        return false;
    }
    const bool ok = fwrite(bytes, 1, size, file) == size;
    if (fclose(file) != 0 || !ok) {
        remove(staged.c_str());
        return false;
    }
    return rename(staged.c_str(), path.c_str()) == 0;
}

double nowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

bool textureHasHoles(const mdk::formats::Texture* texture) {
    if (!texture) {
        return false;   // a flat colour: the 3DS draws it with an opaque 1x1 texture
    }
    const int32_t texels = texture->width() * texture->height() * texture->frameCount();
    const uint8_t* indices = texture->indices();
    if (!indices || texels <= 0 || texture->indexCount() < texels) {
        return false;   // no pixels: never made at runtime either, so never drawn
    }
    return mdk::pica::textureSourceHasHoles(indices, (size_t)texels, true);
}

// The atlas's m at texel coordinates (x, y), bilinear like the PICA (0..255).
float sampleAtlas(const mdk::SunBakeImage& image, float x, float y) {
    const float fx = x - 0.5f, fy = y - 0.5f;
    const int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    const float wx = fx - (float)x0, wy = fy - (float)y0;
    float out = 0.0f;
    for (int j = 0; j < 2; j++) {
        for (int i = 0; i < 2; i++) {
            const int tx = std::min(image.atlasWidth - 1, std::max(0, x0 + i));
            const int ty = std::min(image.atlasHeight - 1, std::max(0, y0 + j));
            const float w = (i ? wx : 1.0f - wx) * (j ? wy : 1.0f - wy);
            out += w * (float)image.grey[(size_t)ty * image.atlasWidth + tx];
        }
    }
    return out;
}

struct Fidelity {
    int32_t litSides = 0, shadowSides = 0, chartSides = 0, missing = 0;
    float litMax = 0.0f;         // in 0..255 units of F / 2
    float shadowMax = 0.0f;
    double chartSum = 0.0;
    float chartMax = 0.0f;
    int32_t litOver2 = 0, chartOver4 = 0;
};

Fidelity checkFidelity(const std::vector<mdk::BakeTriangle>& triangles,
                       const mdk::BakeSettings& settings, const mdk::SunBakeImage& image,
                       const std::vector<uint8_t>& bytes) {
    Fidelity f;
    std::vector<uint8_t> copy(bytes);
    mdk::SunBake bake;
    const char* why = nullptr;
    if (!bake.parse(copy.data(), copy.size(), &why)) {
        f.missing = -1;
        return f;
    }
    const float* sd = settings.sunDirection;
    // The sides, their centroids pushed off the surface as the baker's rays are.
    struct Probe {
        size_t triangle;
        bool back;
        float n[3];
    };
    std::vector<Probe> probes;
    std::vector<float> points;
    for (size_t i = 0; i < triangles.size(); i++) {
        const mdk::BakeTriangle& t = triangles[i];
        if (!t.receiver) {
            continue;
        }
        float e1[3], e2[3];
        for (int k = 0; k < 3; k++) {
            e1[k] = t.p[1][k] - t.p[0][k];
            e2[k] = t.p[2][k] - t.p[0][k];
        }
        float n[3] = {-(e1[1] * e2[2] - e1[2] * e2[1]), -(e1[2] * e2[0] - e1[0] * e2[2]),
                      -(e1[0] * e2[1] - e1[1] * e2[0])};
        const float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len <= 1e-6f) {
            continue;
        }
        for (int back = 0; back < (t.doubleSided ? 2 : 1); back++) {
            Probe p;
            p.triangle = i;
            p.back = back != 0;
            for (int k = 0; k < 3; k++) {
                p.n[k] = n[k] / len * (back ? -1.0f : 1.0f);
            }
            probes.push_back(p);
            for (int k = 0; k < 3; k++) {
                points.push_back((t.p[0][k] + t.p[1][k] + t.p[2][k]) / 3.0f +
                                 p.n[k] * settings.normalOffset);
            }
        }
    }
    std::vector<uint8_t> blocked;
    mdk::sunBlockedAtMany(triangles, sd, points, &blocked);
    for (size_t i = 0; i < probes.size(); i++) {
        const Probe& p = probes[i];
        const mdk::BakeTriangle& t = triangles[p.triangle];
        const uint64_t key = p.back ? mdk::sunBakeTriangleKey(t.p[0], t.p[2], t.p[1])
                                    : mdk::sunBakeTriangleKey(t.p[0], t.p[1], t.p[2]);
        int32_t chart = -1;
        uint32_t colour = 0;
        if (!bake.sideOf(key, &chart, &colour)) {
            f.missing++;
            continue;
        }
        const float centre[3] = {(t.p[0][0] + t.p[1][0] + t.p[2][0]) / 3.0f,
                                 (t.p[0][1] + t.p[1][1] + t.p[2][1]) / 3.0f,
                                 (t.p[0][2] + t.p[1][2] + t.p[2][2]) / 3.0f};
        int16_t uv[2];
        bake.uvOf(chart, centre, uv);
        const float x = (float)uv[0] / mdk::kSunBakeUvOne * (float)image.atlasWidth;
        const float y = (1.0f - (float)uv[1] / mdk::kSunBakeUvOne) * (float)image.atlasHeight;
        const float m = sampleAtlas(image, x, y) / 255.0f;
        const mdk::BakeRig& rig = settings.rigs[(size_t)t.rig];
        const bool reaches = blocked[i] == 0 || !rig.shadows;
        float want[3];
        mdk::sunBakeLight(rig, p.n, sd, reaches, want);
        float diff = 0.0f;
        for (int c = 0; c < 3; c++) {
            const float got = (float)((colour >> (8 * c)) & 0xFFu) * m;
            diff = std::max(diff, fabsf(got - want[c] * 255.0f));
        }
        if ((uint32_t)chart < image.colourBlocks) {
            if (reaches) {
                f.litSides++;
                f.litMax = std::max(f.litMax, diff);
                f.litOver2 += diff > 2.0f ? 1 : 0;
            } else {
                f.shadowSides++;
                f.shadowMax = std::max(f.shadowMax, diff);
            }
        } else {
            f.chartSides++;
            f.chartSum += diff;
            f.chartMax = std::max(f.chartMax, diff);
            f.chartOver4 += diff > 4.0f ? 1 : 0;
        }
    }
    return f;
}

// --topdown: the level from above, every upward side drawn with the light the
// RUNTIME would make (its key's vertex colour times the atlas's m at its uv);
// dark grey where nothing is. A debugging aid; nothing ships it.
void writeTopDown(const std::string& prefix, int level, const std::vector<uint8_t>& bytes,
                  const mdk::SunBakeImage& image, const std::vector<mdk::BakeTriangle>& triangles) {
    std::vector<uint8_t> copy(bytes);
    mdk::SunBake bake;
    const char* why = nullptr;
    if (!bake.parse(copy.data(), copy.size(), &why)) {
        return;
    }
    float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
    for (size_t i = 0; i < triangles.size(); i++) {
        for (int k = 0; k < 3; k++) {
            for (int a = 0; a < 2; a++) {
                lo[a] = std::min(lo[a], triangles[i].p[k][a]);
                hi[a] = std::max(hi[a], triangles[i].p[k][a]);
            }
        }
    }
    const int side = 1024;
    const float scale = (float)(side - 1) / std::max(hi[0] - lo[0], hi[1] - lo[1]);
    std::vector<uint8_t> pixels((size_t)side * side * 3, 64);
    std::vector<float> top((size_t)side * side, -1e30f);
    for (size_t i = 0; i < triangles.size(); i++) {
        const mdk::BakeTriangle& t = triangles[i];
        if (!t.receiver) continue;
        const float e1[2] = {t.p[1][0] - t.p[0][0], t.p[1][1] - t.p[0][1]};
        const float e2[2] = {t.p[2][0] - t.p[0][0], t.p[2][1] - t.p[0][1]};
        const float nz = -(e1[0] * e2[1] - e1[1] * e2[0]);
        const bool back = !(nz > 0.0f);
        if (back && !t.doubleSided) continue;
        int chart = -1;
        uint32_t colour = 0;
        bake.sideOf(back ? mdk::sunBakeTriangleKey(t.p[0], t.p[2], t.p[1])
                         : mdk::sunBakeTriangleKey(t.p[0], t.p[1], t.p[2]),
                    &chart, &colour);
        float sx[3], sy[3], u[3], v[3];
        for (int k = 0; k < 3; k++) {
            int16_t uv[2];
            bake.uvOf(chart, t.p[k], uv);
            u[k] = (float)uv[0] / mdk::kSunBakeUvOne * (float)image.atlasWidth;
            v[k] = (1.0f - (float)uv[1] / mdk::kSunBakeUvOne) * (float)image.atlasHeight;
            sx[k] = (t.p[k][0] - lo[0]) * scale;
            sy[k] = (hi[1] - t.p[k][1]) * scale;
        }
        const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sy[1] - sy[0]) * (sx[2] - sx[0]);
        if (fabsf(area) < 1e-6f) continue;
        const int x0 = std::max(0, (int)std::min(sx[0], std::min(sx[1], sx[2])));
        const int x1 = std::min(side - 1, (int)std::max(sx[0], std::max(sx[1], sx[2])) + 1);
        const int y0 = std::max(0, (int)std::min(sy[0], std::min(sy[1], sy[2])));
        const int y1 = std::min(side - 1, (int)std::max(sy[0], std::max(sy[1], sy[2])) + 1);
        for (int y = y0; y <= y1; y++) {
            for (int x = x0; x <= x1; x++) {
                const float px = x + 0.5f, py = y + 0.5f;
                const float w1 = ((px - sx[0]) * (sy[2] - sy[0]) - (py - sy[0]) * (sx[2] - sx[0])) / area;
                const float w2 = ((sx[1] - sx[0]) * (py - sy[0]) - (sy[1] - sy[0]) * (px - sx[0])) / area;
                const float w0 = 1.0f - w1 - w2;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                const float z = w0 * t.p[0][2] + w1 * t.p[1][2] + w2 * t.p[2][2];
                const size_t at = (size_t)y * side + x;
                if (z <= top[at]) continue;
                top[at] = z;
                const float m = sampleAtlas(image, w0 * u[0] + w1 * u[1] + w2 * u[2],
                                            w0 * v[0] + w1 * v[1] + w2 * v[2]) / 255.0f;
                for (int k = 0; k < 3; k++) {
                    pixels[at * 3 + k] =
                        (uint8_t)std::min(255.0f, (float)((colour >> (8 * k)) & 0xFFu) * m);
                }
            }
        }
    }
    char path[512];
    snprintf(path, sizeof(path), "%s%d.ppm", prefix.c_str(), level);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", side, side);
    fwrite(pixels.data(), 1, pixels.size(), f);
    fclose(f);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <out dir> [level ...] [--l8] [--texel <units>] [--budget <bytes>] "
                "[--chart-max <texels>] [--cone <cos>] [--topdown <prefix>]\n",
                argv[0]);
        return 2;
    }
    const std::string out = argv[1];
    std::vector<int32_t> levels;
    mdk::BakeSettings settings;
    std::string topdown;   // --topdown <prefix>: a PPM per level, for a look
    bool budgetTiers = true;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--l8") == 0) {
            settings.format = mdk::kSunBakeL8;
        } else if (strcmp(argv[i], "--texel") == 0 && i + 1 < argc) {
            settings.texelSize = (float)atof(argv[++i]);
        } else if (strcmp(argv[i], "--topdown") == 0 && i + 1 < argc) {
            topdown = argv[++i];
        } else if (strcmp(argv[i], "--chart-max") == 0 && i + 1 < argc) {
            settings.chartMostSide = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--cone") == 0 && i + 1 < argc) {
            settings.chartCone = (float)atof(argv[++i]);
        } else if (strcmp(argv[i], "--budget") == 0 && i + 1 < argc) {
            settings.budgetBytes = (size_t)atol(argv[++i]);
            budgetTiers = false;   // an explicit budget is the budget
        } else {
            levels.push_back(atoi(argv[i]));
        }
    }
    if (levels.empty()) {
        for (int32_t level = 3; level <= 8; level++) {
            levels.push_back(level);
        }
    }
    const mdk::Vec3 sun = mdk::sunDirection();
    settings.sunDirection[0] = sun.x;
    settings.sunDirection[1] = sun.y;
    settings.sunDirection[2] = sun.z;
    mkdir(out.c_str(), 0755);

    mdk::formats::setFileSystem(mdk::test::host::fileSystem());
    const mdk::formats::MdkData data = mdk::formats::MdkData::find(mdk::formats::fileSystem());
    if (!data.found()) {
        fprintf(stderr, "sun_bake_level: no MDK data (set MDK_DATA_DIR)\n");
        return 1;
    }
    for (size_t l = 0; l < levels.size(); l++) {
        const double started = nowMs();
        mdk::LevelData level;
        if (!mdk::LevelData::load(&level, &data, levels[l])) {
            fprintf(stderr, "sun_bake_level: level %d did not load\n", (int)levels[l]);
            return 1;
        }
        std::vector<mdk::BakeTriangle> triangles;
        settings.rigs.clear();
        int32_t holeBatches = 0, hiddenBatches = 0, skipped = 0, open = 0;
        for (size_t a = 0; a < level.arenas().size(); a++) {
            const mdk::formats::Arena* arena = level.arenas()[a];
            // The arena's rig, as main_3ds.cpp makes it when Kurt enters the arena.
            const mdk::Lighting light = mdk::enhancedLighting(level.dti(), *arena);
            mdk::BakeRig rig;
            const float sky[3] = {light.sky.r, light.sky.g, light.sky.b};
            const float ground[3] = {light.ground.r, light.ground.g, light.ground.b};
            const float sunColour[3] = {light.sun.r, light.sun.g, light.sun.b};
            memcpy(rig.sky, sky, sizeof(sky));
            memcpy(rig.ground, ground, sizeof(ground));
            memcpy(rig.sun, sunColour, sizeof(sunColour));
            rig.exposure = light.exposure;
            rig.shadows = light.shadows && light.shadow_distance > 0.0f;
            open += rig.shadows ? 1 : 0;
            const int32_t rigIndex = (int32_t)settings.rigs.size();
            settings.rigs.push_back(rig);

            mdk::MaterialResolver resolver(&level.dti(), mdk::Shading::Lit);
            mdk::ArenaScene scene;
            if (!mdk::buildArenaScene(arena, resolver, level.paletteOf(arena),
                                      level.archivesOf(arena), &scene)) {
                continue;
            }
            skipped += arena->triangleCount() - (int32_t)scene.vertices.size() / 3;
            for (size_t b = 0; b < scene.batches.size(); b++) {
                const mdk::ArenaBatch& batch = scene.batches[b];
                const bool receiver = mdk::pica::castsSunShadow(
                    batch.surface.pass, batch.surface.shading, mdk::Primitive::Triangles);
                if (!receiver) {
                    continue;
                }
                const bool visible = mdk::arenaBatchVisible(arena, batch);
                const bool holes = textureHasHoles(batch.surface.texture);
                holeBatches += holes ? 1 : 0;
                hiddenBatches += visible ? 0 : 1;
                for (int32_t v = batch.first; v + 2 < batch.first + batch.count; v += 3) {
                    mdk::BakeTriangle t;
                    for (int k = 0; k < 3; k++) {
                        const mdk::Vertex& vertex = scene.vertices[(size_t)(v + k)];
                        t.p[k][0] = vertex.pos.x;
                        t.p[k][1] = vertex.pos.y;
                        t.p[k][2] = vertex.pos.z;
                    }
                    t.receiver = true;
                    t.caster = visible && !holes;
                    t.doubleSided = batch.surface.pass == mdk::Pass::DoubleSided;
                    t.rig = rigIndex;
                    triangles.push_back(t);
                }
            }
        }
        // THE BUDGET: 512 x 1024 texels (256 KiB in L4); a level that would
        // coarsen past twice the aimed-for density in it gets 1024 x 1024.
        mdk::SunBakeImage image;
        mdk::BakeReport report;
        mdk::BakeSettings levelSettings = settings;
        bool baked = mdk::bakeSunShadows(triangles, levelSettings, &image, &report);
        if (budgetTiers && baked && report.texelSize > 2.0f * settings.texelSize) {
            levelSettings.budgetBytes = 2u * settings.budgetBytes;
            image = mdk::SunBakeImage();
            baked = mdk::bakeSunShadows(triangles, levelSettings, &image, &report);
        }
        if (!baked) {
            fprintf(stderr, "sun_bake_level: level %d: nothing to bake, or it does not fit (%d "
                    "sides, %d colour blocks)\n",
                    (int)levels[l], (int)report.sides, (int)report.colourBlocks);
            return 1;
        }
        std::vector<uint8_t> bytes;
        if (!mdk::writeSunBake(image, &bytes)) {
            fprintf(stderr, "sun_bake_level: level %d: the image did not write\n", (int)levels[l]);
            return 1;
        }
        const Fidelity fid = checkFidelity(triangles, levelSettings, image, bytes);
        if (!topdown.empty()) {
            writeTopDown(topdown, (int)levels[l], bytes, image, triangles);
        }
        char name[64];
        snprintf(name, sizeof(name), "/level%d.grey", (int)levels[l]);
        if (!writeFile(out + name, image.grey.data(), image.grey.size())) {
            fprintf(stderr, "sun_bake_level: cannot write %s%s\n", out.c_str(), name);
            return 1;
        }
        snprintf(name, sizeof(name), "/level%d.sbk", (int)levels[l]);
        if (!writeFile(out + name, bytes.data(), bytes.size())) {
            fprintf(stderr, "sun_bake_level: cannot write %s%s\n", out.c_str(), name);
            return 1;
        }
        printf("sun_bake_level: level %d: %d arenas (%d open), %d receivers -> %d sides (%d the "
               "sun may reach), %d casters (%d block this sun; %d batches left out for holes, %d "
               "hidden at start; %d clip/disabled triangles not in the meshes); %d sides of one "
               "shade in %d grey blocks, %d charts with a shadow edge (%lld probe rays); atlas "
               "%dx%d (%u bytes L4, %u L8) at %.2f units/texel (%d coarsenings), %lld chart "
               "texels, %lld rays (%.1f%% shadowed); depth map %dx%d at %.1f units; .sbk %u "
               "bytes; %.0f ms\n",
               (int)levels[l], (int)level.arenas().size(), (int)open, (int)report.receivers,
               (int)report.sides, (int)report.sunSides, (int)report.casters,
               (int)report.effectiveCasters, (int)holeBatches, (int)hiddenBatches, (int)skipped,
               (int)report.uniformSides, (int)report.colourBlocks, (int)report.charts,
               (long long)report.probeRays, (int)report.atlasWidth, (int)report.atlasHeight,
               (unsigned)report.atlasBytesL4, (unsigned)report.atlasBytesL8,
               (double)report.texelSize, (int)report.densitySteps, (long long)report.chartTexels,
               (long long)report.rays,
               report.rays ? 100.0 * (double)report.shadowedSamples / (double)report.rays : 0.0,
               (int)report.depthWidth, (int)report.depthHeight, (double)report.depthCell,
               (unsigned)bytes.size(), nowMs() - started);
        printf("sun_bake_level: level %d fidelity (F/2 in 0..255; the pixel's F is twice it; "
               "before the atlas encoding): %d unshadowed block sides max |d| %.2f (%d over 2); "
               "%d shadowed block sides max |d| %.2f; %d chart sides mean |d| %.2f, max %.1f, %d "
               "over 4; %d sides with no key. The mean ratio's worst full-shadow error per "
               "channel: %.2f exact, %.2f at L4's steps, %.2f at L8's\n",
               (int)levels[l], (int)fid.litSides, (double)fid.litMax, (int)fid.litOver2,
               (int)fid.shadowSides, (double)fid.shadowMax, (int)fid.chartSides,
               fid.chartSides ? fid.chartSum / fid.chartSides : 0.0, (double)fid.chartMax,
               (int)fid.chartOver4, (int)fid.missing, (double)report.ratioError,
               (double)report.ratioErrorL4, (double)report.ratioErrorL8);
    }
    return 0;
}

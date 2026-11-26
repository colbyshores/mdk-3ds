// ============================================================================
// effects_dump.cpp -- see effects_dump.h.
// ============================================================================

#include "effects_dump.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <memory>
#include <vector>

#include "formats/texture.h"
#include "game/effects_view.h"
#include "game/follow_camera.h"
#include "game/level_data.h"
#include "game/player.h"
#include "render/camera_builder.h"
#include "render/material_resolver.h"
#include "render/post_params.h"
#include "scripts/debris.h"
#include "scripts/effects.h"
#include "scripts/script_runtime.h"
#include "util/math_minmax.h"

namespace mdk {
namespace test {

namespace {

uint32_t fnv(uint32_t hash, uint32_t value) {
    for (int32_t i = 0; i < 4; i++) {
        hash = (hash ^ ((value >> (8 * i)) & 0xFFu)) * 16777619u;
    }
    return hash;
}

uint32_t fnvF(uint32_t hash, float v) {
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    return fnv(hash, bits);
}

uint32_t fnvS(uint32_t hash, const std::string& s) {
    for (size_t i = 0; i < s.size(); i++) {
        hash = (hash ^ (uint8_t)s[i]) * 16777619u;
    }
    return fnv(hash, (uint32_t)s.size());
}

double now() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

const int kCameras = 6;
const char* const kCameraNames[kCameras] = {"near", "wide", "zoom", "inside", "edge", "none"};
const int32_t kTopWidth = 400;
const int32_t kTopHeight = 240;
// A step with 90-odd sprite effects alive (the storm's peak is 96).
const int kSpriteCaseStep = 120;

}  // namespace

struct EffectsTrace {
    std::unique_ptr<MaterialResolver> resolver;
    std::unique_ptr<EffectsView> view;
    EffectsFrame frame;
    std::string out;
    int bench = 0;
    double buildSeconds[kCameras] = {0, 0, 0, 0, 0, 0};
    double reachSeconds = 0.0;
    double tintSeconds = 0.0;
    long long builds = 0;
    long long reachPieces = 0;
    long long reachCorners = 0;
    long long tints = 0;
    // Slot reuse with a different shape: a pool slot seen again holding another
    // corner count (a large piece's slot given to a small one, or the reverse).
    std::vector<const scripts::Debris::Piece*> seen;
    std::vector<size_t> seenCorners;
    int reused = 0;
    // Piece::reach against the old per-frame measure, as bits, after every step.
    long long reachChecked = 0;
    long long reachDiffer = 0;
};

namespace {

// Camera `which` at this step, with the culls the 3DS sets (main_3ds.cpp: the
// frustum at half the stereo separation, the tiny-debris cull at 400 x 240).
void setCamera(EffectsTrace* t, const Player& player, int which, Camera* camera) {
    const Vec3 feet = player.feet;
    const float aspect = (float)kTopWidth / (float)kTopHeight;
    Vec3 eye = Vec3{feet.x, feet.y - 30.0f, feet.z + 25.0f};
    float heading = player.yaw;
    float pitch = -10.0f;
    float fov = follow_camera::kFieldOfView;
    float margin = 0.125f;   // the default 0.25 separation, halved
    float tiny = 2.0f;
    switch (which) {
    case 1:   // the widest stereo margin (kMaxSeparation 2.0, halved), the largest tiny cut
        margin = 1.0f;
        tiny = 3.0f;
        break;
    case 2:   // a scope-like zoom: the tiny cull sees the zoom in the projection
        eye = Vec3{feet.x, feet.y, feet.z + 10.0f};
        fov = 6.0f;
        pitch = -2.0f;
        break;
    case 3:   // the eye in the explosion: pieces through the near plane
        eye = Vec3{feet.x, feet.y + 105.0f, feet.z + 12.0f};
        pitch = 0.0f;
        break;
    case 4:   // a frustum edge through the cloud: barely inside and barely outside
        heading = player.yaw + 38.0f;
        break;
    default:
        break;
    }
    buildHeadingCamera(camera, eye, heading, pitch, fov, aspect, follow_camera::kNear,
                       follow_camera::kFar);
    Frustum frustum;
    if (which != 5 && buildFrustum(*camera, &frustum)) {
        t->view->setCullFrustum(&frustum, margin);
    } else {
        t->view->setCullFrustum(nullptr, 0.0f);
    }
    t->view->setTinyPieceCull(which == 5 ? 0.0f : tiny, kTopWidth, kTopHeight);
}

std::string frameLine(const EffectsFrame& frame, const EffectsView& view) {
    uint32_t v = 2166136261u;
    for (size_t i = 0; i < frame.vertices.size(); i++) {
        const Vertex& x = frame.vertices[i];
        v = fnvF(fnvF(fnvF(v, x.pos.x), x.pos.y), x.pos.z);
        v = fnv(fnvF(fnvF(v, x.uv.x), x.uv.y), x.colour);
    }
    uint32_t d = 2166136261u;
    for (size_t i = 0; i < frame.draws.size(); i++) {
        const EffectsDrawItem& q = frame.draws[i];
        d = fnv(fnv(fnv(d, (uint32_t)q.first), (uint32_t)q.count), (uint32_t)q.frame);
        d = fnvS(d, q.surface.texture ? q.surface.texture->name() : std::string("-"));
        d = fnvF(fnvF(fnvF(d, q.surface.colour.r), q.surface.colour.g), q.surface.colour.b);
        d = fnv(fnv(d, (uint32_t)q.surface.pass), (uint32_t)q.surface.shading);
    }
    uint32_t g = 2166136261u;
    for (size_t i = 0; i < frame.glows.size(); i++) {
        const GlowQuad& q = frame.glows[i];
        for (int k = 0; k < 6; k++) {
            g = fnvF(fnvF(fnvF(g, q.corners[k].pos.x), q.corners[k].pos.y), q.corners[k].pos.z);
            g = fnvF(fnvF(g, q.corners[k].uv.x), q.corners[k].uv.y);
        }
        g = fnvF(fnvF(fnvF(g, q.colour[0]), q.colour[1]), q.colour[2]);
    }
    char line[256];
    snprintf(line, sizeof(line), "v%d/%08X d%d/%08X g%d/%08X s%d/%d/%d",
             (int)frame.vertices.size(), v, (int)frame.draws.size(), d, (int)frame.glows.size(),
             g, view.spriteEffects(), view.glowingEffects(), view.unmeasuredGlows());
    return line;
}

// The work the radius cache removes, as the build did it (effects_view.cpp before the
// cache): every piece's corners, the largest squared length, its root.
float oldReach(const scripts::Debris::Piece& piece) {
    float reach2 = 0.0f;
    for (size_t k = 0; k < piece.corners.size(); k++) {
        const Vec3& v = piece.corners[k];
        const float d2 = v.x * v.x + v.y * v.y + v.z * v.z;
        reach2 = mathMax(reach2, d2);
    }
    return sqrtf(reach2);
}

void bench(EffectsTrace* t, const scripts::ScriptRuntime& runtime, const Camera& camera,
           int which) {
    if (t->bench <= 0) {
        return;
    }
    EffectsFrame scratch;
    const double start = now();
    for (int i = 0; i < t->bench; i++) {
        t->view->build(runtime, camera, &scratch);
    }
    t->buildSeconds[which] += now() - start;
    t->builds += t->bench;
}

void benchParts(EffectsTrace* t, const scripts::ScriptRuntime& runtime) {
    if (t->bench <= 0) {
        return;
    }
    const std::vector<scripts::Debris::Piece*>& pieces = runtime.debris().pieces();
    volatile float sink = 0.0f;
    double start = now();
    for (int i = 0; i < t->bench; i++) {
        float sum = 0.0f;
        for (size_t p = 0; p < pieces.size(); p++) {
            sum += oldReach(*pieces[p]);
        }
        sink = sum;
    }
    t->reachSeconds += now() - start;
    for (size_t p = 0; p < pieces.size(); p++) {
        t->reachPieces += t->bench;
        t->reachCorners += (long long)t->bench * (long long)pieces[p]->corners.size();
    }
    // Three colour powers per halo of the "near" camera's frame, on its own tints.
    const std::vector<GlowQuad>& glows = t->frame.glows;
    start = now();
    for (int i = 0; i < t->bench; i++) {
        float sum = 0.0f;
        for (size_t q = 0; q < glows.size(); q++) {
            for (int c = 0; c < 3; c++) {
                sum += powf(kGlowIntensity * glows[q].colour[c], 0.45454545f) * 0.5f;
            }
        }
        sink = sum;
    }
    t->tintSeconds += now() - start;
    t->tints += (long long)t->bench * (long long)glows.size() * 3;
    (void)sink;
}

void ensureView(EffectsTrace* t, LevelData& level) {
    if (!t->view) {
        t->resolver.reset(new MaterialResolver(&level.dti(), Shading::Lit));
        t->view.reset(new EffectsView(*t->resolver, &level, Shading::Sprite));
    }
}

// All six cameras once, as one line.
std::string allCameras(EffectsTrace* t, const scripts::ScriptRuntime& runtime,
                       const Player& player) {
    std::string line;
    for (int which = 0; which < kCameras; which++) {
        Camera camera;
        setCamera(t, player, which, &camera);
        t->view->build(runtime, camera, &t->frame);
        line += std::string(which ? " " : "") + kCameraNames[which] + "=" +
                frameLine(t->frame, *t->view);
    }
    return line;
}

// Every measured Piece::reach has the bits the old per-frame loop gives.
void checkReach(EffectsTrace* t, const scripts::ScriptRuntime& runtime) {
    const std::vector<scripts::Debris::Piece*>& pieces = runtime.debris().pieces();
    for (size_t p = 0; p < pieces.size(); p++) {
        if (pieces[p]->reach < 0.0f) {
            continue;
        }
        const float old = oldReach(*pieces[p]);
        t->reachChecked++;
        if (memcmp(&old, &pieces[p]->reach, sizeof(old)) != 0) {
            t->reachDiffer++;
        }
    }
}

// The sprite cases, mid-storm (no RNG drawn, everything put back): the textures with
// their pixels away when the entries are made, then measured late; a cleared view; the
// sprites through every other palette of the level.
void spriteCases(EffectsTrace* t, LevelData& level, scripts::ScriptRuntime& runtime,
                 const Player& player) {
    char text[128];
    // The sprites' textures with their pixels away when the entries are made (a
    // cleared view, as a level load leaves it): no halos, counted unmeasured; then
    // the pixels back and the next build measures them late.
    std::vector<const formats::Texture*> textures;
    Camera camera;
    setCamera(t, player, 5, &camera);
    t->view->build(runtime, camera, &t->frame);
    const std::vector<scripts::Effects::Effect*>& effects = runtime.effects.all();
    for (size_t i = 0; i < t->frame.draws.size(); i++) {
        const formats::Texture* texture = t->frame.draws[i].surface.texture;
        if (texture == nullptr) {
            continue;
        }
        bool sprite = false;
        for (size_t e = 0; e < effects.size() && !sprite; e++) {
            sprite = effects[e]->texture == texture->name();
        }
        bool listed = false;
        for (size_t k = 0; k < textures.size() && !listed; k++) {
            listed = textures[k] == texture;
        }
        if (sprite && !listed) {
            textures.push_back(texture);
        }
    }
    std::vector<formats::Texture> kept;
    for (size_t k = 0; k < textures.size(); k++) {
        kept.push_back(*textures[k]);
        const_cast<formats::Texture*>(textures[k])->dropPixels();
    }
    t->view->clear();
    snprintf(text, sizeof(text), "away(%d) ", (int)textures.size());
    t->out += text + allCameras(t, runtime, player) + "\n";
    for (size_t k = 0; k < textures.size(); k++) {
        const_cast<formats::Texture*>(textures[k])->adoptPixels(&kept[k]);
    }
    t->out += "late " + allCameras(t, runtime, player) + "\n";
    t->out += "warm " + allCameras(t, runtime, player) + "\n";
    t->view->clear();
    t->out += "cleared " + allCameras(t, runtime, player) + "\n";

    // The sprites through each other arena with a palette of its own (an entry per
    // arena and texture: another palette, another measured tint).
    const std::vector<const formats::Arena*>& arenas = level.arenas();
    std::vector<std::string> original;
    for (size_t e = 0; e < effects.size(); e++) {
        original.push_back(effects[e]->arena);
    }
    const formats::Palette* home = level.arenaNamed(runtime.currentArena())
                                       ? &level.paletteOf(level.arenaNamed(runtime.currentArena()))
                                       : nullptr;
    std::vector<const formats::Palette*> done;
    for (size_t a = 0; a < arenas.size(); a++) {
        const formats::Palette* palette = &level.paletteOf(arenas[a]);
        bool listed = palette == home;
        for (size_t k = 0; k < done.size() && !listed; k++) {
            listed = done[k] == palette;
        }
        if (listed) {
            continue;
        }
        done.push_back(palette);
        for (size_t e = 0; e < effects.size(); e++) {
            effects[e]->arena = arenas[a]->name();
        }
        t->out += "palette " + arenas[a]->name() + " " + allCameras(t, runtime, player) + "\n";
    }
    // An arena the level does not have: the view's fallback look, the level's DTI
    // palette and texture archive.
    for (size_t e = 0; e < effects.size(); e++) {
        effects[e]->arena = "NO_ARENA";
    }
    t->out += "palette dti " + allCameras(t, runtime, player) + "\n";
    for (size_t e = 0; e < effects.size(); e++) {
        effects[e]->arena = original[e];
    }

}

}  // namespace

EffectsTrace* effectsTraceNew() {
    EffectsTrace* t = new EffectsTrace();
    const char* bench = getenv("MDK_FX_BENCH");
    t->bench = bench ? atoi(bench) : 0;
    return t;
}

void effectsTraceStep(EffectsTrace* t, LevelData& level, scripts::ScriptRuntime& runtime,
                      const Player& player, int step) {
    ensureView(t, level);
    const std::vector<scripts::Debris::Piece*>& pieces = runtime.debris().pieces();
    size_t corners = 0;
    for (size_t p = 0; p < pieces.size(); p++) {
        corners += pieces[p]->corners.size();
        size_t k = 0;
        while (k < t->seen.size() && t->seen[k] != pieces[p]) {
            k++;
        }
        if (k == t->seen.size()) {
            t->seen.push_back(pieces[p]);
            t->seenCorners.push_back(pieces[p]->corners.size());
        } else if (t->seenCorners[k] != pieces[p]->corners.size()) {
            t->reused++;
            t->seenCorners[k] = pieces[p]->corners.size();
        }
    }
    char head[96];
    snprintf(head, sizeof(head), "s=%d pieces=%d corners=%d effects=%d ", step,
             (int)pieces.size(), (int)corners, (int)runtime.effects.all().size());
    std::string line = head;
    for (int which = 0; which < kCameras; which++) {
        Camera camera;
        setCamera(t, player, which, &camera);
        t->view->build(runtime, camera, &t->frame);
        line += std::string(which ? " " : "") + kCameraNames[which] + "=" +
                frameLine(t->frame, *t->view);
        if (which == 0) {
            benchParts(t, runtime);
        }
        bench(t, runtime, camera, which);
    }
    t->out += line + "\n";
    checkReach(t, runtime);
    if (step == kSpriteCaseStep) {
        spriteCases(t, level, runtime, player);
    }
}

void effectsTraceEnd(EffectsTrace* t, LevelData& level, scripts::ScriptRuntime& runtime,
                     const Player& player, std::string* out) {
    ensureView(t, level);
    char text[128];
    snprintf(text, sizeof(text), "reused=%d (pool slots seen again with another shape)\n",
             t->reused);
    t->out += text;

    // Sparks of size 0 (every corner at the centre: a zero radius) and of NaN size
    // (a NaN radius: never frustum-culled, never tiny), in front of the near camera.
    // They are rebuilt on a second build too (the radius cache's second use).
    const Vec3 at{player.feet.x, player.feet.y + 60.0f, player.feet.z + 10.0f};
    runtime.debris().spark(runtime.currentArena(), at, 3, 0.0f, 0, 15, 0.0f,
                           scripts::Debris::Launch::Still);
    runtime.debris().spark(runtime.currentArena(), at, 2, nanf(""), 0, 15, 0.0f,
                           scripts::Debris::Launch::Still);
    t->out += "zero+nan " + allCameras(t, runtime, player) + "\n";
    t->out += "zero+nan again " + allCameras(t, runtime, player) + "\n";
    checkReach(t, runtime);
    snprintf(text, sizeof(text), "reach checked=%lld differ=%lld\n", t->reachChecked,
             t->reachDiffer);
    t->out += text;

    if (t->bench > 0) {
        double total = 0.0;
        for (int which = 0; which < kCameras; which++) {
            total += t->buildSeconds[which];
            fprintf(stderr, "fxbench: camera %-6s build %.3f us mean\n", kCameraNames[which],
                    t->buildSeconds[which] * 1e6 / ((double)t->builds / kCameras));
        }
        fprintf(stderr, "fxbench: all cameras %.3f us mean build (%lld builds)\n",
                total * 1e6 / (double)t->builds, t->builds);
        fprintf(stderr,
                "fxbench: old reach loop %.3f ns/piece, %.3f ns/corner (%lld pieces, %.2f "
                "corners/piece)\n",
                t->reachSeconds * 1e9 / (double)t->reachPieces,
                t->reachSeconds * 1e9 / (double)t->reachCorners, t->reachPieces,
                (double)t->reachCorners / (double)t->reachPieces);
        fprintf(stderr, "fxbench: halo tint powf %.3f ns/call (%lld calls)\n",
                t->tints ? t->tintSeconds * 1e9 / (double)t->tints : 0.0, t->tints);
    }
    *out = t->out;
    delete t;
}

}  // namespace test
}  // namespace mdk

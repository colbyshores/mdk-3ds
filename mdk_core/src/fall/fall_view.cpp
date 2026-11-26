// fall_view.cpp — see fall_view.h. The C#'s FallView.cs, method for method, with
// FallScreen.Frame's drawing half and the lean way's canvas parts.

#include "fall/fall_view.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "fall/fall_layout.h"
#include "fall/fall_missile.h"
#include "fall/fall_sim.h"
#include "formats/bni.h"
#include "formats/fall3d.h"
#include "formats/font.h"
#include "formats/fti.h"
#include "formats/palette.h"
#include "formats/texture.h"
#include "formats/texture_archive.h"
#include "render/camera_builder.h"
#include "util/math_lut.h"

namespace mdk {
namespace fall {

namespace {

using formats::Vec3f;

constexpr int32_t kBackQuads = 16;
constexpr int32_t kQuadVertices = 6;
constexpr int32_t kRadarVertices = 138;
constexpr int32_t kTrailMissiles = 24;
// A trail point's width by its age in ticks (0x491d64): the ribbon swells just
// behind the missile, then tapers to nothing at the 32nd point (trailWidth).
const float kTrailSwell[] = {1.0f, 1.25f, 1.2f, 1.1f, 1.05f, 1.0f};
constexpr int32_t kTrailSwellCount = 6;
// The trail's colours (0x410874's translucent table): white at 215/255, and over
// the last 8 segments before the missile less and less blue (yellow) and more
// opaque.
constexpr int32_t kTrailAlpha = 215;
constexpr int32_t kTrailYellowSegments = 8;
constexpr float kRadarBaseFactor = -4.0f;
constexpr float kRadarFirstAngle = 60.0f;
constexpr float kDegrees = 3.14159265358979323846f / 180.0f;
// The intro as f goes 0 to 1 (0x41106c): space (600 x 360) behind, the moon (128²)
// centred at (300, 270 − 90 f) scaled (64 + 256 f) / 256, the earth (512²) centred at
// (300, 488 − 224 f) scaled (300 + 128 f) / 256 by (100 + 42 f) / 256.
constexpr float kViewWidth = 600.0f;
constexpr float kSpriteUnit = 256.0f;
constexpr float kMoonX = 300.0f;
constexpr float kMoonY = 270.0f;
constexpr float kMoonRise = 90.0f;
constexpr float kMoonScale0 = 64.0f;
constexpr float kMoonScale1 = 256.0f;
constexpr float kEarthX = 300.0f;
constexpr float kEarthY = 488.0f;
constexpr float kEarthRise = 224.0f;
constexpr float kEarthScaleX0 = 300.0f;
constexpr float kEarthScaleX1 = 128.0f;
constexpr float kEarthScaleY0 = 100.0f;
constexpr float kEarthScaleY1 = 42.0f;
constexpr int32_t kPaletteBytes = 256 * 4;
// The lean haze's screen pixels per texel: a 300 x 90 image.
constexpr int32_t kLeanHazeStep = 2;

const char* const kKurtAnimation = "KURTANIM";
const char* const kKurtHit = "KURT_HIT";
const char* const kBonesAnimation = "BONESANM";

Vec3f add(const Vec3f& a, const Vec3f& b) {
    Vec3f r;
    r.x = a.x + b.x;
    r.y = a.y + b.y;
    r.z = a.z + b.z;
    return r;
}

Vec3f scaled(const Vec3f& a, float s) {
    Vec3f r;
    r.x = a.x * s;
    r.y = a.y * s;
    r.z = a.z * s;
    return r;
}

Material plain(int32_t texture, int32_t palette) {
    // C# Plain: `new(texture, palette, Vector4.One, 1, Pass.DoubleSided)`.
    Material m;
    m.texture = texture;
    m.palette = palette;
    m.pass = Pass::DoubleSided;
    return m;
}

Material flat(const Colour& colour, Pass pass) {
    Material m;
    m.colour = colour;
    m.pass = pass;
    return m;
}

bool paletteOf(const formats::Bni& bni, const char* name, uint8_t* rgba) {
    // C# PaletteOf: Palette.FromRgb of the entry's 768 bytes; colour 0 forced to black.
    const formats::ArchiveIndex::Entry* entry = bni.index().find(name);
    const int32_t size = formats::Palette::kSize * 3;
    if (entry == nullptr || entry->offset < 0 || entry->offset + size > (int32_t)bni.bytes().size()) {
        return false;
    }
    const formats::Palette palette = formats::Palette::fromRgb(bni.bytes().data() + entry->offset, size);
    memcpy(rgba, palette.rgba(), kPaletteBytes);
    return true;
}

uint32_t argbOf(const uint8_t* rgba) {
    return 0xFF000000u | ((uint32_t)rgba[0] << 16) | ((uint32_t)rgba[1] << 8) | (uint32_t)rgba[2];
}

} // namespace

bool FallView::image(const formats::Texture& texture, const uint8_t* paletteRgba, Image* out) {
    out->texture = mdk_render_create_index_texture(renderer_, texture.width(),
                                                   texture.height() * texture.frameCount(),
                                                   texture.indices(), paletteRgba);
    out->width = (float)texture.width();
    out->height = (float)texture.height();
    return out->texture >= 0;
}

bool FallView::load(MdkRenderer renderer, const formats::Bni& bni, const formats::TextureArchive& mti,
                    int32_t index, const formats::Fti& fti, const formats::Font& big,
                    const formats::Font& small, const std::vector<std::string>& pickups,
                    const FallViewOptions& options, std::string* why) {
    renderer_ = renderer;
    options_ = options;
    index_ = index;
    const int32_t n = index + 1;
    char name[32];

    // The palettes: FALLPn (with its effects), SPACEPAL, and white.
    uint8_t fallRgba[kPaletteBytes];
    snprintf(name, sizeof(name), "FALLP%d", (int)n);
    if (!paletteOf(bni, name, fallRgba) || !paletteOf(bni, "SPACEPAL", spaceRgba_)) {
        *why = "the fall's palettes are missing from FALL3D.BNI";
        return false;
    }
    effects_.reset(new FallPalette(fallRgba));
    palette_ = mdk_render_create_palette(renderer, effects_->base());
    spacePalette_ = mdk_render_create_palette(renderer, spaceRgba_);
    memset(whiteRgba_, 0xFF, sizeof(whiteRgba_));
    whitePalette_ = mdk_render_create_palette(renderer, whiteRgba_);

    // The models: the ones at rest, then Kurt and Bones with their animations.
    std::vector<std::string> still;
    still.push_back("MISSILE");
    still.push_back("CHUTE");
    still.push_back("EXPLODE");
    for (size_t i = 0; i < pickups.size(); i++) {
        still.push_back(pickups[i]);
    }
    std::vector<std::string> animated;
    animated.push_back("KURT");
    animated.push_back("BONES");
    std::vector<std::vector<std::string>> animations(2);
    animations[0].push_back(kKurtAnimation);
    animations[0].push_back(kKurtHit);
    animations[1].push_back(kBonesAnimation);
    if (!models_.load(renderer, bni, mti, palette_, effects_->base(), still, animated, animations)) {
        *why = "a model of FALL3D.BNI did not parse";
        return false;
    }
    // C# FallView: the trail's ends are the MISSILE model's leftmost and rightmost
    // vertices (the first of each, as MinBy/MaxBy), or -x and +x without it.
    trailLeft_ = Vec3f();
    trailRight_ = Vec3f();
    trailLeft_.x = -1.0f;
    trailRight_.x = 1.0f;
    if (const formats::Model* missile = models_.find("MISSILE")) {
        bool first = true;
        for (size_t p = 0; p < missile->partList.size(); p++) {
            const std::vector<Vec3f>& vertices = missile->partList[p].vertices;
            for (size_t v = 0; v < vertices.size(); v++) {
                if (first || vertices[v].x < trailLeft_.x) {
                    trailLeft_ = vertices[v];
                }
                if (first || vertices[v].x > trailRight_.x) {
                    trailRight_ = vertices[v];
                }
                first = false;
            }
        }
    }
    if (!hud_.load(renderer, bni, palette_, effects_->base(), fti, big, small)) {
        *why = "the fall's HUD did not load";
        return false;
    }

    // The ground and the track.
    snprintf(name, sizeof(name), "LEVEL%d", (int)n);
    const formats::Texture* ground = mti.findTexture(name);
    snprintf(name, sizeof(name), "POD%d", (int)n);
    const formats::Texture* pod = mti.findTexture(name);
    if (ground == nullptr || pod == nullptr || !ground_.load(*ground, *pod, !options_.lean)) {
        *why = "the fall's ground (LEVELn, PODn) is missing from its MTI";
        return false;
    }
    groundTexture_ = mdk_render_create_index_texture(renderer, FallGround::kSize, FallGround::kSize,
                                                     ground->indices(), effects_->base());
    if (options_.lean) {
        image(*pod, effects_->base(), &pod_);
    }
    crawler_.reserve(FallSim::kCrawlerFrames);
    for (int32_t i = 1; i <= FallSim::kCrawlerFrames; i++) {
        snprintf(name, sizeof(name), "L%d_C%04d", (int)n, (int)i);
        const formats::Texture* frame = mti.findTexture(name);
        if (frame != nullptr) {
            Image crawler;
            image(*frame, effects_->base(), &crawler);
            crawler_.push_back(crawler);
        }
    }

    // The intro's images.
    formats::Texture texture;
    if (bni.image("SPACE", &texture)) {
        image(texture, spaceRgba_, &space_);
    }
    if (bni.image("MOON", &texture)) {
        image(texture, spaceRgba_, &moon_);
    }
    if (bni.image("EARTH", &texture)) {
        image(texture, spaceRgba_, &earth_);
    }

    // The haze.
    const int32_t size = formats::Fall3d::kHazeWidth * formats::Fall3d::kHazeRows;
    std::vector<uint8_t> haze;
    std::vector<uint8_t> masks;
    if (options_.lean) {
        hazeWidth_ = formats::Fall3d::kHazeWidth / kLeanHazeStep;
        hazeHeight_ = formats::Fall3d::kHazeRows / kLeanHazeStep;
        hazeOffsets_.reserve(FallSim::kHazeFrames);
    }
    for (int32_t i = 0; i < FallSim::kHazeFrames; i++) {
        snprintf(name, sizeof(name), "ZOOM%04d", (int)i);
        if (!formats::Fall3d::haze(bni, name, &haze) || (int32_t)haze.size() < size) {
            *why = "a haze frame of FALL3D.BNI did not read";
            return false;
        }
        if (options_.lean) {
            // Each frame's offsets at the lean step (a texel per `step` pixels).
            std::vector<uint8_t> offsets((size_t)hazeWidth_ * (size_t)hazeHeight_);
            for (int32_t y = 0; y < hazeHeight_; y++) {
                const uint8_t* row = haze.data() + (size_t)(y * kLeanHazeStep) * formats::Fall3d::kHazeWidth;
                for (int32_t x = 0; x < hazeWidth_; x++) {
                    offsets[(size_t)y * (size_t)hazeWidth_ + (size_t)x] = row[x * kLeanHazeStep];
                }
            }
            hazeOffsets_.push_back(offsets);
            continue;
        }
        // C# HazeTexture: 8 stacked masks, one per blend offset b (1-8): index 1
        // where the pixel's offset is b, so each level draws with its own alpha.
        masks.assign((size_t)size * kHazeLevels, 0);
        for (int32_t p = 0; p < size; p++) {
            const uint8_t b = haze[(size_t)p];
            if (b > 0 && b <= kHazeLevels) {
                masks[(size_t)(b - 1) * (size_t)size + (size_t)p] = 1;
            }
        }
        haze_.push_back(mdk_render_create_index_texture(renderer, formats::Fall3d::kHazeWidth,
                                                         formats::Fall3d::kHazeRows * kHazeLevels,
                                                         masks.data(), whiteRgba_));
    }
    if (options_.lean) {
        hazeRgba_.assign((size_t)hazeWidth_ * (size_t)hazeHeight_ * 4, 0);
        hazeTexture_ = mdk_render_create_rgba_texture(renderer, hazeWidth_, hazeHeight_, hazeRgba_.data());
    }

    backMesh_ = mdk_render_create_dynamic_mesh(renderer, kBackQuads * kQuadVertices);
    back_.reserve((size_t)(kBackQuads * kQuadVertices));
    backDraws_.reserve((size_t)kBackQuads);
    if (!options_.lean) {
        radarMesh_ = mdk_render_create_dynamic_mesh(renderer, kRadarVertices);
        trailMesh_ = mdk_render_create_dynamic_mesh(renderer,
                                                    kTrailMissiles * FallMissile::kTrailPoints * kQuadVertices);
    }
    scratch_.reserve((size_t)(kTrailMissiles * FallMissile::kTrailPoints * kQuadVertices));
    if (groundTexture_ < 0 || backMesh_ < 0 || palette_ < 0) {
        *why = "the renderer refused the fall's ground or meshes";
        return false;
    }
    return true;
}

uint32_t FallView::backdrop() const {
    return argbOf(effects_->base());
}

void FallView::draw(MdkRenderer renderer, const FallSim& sim) {
    renderer_ = renderer;
    // C# SetEffects: the fall's whitening and the red of death in the palette;
    // colour 0 is whitened in the first second only.
    const float brightness = sim.inIntro() ? 1.0f : sim.whiten();
    effects_->set(brightness, sim.red(), sim.time() < 1.0f ? FallPalette::Whitened : FallPalette::Kept);

    // The frame's state (AGENTS.md P2): no sky, no lights, no glow, full
    // brightness -- whatever the last level left. The rig (unlit, no haze) once:
    // the backends take it when it changes (the 3DS dials it on each call).
    if (!rigSet_) {
        Lighting lighting;
        mdk_render_set_lighting(renderer, &lighting);
        rigSet_ = true;
    }
    Panorama panorama;
    mdk_render_set_panorama(renderer, &panorama);
    mdk_render_set_point_lights(renderer, nullptr, 0);
    mdk_render_set_glow_quads(renderer, nullptr, 0);
    mdk_render_set_fade(renderer, 1.0f);

    // The camera (C# Draw's tail): straight down, y up the screen, 250 px focal.
    const Vec3f camera = sim.cameraPosition();
    const float width = mdk_render_canvas_width(renderer);
    // 2 atan(180 / 250): the view's vertical field of view, in degrees.
    const float fieldOfView = 2.0f * fastAtan2(kViewCentreY, kViewFocal) / kDegrees;
    const Vec3 eye{camera.x, camera.y, camera.z};
    buildLookAtCamera(&camera_, eye, Vec3{camera.x, camera.y, camera.z - 1.0f}, Vec3{0.0f, 1.0f, 0.0f},
                      fieldOfView, width / kViewHeight, kViewNear, kViewFar);
    mdk_render_set_camera(renderer, &camera_);

    back_.clear();
    backDraws_.clear();
    if (sim.inIntro()) {
        queueIntro(sim, camera, width);
    } else {
        queueGround(sim, camera, width);
    }
    if (!back_.empty()) {
        mdk_render_update_mesh(renderer, backMesh_, back_.data(), (int32_t)back_.size());
        for (size_t i = 0; i < backDraws_.size(); i++) {
            MeshDraw draw;
            draw.mesh = backMesh_;
            draw.first_vertex = backDraws_[i].first;
            draw.vertex_count = kQuadVertices;
            draw.material = backDraws_[i].material;
            draw.frame = backDraws_[i].frame;
            memcpy(draw.world, Mat4::identity().m, sizeof(draw.world));
            mdk_render_draw_mesh(renderer, &draw);
        }
    }

    queueModels(renderer, sim);
    if (!options_.lean) {
        queueRadar(renderer, sim);
        queueTrails(renderer, sim);
    }

    mdk_render_canvas_begin(renderer);
    if (options_.lean) {
        if (!sim.inIntro()) {
            canvasHaze(renderer, sim, width);
        }
        canvasRadar(renderer, sim, width);
        canvasTrails(renderer, sim, width);
    }
    // FallScreen.Frame: the HUD after the scene, unless in the intro.
    if (!sim.inIntro()) {
        hud_.draw(renderer, sim.health(), sim.inventory(), sim.dying() ? FallHud::Dying : FallHud::Alive, sim.red());
    }
    // The palette's effects over all of it (fall_palette.h): the red added, then
    // the whitening, as Set applies them.
    if (effects_->appliedRed() > 0) {
        fill(renderer, width, Colour{1.0f, 0.0f, 0.0f, (float)effects_->appliedRed() / 255.0f}, true);
    }
    if (effects_->appliedLevel() < FallPalette::kUnit) {
        fill(renderer, width,
             Colour{1.0f, 1.0f, 1.0f, (float)(FallPalette::kUnit - effects_->appliedLevel()) / (float)FallPalette::kUnit},
             false);
    }
    // C# DrawEffects: towards white in the intro, then darkened.
    if (sim.inIntro() && sim.whiten() < 1.0f) {
        fill(renderer, width, Colour{1.0f, 1.0f, 1.0f, 1.0f - sim.whiten()}, false);
    }
    if (sim.dark() < 1.0f) {
        fill(renderer, width, Colour{0.0f, 0.0f, 0.0f, 1.0f - sim.dark()}, false);
    }
    mdk_render_canvas_end(renderer);
}

void FallView::fill(MdkRenderer renderer, float width, const Colour& colour, bool additive) {
    CanvasQuad q;
    q.x0 = 0.0f;
    q.y0 = 0.0f;
    q.x1 = width;
    q.y1 = 0.0f;
    q.x2 = width;
    q.y2 = kCanvasHeight;
    q.x3 = 0.0f;
    q.y3 = kCanvasHeight;
    q.u1 = 1.0f;
    q.u2 = 1.0f;
    q.v2 = 1.0f;
    q.v3 = 1.0f;
    q.colour = colour;
    q.additive = additive;
    mdk_render_canvas_quad(renderer, &q);
}

void FallView::queueIntro(const FallSim& sim, const Vec3f& camera, float width) {
    // C# QueueIntro: space as the background, the moon and the flattened earth
    // rising and growing.
    const float f = 1.0f - (float)sim.introLeft() / (float)FallSim::kIntroTicks;
    const float originX = (width - kViewWidth) / 2.0f;
    FallRect screen;
    screen.x = originX;
    screen.width = kViewWidth;
    screen.height = kViewHeight;
    if (space_.texture >= 0) {
        quad(camera, width, screen, kBackDepth, plain(space_.texture, spacePalette_), nullptr, 0);
    }
    const float moon = (kMoonScale0 + kMoonScale1 * f) / kSpriteUnit;
    sprite(camera, width, moon_, originX + kMoonX, kMoonY - kMoonRise * f, moon, moon, kBackDepth - kLayerStep,
           spacePalette_);
    sprite(camera, width, earth_, originX + kEarthX, kEarthY - kEarthRise * f,
           (kEarthScaleX0 + kEarthScaleX1 * f) / kSpriteUnit, (kEarthScaleY0 + kEarthScaleY1 * f) / kSpriteUnit,
           kBackDepth - 2.0f * kLayerStep, spacePalette_);
}

void FallView::queueGround(const FallSim& sim, const Vec3f& camera, float width) {
    // C# QueueGround: the ground with the track, the haze over it, the minecrawler
    // over both (fall.gd).
    const float centreRow = FallGround::centreRow(sim.time());
    if (ground_.updateTrack(centreRow) && !options_.lean) {
        mdk_render_update_texture(renderer_, groundTexture_, FallGround::kSize, FallGround::kSize,
                                  ground_.indices().data());
    }

    FallRect screen;
    FallRect texels;
    if (FallGround::visible(camera, centreRow, width, &screen, &texels) && screen.width > 0.0f) {
        const float perTexel = 1.0f / (float)FallGround::kSize;
        FallRect uv;
        uv.x = texels.x * perTexel;
        uv.y = texels.y * perTexel;
        uv.width = texels.width * perTexel;
        uv.height = texels.height * perTexel;
        quad(camera, width, screen, kBackDepth, plain(groundTexture_, palette_), &uv, 0);
    }
    if (options_.lean) {
        queueTrack(camera, centreRow, width);
    }

    if (!crawler_.empty()) {
        float x = 0.0f;
        float y = 0.0f;
        float scale = 0.0f;
        FallGround::crawler(camera, width, &x, &y, &scale);
        // `(int)sim.CrawlerFrame % _crawler.Count`, without a runtime divide.
        const int32_t count = (int32_t)crawler_.size();
        int32_t frame = (int32_t)sim.crawlerFrame();
        while (frame >= count) {
            frame -= count;
        }
        sprite(camera, width, crawler_[(size_t)(frame < 0 ? 0 : frame)], x, y, scale, scale,
               kBackDepth - 3.0f * kLayerStep, palette_);
    }

    if (options_.lean) {
        return;   // the haze is the canvas's (canvasHaze)
    }
    // Each blend offset b uses table (wind level + b): white by its alpha.
    const int32_t level = sim.wind() >> FallSim::kWindShift;
    for (int32_t b = 1; b <= kHazeLevels; b++) {
        const float alpha = hazeAlpha(level, b);
        if (alpha <= 0.0f || sim.hazeFrame() < 0 || sim.hazeFrame() >= (int32_t)haze_.size()) {
            continue;
        }
        Material material;
        material.texture = haze_[(size_t)sim.hazeFrame()];
        material.palette = whitePalette_;
        material.colour = Colour{1.0f, 1.0f, 1.0f, alpha};
        material.frame_count = kHazeLevels;
        material.pass = Pass::Blended;
        FallRect view;
        view.width = width;
        view.height = kViewHeight;
        quad(camera, width, view, kBackDepth - kLayerStep, material, nullptr, b - 1);
    }
}

void FallView::queueTrack(const Vec3f& camera, float centreRow, float width) {
    // The lean track: PODn over the ground from trackRow() down (fall_view.h).
    const int32_t row = ground_.trackRow();
    if (pod_.texture < 0 || row >= FallGround::kSize) {
        return;
    }
    // The ground's texel -> view map (FallGround.Visible's u0, v0 and S).
    const float s = camera.z / FallGround::kUnitHeight;
    if (s <= 0.0f) {
        return;
    }
    const float u0 = FallGround::kCentreColumn + 0.36f * camera.x - s * width / 2.0f;
    const float v0 = centreRow - 0.36f * camera.y - s * kViewCentreY;
    const float half = pod_.width * 0.5f;
    const float perU = 1.0f / pod_.width;
    const float perV = 1.0f / pod_.height;
    const float size = (float)FallGround::kSize;
    const float ramp = (float)(row + FallGround::kTrackRamp < FallGround::kSize ? row + FallGround::kTrackRamp
                                                                                 : FallGround::kSize);
    const float top = (float)row;
    const float narrow = (float)FallGround::trackWidth(0);
    const float wide = (float)FallGround::kTrackWide;
    // The ramp's trapezoid, then the full width to the bottom.
    const float rows[3] = {top, ramp, size};
    const float halves[3] = {narrow, wide, wide};
    for (int32_t part = 0; part < 2; part++) {
        const float r0 = rows[part];
        const float r1 = rows[part + 1];
        if (r1 <= r0) {
            continue;
        }
        const float w0 = halves[part];
        const float w1 = halves[part + 1];
        const float cols[4] = {FallGround::kCentreColumn - w0, FallGround::kCentreColumn + w0,
                               FallGround::kCentreColumn + w1, FallGround::kCentreColumn - w1};
        const float texRows[4] = {r0, r0, r1, r1};
        float x[4];
        float y[4];
        float u[4];
        float v[4];
        for (int32_t k = 0; k < 4; k++) {
            x[k] = (cols[k] - u0) / s;
            y[k] = (texRows[k] - v0) / s;
            u[k] = (cols[k] - FallGround::kCentreColumn + half) * perU;
            v[k] = texRows[k] * perV;
        }
        corners(camera, width, x, y, u, v, kBackDepth - kLayerStep / 2.0f, plain(pod_.texture, palette_));
    }
}

void FallView::sprite(const Vec3f& camera, float width, const Image& image, float centreX, float centreY,
                      float scaleX, float scaleY, float depth, int32_t palette) {
    // C# Sprite: an image centred on (centreX, centreY) (view pixels), scaled.
    if (image.texture < 0) {
        return;
    }
    const float w = image.width * scaleX;
    const float h = image.height * scaleY;
    FallRect screen;
    screen.x = centreX - w / 2.0f;
    screen.y = centreY - h / 2.0f;
    screen.width = w;
    screen.height = h;
    quad(camera, width, screen, depth, plain(image.texture, palette), nullptr, 0);
}

void FallView::quad(const Vec3f& camera, float width, const FallRect& screen, float depth,
                    const Material& material, const FallRect* uv, int32_t frame) {
    const float left = screen.x;
    const float right = screen.x + screen.width;
    const float topY = screen.y;
    const float bottom = screen.y + screen.height;
    FallRect whole;
    whole.width = 1.0f;
    whole.height = 1.0f;
    const FallRect& t = uv != nullptr ? *uv : whole;
    // The C#'s corner order: TL, TR, BR, BL as two triangles (TL TR BR, TL BR BL).
    const float x[4] = {left, right, right, left};
    const float y[4] = {topY, topY, bottom, bottom};
    const float u[4] = {t.x, t.x + t.width, t.x + t.width, t.x};
    const float v[4] = {t.y, t.y, t.y + t.height, t.y + t.height};
    const size_t first = back_.size();
    corners(camera, width, x, y, u, v, depth, material);
    if (back_.size() != first && !backDraws_.empty()) {
        backDraws_.back().frame = frame;
    }
}

void FallView::corners(const Vec3f& camera, float width, const float* x, const float* y, const float* u,
                       const float* v, float depth, const Material& material) {
    if ((int32_t)back_.size() + kQuadVertices > kBackQuads * kQuadVertices) {
        return;
    }
    // C# Corner: a view pixel at `depth` below the camera, through the pinhole.
    const int32_t order[kQuadVertices] = {0, 1, 2, 0, 2, 3};
    BackDraw draw;
    draw.first = (int32_t)back_.size();
    draw.material = material;
    for (int32_t i = 0; i < kQuadVertices; i++) {
        const int32_t k = order[i];
        Vertex vertex;
        vertex.pos = Vec3{camera.x + (x[k] - width / 2.0f) * depth / kViewFocal,
                          camera.y - (y[k] - kViewCentreY) * depth / kViewFocal, camera.z - depth};
        vertex.uv = Vec2{u[k], v[k]};
        vertex.colour = kVertexWhite;
        back_.push_back(vertex);
    }
    backDraws_.push_back(draw);
}

void FallView::queueModels(MdkRenderer renderer, const FallSim& sim) {
    const Mat4 turn = kurtTurn();
    if (sim.kurtVisible()) {
        const char* animation = sim.kurtPose() == FallSim::AnimHit ? kKurtHit : kKurtAnimation;
        models_.draw(renderer, "KURT", multiply(turn, translation(sim.kurtPosition())), animation,
                     (int32_t)sim.kurtFrame(), 0, Pass::DoubleSided);
    }
    if (sim.hasBones()) {
        models_.draw(renderer, "BONES", multiply(turn, translation(sim.bonesPosition())), kBonesAnimation,
                     (int32_t)sim.bonesFrame(), 0, Pass::DoubleSided);
    }

    const Vec3f camera = sim.cameraPosition();
    const std::vector<FallMissile>& missiles = sim.missiles();
    for (size_t i = 0; i < missiles.size(); i++) {
        if (nearCamera(missiles[i].position, camera)) {
            continue;
        }
        models_.draw(renderer, "MISSILE", multiply(orient(missiles[i].velocity), translation(missiles[i].position)),
                     nullptr, 0, 0, Pass::DoubleSided);
    }

    const std::vector<FallSim::Pickup>& pickups = sim.pickups();
    for (size_t i = 0; i < pickups.size(); i++) {
        const FallSim::Pickup& pickup = pickups[i];
        if (nearCamera(pickup.position, camera)) {
            continue;
        }
        const Mat4 yaw = rotationZ(degreesToRadians(pickup.yaw));
        models_.draw(renderer, pickup.name.c_str(), multiply(yaw, translation(pickup.position)), nullptr, 0, 0,
                     Pass::DoubleSided);
        Vec3f chute = pickup.position;
        chute.z += kChuteAbove;
        if (pickup.chute && !nearCamera(chute, camera)) {
            models_.draw(renderer, "CHUTE", multiply(yaw, translation(chute)), nullptr, 0, 0, Pass::DoubleSided);
        }
    }

    // Explosions grow and play their texture's frames, over everything.
    const std::vector<FallSim::Explosion>& explosions = sim.explosions();
    for (size_t i = 0; i < explosions.size(); i++) {
        const int32_t frame = (int32_t)explosions[i].ticks;
        const Mat4 world = multiply(multiply(scaling(explosionScale(frame)), rotationZ(explosions[i].yaw)),
                                    translation(explosions[i].position));
        models_.draw(renderer, "EXPLODE", world, nullptr, 0, frame, Pass::OnTop);
    }
}

namespace {

// The beam's points (C# QueueRadar's rings), shared by the scene and the canvas.
void radarRings(const FallSim& sim, Vec3f* basePoint, Vec3f rings[kRadarRings][kRadarSides]) {
    const Vec3f camera = sim.cameraPosition();
    basePoint->x = kRadarBaseFactor * camera.x;
    basePoint->y = kRadarBaseFactor * camera.y;
    basePoint->z = 0.0f;
    for (int32_t k = 0; k < kRadarRings; k++) {
        Vec3f centre;
        float radius = 0.0f;
        radarRing(*basePoint, sim.radarSpot(), sim.radarSpotRadius(), k, &centre, &radius);
        for (int32_t m = 0; m < kRadarSides; m++) {
            // Presentation trig through the table (AGENTS.md R6).
            const float angle = kRadarFirstAngle * (float)(m + 1) * kDegrees;
            Vec3f offset;
            offset.x = fastCos(angle);
            offset.y = fastSin(angle);
            rings[k][m] = add(centre, scaled(offset, radius));
        }
    }
}

// The beam's triangles in the C#'s order, and the first triangle of each band.
int32_t radarTriangles(const Vec3f& basePoint, const Vec3f rings[kRadarRings][kRadarSides], Vec3f* points,
                       int32_t* bands) {
    int32_t count = 0;
    bands[0] = 0;
    for (int32_t m = 0; m < kRadarSides; m++) {
        const int32_t next = m + 1 < kRadarSides ? m + 1 : 0;
        points[count++] = basePoint;
        points[count++] = rings[0][m];
        points[count++] = rings[0][next];
    }
    for (int32_t k = 0; k < kRadarRings - 1; k++) {
        bands[k + 1] = count;
        for (int32_t m = 0; m < kRadarSides; m++) {
            const int32_t next = m + 1 < kRadarSides ? m + 1 : 0;
            const Vec3f a = rings[k][m];
            const Vec3f b = rings[k][next];
            const Vec3f c = rings[k + 1][m];
            const Vec3f d = rings[k + 1][next];
            points[count++] = a;
            points[count++] = b;
            points[count++] = c;
            points[count++] = b;
            points[count++] = d;
            points[count++] = c;
        }
    }
    bands[kRadarRings] = count;
    for (int32_t m = 1; m < kRadarSides - 1; m++) {
        points[count++] = rings[kRadarRings - 1][0];
        points[count++] = rings[kRadarRings - 1][m];
        points[count++] = rings[kRadarRings - 1][m + 1];
    }
    bands[kRadarRings + 1] = count;
    return count;
}

} // namespace

void FallView::queueRadar(MdkRenderer renderer, const FallSim& sim) {
    // C# QueueRadar (0x412f94): a fan from its base to ring 1, rings 1-3 at
    // 1 − 2^−k of the way to the spot, ring 4 on the spot, and a cap on ring 4.
    if (!sim.radarActive() || radarMesh_ < 0) {
        return;
    }
    Vec3f basePoint;
    Vec3f rings[kRadarRings][kRadarSides];
    radarRings(sim, &basePoint, rings);
    Vec3f points[kRadarVertices];
    int32_t bands[kRadarColours + 1];
    const int32_t count = radarTriangles(basePoint, rings, points, bands);
    scratch_.clear();
    for (int32_t i = 0; i < count; i++) {
        Vertex v;
        v.pos = Vec3{points[i].x, points[i].y, points[i].z};
        v.uv = Vec2{0.0f, 0.0f};
        v.colour = kVertexWhite;
        scratch_.push_back(v);
    }
    mdk_render_update_mesh(renderer, radarMesh_, scratch_.data(), count);
    for (int32_t c = 0; c < kRadarColours; c++) {
        MeshDraw draw;
        draw.mesh = radarMesh_;
        draw.first_vertex = bands[c];
        draw.vertex_count = bands[c + 1] - bands[c];
        draw.material = flat(Colour{0.0f, 1.0f, 0.0f, radarAlpha(c)}, Pass::Blended);
        memcpy(draw.world, Mat4::identity().m, sizeof(draw.world));
        mdk_render_draw_mesh(renderer, &draw);
    }
}

namespace {

// C# FallView.TrailWidth: a trail point's width factor by its age in ticks, the
// swell's (0-5), then down to 0 at the 32nd.
float trailWidth(int32_t age) {
    if (age < kTrailSwellCount) {
        return kTrailSwell[age];
    }
    const int32_t taper = FallMissile::kTrailPoints - kTrailSwellCount;
    return 1.0f - (float)(age - kTrailSwellCount) / (float)taper;
}

// C# FallView.TrailColour: segment k (1 to count - 1, the last one at the
// missile) of a trail of `count` points, packed as the vertices' RGBA: white at
// 215/255, the last 8 with blue 255 down to 31 and alpha 215 up to 250.
uint32_t trailColour(int32_t k, int32_t count) {
    const int32_t step = k - (count - kTrailYellowSegments);
    int32_t blue = 255;
    int32_t alpha = kTrailAlpha;
    if (step >= 0) {
        blue = (2040 - 255 * step) >> 3;
        alpha = ((kTrailYellowSegments - step) * kTrailAlpha + 255 * step) >> 3;
    }
    return 0xFFu | 0xFF00u | ((uint32_t)blue << 16) | ((uint32_t)alpha << 24);
}

// Vector3.TransformNormal(v, m): v by the matrix's upper 3x3, row-vector.
Vec3f turned(const Vec3f& v, const Mat4& m) {
    Vec3f r;
    r.x = v.x * m.m[0] + v.y * m.m[4] + v.z * m.m[8];
    r.y = v.x * m.m[1] + v.y * m.m[5] + v.z * m.m[9];
    r.z = v.x * m.m[2] + v.y * m.m[6] + v.z * m.m[10];
    return r;
}

// C# QueueTrails' ribbon (0x439454; set up by 0x43911c, sampled by 0x4392a4,
// upstream 04c25852): along the last 32 points of a missile, oldest first,
// between its tail fins' tips as the missile was turned at each point, widened
// by trailWidth. Writes each drawn segment's four corners (left, right at the
// older point, right, left at the newer) and its colour; returns the segments.
//
//   tail ---- white (215/255) ---- last 8 segments: yellower, more opaque -> missile
//   width x0 <---- tapering ---- x1, x1.05, x1.1, x1.2, x1.25 (swell), x1 at the missile
int32_t trailRibbon(const FallMissile& missile, const Vec3f& camera, const Vec3f& left, const Vec3f& right,
                    Vec3f* corners, uint32_t* colours) {
    const Vec3f* trail = missile.trail();
    const Vec3f* headings = missile.trailHeadings();
    const int32_t count = missile.trailCount();
    const Vec3f middle = scaled(add(left, right), 0.5f);
    const Vec3f toLeft = add(left, scaled(middle, -1.0f));
    const Vec3f toRight = add(right, scaled(middle, -1.0f));
    Vec3f lefts[FallMissile::kTrailPoints];
    Vec3f rights[FallMissile::kTrailPoints];
    for (int32_t i = 0; i < count; i++) {
        // The fins' tips around their middle, as turned then, scaled by the point's age.
        const Mat4 turn = orient(headings[i]);
        const Vec3f centre = add(trail[i], turned(middle, turn));
        const float width = trailWidth(count - 1 - i);
        lefts[i] = add(centre, scaled(turned(toLeft, turn), width));
        rights[i] = add(centre, scaled(turned(toRight, turn), width));
    }
    int32_t used = 0;
    for (int32_t k = 1; k < count; k++) {
        if (nearCamera(trail[k - 1], camera) || nearCamera(trail[k], camera)) {
            continue;
        }
        corners[used * 4 + 0] = lefts[k - 1];
        corners[used * 4 + 1] = rights[k - 1];
        corners[used * 4 + 2] = rights[k];
        corners[used * 4 + 3] = lefts[k];
        colours[used] = trailColour(k, count);
        used++;
    }
    return used;
}

// A packed vertex colour as the canvas's float colour.
Colour colourOf(uint32_t rgba) {
    return Colour{(float)(rgba & 0xFFu) / 255.0f, (float)((rgba >> 8) & 0xFFu) / 255.0f,
                  (float)((rgba >> 16) & 0xFFu) / 255.0f, (float)(rgba >> 24) / 255.0f};
}

} // namespace

void FallView::queueTrails(MdkRenderer renderer, const FallSim& sim) {
    if (trailMesh_ < 0) {
        return;
    }
    const Vec3f camera = sim.cameraPosition();
    const std::vector<FallMissile>& missiles = sim.missiles();
    scratch_.clear();
    Vec3f corners[FallMissile::kTrailPoints * 4];
    uint32_t colours[FallMissile::kTrailPoints];
    const int32_t order[6] = {0, 1, 2, 0, 2, 3};
    for (size_t m = 0; m < missiles.size() && m < (size_t)kTrailMissiles; m++) {
        const int32_t segments = trailRibbon(missiles[m], camera, trailLeft_, trailRight_, corners, colours);
        for (int32_t q = 0; q < segments; q++) {
            for (int32_t k = 0; k < 6; k++) {
                const Vec3f& p = corners[q * 4 + order[k]];
                Vertex v;
                v.pos = Vec3{p.x, p.y, p.z};
                v.uv = Vec2{0.0f, 0.0f};
                v.colour = colours[q];
                scratch_.push_back(v);
            }
        }
    }
    if (scratch_.empty()) {
        return;
    }
    mdk_render_update_mesh(renderer, trailMesh_, scratch_.data(), (int32_t)scratch_.size());
    MeshDraw draw;
    draw.mesh = trailMesh_;
    draw.vertex_count = (int32_t)scratch_.size();
    // The colour is the vertices' (C# Material.Flat(_effects.Apply(Vector4.One))).
    draw.material = flat(Colour{1.0f, 1.0f, 1.0f, 1.0f}, Pass::Blended);
    memcpy(draw.world, Mat4::identity().m, sizeof(draw.world));
    mdk_render_draw_mesh(renderer, &draw);
}

// ---- The lean way's canvas parts ----

void FallView::canvasHaze(MdkRenderer renderer, const FallSim& sim, float width) {
    const int32_t frame = sim.hazeFrame();
    const int32_t level = sim.wind() >> FallSim::kWindShift;
    if (hazeTexture_ < 0 || frame < 0 || frame >= (int32_t)hazeOffsets_.size()) {
        return;
    }
    if (frame != hazeShownFrame_ || level != hazeShownLevel_) {
        // The composite of the C#'s 8 mask draws: white at HazeBlend[level + b].
        uint8_t alphas[kHazeLevels + 1];
        alphas[0] = 0;
        bool any = false;
        for (int32_t b = 1; b <= kHazeLevels; b++) {
            alphas[b] = (uint8_t)rintf(hazeAlpha(level, b) * 255.0f);
            any = any || alphas[b] != 0;
        }
        const std::vector<uint8_t>& offsets = hazeOffsets_[(size_t)frame];
        for (size_t i = 0; i < offsets.size(); i++) {
            const uint8_t b = offsets[i];
            uint8_t* texel = hazeRgba_.data() + i * 4;
            texel[0] = 0xFF;
            texel[1] = 0xFF;
            texel[2] = 0xFF;
            texel[3] = b <= kHazeLevels ? alphas[b] : 0;
        }
        mdk_render_update_texture(renderer, hazeTexture_, hazeWidth_, hazeHeight_, hazeRgba_.data());
        hazeShownFrame_ = frame;
        hazeShownLevel_ = level;
        if (!any) {
            return;
        }
    }
    CanvasQuad q;
    q.x0 = 0.0f;
    q.y0 = 0.0f;
    q.x1 = width;
    q.y1 = 0.0f;
    q.x2 = width;
    q.y2 = kViewHeight;
    q.x3 = 0.0f;
    q.y3 = kViewHeight;
    q.u1 = 1.0f;
    q.u2 = 1.0f;
    q.v2 = 1.0f;
    q.v3 = 1.0f;
    q.texture = hazeTexture_;
    mdk_render_canvas_quad(renderer, &q);
}

void FallView::canvasTriangle(MdkRenderer renderer, const Vec3f* points, const Vec3f& camera, float width,
                              const Colour& colour) {
    canvasPolygon(renderer, points, 3, camera, width, colour);
}

void FallView::canvasPolygon(MdkRenderer renderer, const Vec3f* points, int32_t count, const Vec3f& camera,
                             float width, const Colour& colour) {
    // The view's pinhole (FallView.Quad's inverse): x right, y up the screen. A
    // triangle is a quad with its last corner doubled.
    float x[4];
    float y[4];
    for (int32_t k = 0; k < count; k++) {
        const float depth = camera.z - points[k].z;
        if (depth <= kViewNear) {
            return;
        }
        x[k] = width / 2.0f + (points[k].x - camera.x) * kViewFocal / depth;
        y[k] = kViewCentreY - (points[k].y - camera.y) * kViewFocal / depth;
    }
    CanvasQuad q;
    q.x0 = x[0];
    q.y0 = y[0];
    q.x1 = x[1];
    q.y1 = y[1];
    q.x2 = x[2];
    q.y2 = y[2];
    q.x3 = x[count - 1];
    q.y3 = y[count - 1];
    q.colour = colour;
    mdk_render_canvas_quad(renderer, &q);
}

void FallView::canvasRadar(MdkRenderer renderer, const FallSim& sim, float width) {
    if (!sim.radarActive()) {
        return;
    }
    Vec3f basePoint;
    Vec3f rings[kRadarRings][kRadarSides];
    radarRings(sim, &basePoint, rings);
    Vec3f points[kRadarVertices];
    int32_t bands[kRadarColours + 1];
    radarTriangles(basePoint, rings, points, bands);
    for (int32_t c = 0; c < kRadarColours; c++) {
        const Colour colour{0.0f, 1.0f, 0.0f, radarAlpha(c)};
        for (int32_t i = bands[c]; i + 3 <= bands[c + 1]; i += 3) {
            canvasTriangle(renderer, points + i, sim.cameraPosition(), width, colour);
        }
    }
}

void FallView::canvasTrails(MdkRenderer renderer, const FallSim& sim, float width) {
    const Vec3f camera = sim.cameraPosition();
    const std::vector<FallMissile>& missiles = sim.missiles();
    Vec3f corners[FallMissile::kTrailPoints * 4];
    uint32_t colours[FallMissile::kTrailPoints];
    for (size_t m = 0; m < missiles.size() && m < (size_t)kTrailMissiles; m++) {
        // One canvas quad a segment, in the segment's colour.
        const int32_t segments = trailRibbon(missiles[m], camera, trailLeft_, trailRight_, corners, colours);
        for (int32_t q = 0; q < segments; q++) {
            canvasPolygon(renderer, corners + q * 4, 4, camera, width, colourOf(colours[q]));
        }
    }
}

} // namespace fall
} // namespace mdk

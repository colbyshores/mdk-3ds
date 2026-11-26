#pragma once

// ============================================================================
// fall_view.h — draws the fall (fall/fall_sim.h).
//
// THE C# ORIGINAL is src/Mdk.Game/Fall/FallView.cs (with FallScreen.Frame's
// drawing half: the HUD after the scene, the effects over everything). A camera
// looking straight down with a focal length of 250 pixels on the 360-high view
// (wider views see more on the sides). The original's 2D layers behind the
// models (the ground, the minecrawler, the haze; the intro's space, moon and
// earth) are screen-aligned quads far below the camera, so the models stay in
// front of them.
//
//   camera ──►  models (Kurt, Bones, missiles, pickups, chutes, explosions), radar beam, trails
//      │
//      ▼ 6970  minecrawler             (fall)
//        6980  earth                   (intro)
//        6990  haze        │ moon      (fall: white streaks by the wind level │ intro)
//        7000  ground      │ space
//
// Then the canvas: the HUD (fall/fall_hud.h), the palette's effects over the
// frame (fall/fall_palette.h: the seam has no palette update, so the C#'s
// palette arithmetic is drawn as a white fill and an additive red), then the
// intro's whitening and the darkening as the C#'s fills.
//
// THE LEAN WAY (FallViewOptions::lean, the 3DS's). Four parts of the C#'s view
// cannot be done the C#'s way on the PICA200 (SPEC.md 7.3) and are drawn
// differently, each the same picture or the nearest the console can afford:
//   * the TRACK: the C# rewrites the 1024² ground texture as the crawler moves
//     (a 1 MB re-upload most frames); a 3DS re-upload is a CPU palette expansion
//     of the whole texture, so PODn is drawn OVER the ground instead, through
//     the ground's texel->screen map, from FallGround::trackRow() down -- the
//     track's 24-row ramp as one trapezoid;
//   * the HAZE: the C# draws 8 masks a frame, each white at its own alpha, from
//     a 600 x 1440 texture per haze frame; the 3DS takes neither the height nor a
//     scene draw's material alpha (its scene combiner REPLACES with the texture,
//     and a filtered scene draw cuts alpha under half cover). It is one RGBA
//     image at half resolution, white at the composite's alpha per pixel --
//     rewritten when the haze frame or the wind level changes -- drawn first on
//     the canvas, under the HUD (so over the models: the approximation);
//   * the RADAR BEAM and the missiles' TRAILS are translucent flat colours (48/256
//     to 0.35), which the same cut deletes in the scene: projected through the
//     view's own pinhole and drawn on the canvas, after the haze;
//   * EXPLODE's 26 stacked frames are a texture each (fall_models.h).
// The desktop takes the C#'s way for all of them; `--fall-lean` shows this way
// there too.
//
// Cold (one screen, a frame at a time): AGENTS.md §2. The containers are
// reserved at load and reused every frame. No virtuals, no templates, no
// exceptions (AGENTS.md §10).
// ============================================================================

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "fall/fall_ground.h"
#include "fall/fall_hud.h"
#include "fall/fall_models.h"
#include "fall/fall_palette.h"
#include "render/render.h"
#include "render/render_types.h"

namespace mdk {

namespace formats {
class Bni;
class Font;
class Fti;
class TextureArchive;
}

namespace fall {

class FallSim;

struct FallViewOptions {
    // The 3DS's way (see the header). Its haze is a 300 x 90 image.
    bool lean = false;
};

class FallView {
public:
    FallView() = default;
    FallView(const FallView&) = delete;
    FallView& operator=(const FallView&) = delete;

    // C# FallView(renderer, bni, mti, index, fti, ...): the palettes, the models,
    // the HUD, the ground, the crawler, the intro's images, the haze, the meshes.
    // `pickups` are the fall's pickups (FALLPU_n), whose models are built now.
    // The BNI and the MTI must outlive the view. False (with `why`) when a part
    // the fall cannot do without did not load.
    bool load(MdkRenderer renderer, const formats::Bni& bni, const formats::TextureArchive& mti, int32_t index,
              const formats::Fti& fti, const formats::Font& big, const formats::Font& small,
              const std::vector<std::string>& pickups, const FallViewOptions& options, std::string* why);

    // FallView.Backdrop: the colour behind the scene, 0xAARRGGBB for
    // MdkFrameParams.clear_colour (colour 0; its effects are drawn over it).
    uint32_t backdrop() const;

    // FallHud.Messages, for the sim's Message event.
    FallHud& hud() { return hud_; }

    // One frame, between mdk_render_begin_frame and mdk_render_end_frame:
    // FallView.Draw (the scene and its camera), FallHud.Draw unless in the intro,
    // the palette's effects, FallView.DrawEffects.
    void draw(MdkRenderer renderer, const FallSim& sim);

private:
    struct Image {
        int32_t texture = -1;
        float width = 0.0f;
        float height = 0.0f;
    };
    struct BackDraw {
        int32_t first = 0;
        Material material;
        int32_t frame = 0;
    };

    bool image(const formats::Texture& texture, const uint8_t* paletteRgba, Image* out);
    void queueIntro(const FallSim& sim, const formats::Vec3f& camera, float width);
    void queueGround(const FallSim& sim, const formats::Vec3f& camera, float width);
    void queueTrack(const formats::Vec3f& camera, float centreRow, float width);
    void sprite(const formats::Vec3f& camera, float width, const Image& image, float centreX, float centreY,
                float scaleX, float scaleY, float depth, int32_t palette);
    // FallView.Quad: a quad covering `screen` (view pixels) at `depth` below the
    // camera; `uv` null is the whole texture.
    void quad(const formats::Vec3f& camera, float width, const FallRect& screen, float depth,
              const Material& material, const FallRect* uv, int32_t frame);
    // Any four corners (view pixels) with their uvs, the lean track's trapezoid.
    void corners(const formats::Vec3f& camera, float width, const float* x, const float* y, const float* u,
                 const float* v, float depth, const Material& material);
    void queueModels(MdkRenderer renderer, const FallSim& sim);
    void queueRadar(MdkRenderer renderer, const FallSim& sim);
    void queueTrails(MdkRenderer renderer, const FallSim& sim);
    // The lean way's canvas parts.
    void canvasHaze(MdkRenderer renderer, const FallSim& sim, float width);
    void canvasRadar(MdkRenderer renderer, const FallSim& sim, float width);
    void canvasTrails(MdkRenderer renderer, const FallSim& sim, float width);
    void canvasTriangle(MdkRenderer renderer, const formats::Vec3f* points, const formats::Vec3f& camera,
                        float width, const Colour& colour);
    // A triangle or a quad (`count` 3 or 4 points) projected onto the canvas.
    void canvasPolygon(MdkRenderer renderer, const formats::Vec3f* points, int32_t count,
                       const formats::Vec3f& camera, float width, const Colour& colour);
    void fill(MdkRenderer renderer, float width, const Colour& colour, bool additive);

    MdkRenderer renderer_ = nullptr;
    FallViewOptions options_;
    int32_t index_ = 0;
    std::unique_ptr<FallPalette> effects_;
    uint8_t spaceRgba_[256 * 4] = {};
    uint8_t whiteRgba_[256 * 4] = {};
    int32_t palette_ = -1;
    int32_t spacePalette_ = -1;
    int32_t whitePalette_ = -1;
    FallGround ground_;
    int32_t groundTexture_ = -1;
    Image pod_;
    std::vector<Image> crawler_;
    Image space_;
    Image moon_;
    Image earth_;
    // The C#'s haze: a 600 x 1440 mask texture per haze frame.
    std::vector<int32_t> haze_;
    // The lean haze: each frame's blend offsets at half resolution, the RGBA
    // image the canvas draws, and what it shows now.
    std::vector<std::vector<uint8_t>> hazeOffsets_;
    std::vector<uint8_t> hazeRgba_;
    int32_t hazeTexture_ = -1;
    int32_t hazeWidth_ = 0;
    int32_t hazeHeight_ = 0;
    int32_t hazeShownFrame_ = -1;
    int32_t hazeShownLevel_ = -1;
    int32_t backMesh_ = -1;
    int32_t radarMesh_ = -1;
    int32_t trailMesh_ = -1;
    // The trail ribbon's ends in the missile's model space (0x43911c): its
    // leftmost and rightmost vertices by x, the tips of its tail fins.
    formats::Vec3f trailLeft_;
    formats::Vec3f trailRight_;
    std::vector<Vertex> back_;
    std::vector<BackDraw> backDraws_;
    std::vector<Vertex> scratch_;
    Camera camera_;
    bool rigSet_ = false;
    FallModels models_;
    FallHud hud_;
};

} // namespace fall
} // namespace mdk

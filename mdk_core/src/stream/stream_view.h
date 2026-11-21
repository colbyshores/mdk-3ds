#pragma once

// ============================================================================
// stream_view.h — draws the stream (C# src/Mdk.Game/Stream/StreamView.cs;
// original 0x436b00, 0x438bfc): the scrolling background far behind, the
// models, the translucent tube far to near, the lights and the planet, the
// health box and the fade.
//
//   background quad (far) -> Kurt, Bones, bonus / Gunter -> tube (blended, far
//   to near) -> lights -> canvas: health box, fade (red added, white, black)
//
// Package #18b. Everything goes through the renderer seam (render/render.h):
// the backend sets every GPU state per draw (AGENTS.md P2), this file only
// queues draws.
//
// WHAT DIFFERS FROM THE C#, AND WHY (none of it is the picture's arithmetic):
//   * THE MODELS. The C# builds one static mesh per (model, animation, frame)
//     the first time that pose is drawn. Here each of the three model slots
//     (Kurt, Bones, the bonus or Gunter) owns ONE dynamic mesh, rewritten every
//     frame from the pose baked at load: the triangles, their surfaces and UVs
//     do not depend on the pose, so only the corners' positions are written. The
//     same vertices reach the GPU; the 3DS does not grow a mesh per pose
//     (AGENTS.md §10: no per-frame container growth -- every buffer below is
//     sized at load).
//   * THE BACKGROUND'S WRAP. The C#'s quad samples BG with UVs past 1 and the
//     sampler repeats. The 3DS pads a 600x360 texture to 1024x512 and its UVs are
//     scaled into the real part, so a repeat would read the padding; the quad is
//     cut at the image's edges instead (at most 3 x 2 pieces), each piece's UVs
//     inside [0, 1]. Same texels, both targets.
//   * THE TUBE ON A BACKEND WITHOUT VERTEX COLOURS (MdkCapabilities::
//     vertex_colours false: the 3DS, whose PICA vertex is position + UV). The
//     C#'s Gouraud RGBA is the ramp colour (0-63) by the level's alpha (0-5), so
//     the tube is drawn textured instead, from a 64 x 6 RGBA table of exactly
//     those colours, each corner's UV the centre of its (colour, level) texel.
//     Filtered, the UV interpolation is the colour interpolation (the ramp is
//     linear between its keys); nearest, it steps by ramp entry.
//   * MODS. The C# gives the canvas images (SC_STAT, SNIP_TXT) their mod
//     replacement (CanvasImages.Create); the stream's mods are not wired here.
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

#include "formats/bni.h"
#include "formats/dti.h"
#include "formats/model.h"
#include "formats/model_animation.h"
#include "formats/palette.h"
#include "formats/texture_archive.h"
#include "render/material_resolver.h"
#include "render/render.h"
#include "render/surface_cache.h"
#include "stream/stream_flight.h"

namespace mdk {
namespace stream {

class StreamView {
public:
    // The model slots the frame draws (C# Draw's three DrawModel calls).
    enum Slot { KurtSlot, BonesSlot, OtherSlot, kSlotCount };

    StreamView() = default;
    StreamView(const StreamView&) = delete;
    StreamView& operator=(const StreamView&) = delete;

    // C# StreamView(renderer, bni, palette, archive): the images, the meshes,
    // and here also every model and pose the stream can draw, baked now (cold)
    // rather than when first drawn. `kind` picks the other model and animation.
    // The BNI, the palette and the archive are BORROWED and must outlive the
    // view. False (with `error` filled) when an asset is missing.
    bool load(MdkRenderer renderer, const formats::Bni* bni, const formats::Palette* palette,
              const formats::TextureArchive* archive, StreamTube::Kind kind, char* error,
              int32_t errorSize);

    // The tube's colour table, made from the tube once it is built (a no-op on a
    // backend with vertex colours). False when the GPU has no room for it.
    bool loadTubeTable(const StreamTube& tube, char* error, int32_t errorSize);

    // An animation's clip of STREAM.BNI (C# AnimationOf -> Clip). False when it
    // is missing.
    bool clipOf(const char* animation, Clip* out) const;

    // C# ViewOf: the camera, a 250-pixel focal length on the 360-high view.
    void camera(const Flight& flight, Camera* out) const;

    // C# Draw: the whole frame, queued. Call between begin_frame and end_frame
    // (the dynamic meshes are written here).
    void draw(const Flight& flight);

    // The other model and animation for this stream's kind.
    const char* otherModel() const { return otherModel_; }
    const char* otherAnimation() const { return otherAnimation_; }

private:
    // An image of the archive on the GPU (C# Image).
    struct Image {
        MdkTextureId texture = -1;
        float width = 0.0f;
        float height = 0.0f;
    };

    // One pose source: a model, an animation, its baked frames.
    struct Pose {
        std::string model;
        std::string animation;
        std::vector<std::vector<std::vector<formats::Vec3f>>> frames;
    };

    // A run of corners drawn with one material (C# PoseMesh.Batches).
    struct Batch {
        int32_t first = 0;
        int32_t count = 0;
        Material material;
    };

    // A model's triangles, independent of the pose: each corner's part, vertex
    // and UV, and the batches (C# Build, minus the positions).
    struct ModelMesh {
        std::string name;
        formats::Model model;
        std::vector<int32_t> parts;
        std::vector<int32_t> indices;
        std::vector<Vec2> uvs;
        std::vector<Batch> batches;
    };

    bool loadImage(const char* name, Image* out, char* error, int32_t errorSize);
    bool loadModel(const char* name, char* error, int32_t errorSize);
    bool loadPose(const char* model, const char* animation, char* error, int32_t errorSize);
    const ModelMesh* meshOf(const std::string& name) const;
    const Pose* poseOf(const std::string& model, const std::string& animation) const;

    void drawBackground(const Flight& flight);
    void addQuad(Vec3 topLeft, Vec3 topRight, Vec3 bottomRight, Vec3 bottomLeft, Vec2 uv0, Vec2 uv1,
                 uint32_t colour);
    void drawTube(const Flight& flight);
    void drawSprites(const Flight& flight);
    int32_t addSprites(const Flight& flight, StreamTube::SpawnKind kind, float scale);
    void drawModel(Slot slot, const char* name, const char* animation, int32_t frame, const Mat4& transform);
    void drawHealth(int32_t health, int32_t blink);
    void drawImage(const Image& image, float sx, float sy, float sw, float sh, float tx, float ty, float tw,
                   float th);
    void fillRect(float x, float y, float w, float h, Colour colour, bool additive);
    void drawFade(Fade fade);

    MdkRenderer renderer_ = nullptr;
    const formats::Bni* bni_ = nullptr;
    const formats::Palette* palette_ = nullptr;
    std::vector<const formats::TextureArchive*> archives_;
    formats::Dti dti_;
    MaterialResolver resolver_{nullptr};
    SurfaceCache surfaces_;
    MdkPaletteId paletteId_ = -1;
    bool vertexColours_ = true;
    const char* otherModel_ = "SWH150";
    const char* otherAnimation_ = "SWHANM";

    Image background_;
    Image light_;
    Image planet_;
    Image panel_;
    Image digits_;
    // The tube's colour table, when the backend has no vertex colours.
    MdkTextureId tubeTable_ = -1;

    MdkMeshId tubeMesh_ = -1;
    MdkMeshId spriteMesh_ = -1;
    MdkMeshId backMesh_ = -1;
    MdkMeshId modelMeshes_[kSlotCount] = {-1, -1, -1};
    int32_t modelCapacity_ = 0;

    std::vector<ModelMesh> models_;
    std::vector<Pose> poses_;
    std::vector<formats::ModelAnimation> animations_;

    // The frame's scratch, sized at load (no per-frame growth).
    std::vector<TubeVertex> tubeVertices_;
    std::vector<Vertex> vertices_;
};

} // namespace stream
} // namespace mdk

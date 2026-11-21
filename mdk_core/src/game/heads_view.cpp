// heads_view.cpp -- see heads_view.h. C# Menu/HeadsView.cs: the constructor and
// Build/Surface (:51-65, :120-169), Update (:68-74), Draw (:78-96), Places
// (:99-112).

#include "game/heads_view.h"

#include <math.h>
#include <string.h>

#include <string>

#include "formats/bni.h"
#include "formats/mdk_data.h"
#include "formats/model.h"
#include "formats/palette.h"
#include "formats/texture.h"
#include "formats/texture_archive.h"
#include "render/camera_builder.h"
#include "util/math_lut.h"

namespace mdk {
namespace {

// C# HeadsView's constants.
const char kModelName[] = "XGHEAD";
const char kPaletteName[] = "PAL";
const char kTextures[] = "MISC/STATS.MTI";
const float kSpin = 157.0f;
// C# Focal: 250 pixels of the 360-high view; the field of view it makes,
// float.RadiansToDegrees(2 MathF.Atan(360 / 2 / 250)).
const float kFieldOfView = 71.507774f;
const float kScale = 0.8f;
const float kNear = 0.5f;
const float kFar = 1000.0f;
const float kDistance = 8.0f;
const float kRowWidth = 20.0f;
const float kFirstRowZ = -3.0f;
const float kSecondRowZ = -4.5f;
// Up to 4 heads make one row.
const int32_t kOneRow = 5;
const int32_t kMaxPerRow = 8;
const int32_t kPaletteBytes = 768;
// StatsScreen.SystemColours, in bytes (colours 0-63).
const int32_t kSystemColours = 192;
const float kPi = 3.14159265358979323846f;

// A batch's surface while the mesh is built (C# Dictionary<Material, List<Vertex>>,
// enumerated in insertion order: no key is ever removed).
struct SurfaceKey {
    Material material;
    std::vector<Vertex> vertices;
};

bool sameMaterial(const Material& a, const Material& b) {
    return a.texture == b.texture && a.palette == b.palette && a.colour.r == b.colour.r &&
           a.colour.g == b.colour.g && a.colour.b == b.colour.b && a.colour.a == b.colour.a &&
           a.frame_count == b.frame_count && a.pass == b.pass;
}

} // namespace

int32_t HeadsView::places(int32_t shown, int32_t count, Vec3 out[kMaxHeads]) {
    // C#: count < 5: one row of `count`; up to 16: two rows of (count + 1) / 2;
    // more: rows of 8.
    const int32_t perRow = count < kOneRow ? count : count <= kMaxHeads ? (count + 1) / 2 : kMaxPerRow;
    if (shown > perRow * 2) {
        shown = perRow * 2;
    }
    if (shown > kMaxHeads) {
        shown = kMaxHeads;
    }
    // The C#'s row = i / perRow and k = i % perRow, counted instead of divided.
    int32_t row = 0;
    int32_t k = 0;
    for (int32_t i = 0; i < shown; i++) {
        const int32_t left = shown - row * perRow;
        const int32_t m = perRow < left ? perRow : left;
        const float x = kRowWidth * (float)(k + 1) / (float)(m + 1) - kRowWidth / 2.0f;
        out[i] = Vec3{x, kDistance, row == 0 ? kFirstRowZ : kSecondRowZ};
        k++;
        if (k == perRow) {
            k = 0;
            row++;
        }
    }
    return shown > 0 ? shown : 0;
}

void HeadsView::update(int32_t shown, float delta) {
    const int32_t n = shown < kMaxHeads ? shown : kMaxHeads;
    for (int32_t i = 0; i < n; i++) {
        angles_[i] += kSpin * delta;
    }
}

bool HeadsView::load(MdkRenderer renderer, const formats::Bni& bni, const uint8_t* systemRgb,
                     const formats::MdkData& data) {
    mesh_ = -1;
    batches_.clear();
    const formats::ArchiveIndex::Entry* modelEntry = bni.index().find(kModelName);
    const formats::ArchiveIndex::Entry* paletteEntry = bni.index().find(kPaletteName);
    const std::vector<uint8_t>& bytes = bni.bytes();
    if (!modelEntry || !paletteEntry || paletteEntry->offset < 0 ||
        (int64_t)paletteEntry->offset + kPaletteBytes > (int64_t)bytes.size()) {
        return false;
    }
    formats::Model model;
    if (!formats::Model::parse(&model, kModelName, bytes.data(), (int32_t)bytes.size(),
                               modelEntry->offset, formats::Model::Header::NoFlags)) {
        return false;
    }
    // The page's palette (StatsScreen.PageRgb): the system's 0-63, PAL's 64-255.
    uint8_t rgb[kPaletteBytes];
    memcpy(rgb, &bytes[(size_t)paletteEntry->offset], (size_t)kPaletteBytes);
    if (systemRgb) {
        memcpy(rgb, systemRgb, (size_t)kSystemColours);
    }
    const formats::Palette palette = formats::Palette::fromRgb(rgb, kPaletteBytes);
    std::vector<uint8_t> mti;
    formats::TextureArchive archive;
    if (data.read(kTextures, &mti)) {
        // C# TextureArchive.Load: the file's size word, the 16-byte header, the
        // names as stored, index 0 see-through.
        archive = formats::TextureArchive::parse(mti.data(), (int32_t)mti.size(),
                                                 formats::TextureArchive::kFileBase,
                                                 formats::TextureArchive::kHeader,
                                                 formats::TextureArchive::Names::AsStored,
                                                 formats::TextureArchive::Zero::SeeThrough);
    }

    // C# Build: the rest pose, both faces, in batches by surface.
    const int32_t paletteId = mdk_render_create_palette(renderer, palette.rgba());
    std::vector<std::pair<const formats::Texture*, int32_t>> textures;
    std::vector<SurfaceKey> bySurface;
    for (size_t p = 0; p < model.partList.size(); p++) {
        const formats::Model::Part& part = model.partList[p];
        for (size_t t = 0; t < part.triangleMaterials.size(); t++) {
            // C# Surface: a texture of the archive, else a palette colour (a
            // negative value is the colour itself; else the archive's colour of
            // the material's name).
            const int32_t value = part.triangleMaterials[t];
            Material material;
            const formats::Texture* texture = nullptr;
            const std::string name =
                value >= 0 && value < (int32_t)model.materials.size()
                    ? model.materials[(size_t)value].nameString()
                    : std::string();
            if (value >= 0 && value < (int32_t)model.materials.size()) {
                texture = archive.findTexture(name);
            }
            if (texture) {
                int32_t id = -1;
                for (size_t k = 0; k < textures.size(); k++) {
                    if (textures[k].first == texture) {
                        id = textures[k].second;
                    }
                }
                if (id < 0) {
                    id = mdk_render_create_index_texture(
                        renderer, texture->width(), texture->height() * texture->frameCount(),
                        texture->indices(), palette.rgba());
                    textures.push_back(std::make_pair(texture, id));
                }
                material.texture = id;
                material.palette = paletteId;
                material.colour = Colour{1.0f, 1.0f, 1.0f, 1.0f};
                material.frame_count = texture->frameCount();
                material.pass = Pass::DoubleSided;
            } else {
                const int32_t colour =
                    value >= 0 && value < (int32_t)model.materials.size() ? archive.findColor(name)
                                                                          : -1;
                const int32_t index = value < 0 ? -value : colour;
                if (index < 0 || index >= formats::Palette::kSize) {
                    continue;
                }
                const uint8_t* rgba = palette.rgba();
                // C# Material.Flat(rgba / 255, DoubleSided).
                material.texture = -1;
                material.palette = -1;
                material.colour = Colour{(float)rgba[index * 4] / 255.0f,
                                         (float)rgba[index * 4 + 1] / 255.0f,
                                         (float)rgba[index * 4 + 2] / 255.0f, 1.0f};
                material.frame_count = 1;
                material.pass = Pass::DoubleSided;
            }
            const float su = texture ? 1.0f / (float)texture->width() : 0.0f;
            const float sv = texture ? 1.0f / (float)texture->height() : 0.0f;
            SurfaceKey* surface = nullptr;
            for (size_t k = 0; k < bySurface.size(); k++) {
                if (sameMaterial(bySurface[k].material, material)) {
                    surface = &bySurface[k];
                }
            }
            if (!surface) {
                bySurface.push_back(SurfaceKey());
                surface = &bySurface.back();
                surface->material = material;
            }
            for (size_t k = 0; k < 3; k++) {
                const size_t corner = t * 3 + k;
                const int32_t vi = part.triangleIndices[corner];
                if (vi < 0 || vi >= (int32_t)part.vertices.size() || corner >= part.triangleUvs.size()) {
                    continue;
                }
                const formats::Vec3f& v = part.vertices[(size_t)vi];
                const formats::Vec2f& uv = part.triangleUvs[corner];
                Vertex vertex;
                vertex.pos = Vec3{v.x, v.y, v.z};
                vertex.uv = Vec2{uv.x * su, uv.y * sv};
                vertex.colour = kVertexWhite;
                surface->vertices.push_back(vertex);
            }
        }
    }
    std::vector<Vertex> all;
    for (size_t k = 0; k < bySurface.size(); k++) {
        Batch batch;
        batch.first = (int32_t)all.size();
        batch.count = (int32_t)bySurface[k].vertices.size();
        batch.material = bySurface[k].material;
        batches_.push_back(batch);
        all.insert(all.end(), bySurface[k].vertices.begin(), bySurface[k].vertices.end());
    }
    mesh_ = all.empty() ? -1 : mdk_render_create_mesh(renderer, all.data(), (int32_t)all.size());
    if (mesh_ < 0) {
        batches_.clear();
    }
    return mesh_ >= 0;
}

void HeadsView::draw(MdkRenderer renderer, int32_t shown, int32_t count, float areaX, float areaY,
                     float areaWidth, float areaHeight, float canvasWidth) const {
    if (mesh_ < 0 || shown <= 0 || areaWidth <= 0.0f || areaHeight <= 0.0f || canvasWidth <= 0.0f) {
        return;
    }
    // C#: fieldOfView = 2 atan(ScreenView.Size.Y / 2 / Focal) in degrees, the
    // view from the origin along +Y, Z up, at the area's aspect. A constant: 2
    // atan(180 / 250) is 71.507774 degrees in float (no libm call, R6).
    const float fieldOfView = kFieldOfView;
    Camera camera;
    if (!buildLookAtCamera(&camera, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f},
                           Vec3{0.0f, 0.0f, 1.0f}, fieldOfView, areaWidth / areaHeight, kNear,
                           kFar)) {
        return;
    }
    // The inset (heads_view.h): clip' = clip * M, M scaling NDC into the area's
    // rectangle of the canvas (y up in NDC, down on the canvas).
    const float sx = areaWidth / canvasWidth;
    const float sy = areaHeight / kCanvasHeight;
    const float ox = (areaX + areaWidth * 0.5f) / canvasWidth * 2.0f - 1.0f;
    const float oy = 1.0f - (areaY + areaHeight * 0.5f) / kCanvasHeight * 2.0f;
    for (int32_t row = 0; row < 4; row++) {
        float* p = &camera.projection[row * 4];
        p[0] = p[0] * sx + p[3] * ox;
        p[1] = p[1] * sy + p[3] * oy;
    }
    // Every piece of the 3D state the heads' draws read, set for this frame (AGENTS.md
    // P2): the camera, no lighting rig (the original look; no haze left over from
    // the last level), no point lights, no glow billboards.
    mdk_render_set_camera(renderer, &camera);
    const Lighting unlit;
    mdk_render_set_lighting(renderer, &unlit);
    mdk_render_set_point_lights(renderer, nullptr, 0);
    mdk_render_set_glow_quads(renderer, nullptr, 0);

    Vec3 at[kMaxHeads];
    const int32_t n = places(shown, count, at);
    for (int32_t i = 0; i < n; i++) {
        // C# CreateScale(0.8) * CreateRotationZ(angle) * CreateTranslation(place).
        const float radians = angles_[i] * kPi / 180.0f;
        const float c = fastCos(radians);
        const float s = fastSin(radians);
        MeshDraw draw;
        draw.mesh = mesh_;
        float* w = draw.world;
        memset(w, 0, sizeof(draw.world));
        w[0] = kScale * c;
        w[1] = kScale * s;
        w[4] = -kScale * s;
        w[5] = kScale * c;
        w[10] = kScale;
        w[12] = at[i].x;
        w[13] = at[i].y;
        w[14] = at[i].z;
        w[15] = 1.0f;
        for (size_t b = 0; b < batches_.size(); b++) {
            draw.first_vertex = batches_[b].first;
            draw.vertex_count = batches_[b].count;
            draw.material = batches_[b].material;
            draw.frame = 0;
            mdk_render_draw_mesh(renderer, &draw);
        }
    }
}

void HeadsView::forget() {
    mesh_ = -1;
    batches_.clear();
    for (int32_t i = 0; i < kMaxHeads; i++) {
        angles_[i] = 0.0f;
    }
}

} // namespace mdk

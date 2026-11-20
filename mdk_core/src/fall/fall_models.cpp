// fall_models.cpp — see fall_models.h. The C#'s FallModels.cs: Get, Build, Resolve,
// Colour and TextureId, with Draw's batches.

#include "fall/fall_models.h"

#include <string.h>

#include "formats/bni.h"
#include "formats/texture.h"
#include "formats/texture_archive.h"

namespace mdk {
namespace fall {

namespace {

// FallModels.NamedParts: models with named parts (bit 7 of the table 0x490ca4).
const char* const kNamedParts[] = {"KURT",     "MISSILE", "CHUTE",    "BONES",   "SW_DUMMY",
                                   "SW_H150",  "SW_THUMP", "SW_TWIST", "SW_INTER"};
// Material values from 256 are special (glass, mirrors): not in the fall.
constexpr int32_t kSpecialFirst = 256;
constexpr const char* kPenPrefix = "PEN_";
constexpr int32_t kNoPen = -1;

bool named(const std::string& name) {
    for (size_t i = 0; i < sizeof(kNamedParts) / sizeof(kNamedParts[0]); i++) {
        if (name == kNamedParts[i]) {
            return true;
        }
    }
    return false;
}

// C# `int.TryParse(name.AsSpan(4), out var pen)`: decimal digits with an optional
// sign, nothing else.
bool penOf(const std::string& name, int32_t* pen) {
    const size_t prefix = strlen(kPenPrefix);
    if (name.compare(0, prefix, kPenPrefix) != 0 || name.size() == prefix) {
        return false;
    }
    size_t i = prefix;
    bool negative = false;
    if (name[i] == '-' || name[i] == '+') {
        negative = name[i] == '-';
        i++;
    }
    if (i == name.size()) {
        return false;
    }
    int64_t value = 0;
    for (; i < name.size(); i++) {
        if (name[i] < '0' || name[i] > '9') {
            return false;
        }
        value = value * 10 + (name[i] - '0');
        if (value > 0x80000000LL) {
            return false;
        }
    }
    value = negative ? -value : value;
    if (value > 0x7FFFFFFFLL) {
        return false;
    }
    *pen = (int32_t)value;
    return true;
}

} // namespace

bool FallModels::load(MdkRenderer renderer, const formats::Bni& bni, const formats::TextureArchive& mti,
                      int32_t palette, const uint8_t* paletteRgba, const std::vector<std::string>& still,
                      const std::vector<std::string>& animated,
                      const std::vector<std::vector<std::string>>& animations) {
    renderer_ = renderer;
    bni_ = &bni;
    mti_ = &mti;
    palette_ = palette;
    paletteRgba_ = paletteRgba;
    const MdkCapabilities caps = mdk_render_capabilities(renderer);
    maxTexture_ = caps.max_texture_size > 0 ? caps.max_texture_size : 1024;
    entries_.reserve(still.size() + animated.size());
    bool ok = true;
    for (size_t i = 0; i < still.size(); i++) {
        ok = add(still[i], false, nullptr) && ok;
    }
    for (size_t i = 0; i < animated.size(); i++) {
        ok = add(animated[i], true, i < animations.size() ? &animations[i] : nullptr) && ok;
    }
    return ok;
}

bool FallModels::add(const std::string& name, bool animated, const std::vector<std::string>* animations) {
    for (size_t i = 0; i < entries_.size(); i++) {
        if (entries_[i].name == name) {
            return true;
        }
    }
    // C# Get: `bni.Has(name) ? Model.Parse(..., Header.NoFlags, parts) : null`.
    const formats::ArchiveIndex::Entry* found = bni_->index().find(name);
    if (found == nullptr) {
        return true;   // the C#'s null model: drawn as nothing
    }
    entries_.push_back(Entry());
    Entry& entry = entries_.back();
    entry.name = name;
    entry.animated = animated;
    const std::vector<uint8_t>& bytes = bni_->bytes();
    if (!formats::Model::parse(&entry.model, name, bytes.data(), (int32_t)bytes.size(), found->offset,
                               formats::Model::Header::NoFlags,
                               named(name) ? formats::Model::Parts::Named : formats::Model::Parts::Single)) {
        entries_.pop_back();
        return false;
    }

    int32_t triangles = 0;
    for (size_t p = 0; p < entry.model.partList.size(); p++) {
        triangles += (int32_t)entry.model.partList[p].triangleMaterials.size();
    }
    entry.capacity = triangles * 3;
    if (scratch_.capacity() < (size_t)entry.capacity) {
        scratch_.reserve((size_t)entry.capacity);
        sorted_.reserve((size_t)entry.capacity);
    }

    if (!animated) {
        // The rest pose, built once (the C#'s first Draw of it).
        std::vector<Batch> batches;
        build(entry.model, entry.model.restPose(), &sorted_, &batches);
        if (!sorted_.empty()) {
            entry.mesh = mdk_render_create_mesh(renderer_, sorted_.data(), (int32_t)sorted_.size());
            entry.batches = batches;
        }
        return true;
    }

    // C# `_baked[animation] = Animation(animation).Bake(model)`, at load.
    if (animations != nullptr) {
        for (size_t a = 0; a < animations->size(); a++) {
            const std::string& animationName = (*animations)[a];
            const formats::ArchiveIndex::Entry* at = bni_->index().find(animationName);
            formats::ModelAnimation animation;
            if (at == nullptr || !formats::ModelAnimation::parse(&animation, animationName, bytes.data(),
                                                                 (int32_t)bytes.size(), at->offset)) {
                return false;
            }
            Baked baked;
            baked.name = animationName;
            baked.frames = animation.bake(entry.model);
            entry.baked.push_back(baked);
        }
    }
    entry.mesh = mdk_render_create_dynamic_mesh(renderer_, entry.capacity > 0 ? entry.capacity : 3);
    entry.batches.reserve(8);
    // One build now makes the model's textures (C# TextureId) at load, not on the
    // fall's first frame.
    if (!entry.baked.empty() && !entry.baked[0].frames.empty()) {
        build(entry.model, entry.baked[0].frames[0], &sorted_, &entry.batches);
    }
    return true;
}

int32_t FallModels::surfaceOf(const formats::Texture* texture) {
    for (size_t i = 0; i < surfaces_.size(); i++) {
        if (surfaces_[i].texture == texture) {
            return (int32_t)i;
        }
    }
    // C# TextureId: CreateIndexTexture(width, height × frames, indices), once a texture.
    Surface surface;
    surface.texture = texture;
    const int32_t frames = texture->frameCount() > 0 ? texture->frameCount() : 1;
    if (frames > 1 && texture->height() * frames > maxTexture_) {
        for (int32_t f = 0; f < frames; f++) {
            surface.frames.push_back(mdk_render_create_index_texture(
                renderer_, texture->width(), texture->height(), texture->frame(f), paletteRgba_));
        }
    } else {
        surface.id = mdk_render_create_index_texture(renderer_, texture->width(), texture->height() * frames,
                                                     texture->indices(), paletteRgba_);
    }
    surfaces_.push_back(surface);
    return (int32_t)surfaces_.size() - 1;
}

bool FallModels::colour(int32_t index, Batch* out) {
    // C# Colour: `index >= SpecialFirst` is not drawn; otherwise a flat colour.
    if (index >= kSpecialFirst || index < 0) {
        return false;
    }
    out->material = Material();
    out->material.pass = Pass::DoubleSided;
    out->pen = index;
    out->surface = -1;
    return true;
}

bool FallModels::resolve(int32_t value, const formats::Model& model, Batch* out) {
    if (value < 0) {
        return colour(-value, out);
    }
    if ((size_t)value >= model.materials.size()) {
        return false;
    }
    const std::string name = model.materials[(size_t)value].nameString();
    const formats::Texture* texture = mti_->findTexture(name);
    if (texture != nullptr) {
        out->surface = surfaceOf(texture);
        out->pen = kNoPen;
        out->material = Material();
        out->material.texture = surfaces_[(size_t)out->surface].id;
        out->material.palette = palette_;
        out->material.frame_count = texture->frameCount();
        out->material.pass = Pass::DoubleSided;
        return true;
    }
    const int32_t index = mti_->findColor(name);
    if (index >= 0) {
        return colour(index, out);
    }
    int32_t pen = 0;
    return penOf(name, &pen) && colour(pen, out);
}

void FallModels::build(const formats::Model& model, const std::vector<std::vector<formats::Vec3f>>& pose,
                       std::vector<Vertex>* vertices, std::vector<Batch>* batches) {
    // C# Build: `bySurface[(material, pen)]`, a group per surface in the order the
    // surfaces first appear, then the groups one after the other.
    batches->clear();
    vertices->clear();
    size_t used = 0;
    for (size_t p = 0; p < model.partList.size(); p++) {
        const formats::Model::Part& part = model.partList[p];
        if (p >= pose.size() || pose[p].empty()) {
            continue;
        }
        for (size_t t = 0; t < part.triangleMaterials.size(); t++) {
            Batch surface;
            if (!resolve(part.triangleMaterials[t], model, &surface)) {
                continue;
            }
            size_t g = 0;
            for (; g < batches->size(); g++) {
                if ((*batches)[g].surface == surface.surface && (*batches)[g].pen == surface.pen) {
                    break;
                }
            }
            if (g == batches->size()) {
                batches->push_back(surface);
                if (groups_.size() <= used) {
                    groups_.push_back(std::vector<Vertex>());
                }
                groups_[used].clear();
                used++;
            }
            float scaleU = 0.0f;
            float scaleV = 0.0f;
            if (surface.surface >= 0) {
                const formats::Texture* texture = surfaces_[(size_t)surface.surface].texture;
                scaleU = 1.0f / (float)texture->width();
                scaleV = 1.0f / (float)texture->height();
            }
            for (int32_t k = 0; k < 3; k++) {
                const size_t corner = t * 3 + (size_t)k;
                Vertex v;
                v.pos = Vec3{0.0f, 0.0f, 0.0f};
                const int32_t index = part.triangleIndices[corner];
                if (index >= 0 && (size_t)index < pose[p].size()) {
                    const formats::Vec3f& at = pose[p][(size_t)index];
                    v.pos = Vec3{at.x, at.y, at.z};
                }
                v.uv = Vec2{part.triangleUvs[corner].x * scaleU, part.triangleUvs[corner].y * scaleV};
                v.colour = kVertexWhite;
                groups_[g].push_back(v);
            }
        }
    }
    for (size_t g = 0; g < batches->size(); g++) {
        (*batches)[g].first = (int32_t)vertices->size();
        (*batches)[g].count = (int32_t)groups_[g].size();
        vertices->insert(vertices->end(), groups_[g].begin(), groups_[g].end());
    }
}

void FallModels::draw(MdkRenderer renderer, const char* name, const Mat4& world, const char* animation,
                      int32_t frame, int32_t textureFrame, Pass pass) {
    Entry* entry = nullptr;
    for (size_t i = 0; i < entries_.size(); i++) {
        if (entries_[i].name == name) {
            entry = &entries_[i];
            break;
        }
    }
    if (entry == nullptr || entry->mesh < 0) {
        return;
    }

    if (entry->animated) {
        const std::vector<std::vector<formats::Vec3f>>* pose = nullptr;
        for (size_t a = 0; animation != nullptr && a < entry->baked.size(); a++) {
            if (entry->baked[a].name == animation) {
                const Baked& baked = entry->baked[a];
                if (frame >= 0 && (size_t)frame < baked.frames.size()) {
                    pose = &baked.frames[(size_t)frame];
                }
                break;
            }
        }
        if (pose == nullptr) {
            return;
        }
        build(entry->model, *pose, &sorted_, &entry->batches);
        if (sorted_.empty() || (int32_t)sorted_.size() > entry->capacity) {
            return;
        }
        mdk_render_update_mesh(renderer, entry->mesh, sorted_.data(), (int32_t)sorted_.size());
    }

    for (size_t b = 0; b < entry->batches.size(); b++) {
        const Batch& batch = entry->batches[b];
        MeshDraw draw;
        draw.mesh = entry->mesh;
        draw.first_vertex = batch.first;
        draw.vertex_count = batch.count;
        draw.material = batch.material;
        draw.frame = textureFrame;
        if (batch.pen != kNoPen) {
            // C# `batch.Material with { Colour = palette.Colour(batch.Pen) }`.
            const uint8_t* rgba = paletteRgba_ + batch.pen * 4;
            draw.material.colour = Colour{(float)rgba[0] / 255.0f, (float)rgba[1] / 255.0f,
                                          (float)rgba[2] / 255.0f, 1.0f};
        } else if (batch.surface >= 0 && !surfaces_[(size_t)batch.surface].frames.empty()) {
            // The frames as textures of their own (see the header).
            const std::vector<int32_t>& frames = surfaces_[(size_t)batch.surface].frames;
            // The frame within the texture's frames (an explosion's is 0-25 already).
            const int32_t count = (int32_t)frames.size();
            const int32_t f = textureFrame < 0 ? 0 : (textureFrame >= count ? count - 1 : textureFrame);
            draw.material.texture = frames[(size_t)f];
            draw.material.frame_count = 1;
            draw.frame = 0;
        }
        draw.material.pass = pass;
        memcpy(draw.world, world.m, sizeof(draw.world));
        mdk_render_draw_mesh(renderer, &draw);
    }
}

} // namespace fall
} // namespace mdk

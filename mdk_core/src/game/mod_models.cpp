// ============================================================================
// mod_models.cpp -- see mod_models.h.
// ============================================================================

#include "game/mod_models.h"

#include <stdio.h>

#include <cmath>
#include <map>

#include "formats/file_system.h"
#include "formats/glb.h"
#include "formats/png.h"
#include "game/mod_images.h"
#include "game/mod_info.h"

namespace mdk {

namespace {

const float kGamma = 2.2f;

// A node's primitives as one mesh, in MDK coordinates (ModelSwap.Merge).
SwapMesh mergeMesh(const formats::Glb::Mesh& mesh) {
    SwapMesh out;
    for (size_t i = 0; i < mesh.primitives.size(); i++) {
        const formats::Glb::Primitive& primitive = mesh.primitives[i];
        const int32_t first = (int32_t)out.vertices.size();
        const int32_t count = primitive.vertexCount();
        for (int32_t v = 0; v < count; v++) {
            out.vertices.push_back(ModelSwap::toMdk(Vec3{primitive.positions[(size_t)v * 3],
                                                         primitive.positions[(size_t)v * 3 + 1],
                                                         primitive.positions[(size_t)v * 3 + 2]}));
            out.uvs.push_back(Vec2{primitive.uvs[(size_t)v * 2], primitive.uvs[(size_t)v * 2 + 1]});
        }
        for (size_t t = 0; t < primitive.indices.size(); t++) {
            out.triangles.push_back(first + primitive.indices[t]);
        }
        const int32_t triangles = (int32_t)primitive.indices.size() / 3;
        for (int32_t t = 0; t < triangles; t++) {
            out.materials.push_back(primitive.material);
        }
    }
    return out;
}

// glTF's base colour is linear; the game's colours are sRGB.
Colour toSrgb(const float linear[4]) {
    return Colour{std::pow(linear[0], 1.0f / kGamma), std::pow(linear[1], 1.0f / kGamma),
                  std::pow(linear[2], 1.0f / kGamma), linear[3]};
}

// A material's PNG for the renderer, or false (broken: its colour instead).
bool decodePng(const std::vector<uint8_t>& png, HdImage* out) {
    formats::Png::Image image;
    std::string error;
    if (!formats::Png::decode(png.data(), (int32_t)png.size(), &image, &error)) {
        std::fprintf(stderr, "Mod model image: %s\n", error.c_str());
        return false;
    }
    // ModImages' premultiply, shared shape: straight to premultiplied.
    for (size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
        const uint32_t alpha = image.rgba[i + 3];
        for (size_t c = 0; c < 3; c++) {
            image.rgba[i + c] = (uint8_t)(((uint32_t)image.rgba[i + c] * alpha + 127) / 255);
        }
    }
    out->width = image.width;
    out->height = image.height;
    out->frames = 1;
    out->rgba = image.rgba;
    return true;
}

// A mod's models by name: the level's folder first (ModModels.Files).
void scanModels(const std::string& folder, int32_t level,
                std::map<std::string, std::string>* files) {
    const formats::FileSystem* system = formats::fileSystem();
    if (!system || !system->listDirectory) {
        return;
    }
    const std::string dirs[2] = {modPathJoin(folder, ModImages::levelFolder(level)), folder};
    for (int d = 0; d < 2; d++) {
        std::vector<std::string> entries;
        if (!system->listDirectory(system->user, dirs[d].c_str(), &entries)) {
            continue;
        }
        for (size_t i = 0; i < entries.size(); i++) {
            const std::string& name = entries[i];
            if (name.size() < 4 || modLower(name.substr(name.size() - 4)) != ".glb") {
                continue;
            }
            const std::string stem = modLower(name.substr(0, name.size() - 4));
            if (files->find(stem) == files->end()) {
                (*files)[stem] = modPathJoin(dirs[d], name);
            }
        }
    }
}

} // namespace

std::string ModelSwap::partName(const formats::Model& model, int32_t part) {
    const std::string name = model.partList[(size_t)part].nameString();
    return name.empty() ? model.name : name;
}

ModelSwap::ModelSwap(const formats::Model& model, std::vector<SwapMesh>&& meshes,
                     std::vector<bool>&& present, std::vector<SwapSurface>&& surfaces)
    : meshes_(std::move(meshes)), present_(std::move(present)), surfaces_(std::move(surfaces)) {
    for (size_t p = 0; p < model.partList.size(); p++) {
        const formats::Model::Part& part = model.partList[p];
        std::vector<Vec3> vertices;
        vertices.reserve(part.vertices.size());
        for (size_t v = 0; v < part.vertices.size(); v++) {
            vertices.push_back(Vec3{part.vertices[v].x, part.vertices[v].y, part.vertices[v].z});
        }
        fits_.push_back(PartFit(vertices, part.triangleIndices));
    }
    poses_.assign(model.partList.size() * 16, 0.0f);
    for (size_t p = 0; p < model.partList.size(); p++) {
        poses_[p * 16 + 0] = poses_[p * 16 + 5] = poses_[p * 16 + 10] = poses_[p * 16 + 15] = 1.0f;
    }
}

ModelSwap* ModelSwap::of(const formats::Glb::Scene& scene, const formats::Model& model,
                         std::vector<std::string>* unknown) {
    // Parts of the same name take the nodes of that name in order.
    std::map<std::string, std::vector<int32_t>> byName;
    for (size_t p = 0; p < model.partList.size(); p++) {
        byName[modLower(partName(model, (int32_t)p))].push_back((int32_t)p);
    }

    std::vector<SwapMesh> meshes(model.partList.size());
    std::vector<bool> present(model.partList.size(), false);
    bool any = false;
    for (size_t i = 0; i < scene.meshes.size(); i++) {
        const formats::Glb::Mesh& mesh = scene.meshes[i];
        std::map<std::string, std::vector<int32_t>>::iterator queue =
            byName.find(modLower(mesh.name));
        if (queue == byName.end() || queue->second.empty()) {
            if (unknown) {
                unknown->push_back(mesh.name);
            }
            continue;
        }
        const int32_t part = queue->second.front();
        queue->second.erase(queue->second.begin());
        meshes[(size_t)part] = mergeMesh(mesh);
        present[(size_t)part] = true;
        any = true;
    }
    if (!any) {
        return nullptr;
    }

    std::vector<SwapSurface> surfaces;
    for (size_t i = 0; i < scene.materials.size(); i++) {
        const formats::Glb::Material& material = scene.materials[i];
        SwapSurface surface;
        surface.name = material.name;
        surface.colour = toSrgb(material.colour);
        if (!material.png.empty()) {
            surface.hasImage = decodePng(material.png, &surface.image);
        }
        surfaces.push_back(surface);
    }
    return new ModelSwap(model, std::move(meshes), std::move(present), std::move(surfaces));
}

void ModelSwap::pose(const std::vector<std::vector<Vec3>>& posed, int32_t hidden) const {
    for (size_t p = 0; p < meshes_.size(); p++) {
        if (!present_[p] || (hidden & (1 << (p & 31))) != 0) {
            continue;
        }
        if (p < posed.size()) {
            fits_[p].fit(posed[p], &poses_[p * 16]);
        }
    }
}

Vec3 ModelSwap::vertex(int32_t part, int32_t index) const {
    const Vec3& v = meshes_[(size_t)part].vertices[(size_t)index];
    const float* m = &poses_[(size_t)part * 16];
    return Vec3{v.x * m[0] + v.y * m[4] + v.z * m[8] + m[12],
                v.x * m[1] + v.y * m[5] + v.z * m[9] + m[13],
                v.x * m[2] + v.y * m[6] + v.z * m[10] + m[14]};
}

ModelSwap* ModModels::make(const formats::Model& model) {
    if (!scanned_) {
        scanned_ = true;
        for (size_t m = 0; m < mods_.size(); m++) {
            // The first mod's file for a name wins: scan in order and
            // keep the earliest (scanModels already keeps a folder's
            // first, so only fill names no earlier mod claimed).
            std::map<std::string, std::string> byName;
            scanModels(modPathJoin(mods_[m], folderName()), level_, &byName);
            for (std::map<std::string, std::string>::const_iterator it = byName.begin();
                 it != byName.end(); ++it) {
                if (files_.find(it->first) == files_.end()) {
                    files_[it->first] = it->second;
                }
            }
        }
    }
    const std::map<std::string, std::string>::const_iterator found =
        files_.find(modLower(model.name));
    if (found == files_.end()) {
        return nullptr;
    }
    const std::string& path = found->second;
    const formats::FileSystem* system = formats::fileSystem();
    std::vector<uint8_t> bytes;
    if (!system || !system->readFile || !system->readFile(system->user, path.c_str(), &bytes)) {
        return nullptr;
    }
    formats::Glb::Scene scene;
    std::string error;
    if (!formats::Glb::read(bytes.data(), (int32_t)bytes.size(), &scene, &error)) {
        std::fprintf(stderr, "Mod model %s: %s\n", path.c_str(), error.c_str());
        return nullptr;
    }
    std::vector<std::string> unknown;
    ModelSwap* swap = ModelSwap::of(scene, model, &unknown);
    if (!unknown.empty()) {
        std::string joined;
        for (size_t u = 0; u < unknown.size(); u++) {
            if (u) {
                joined += ", ";
            }
            joined += unknown[u];
        }
        std::fprintf(stderr, "Mod model %s: no part %s in %s\n", path.c_str(), joined.c_str(),
                     model.name.c_str());
    }
    if (!swap) {
        return nullptr;
    }
    bool already = false;
    for (size_t f = 0; f < found_.size(); f++) {
        if (found_[f] == model.name) {
            already = true;
        }
    }
    if (!already) {
        found_.push_back(model.name);
    }
    return swap;
}

const ModelSwap* ModModels::of(const formats::Model& model) {
    std::map<const formats::Model*, std::unique_ptr<ModelSwap>>::iterator found =
        swaps_.find(&model);
    if (found != swaps_.end()) {
        return found->second.get();
    }
    ModelSwap* swap = make(model);
    swaps_[&model] = std::unique_ptr<ModelSwap>(swap);
    return swap;
}
} // namespace mdk

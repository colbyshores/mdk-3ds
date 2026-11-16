#pragma once

// ============================================================================
// mod_models.h -- the enabled mods' model replacements for a level,
// ported 1:1 from src/Mdk.Game/Mods/ModModels.cs + ModelSwap.cs.
//
// Per model, the first mod's models/LEVELn/NAME.glb, else its
// models/NAME.glb (names ignore case). A node's mesh replaces the part
// of its name (parts of the same name take the nodes in order); a node
// naming no part is listed as unknown, and a broken file keeps the
// original model (and says why, on stderr). A replaced part follows the
// original part's animation through a least-squares fit (part_fit.h).
//
// A replacement material: its image (premultiplied, for the renderer),
// else a material of its name in the model's own materials, else its
// glTF base colour (linear in the file, sRGB in the game).
// ============================================================================

#include <stdint.h>

#include <map>

#include <memory>
#include <string>
#include <vector>

#include "formats/glb.h"
#include "formats/model.h"
#include "game/hd_key.h"
#include "game/part_fit.h"
#include "render/render_types.h"

namespace mdk {


// A replacement part in the model's space (rest pose): triangles of
// vertices with UVs, a material index per triangle.
struct SwapMesh {
    std::vector<Vec3> vertices;
    std::vector<Vec2> uvs;
    std::vector<int32_t> triangles;
    std::vector<int32_t> materials;
};

// A replacement material: its name, colour (sRGB) and image, if any.
struct SwapSurface {
    std::string name;
    Colour colour = {1.0f, 1.0f, 1.0f, 1.0f};
    bool hasImage = false;
    HdImage image;
};

// One model's replacement: per original part its mesh (null: kept),
// its surfaces, and the pose fits.
class ModelSwap {
public:
    // A glTF point (Y up) in MDK coordinates (Z up), and back.
    static Vec3 toMdk(const Vec3& gltf) { return Vec3{gltf.x, -gltf.z, gltf.y}; }
    static Vec3 toGltf(const Vec3& mdk) { return Vec3{mdk.x, mdk.z, -mdk.y}; }

    // A part's name in glTF files: its own, or the model's for a model
    // of one unnamed part.
    static std::string partName(const formats::Model& model, int32_t part);

    // The replacement a scene makes of a model; null when no node names
    // a part. Nodes naming no part are appended to `unknown`.
    static ModelSwap* of(const formats::Glb::Scene& scene, const formats::Model& model,
                         std::vector<std::string>* unknown);

    // Fits each replaced, shown part to its pose (a draw).
    void pose(const std::vector<std::vector<Vec3>>& posed, int32_t hidden) const;

    // A replaced part's vertex in the pose last fitted.
    Vec3 vertex(int32_t part, int32_t index) const;

    // A part's replacement mesh, or null when the part is kept.
    const SwapMesh* part(int32_t index) const {
        return present_[(size_t)index] ? &meshes_[(size_t)index] : nullptr;
    }
    int32_t partCount() const { return (int32_t)meshes_.size(); }
    const std::vector<SwapSurface>& surfaces() const { return surfaces_; }

private:
    ModelSwap(const formats::Model& model, std::vector<SwapMesh>&& meshes,
              std::vector<bool>&& present, std::vector<SwapSurface>&& surfaces);

    std::vector<SwapMesh> meshes_;
    std::vector<bool> present_;
    std::vector<SwapSurface> surfaces_;
    std::vector<PartFit> fits_;
    mutable std::vector<float> poses_;  // 16 floats per part (fit scratch)
};

// The enabled mods' model replacements for one level. The oracle loads
// them at the level's load from its model list; this port parses models
// lazily (the runtime's model cache), so a model's swap is made the
// first time it is asked for -- the same first-mod-wins result, with a
// broken file keeping the original from that draw on.
class ModModels {
public:
    static const char* folderName() { return "models"; }

    ModModels() = default;
    ModModels(std::vector<std::string> mods, int32_t level)
        : mods_(std::move(mods)), level_(level) {}

    // A model's replacement, or null (made on first ask).
    const ModelSwap* of(const formats::Model& model);

    // The models replaced so far, by name.
    const std::vector<std::string>& found() const { return found_; }

private:
    // This model's swap, or null (also null for "asked, none").
    ModelSwap* make(const formats::Model& model);

    std::vector<std::string> mods_;
    int32_t level_ = 0;
    bool scanned_ = false;
    std::map<std::string, std::string> files_;
    std::map<const formats::Model*, std::unique_ptr<ModelSwap>> swaps_;
    std::vector<std::string> found_;
};

} // namespace mdk

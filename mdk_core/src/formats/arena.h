#pragma once

// ============================================================================
// arena.h — one arena of a level (HMO_n in LEVELnO.MTO).
//
// A 1:1 port of src/Mdk.Formats/Arena.cs.
//
// AN ARENA IS THE WORLD. It holds the geometry (the vertices, the triangles
// and their surfaces), the BSP tree that decides what is visible, the
// material list that names each surface, and the arena's own 112 palette
// colours.
//
// THE BSP NODE. A triangle lies on its node's plane, `n·p + d`, and the
// node splits the space into a front list (the faces looking at the
// positive side) and a back list. A child of -1 is an empty leaf. The
// distance function is what the game's collision and its visibility walk
// both use.
//
// THE NODE IS 44 BYTES AND ONLY 28 ARE READ. The rest is two bit-set
// offsets and two run-time object lists, which the game reads through
// other paths. The reader SKIPS to the next node rather than reading
// through, because the fields after the eighth are not the same in the
// 1996 demo's 36-byte node.
//
// THE MATERIAL INDEX. A triangle's material is an index into the material
// list, or a NEGATIVE number for a flat palette colour: -1 is palette
// entry 1, -2 is entry 2, and so on. The game's PEN_n materials are
// resolved this way.
//
// The MODELS, ANIMATIONS and SOUNDS are parsed in a separate translation
// unit, because they need the model formats, and the world does not.
// ============================================================================

#include <stdint.h>

#include <map>
#include <string>
#include <vector>

#include "formats/model.h"
#include "formats/model_animation.h"
#include "formats/texture_archive.h"
#include "formats/vector_types.h"

namespace mdk {
namespace formats {

// A BSP node.
struct BspNode {
    float normal[3] = {0.0f, 0.0f, 0.0f};
    float d = 0.0f;
    int32_t negative = -1;   // the child on the negative side, or -1
    int32_t positive = -1;   // the child on the positive side, or -1
    int32_t frontFirst = 0;
    int32_t frontCount = 0;
    int32_t backFirst = 0;
    int32_t backCount = 0;

    // The signed distance from a point to the node's plane.
    float distance(const Vec3f& point) const {
        return normal[0] * point.x + normal[1] * point.y + normal[2] * point.z + d;
    }
};

// An arena of a level.
class Arena {
public:
    // The colours an arena brings to the palette (indices 64-175).
    static const int32_t kPaletteColors = 112;

    // The retail BSP node's size.
    static const int32_t kBspNodeSize = 44;

    // The retail material name's length.
    static const int32_t kMaterialNameLength = 10;

    // The 1996 demo's BSP node: f32 plane[4], then ten s16. Only the first
    // six of them are the retail fields.
    static const int32_t kBetaBspNodeSize = 36;

    // The 1996 demo's material name's length.
    static const int32_t kBetaMaterialNameLength = 16;

    ~Arena();

    // How many Arena objects are alive right now.
    //
    // It exists so OWNERSHIP is testable, and nothing else reads it. The
    // contract in mto.h is that an Mto frees the arenas it parsed and not the
    // ones it was given; a test can only prove that by watching the objects
    // die. tests/formats/format_tests.cpp is that test. It is a counter, not
    // a registry: it says how many, never which.
    //
    // Arenas are constructed and destroyed in level load and teardown, which
    // is main-thread work, so the counter needs no atomic; see arena.cpp.
    static int64_t liveCount();

    // An arena of an MTO. `offset` points to its size field.
    static Arena* parse(const std::string& name, const uint8_t* bytes, int32_t byteCount, int32_t offset);

    // A world section alone (a corridor in LEVELnO.SNI).
    static Arena* parseWorld(const std::string& name, const uint8_t* bytes, int32_t byteCount,
                             int32_t offset);

    // A world section of the 1996 demo (ARENAS/name.BSP). Its material
    // names, in lower case, are made upper case.
    static Arena* parseBetaWorld(const std::string& name, const uint8_t* bytes, int32_t byteCount);

    const std::string& name() const { return name_; }

    // The arena's palette colours, RGB, three bytes each. Empty for a
    // corridor.
    const uint8_t* paletteRgb() const { return paletteRgb_.data(); }
    int32_t paletteRgbBytes() const { return (int32_t)paletteRgb_.size(); }

    const TextureArchive& textures() const { return textures_; }

    // The setters the 1996 demo's loader needs: it reads an arena's
    // textures and palette from files of their own, beside the world.
    void setTextures(const TextureArchive& textures) { textures_ = textures; }
    void setPaletteRgb(const uint8_t* rgb, int32_t count) {
        paletteRgb_.assign(rgb, rgb + count);
    }

    // The texture names, or PEN_n for the flat palette colours.
    const std::vector<std::string>& materials() const { return materials_; }

    // The vertices, three floats each. MDK coordinates, Z up.
    const std::vector<Vec3f>& vertices() const { return vertices_; }

    // Three vertex indices per triangle.
    const std::vector<int32_t>& triangleIndices() const { return triangleIndices_; }

    // Per triangle: the index into materials(), or -n for the palette
    // colour n.
    const std::vector<int32_t>& triangleMaterials() const { return triangleMaterials_; }

    // Three texture coordinates per triangle, in texels.
    const std::vector<Vec2f>& triangleUvs() const { return triangleUvs_; }

    // Per triangle: the group in bits 24-31 and the outline bits in 20-23.
    const std::vector<uint32_t>& triangleFlags() const { return triangleFlags_; }
    std::vector<uint32_t>& mutableTriangleFlags() { return triangleFlags_; }

    // The group state bits, as the C# TriangleGroups.State: the two triangle
    // bits group_set_state moves. A group EXISTS when a triangle's top byte is
    // its number (group 0 is the rest of the arena and never changes).
    static const uint32_t kGroupHidden = 0x10;
    static const uint32_t kGroupNotSolid = 0x20;
    // The flag the arena activation turns into hidden and not solid.
    static const uint32_t kGroupDisabled = 0x2;

    bool hasGroup(int32_t group) const;
    // The group's stored state (the C# Group.State), 0 when it was never set.
    uint32_t groupState(int32_t group) const;
    void setGroupState(int32_t group, uint32_t state);
    void setGroupMaterial(int32_t group, int32_t material);
    // The group's stored material (the C# Group.Material, an int?): false when
    // the group has none. A negative material is a VALID palette colour
    // (-1 is entry 1), so absence cannot be a sentinel value.
    bool groupMaterial(int32_t group, int32_t* out) const;

    // The conversion the C# TriangleGroups registration makes when an arena is
    // registered for play: a retail triangle flagged Disabled is hidden and not
    // solid (LEVEL8 GUNT_2's invisible diagonal wall). An arena with a clip
    // flag (the 1996 demo) keeps its flags, as the C#'s Disable returns then.
    void applyDisabledTriangles();

    // The BSP nodes. Node 0 is the root, and a child's index is higher than
    // its parent's.
    const std::vector<BspNode>& nodes() const { return nodes_; }

    // The flag of the triangles that only stop Kurt: they are not drawn, and
    // the scripts' rays pass through them. Zero for none.
    uint32_t clipFlag() const { return clipFlag_; }
    void setClipFlag(uint32_t flag) { clipFlag_ = flag; }

    // The arena's own models and animations, parsed from the arena block.
    // The scripts' FindModel looks a type up here when it is not a global
    // CMI model (e.g. HMO_1's XH1_DOOR). Null when there is no such model.
    const Model* model(const std::string& name) const;
    const ModelAnimation* animation(const std::string& name) const;

    // How many models and animations the arena has, and a model's name by
    // index (for the load-time checks).
    int32_t modelCount() const { return (int32_t)models_.size(); }
    int32_t animationCount() const { return (int32_t)animations_.size(); }
    const std::string& modelName(int32_t index) const { return models_[index].first; }

    int32_t triangleCount() const { return (int32_t)triangleMaterials_.size(); }

private:
    Arena();

    // AN ARENA IS NOT COPYABLE AND NOT MOVABLE.
    //
    // An arena is made by one of the three parse functions above and lives
    // behind the pointer they return: the MTO's cache, a level's corridor list
    // and the demo's vector all hold `Arena*`, never an `Arena`. So copying
    // one is not merely unused, it is wrong twice over. liveCount() is the
    // instrument the ownership contract in mto.h is pinned with, and the
    // implicit copy operations do not pass through the constructor and
    // destructor that maintain it: copying a live arena makes the counter say
    // one object while two exist, and deleting the original then says zero
    // while the copy is still being drawn. A move is no better — the source's
    // destructor decrements for an object the destination is now using.
    //
    // With these gone the counter's invariant is a compile-time property
    // instead of a comment, and the copy that would have broken it does not
    // build. The pin is in tests/formats/format_tests.cpp.
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&&) = delete;
    Arena& operator=(Arena&&) = delete;

    // The world section: the materials, the BSP tree, the triangles and the
    // vertices.
    void parseWorldSection(const uint8_t* bytes, int32_t byteCount, int32_t baseOffset,
                           int32_t nameLength, int32_t nodeSize);

    // The model, animation and sound directories (arena_parse_models). The
    // sounds are read past, not kept: the audio path is elsewhere.
    void parseModels(const uint8_t* bytes, int32_t byteCount, int32_t baseOffset);

    std::string name_;
    std::vector<uint8_t> paletteRgb_;
    TextureArchive textures_;
    std::vector<std::string> materials_;
    std::vector<Vec3f> vertices_;
    std::vector<int32_t> triangleIndices_;
    std::vector<int32_t> triangleMaterials_;
    std::vector<Vec2f> triangleUvs_;
    std::vector<uint32_t> triangleFlags_;
    std::map<int32_t, int32_t> groupMaterials_;
    std::map<int32_t, uint32_t> groupStates_;
    std::vector<BspNode> nodes_;
    uint32_t clipFlag_ = 0;
    std::vector<std::pair<std::string, Model>> models_;
    std::vector<std::pair<std::string, ModelAnimation>> animations_;
};

} // namespace formats
} // namespace mdk

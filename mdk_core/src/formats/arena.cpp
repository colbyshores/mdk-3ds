// ============================================================================
// arena.cpp — see arena.h.
// ============================================================================

#include "formats/arena.h"

#include <string.h>

#include "formats/bin_reader.h"

namespace mdk {
namespace formats {

namespace {

// Names of 10 bytes: an odd count pads to 4.
const int32_t kAlignment = 4;

// The Arena objects alive right now, behind Arena::liveCount(). A plain
// counter, not an atomic: arenas are constructed and destroyed in level load
// and teardown, which is main-thread work (the tree creates no worker thread
// today), and the only reader is a test. If a worker ever parses an arena,
// this becomes an atomic and AGENTS.md P4's handoff applies to it.
int64_t g_liveArenas = 0;

} // namespace

Arena::Arena() {
    ++g_liveArenas;
}

Arena::~Arena() {
    --g_liveArenas;
    // Nothing else to release: every buffer is a member. The destructor exists
    // so the class can be held by pointer and freed with delete, which is
    // how the arenas are handed out.
}

int64_t Arena::liveCount() {
    return g_liveArenas;
}

Arena* Arena::parse(const std::string& name, const uint8_t* bytes, int32_t byteCount, int32_t offset) {
    if (!bytes || offset < 0 || offset + 4 > byteCount) {
        return nullptr;
    }
    Arena* arena = new Arena();
    arena->name_ = name;

    // The game loads the bytes AFTER the size field, and the offsets are
    // relative to that buffer. So every offset below is `offset + 4` plus
    // the value read at it.
    const int32_t buf = offset + 4;
    if (buf + 12 > byteCount) {
        delete arena;
        return nullptr;
    }
    const int32_t models = buf + (int32_t)binU32(bytes, buf);
    const int32_t palette = buf + (int32_t)binU32(bytes, buf + 4);
    const int32_t world = buf + (int32_t)binU32(bytes, buf + 8);

    if (palette >= 0 && palette + kPaletteColors * 3 <= byteCount) {
        arena->paletteRgb_.assign(bytes + palette, bytes + palette + kPaletteColors * 3);
    }
    arena->textures_ = TextureArchive::parse(bytes, byteCount, buf + 0x10);
    arena->parseWorldSection(bytes, byteCount, world, kMaterialNameLength, kBspNodeSize);

    // The models, the animations and the sounds. The scripts' FindModel
    // needs the arena's own models (e.g. HMO_1's XH1_DOOR), so they are
    // parsed here; the sounds are read past, as the audio path keeps them
    // elsewhere.
    arena->parseModels(bytes, byteCount, models + 4);

    return arena;
}

Arena* Arena::parseWorld(const std::string& name, const uint8_t* bytes, int32_t byteCount,
                         int32_t offset) {
    if (!bytes) {
        return nullptr;
    }
    Arena* arena = new Arena();
    arena->name_ = name;
    arena->parseWorldSection(bytes, byteCount, offset, kMaterialNameLength, kBspNodeSize);
    return arena;
}

Arena* Arena::parseBetaWorld(const std::string& name, const uint8_t* bytes, int32_t byteCount) {
    if (!bytes) {
        return nullptr;
    }
    Arena* arena = new Arena();
    arena->name_ = name;
    arena->parseWorldSection(bytes, byteCount, 0, kBetaMaterialNameLength, kBetaBspNodeSize);

    // The 1996 demo's material names are in lower case, and the game looks
    // them up in upper case. The conversion is done HERE, at parse time,
    // because the C# does the same and doing it later would change what the
    // game reads.
    for (size_t i = 0; i < arena->materials_.size(); i++) {
        std::string& material = arena->materials_[i];
        for (size_t c = 0; c < material.size(); c++) {
            if (material[c] >= 'a' && material[c] <= 'z') {
                material[c] = (char)(material[c] - 'a' + 'A');
            }
        }
    }
    return arena;
}

void Arena::parseWorldSection(const uint8_t* bytes, int32_t byteCount, int32_t baseOffset,
                               int32_t nameLength, int32_t nodeSize) {
    if (!bytes || baseOffset < 0 || baseOffset + 4 > byteCount) {
        return;
    }
    BinReader reader(bytes, byteCount, baseOffset);

    const uint32_t materialCount = reader.u32();
    for (uint32_t i = 0; i < materialCount; i++) {
        char name[64];
        reader.name(name, nameLength);
        materials_.push_back(name);
    }

    // Names of 10 bytes: an odd count pads to 4.
    if ((materialCount * (uint32_t)nameLength) % (uint32_t)kAlignment != 0) {
        reader.skip(2);
    }

    const uint32_t nodeCount = reader.u32();
    nodes_.resize(nodeCount);
    for (uint32_t i = 0; i < nodeCount; i++) {
        // The reader is moved to the next node rather than read through:
        // the fields after the eighth are not the same in the two layouts,
        // and the node's SIZE is what separates them.
        const int32_t next = reader.pos() + nodeSize;
        BspNode& node = nodes_[i];
        reader.vec3(node.normal);
        node.d = reader.f32();
        node.negative = reader.s16();
        node.positive = reader.s16();
        node.frontCount = (int32_t)reader.u16();
        node.frontFirst = reader.s16();
        node.backCount = (int32_t)reader.u16();
        node.backFirst = reader.s16();
        reader.setPos(next);
    }

    const uint32_t triangleCount = reader.u32();
    triangleIndices_.resize((size_t)triangleCount * 3);
    triangleMaterials_.resize(triangleCount);
    triangleUvs_.resize((size_t)triangleCount * 3);
    triangleFlags_.resize(triangleCount);
    for (uint32_t i = 0; i < triangleCount; i++) {
        triangleIndices_[i * 3 + 0] = reader.s16();
        triangleIndices_[i * 3 + 1] = reader.s16();
        triangleIndices_[i * 3 + 2] = reader.s16();
        triangleMaterials_[i] = reader.s16();
        for (int32_t k = 0; k < 3; k++) {
            triangleUvs_[i * 3 + k].x = reader.f32();
            triangleUvs_[i * 3 + k].y = reader.f32();
        }
        triangleFlags_[i] = reader.u32();
    }

    const uint32_t vertexCount = reader.u32();
    vertices_.resize(vertexCount);
    for (uint32_t i = 0; i < vertexCount; i++) {
        reader.vec3(&vertices_[i].x);
    }
}

void Arena::parseModels(const uint8_t* bytes, int32_t byteCount, int32_t baseOffset) {
    if (!bytes || baseOffset < 0 || baseOffset + 12 > byteCount) {
        return;
    }
    BinReader reader(bytes, byteCount, baseOffset);

    const uint32_t animationCount = reader.u32();
    const uint32_t modelCount = reader.u32();
    const uint32_t soundCount = reader.u32();

    for (uint32_t i = 0; i < animationCount; i++) {
        char name[16];
        reader.name(name, 8);
        const int32_t offset = baseOffset + (int32_t)reader.u32();
        ModelAnimation animation;
        if (ModelAnimation::parse(&animation, name, bytes, byteCount, offset)) {
            animations_.push_back(std::make_pair(std::string(name), animation));
        }
    }

    for (uint32_t i = 0; i < modelCount; i++) {
        char name[16];
        reader.name(name, 8);
        const int32_t offset = baseOffset + (int32_t)reader.u32();
        Model model;
        if (Model::parse(&model, name, bytes, byteCount, offset)) {
            models_.push_back(std::make_pair(std::string(name), model));
        }
    }

    // The sounds are read past: the audio path keeps them, not the arena.
    (void)soundCount;
}

const Model* Arena::model(const std::string& name) const {
    for (size_t i = 0; i < models_.size(); i++) {
        if (models_[i].first == name) {
            return &models_[i].second;
        }
    }
    return nullptr;
}

const ModelAnimation* Arena::animation(const std::string& name) const {
    for (size_t i = 0; i < animations_.size(); i++) {
        if (animations_[i].first == name) {
            return &animations_[i].second;
        }
    }
    return nullptr;
}

bool Arena::hasGroup(int32_t group) const {
    for (size_t i = 0; i < triangleFlags_.size(); i++) {
        if ((int32_t)(triangleFlags_[i] >> 24) == group) {
            return true;
        }
    }
    return false;
}

uint32_t Arena::groupState(int32_t group) const {
    std::map<int32_t, uint32_t>::const_iterator it = groupStates_.find(group);
    return it == groupStates_.end() ? 0u : it->second;
}

void Arena::setGroupState(int32_t group, uint32_t state) {
    groupStates_[group] = state;
}

void Arena::setGroupMaterial(int32_t group, int32_t material) {
    // C# TriangleGroups.SetMaterial: the group must exist and not be group 0.
    if (group == 0 || !hasGroup(group)) {
        return;
    }
    groupMaterials_[group] = material;
}

bool Arena::groupMaterial(int32_t group, int32_t* out) const {
    std::map<int32_t, int32_t>::const_iterator it = groupMaterials_.find(group);
    if (it == groupMaterials_.end()) {
        return false;
    }
    *out = it->second;
    return true;
}

void Arena::applyDisabledTriangles() {
    if (clipFlag_ != 0) {
        return;
    }
    for (size_t i = 0; i < triangleFlags_.size(); i++) {
        if ((triangleFlags_[i] & kGroupDisabled) != 0) {
            triangleFlags_[i] |= kGroupHidden | kGroupNotSolid;
        }
    }
}

} // namespace formats
} // namespace mdk

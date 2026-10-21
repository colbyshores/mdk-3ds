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

} // namespace

Arena::~Arena() {
    // Nothing to release: every buffer is a member. The destructor exists
    // so the class can be held by pointer and freed with delete, which is
    // how the arenas are handed out.
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

    // The models, the animations and the sounds. They need the model
    // formats, which are a separate translation unit; until those land the
    // arena still parses its world, which is what the renderer draws.
    (void)models;

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

} // namespace formats
} // namespace mdk

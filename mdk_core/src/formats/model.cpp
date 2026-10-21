// ============================================================================
// model.cpp — see model.h.
// ============================================================================

#include "formats/model.h"

#include "formats/bin_reader.h"

namespace mdk {
namespace formats {

namespace {

// The bytes left to read, or -1 when the reader is already past the end of
// the buffer.
//
// The C#'s reader throws IndexOutOfRangeException when a read runs off the
// end. Here the reader does not know the buffer's length, so the parsers
// check BEFORE they read, using the counts the file itself gives: a part
// with 40 vertices needs 480 bytes, and if they are not there the parse
// fails, which is what the C#'s exception did.
int32_t leftOf(const BinReader& reader) {
    if (reader.pos() < 0 || reader.pos() > reader.length()) {
        return -1;
    }
    return reader.length() - reader.pos();
}

// Are there at least `count` bytes left to read?
bool fits(const BinReader& reader, int32_t count) {
    const int32_t left = leftOf(reader);
    return left >= 0 && left >= count;
}

Vec3f readVec3(BinReader* reader) {
    float v[3];
    reader->vec3(v);
    return Vec3f{v[0], v[1], v[2]};
}

// The model's box, as the file has it: min x, max x, min y, max y, min z,
// max z. The pairs are INTERLEAVED, which is the format's own order.
Box readBounds(BinReader* reader) {
    float v[6];
    for (int32_t i = 0; i < 6; i++) {
        v[i] = reader->f32();
    }
    Box box;
    box.min = Vec3f{v[0], v[2], v[4]};
    box.max = Vec3f{v[1], v[3], v[5]};
    return box;
}

// One part of the model.
bool parsePart(Model::Part* part, BinReader* reader, bool named) {
    if (named) {
        // The name and the pivot.
        if (!fits(*reader, 24)) {
            return false;
        }
        reader->name(part->name, 12);
        part->pivot = readVec3(reader);
    }

    if (!fits(*reader, 4)) {
        return false;
    }
    const uint32_t vertexCount = reader->u32();
    int32_t left = leftOf(*reader);
    if (left < 0 || vertexCount > (uint32_t)(left / 12)) {
        return false;
    }
    part->vertices.reserve(vertexCount);
    for (uint32_t v = 0; v < vertexCount; v++) {
        part->vertices.push_back(readVec3(reader));
    }

    if (!fits(*reader, 4)) {
        return false;
    }
    const uint32_t triangleCount = reader->u32();
    left = leftOf(*reader);
    // A triangle is three s16 indices, an s16 material, three vec2 texture
    // coordinates and the four skipped bytes: 36 bytes.
    if (left < 0 || triangleCount > (uint32_t)(left / 36)) {
        return false;
    }
    part->triangleIndices.reserve(triangleCount * 3);
    part->triangleMaterials.reserve(triangleCount);
    part->triangleUvs.reserve(triangleCount * 3);
    for (uint32_t t = 0; t < triangleCount; t++) {
        part->triangleIndices.push_back(reader->s16());
        part->triangleIndices.push_back(reader->s16());
        part->triangleIndices.push_back(reader->s16());
        part->triangleMaterials.push_back(reader->s16());
        for (int32_t k = 0; k < 3; k++) {
            const float u = reader->f32();
            const float v = reader->f32();
            part->triangleUvs.push_back(Vec2f{u, v});
        }
        // The four bytes the game does not read.
        reader->skip(4);
    }

    if (named) {
        if (!fits(*reader, 24)) {
            return false;
        }
        part->bounds = readBounds(reader);
    }

    return true;
}

} // namespace

std::string Model::Material::nameString() const {
    // The GAME keeps ten characters of the name; the file has sixteen.
    std::string text(name);
    if (text.size() > (size_t)kMaterialNameLength) {
        text.resize((size_t)kMaterialNameLength);
    }
    return text;
}

std::string Model::Part::nameString() const {
    return std::string(name);
}

bool Model::parse(Model* out, const std::string& name, const uint8_t* bytes, int32_t byteCount,
                  int32_t offset, Header header, Parts parts) {
    if (!out || !bytes || offset < 0 || offset > byteCount) {
        return false;
    }

    Model model;
    model.name = name;
    BinReader reader(bytes, byteCount, offset);

    // The flags word comes FIRST, and only when the caller says it is
    // there. It says whether the parts are named.
    bool named = false;
    if (header == Header::Flags) {
        if (!fits(reader, 4)) {
            return false;
        }
        named = reader.u32() != 0;
    } else {
        named = (parts == Parts::Named);
    }

    if (!fits(reader, 4)) {
        return false;
    }
    const uint32_t materialCount = reader.u32();
    int32_t left = leftOf(reader);
    if (left < 0 || materialCount > (uint32_t)(left / 16)) {
        return false;
    }
    model.materials.reserve(materialCount);
    for (uint32_t i = 0; i < materialCount; i++) {
        Material material;
        reader.name(material.name, 16);
        model.materials.push_back(material);
    }

    uint32_t partCount = 1;
    if (named) {
        if (!fits(reader, 4)) {
            return false;
        }
        partCount = reader.u32();
    }
    for (uint32_t i = 0; i < partCount; i++) {
        Part part;
        if (!parsePart(&part, &reader, named)) {
            return false;
        }
        model.partList.push_back(part);
    }

    if (!fits(reader, 24)) {
        return false;
    }
    model.bounds = readBounds(&reader);

    if (!fits(reader, 4)) {
        return false;
    }
    const uint32_t referenceCount = reader.u32();
    left = leftOf(reader);
    if (left < 0 || referenceCount > (uint32_t)(left / 12)) {
        return false;
    }
    model.referencePoints.reserve(referenceCount);
    for (uint32_t i = 0; i < referenceCount; i++) {
        model.referencePoints.push_back(readVec3(&reader));
    }

    *out = model;
    return true;
}

std::vector<std::vector<Vec3f>> Model::restPose() const {
    std::vector<std::vector<Vec3f>> pose;
    pose.reserve(partList.size());
    for (size_t i = 0; i < partList.size(); i++) {
        pose.push_back(partList[i].vertices);
    }
    return pose;
}

} // namespace formats
} // namespace mdk

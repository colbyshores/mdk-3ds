// ============================================================================
// glb.cpp — see glb.h for the subset and the matrix convention.
//
// The JSON parser and writer (glb_json.inc) are the smallest ones that read
// and write a glTF's JSON chunk. They are here rather than in a shared
// library because the glTF subset is the only JSON in the project, and a
// general JSON library would be a dependency the 3DS build would have to carry.
//
// A NUMBER KEEPS ITS SOURCE TEXT. The C# reads a number with GetInt32 or
// GetSingle, which look at the text and not at a parsed double: "1" is an
// int and "1.0" is not, and a float written as "0.1" is the float nearest
// 0.1 rather than the double nearest 0.1. Keeping the text is what makes
// the two agree.
//
// ONE TRANSLATION UNIT IN THREE FILES (split 2026-11-25 so it reads in pieces):
// the JSON value, parser and writer are in glb_json.inc and the glTF reader in
// glb_reader.inc, both #included inside this file's anonymous namespace at the
// point they used to stand. They are not headers -- no guards, no includes --
// and the split moved code and nothing else (the object code is unchanged).
// ============================================================================

#include "formats/glb.h"

#include <stdio.h>
#include <string.h>

#include <charconv>

#include "formats/bin_reader.h"

namespace mdk {
namespace formats {

namespace {

const uint32_t kMagic = 0x46546C67;
const uint32_t kVersion = 2;
const uint32_t kJsonChunk = 0x4E4F534A;
const uint32_t kBinChunk = 0x004E4942;
const int32_t kHeaderSize = 12;
const int32_t kChunkHeader = 8;
const int32_t kAlignment = 4;

// The accessor component types.
const int32_t kUnsignedByte = 5121;
const int32_t kUnsignedShort = 5123;
const int32_t kUnsignedInt = 5125;
const int32_t kFloat = 5126;

const int32_t kTriangles = 4;
const int32_t kArrayBuffer = 34962;
const int32_t kElementArrayBuffer = 34963;
const char kPngType[] = "image/png";

void setError(std::string* outError, const char* message) {
    if (outError) {
        *outError = message;
    }
}

#include "glb_json.inc"

// === Matrices ===================================================================

// A 4x4 matrix in System.Numerics' layout: m[row][column], and the vectors
// are ROW vectors, so a point is transformed as v * M.
struct Matrix {
    float m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
};

Matrix multiply(const Matrix& a, const Matrix& b) {
    Matrix result;
    for (int32_t r = 0; r < 4; r++) {
        for (int32_t c = 0; c < 4; c++) {
            float sum = 0.0f;
            for (int32_t k = 0; k < 4; k++) {
                sum += a.m[r][k] * b.m[k][c];
            }

            result.m[r][c] = sum;
        }
    }

    return result;
}

// A point transformed by a matrix: v * M.
void transform(const float in[3], const Matrix& matrix, float out[3]) {
    for (int32_t c = 0; c < 3; c++) {
        out[c] = in[0] * matrix.m[0][c] + in[1] * matrix.m[1][c] + in[2] * matrix.m[2][c] + matrix.m[3][c];
    }
}

Matrix createScale(float x, float y, float z) {
    Matrix result;
    result.m[0][0] = x;
    result.m[1][1] = y;
    result.m[2][2] = z;
    return result;
}

Matrix createTranslation(float x, float y, float z) {
    Matrix result;
    result.m[3][0] = x;
    result.m[3][1] = y;
    result.m[3][2] = z;
    return result;
}

// A rotation from a quaternion, in System.Numerics' layout.
//
// The matrix is the TRANSPOSE of the usual column-vector rotation matrix,
// because System.Numerics transforms the points as row vectors.
Matrix createFromQuaternion(float x, float y, float z, float w) {
    const float xx = x * x;
    const float yy = y * y;
    const float zz = z * z;
    const float xy = x * y;
    const float zw = z * w;
    const float zx = z * x;
    const float yw = y * w;
    const float yz = y * z;
    const float xw = x * w;

    Matrix result;
    result.m[0][0] = 1.0f - 2.0f * (yy + zz);
    result.m[0][1] = 2.0f * (xy + zw);
    result.m[0][2] = 2.0f * (zx - yw);
    result.m[1][0] = 2.0f * (xy - zw);
    result.m[1][1] = 1.0f - 2.0f * (zz + xx);
    result.m[1][2] = 2.0f * (yz + xw);
    result.m[2][0] = 2.0f * (zx + yw);
    result.m[2][1] = 2.0f * (yz - xw);
    result.m[2][2] = 1.0f - 2.0f * (xx + yy);
    return result;
}

#include "glb_reader.inc"

// The header and both chunks, the JSON padded with spaces, the BIN with
// zeros.
bool pack(const std::string& json, const std::vector<uint8_t>& bin, std::vector<uint8_t>* out) {
    const int32_t jsonLength = ((int32_t)json.size() + kAlignment - 1) / kAlignment * kAlignment;
    const int32_t total = kHeaderSize + kChunkHeader + jsonLength +
                          (bin.empty() ? 0 : kChunkHeader + (int32_t)bin.size());
    std::vector<uint8_t> file((size_t)total, 0);

    const uint32_t magic = kMagic;
    const uint32_t version = kVersion;
    const uint32_t length = (uint32_t)total;
    const uint32_t jsonChunkLength = (uint32_t)jsonLength;
    const uint32_t jsonChunkType = kJsonChunk;
    memcpy(file.data(), &magic, 4);
    memcpy(file.data() + 4, &version, 4);
    memcpy(file.data() + 8, &length, 4);
    memcpy(file.data() + kHeaderSize, &jsonChunkLength, 4);
    memcpy(file.data() + kHeaderSize + 4, &jsonChunkType, 4);

    // The JSON is padded with spaces, so that the chunk stays 4-aligned.
    memset(file.data() + kHeaderSize + kChunkHeader, ' ', (size_t)jsonLength);
    memcpy(file.data() + kHeaderSize + kChunkHeader, json.data(), json.size());

    if (bin.empty()) {
        *out = file;
        return true;
    }

    const int32_t at = kHeaderSize + kChunkHeader + jsonLength;
    const uint32_t binLength = (uint32_t)bin.size();
    const uint32_t binType = kBinChunk;
    memcpy(file.data() + at, &binLength, 4);
    memcpy(file.data() + at + 4, &binType, 4);
    memcpy(file.data() + at + kChunkHeader, bin.data(), bin.size());

    *out = file;
    return true;
}

} // namespace

bool Glb::read(const uint8_t* bytes, int32_t byteCount, Scene* outScene, std::string* outError) {
    if (!outScene) {
        return false;
    }

    if (byteCount < kHeaderSize) {
        setError(outError, "Not a glTF binary file");
        return false;
    }

    uint32_t magic = 0;
    memcpy(&magic, bytes, 4);
    if (magic != kMagic) {
        setError(outError, "Not a glTF binary file");
        return false;
    }

    uint32_t version = 0;
    memcpy(&version, bytes + 4, 4);
    if (version != kVersion) {
        setError(outError, "Not glTF 2.0");
        return false;
    }

    // The JSON chunk and the BIN chunk (empty when absent).
    const char* json = nullptr;
    int32_t jsonSize = 0;
    const uint8_t* bin = nullptr;
    int32_t binSize = 0;

    int32_t at = kHeaderSize;
    while (at + kChunkHeader <= byteCount) {
        uint32_t length = 0;
        uint32_t type = 0;
        memcpy(&length, bytes + at, 4);
        memcpy(&type, bytes + at + 4, 4);
        if ((int64_t)at + kChunkHeader + (int64_t)length > byteCount) {
            setError(outError, "glTF chunk past the end");
            return false;
        }

        if (type == kJsonChunk) {
            json = (const char*)(bytes + at + kChunkHeader);
            jsonSize = (int32_t)length;
        } else if (type == kBinChunk) {
            bin = bytes + at + kChunkHeader;
            binSize = (int32_t)length;
        }

        at += kChunkHeader + (int32_t)length;
    }

    if (!json) {
        setError(outError, "glTF without JSON");
        return false;
    }

    JsonValue root;
    JsonParser parser(json, jsonSize);
    if (!parser.parse(&root)) {
        setError(outError, "glTF JSON is malformed");
        return false;
    }

    const JsonValue* asset = root.member("asset");
    const JsonValue* versionValue = asset ? asset->member("version") : nullptr;
    if (!versionValue || !versionValue->isString() || versionValue->text.compare(0, 2, "2.") != 0) {
        setError(outError, "Not glTF 2.0");
        return false;
    }

    // The BIN chunk is empty when the file has none; the accessors then have
    // no data to read, which the accessor check reports.
    if (!bin) {
        static const uint8_t empty[1] = {0};
        bin = empty;
        binSize = 0;
    }

    Reader reader(&root, bin, binSize);
    reader.setErrorSink(outError);

    Scene scene;
    const JsonValue* materials = root.member("materials");
    if (materials && materials->kind == JsonValue::Kind::Array) {
        for (size_t i = 0; i < materials->items.size(); i++) {
            Material material;
            if (!reader.material(&materials->items[i], &material)) {
                return false;
            }

            scene.materials.push_back(material);
        }
    }

    std::vector<std::pair<const JsonValue*, Matrix>> nodes;
    if (!reader.nodes(&nodes)) {
        return false;
    }

    const JsonValue* meshes = root.member("meshes");
    for (size_t i = 0; i < nodes.size(); i++) {
        int32_t index = 0;
        const JsonValue* indexValue = nodes[i].first->member("mesh");
        if (!indexValue || !indexValue->asInt(&index)) {
            continue;
        }

        const JsonValue* meshValue = meshes ? meshes->element(index) : nullptr;
        if (!meshValue) {
            setError(outError, "glTF mesh out of range");
            return false;
        }

        Mesh mesh;
        const JsonValue* nameValue = nodes[i].first->member("name");
        if (nameValue && nameValue->isString()) {
            mesh.name = nameValue->text;
        } else {
            const JsonValue* meshName = meshValue->member("name");
            if (meshName && meshName->isString()) {
                mesh.name = meshName->text;
            } else {
                mesh.name = "mesh" + std::to_string(index);
            }
        }

        const JsonValue* primitives = meshValue->member("primitives");
        if (primitives && primitives->kind == JsonValue::Kind::Array) {
            for (size_t k = 0; k < primitives->items.size(); k++) {
                Primitive primitive;
                if (!reader.primitive(&primitives->items[k], nodes[i].second, &primitive)) {
                    return false;
                }

                mesh.primitives.push_back(primitive);
            }
        }

        scene.meshes.push_back(mesh);
    }

    *outScene = scene;
    return true;
}

bool Glb::write(const Scene& scene, std::vector<uint8_t>* outBytes, std::string* outError) {
    std::vector<uint8_t> bin;
    struct View {
        int32_t offset;
        int32_t length;
        int32_t target; // -1 when there is none
    };

    std::vector<View> views;
    std::vector<std::string> accessors;

    // A view of some bytes, 4-aligned.
    auto addView = [&](const uint8_t* data, int32_t count, int32_t target) -> int32_t {
        while (bin.size() % kAlignment != 0) {
            bin.push_back(0);
        }

        View view;
        view.offset = (int32_t)bin.size();
        view.length = count;
        view.target = target;
        views.push_back(view);
        bin.insert(bin.end(), data, data + count);
        return (int32_t)views.size() - 1;
    };

    // An accessor of some bytes. The view's length is the BYTE count
    // (elements x components x 4 here), never the element count the
    // accessor itself carries -- confusing the two truncates the BIN
    // chunk and writes views a reader must refuse as past the end.
    auto addAccessor = [&](const uint8_t* data, int32_t count, int32_t componentType, const char* type,
                           int32_t target, const float* min, const float* max) -> int32_t {
        const int32_t components = type[0] == 'V' ? (type[3] == '3' ? 3 : 2) : 1;
        const int32_t view = addView(data, count * components * 4, target);
        JsonWriter accessor;
        accessor.startObject();
        accessor.number("bufferView", view);
        accessor.number("componentType", componentType);
        accessor.number("count", count);
        accessor.string("type", type);
        if (min && max) {
            accessor.startArray("min");
            accessor.numberValue(min[0]);
            accessor.numberValue(min[1]);
            accessor.numberValue(min[2]);
            accessor.endArray();
            accessor.startArray("max");
            accessor.numberValue(max[0]);
            accessor.numberValue(max[1]);
            accessor.numberValue(max[2]);
            accessor.endArray();
        }

        accessor.endObject();
        accessors.push_back(accessor.text());
        return (int32_t)accessors.size() - 1;
    };

    // The floats of a vector, little-endian.
    auto floatsOf = [&](const std::vector<float>& values) -> std::vector<uint8_t> {
        std::vector<uint8_t> bytes(values.size() * 4);
        for (size_t i = 0; i < values.size(); i++) {
            memcpy(bytes.data() + i * 4, &values[i], 4);
        }

        return bytes;
    };

    auto uintsOf = [&](const std::vector<int32_t>& values) -> std::vector<uint8_t> {
        std::vector<uint8_t> bytes(values.size() * 4);
        for (size_t i = 0; i < values.size(); i++) {
            const uint32_t value = (uint32_t)values[i];
            memcpy(bytes.data() + i * 4, &value, 4);
        }

        return bytes;
    };

    JsonWriter json;
    json.startObject();
    json.startObject("asset");
    json.string("version", "2.0");
    json.string("generator", "mdk-sdl");
    json.endObject();
    json.number("scene", 0);
    json.startArray("scenes");
    json.startObject();
    json.startArray("nodes");
    for (size_t i = 0; i < scene.meshes.size(); i++) {
        json.numberValue((int32_t)i);
    }

    json.endArray();
    json.endObject();
    json.endArray();

    json.startArray("nodes");
    for (size_t i = 0; i < scene.meshes.size(); i++) {
        json.startObject();
        json.string("name", scene.meshes[i].name);
        json.number("mesh", (int32_t)i);
        json.endObject();
    }

    json.endArray();

    json.startArray("meshes");
    for (size_t i = 0; i < scene.meshes.size(); i++) {
        const Mesh& mesh = scene.meshes[i];
        json.startObject();
        json.string("name", mesh.name);
        json.startArray("primitives");
        for (size_t k = 0; k < mesh.primitives.size(); k++) {
            const Primitive& primitive = mesh.primitives[k];

            float min[3] = {0.0f, 0.0f, 0.0f};
            float max[3] = {0.0f, 0.0f, 0.0f};
            if (primitive.positions.size() >= 3) {
                min[0] = max[0] = primitive.positions[0];
                min[1] = max[1] = primitive.positions[1];
                min[2] = max[2] = primitive.positions[2];
                for (int32_t v = 1; v < primitive.vertexCount(); v++) {
                    for (int32_t c = 0; c < 3; c++) {
                        const float value = primitive.positions[(size_t)v * 3 + (size_t)c];
                        if (value < min[c]) {
                            min[c] = value;
                        }

                        if (value > max[c]) {
                            max[c] = value;
                        }
                    }
                }
            }

            const std::vector<uint8_t> positionBytes = floatsOf(primitive.positions);
            const int32_t position = addAccessor(positionBytes.data(), (int32_t)primitive.vertexCount(),
                                                 kFloat, "VEC3", kArrayBuffer, min, max);
            const std::vector<uint8_t> uvBytes = floatsOf(primitive.uvs);
            const int32_t uv = addAccessor(uvBytes.data(), (int32_t)(primitive.uvs.size() / 2), kFloat,
                                           "VEC2", kArrayBuffer, nullptr, nullptr);
            const std::vector<uint8_t> indexBytes = uintsOf(primitive.indices);
            const int32_t indices = addAccessor(indexBytes.data(), (int32_t)primitive.indices.size(),
                                                kUnsignedInt, "SCALAR", kElementArrayBuffer, nullptr,
                                                nullptr);

            json.startObject();
            json.startObject("attributes");
            json.number("POSITION", position);
            json.number("TEXCOORD_0", uv);
            json.endObject();
            json.number("indices", indices);
            if (primitive.material >= 0) {
                json.number("material", primitive.material);
            }

            json.number("mode", kTriangles);
            json.endObject();
        }

        json.endArray();
        json.endObject();
    }

    json.endArray();

    std::vector<int32_t> images;
    json.startArray("materials");
    for (size_t i = 0; i < scene.materials.size(); i++) {
        const Material& material = scene.materials[i];
        json.startObject();
        json.string("name", material.name);
        json.boolean("doubleSided", true);
        json.startObject("pbrMetallicRoughness");
        json.startArray("baseColorFactor");
        for (int32_t c = 0; c < 4; c++) {
            json.numberValue(material.colour[c]);
        }

        json.endArray();
        if (!material.png.empty()) {
            json.startObject("baseColorTexture");
            json.number("index", (int32_t)images.size());
            json.endObject();
            images.push_back(addView(material.png.data(), (int32_t)material.png.size(), -1));
        }

        json.number("metallicFactor", 0);
        json.number("roughnessFactor", 1);
        json.endObject();
        json.endObject();
    }

    json.endArray();

    if (!images.empty()) {
        json.startArray("textures");
        for (size_t i = 0; i < images.size(); i++) {
            json.startObject();
            json.number("source", (int32_t)i);
            json.endObject();
        }

        json.endArray();
        json.startArray("images");
        for (size_t i = 0; i < images.size(); i++) {
            json.startObject();
            json.number("bufferView", images[i]);
            json.string("mimeType", kPngType);
            json.endObject();
        }

        json.endArray();
    }

    json.startArray("accessors");
    for (size_t i = 0; i < accessors.size(); i++) {
        if (i > 0) {
            json.appendRaw(",");
        }

        json.appendRaw(accessors[i]);
    }

    json.endArray();

    while (bin.size() % kAlignment != 0) {
        bin.push_back(0);
    }

    json.startArray("bufferViews");
    for (size_t i = 0; i < views.size(); i++) {
        json.startObject();
        json.number("buffer", 0);
        json.number("byteOffset", views[i].offset);
        json.number("byteLength", views[i].length);
        if (views[i].target >= 0) {
            json.number("target", views[i].target);
        }

        json.endObject();
    }

    json.endArray();
    json.startArray("buffers");
    json.startObject();
    json.number("byteLength", (int32_t)bin.size());
    json.endObject();
    json.endArray();
    json.endObject();

    if (!pack(json.text(), bin, outBytes)) {
        setError(outError, "glTF does not pack");
        return false;
    }

    return true;
}

} // namespace formats
} // namespace mdk

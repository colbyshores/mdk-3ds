// ============================================================================
// glb.cpp — see glb.h for the subset and the matrix convention.
//
// The JSON parser and writer below are the smallest ones that read and write
// a glTF's JSON chunk. They are here rather than in a shared library because
// the glTF subset is the only JSON in the project, and a general JSON
// library would be a dependency the 3DS build would have to carry.
//
// A NUMBER KEEPS ITS SOURCE TEXT. The C# reads a number with GetInt32 or
// GetSingle, which look at the text and not at a parsed double: "1" is an
// int and "1.0" is not, and a float written as "0.1" is the float nearest
// 0.1 rather than the double nearest 0.1. Keeping the text is what makes
// the two agree.
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

// === JSON =========================================================================

// A JSON value.
class JsonValue {
public:
    enum class Kind {
        Undefined,
        Null,
        Boolean,
        Number,
        String,
        Array,
        Object,
    };

    Kind kind = Kind::Undefined;
    bool boolean = false;
    // A string's value, or a number's source text.
    std::string text;
    // An array's elements.
    std::vector<JsonValue> items;
    // An object's members, in the order they were written.
    std::vector<std::pair<std::string, JsonValue>> members;

    // The member of an object, or null.
    const JsonValue* member(const char* name) const {
        if (kind != Kind::Object) {
            return nullptr;
        }

        for (size_t i = 0; i < members.size(); i++) {
            if (members[i].first == name) {
                return &members[i].second;
            }
        }

        return nullptr;
    }

    // The element of an array, or null.
    const JsonValue* element(int32_t index) const {
        if (kind != Kind::Array || index < 0 || index >= (int32_t)items.size()) {
            return nullptr;
        }

        return &items[(size_t)index];
    }

    int32_t count() const { return kind == Kind::Array ? (int32_t)items.size() : 0; }

    bool isNumber() const { return kind == Kind::Number; }
    bool isString() const { return kind == Kind::String; }
    bool isTrue() const { return kind == Kind::Boolean && boolean; }

    // The number as an int, the way the C#'s GetInt32 reads it: the source
    // text must be a plain integer that fits.
    bool asInt(int32_t* out) const {
        if (kind != Kind::Number) {
            return false;
        }

        size_t i = 0;
        if (i < text.size() && (text[i] == '-' || text[i] == '+')) {
            i++;
        }

        if (i >= text.size()) {
            return false;
        }

        for (size_t k = i; k < text.size(); k++) {
            if (text[k] < '0' || text[k] > '9') {
                return false;
            }
        }

        long long value = 0;
        for (size_t k = 0; k < text.size(); k++) {
            if (value > 2147483647LL) {
                return false;
            }

            value = value * 10 + (text[k] - '0');
        }

        if (text[0] == '-') {
            value = -value;
        }

        if (value < -2147483648LL || value > 2147483647LL) {
            return false;
        }

        *out = (int32_t)value;
        return true;
    }

    // The number as a float, the way the C#'s GetSingle reads it.
    bool asFloat(float* out) const {
        if (kind != Kind::Number) {
            return false;
        }

        const char* start = text.c_str();
        char* end = nullptr;
        const float value = strtof(start, &end);
        if (end != start + text.size()) {
            return false;
        }

        *out = value;
        return true;
    }
};

// The JSON parser.
class JsonParser {
public:
    JsonParser(const char* text, int32_t length) : text_(text), length_(length) {}

    bool parse(JsonValue* out) {
        skipWhitespace();
        if (!parseValue(out)) {
            return false;
        }

        skipWhitespace();
        return pos_ == length_;
    }

private:
    void skipWhitespace() {
        while (pos_ < length_) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                pos_++;
            } else {
                break;
            }
        }
    }

    bool literal(const char* word) {
        const int32_t n = (int32_t)strlen(word);
        if (pos_ + n > length_ || memcmp(text_ + pos_, word, (size_t)n) != 0) {
            return false;
        }

        pos_ += n;
        return true;
    }

    bool parseValue(JsonValue* out) {
        if (pos_ >= length_) {
            return false;
        }

        switch (text_[pos_]) {
        case '{':
            return parseObject(out);
        case '[':
            return parseArray(out);
        case '"': {
            out->kind = JsonValue::Kind::String;
            return parseString(&out->text);
        }
        case 't':
            if (!literal("true")) {
                return false;
            }

            out->kind = JsonValue::Kind::Boolean;
            out->boolean = true;
            return true;
        case 'f':
            if (!literal("false")) {
                return false;
            }

            out->kind = JsonValue::Kind::Boolean;
            out->boolean = false;
            return true;
        case 'n':
            if (!literal("null")) {
                return false;
            }

            out->kind = JsonValue::Kind::Null;
            return true;
        default:
            return parseNumber(out);
        }
    }

    bool parseObject(JsonValue* out) {
        out->kind = JsonValue::Kind::Object;
        pos_++; // '{'
        skipWhitespace();
        if (pos_ < length_ && text_[pos_] == '}') {
            pos_++;
            return true;
        }

        while (true) {
            skipWhitespace();
            if (pos_ >= length_ || text_[pos_] != '"') {
                return false;
            }

            std::string name;
            if (!parseString(&name)) {
                return false;
            }

            skipWhitespace();
            if (pos_ >= length_ || text_[pos_] != ':') {
                return false;
            }

            pos_++;
            skipWhitespace();
            JsonValue value;
            if (!parseValue(&value)) {
                return false;
            }

            out->members.push_back(std::make_pair(name, value));
            skipWhitespace();
            if (pos_ >= length_) {
                return false;
            }

            if (text_[pos_] == ',') {
                pos_++;
                continue;
            }

            if (text_[pos_] == '}') {
                pos_++;
                return true;
            }

            return false;
        }
    }

    bool parseArray(JsonValue* out) {
        out->kind = JsonValue::Kind::Array;
        pos_++; // '['
        skipWhitespace();
        if (pos_ < length_ && text_[pos_] == ']') {
            pos_++;
            return true;
        }

        while (true) {
            skipWhitespace();
            JsonValue value;
            if (!parseValue(&value)) {
                return false;
            }

            out->items.push_back(value);
            skipWhitespace();
            if (pos_ >= length_) {
                return false;
            }

            if (text_[pos_] == ',') {
                pos_++;
                continue;
            }

            if (text_[pos_] == ']') {
                pos_++;
                return true;
            }

            return false;
        }
    }

    // A string, with the escapes decoded into UTF-8.
    bool parseString(std::string* out) {
        pos_++; // '"'
        out->clear();
        while (pos_ < length_) {
            const unsigned char c = (unsigned char)text_[pos_];
            if (c == '"') {
                pos_++;
                return true;
            }

            if (c == '\\') {
                pos_++;
                if (pos_ >= length_) {
                    return false;
                }

                const char escape = text_[pos_++];
                switch (escape) {
                case '"':
                    out->push_back('"');
                    break;
                case '\\':
                    out->push_back('\\');
                    break;
                case '/':
                    out->push_back('/');
                    break;
                case 'b':
                    out->push_back('\b');
                    break;
                case 'f':
                    out->push_back('\f');
                    break;
                case 'n':
                    out->push_back('\n');
                    break;
                case 'r':
                    out->push_back('\r');
                    break;
                case 't':
                    out->push_back('\t');
                    break;
                case 'u': {
                    uint32_t code = 0;
                    if (!parseHex4(&code)) {
                        return false;
                    }

                    // A high surrogate is followed by a low one.
                    if (code >= 0xD800 && code <= 0xDBFF && pos_ + 1 < length_ && text_[pos_] == '\\' &&
                        text_[pos_ + 1] == 'u') {
                        pos_ += 2;
                        uint32_t low = 0;
                        if (!parseHex4(&low)) {
                            return false;
                        }

                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    }

                    appendUtf8(code, out);
                    break;
                }
                default:
                    return false;
                }

                continue;
            }

            if (c < 0x20) {
                return false;
            }

            out->push_back((char)c);
            pos_++;
        }

        return false;
    }

    bool parseHex4(uint32_t* out) {
        if (pos_ + 4 > length_) {
            return false;
        }

        uint32_t value = 0;
        for (int32_t i = 0; i < 4; i++) {
            const char c = text_[pos_ + i];
            value <<= 4;
            if (c >= '0' && c <= '9') {
                value |= (uint32_t)(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= (uint32_t)(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= (uint32_t)(c - 'A' + 10);
            } else {
                return false;
            }
        }

        pos_ += 4;
        *out = value;
        return true;
    }

    static void appendUtf8(uint32_t code, std::string* out) {
        if (code < 0x80) {
            out->push_back((char)code);
        } else if (code < 0x800) {
            out->push_back((char)(0xC0 | (code >> 6)));
            out->push_back((char)(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out->push_back((char)(0xE0 | (code >> 12)));
            out->push_back((char)(0x80 | ((code >> 6) & 0x3F)));
            out->push_back((char)(0x80 | (code & 0x3F)));
        } else {
            out->push_back((char)(0xF0 | (code >> 18)));
            out->push_back((char)(0x80 | ((code >> 12) & 0x3F)));
            out->push_back((char)(0x80 | ((code >> 6) & 0x3F)));
            out->push_back((char)(0x80 | (code & 0x3F)));
        }
    }

    // A number. The source text is kept, because the C# reads a number from
    // its text and not from a parsed double.
    bool parseNumber(JsonValue* out) {
        const int32_t start = pos_;
        if (pos_ < length_ && text_[pos_] == '-') {
            pos_++;
        }

        if (pos_ >= length_ || text_[pos_] < '0' || text_[pos_] > '9') {
            return false;
        }

        while (pos_ < length_ && text_[pos_] >= '0' && text_[pos_] <= '9') {
            pos_++;
        }

        if (pos_ < length_ && text_[pos_] == '.') {
            pos_++;
            if (pos_ >= length_ || text_[pos_] < '0' || text_[pos_] > '9') {
                return false;
            }

            while (pos_ < length_ && text_[pos_] >= '0' && text_[pos_] <= '9') {
                pos_++;
            }
        }

        if (pos_ < length_ && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            pos_++;
            if (pos_ < length_ && (text_[pos_] == '+' || text_[pos_] == '-')) {
                pos_++;
            }

            if (pos_ >= length_ || text_[pos_] < '0' || text_[pos_] > '9') {
                return false;
            }

            while (pos_ < length_ && text_[pos_] >= '0' && text_[pos_] <= '9') {
                pos_++;
            }
        }

        out->kind = JsonValue::Kind::Number;
        out->text.assign(text_ + start, (size_t)(pos_ - start));
        return true;
    }

    const char* text_;
    int32_t length_;
    int32_t pos_ = 0;
};

// The JSON writer: compact, in the order the C#'s Utf8JsonWriter writes.
class JsonWriter {
public:
    void startObject() { comma(); out_ += '{'; first_ = true; }
    void endObject() { out_ += '}'; first_ = false; }
    void startArray() { comma(); out_ += '['; first_ = true; }
    void endArray() { out_ += ']'; first_ = false; }
    void startObject(const char* key) { comma(); writeName(key); out_ += '{'; first_ = true; }
    void startArray(const char* key) { comma(); writeName(key); out_ += '['; first_ = true; }

    void number(const char* key, int32_t value) {
        comma();
        writeName(key);
        out_ += std::to_string(value);
        first_ = false;
    }

    void numberValue(int32_t value) {
        comma();
        out_ += std::to_string(value);
        first_ = false;
    }

    void string(const char* key, const std::string& value) {
        comma();
        writeName(key);
        quote(value);
        first_ = false;
    }

    void boolean(const char* key, bool value) {
        comma();
        writeName(key);
        out_ += value ? "true" : "false";
        first_ = false;
    }

    void numberValue(float value) {
        comma();
        char buffer[32];
        const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), value);
        out_.append(buffer, (size_t)(result.ptr - buffer));
        first_ = false;
    }

    const std::string& text() const { return out_; }

    // A piece of JSON written by hand, put into an array.
    void appendRaw(const std::string& value) {
        out_ += value;
        first_ = false;
    }

private:
    void comma() {
        if (!first_) {
            out_ += ',';
        }
    }

    void writeName(const char* key) {
        quote(key);
        out_ += ':';
    }

    void quote(const std::string& value) {
        out_ += '"';
        for (size_t i = 0; i < value.size(); i++) {
            const char c = value[i];
            switch (c) {
            case '"':
                out_ += "\\\"";
                break;
            case '\\':
                out_ += "\\\\";
                break;
            case '\b':
                out_ += "\\b";
                break;
            case '\f':
                out_ += "\\f";
                break;
            case '\n':
                out_ += "\\n";
                break;
            case '\r':
                out_ += "\\r";
                break;
            case '\t':
                out_ += "\\t";
                break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buffer[8];
                    snprintf(buffer, sizeof(buffer), "\\u%04x", (unsigned)(unsigned char)c);
                    out_ += buffer;
                } else {
                    out_ += c;
                }

                break;
            }
        }

        out_ += '"';
    }

    std::string out_;
    bool first_ = true;
};

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

// === The glTF ===================================================================

// A file's accessors and nodes over its BIN chunk.
class Reader {
public:
    Reader(const JsonValue* root, const uint8_t* bin, int32_t binSize) : root_(root), bin_(bin), binSize_(binSize) {}

    // Every node under the scene's roots, with its transform to the scene.
    bool nodes(std::vector<std::pair<const JsonValue*, Matrix>>* out) {
        std::vector<const JsonValue*> nodes;
        const JsonValue* array = root_->member("nodes");
        if (array && array->kind == JsonValue::Kind::Array) {
            for (size_t i = 0; i < array->items.size(); i++) {
                nodes.push_back(&array->items[i]);
            }
        }

        std::vector<int32_t> roots;
        if (!rootNodes(&nodes, &roots)) {
            return false;
        }

        std::vector<std::pair<int32_t, Matrix>> stack;
        for (size_t i = roots.size(); i > 0; i--) {
            stack.push_back(std::make_pair(roots[i - 1], Matrix()));
        }

        std::vector<int32_t> seen;
        while (!stack.empty()) {
            const std::pair<int32_t, Matrix> top = stack.back();
            stack.pop_back();
            const int32_t index = top.first;
            if (index < 0 || index >= (int32_t)nodes.size()) {
                continue;
            }

            bool already = false;
            for (size_t i = 0; i < seen.size(); i++) {
                if (seen[i] == index) {
                    already = true;
                    break;
                }
            }

            if (already) {
                continue;
            }

            seen.push_back(index);
            const JsonValue* node = nodes[(size_t)index];
            Matrix world;
            if (!local(node, &world)) {
                return false;
            }

            world = multiply(world, top.second);
            out->push_back(std::make_pair(node, world));

            const JsonValue* children = node->member("children");
            if (children && children->kind == JsonValue::Kind::Array) {
                std::vector<int32_t> childIndices;
                for (size_t i = 0; i < children->items.size(); i++) {
                    int32_t child = 0;
                    if (!children->items[i].asInt(&child)) {
                        return false;
                    }

                    childIndices.push_back(child);
                }

                for (size_t i = childIndices.size(); i > 0; i--) {
                    stack.push_back(std::make_pair(childIndices[i - 1], world));
                }
            }
        }

        return true;
    }

    // A primitive's triangles, with the world transform applied.
    bool primitive(const JsonValue* value, const Matrix& world, Glb::Primitive* out) {
        int32_t mode = kTriangles;
        bool present = false;
        if (!memberInt(value, "mode", &mode, &present)) {
            return false;
        }

        if (mode != kTriangles) {
            setError(error_, "glTF primitive not of triangles");
            return false;
        }

        const JsonValue* attributes = value->member("attributes");
        int32_t position = 0;
        present = false;
        if (!memberInt(attributes, "POSITION", &position, &present)) {
            return false;
        }

        if (!present) {
            setError(error_, "glTF primitive without POSITION");
            return false;
        }

        std::vector<float> points;
        if (!vectors(position, 3, &points)) {
            return false;
        }

        const int32_t count = (int32_t)(points.size() / 3);
        out->positions.resize((size_t)count * 3);
        for (int32_t i = 0; i < count; i++) {
            const float in[3] = {points[(size_t)i * 3], points[(size_t)i * 3 + 1], points[(size_t)i * 3 + 2]};
            transform(in, world, &out->positions[(size_t)i * 3]);
        }

        out->uvs.assign((size_t)count * 2, 0.0f);
        int32_t texcoord = 0;
        present = false;
        if (!memberInt(attributes, "TEXCOORD_0", &texcoord, &present)) {
            return false;
        }

        if (present) {
            std::vector<float> read;
            if (!vectors(texcoord, 2, &read)) {
                return false;
            }

            const int32_t n = (int32_t)(read.size() / 2) < count ? (int32_t)(read.size() / 2) : count;
            for (int32_t i = 0; i < n; i++) {
                out->uvs[(size_t)i * 2] = read[(size_t)i * 2];
                out->uvs[(size_t)i * 2 + 1] = read[(size_t)i * 2 + 1];
            }
        }

        int32_t indicesAccessor = 0;
        present = false;
        if (!memberInt(value, "indices", &indicesAccessor, &present)) {
            return false;
        }

        if (present) {
            std::vector<float> read;
            if (!vectors(indicesAccessor, 1, &read)) {
                return false;
            }

            out->indices.resize(read.size());
            for (size_t i = 0; i < read.size(); i++) {
                out->indices[i] = (int32_t)read[i];
            }
        } else {
            out->indices.resize((size_t)count);
            for (int32_t i = 0; i < count; i++) {
                out->indices[(size_t)i] = i;
            }
        }

        for (size_t i = 0; i < out->indices.size(); i++) {
            if (out->indices[i] < 0 || out->indices[i] >= count) {
                setError(error_, "glTF indices out of range");
                return false;
            }
        }

        if (out->indices.size() % 3 != 0) {
            setError(error_, "glTF indices out of range");
            return false;
        }

        int32_t material = -1;
        present = false;
        if (!memberInt(value, "material", &material, &present)) {
            return false;
        }

        out->material = present ? material : -1;
        return true;
    }

    // A material: its name, its base colour and its PNG.
    bool material(const JsonValue* value, Glb::Material* out) {
        const JsonValue* pbr = value->member("pbrMetallicRoughness");
        std::vector<float> factor;
        if (pbr) {
            const JsonValue* factorValue = pbr->member("baseColorFactor");
            if (factorValue && factorValue->kind == JsonValue::Kind::Array) {
                for (size_t i = 0; i < factorValue->items.size(); i++) {
                    float number = 0.0f;
                    if (!factorValue->items[i].asFloat(&number)) {
                        setError(error_, "glTF base colour is not a number");
                        return false;
                    }

                    factor.push_back(number);
                }
            }
        }

        if (factor.size() == 4) {
            for (int32_t i = 0; i < 4; i++) {
                out->colour[i] = factor[(size_t)i];
            }
        } else {
            out->colour[0] = 1.0f;
            out->colour[1] = 1.0f;
            out->colour[2] = 1.0f;
            out->colour[3] = 1.0f;
        }

        const JsonValue* texture = pbr ? pbr->member("baseColorTexture") : nullptr;
        int32_t textureIndex = 0;
        const JsonValue* textureValue = texture ? texture->member("index") : nullptr;
        if (textureValue && textureValue->asInt(&textureIndex)) {
            const JsonValue* textures = root_->member("textures");
            const JsonValue* textureEntry = textures ? textures->element(textureIndex) : nullptr;
            if (!textureEntry) {
                setError(error_, "glTF texture out of range");
                return false;
            }

            const JsonValue* sourceValue = textureEntry->member("source");
            int32_t source = 0;
            if (!sourceValue || !sourceValue->asInt(&source)) {
                setError(error_, "glTF texture without a source");
                return false;
            }

            const JsonValue* images = root_->member("images");
            const JsonValue* image = images ? images->element(source) : nullptr;
            if (!image) {
                setError(error_, "glTF image out of range");
                return false;
            }

            if (!imageBytes(image, &out->png)) {
                return false;
            }
        }

        const JsonValue* nameValue = value->member("name");
        if (nameValue && nameValue->isString()) {
            out->name = nameValue->text;
        }

        return true;
    }

    void setErrorSink(std::string* error) { error_ = error; }

private:
    // The C#'s Int: no value unless the member is a number, and an error
    // when the number is not an integer (the C#'s GetInt32 throws).
    bool memberInt(const JsonValue* element, const char* name, int32_t* outValue, bool* outPresent) {
        *outPresent = false;
        const JsonValue* value = element ? element->member(name) : nullptr;
        if (!value || !value->isNumber()) {
            return true;
        }

        if (!value->asInt(outValue)) {
            setError(error_, "glTF number is not an integer");
            return false;
        }

        *outPresent = true;
        return true;
    }

    // The default scene's nodes, else every node no other one holds.
    bool rootNodes(const std::vector<const JsonValue*>* nodes, std::vector<int32_t>* out) {
        const JsonValue* scenes = root_->member("scenes");
        if (scenes && scenes->kind == JsonValue::Kind::Array && scenes->count() > 0) {
            int32_t index = 0;
            const JsonValue* sceneValue = root_->member("scene");
            if (sceneValue && !sceneValue->asInt(&index)) {
                setError(error_, "glTF scene is not a number");
                return false;
            }

            if (index < 0) {
                index = 0;
            }

            if (index > scenes->count() - 1) {
                index = scenes->count() - 1;
            }

            const JsonValue* scene = scenes->element(index);
            const JsonValue* list = scene ? scene->member("nodes") : nullptr;
            if (list && list->kind == JsonValue::Kind::Array) {
                for (size_t i = 0; i < list->items.size(); i++) {
                    int32_t node = 0;
                    if (!list->items[i].asInt(&node)) {
                        setError(error_, "glTF scene node is not a number");
                        return false;
                    }

                    out->push_back(node);
                }
            }

            return true;
        }

        std::vector<int32_t> children;
        for (size_t i = 0; i < nodes->size(); i++) {
            const JsonValue* list = (*nodes)[i]->member("children");
            if (!list || list->kind != JsonValue::Kind::Array) {
                continue;
            }

            for (size_t k = 0; k < list->items.size(); k++) {
                int32_t child = 0;
                if (!list->items[k].asInt(&child)) {
                    setError(error_, "glTF child is not a number");
                    return false;
                }

                children.push_back(child);
            }
        }

        for (int32_t i = 0; i < (int32_t)nodes->size(); i++) {
            bool isChild = false;
            for (size_t k = 0; k < children.size(); k++) {
                if (children[k] == i) {
                    isChild = true;
                    break;
                }
            }

            if (!isChild) {
                out->push_back(i);
            }
        }

        return true;
    }

    // A node's transform: its matrix (column-major, transposed into
    // System.Numerics' layout), else scale, rotation and translation.
    bool local(const JsonValue* node, Matrix* out) {
        std::vector<float> m;
        if (!floats(node, "matrix", &m)) {
            return false;
        }

        if (m.size() == 16) {
            for (int32_t r = 0; r < 4; r++) {
                for (int32_t c = 0; c < 4; c++) {
                    out->m[r][c] = m[(size_t)(r * 4 + c)];
                }
            }

            return true;
        }

        std::vector<float> scale;
        std::vector<float> rotation;
        std::vector<float> translation;
        if (!floats(node, "scale", &scale) || !floats(node, "rotation", &rotation) ||
            !floats(node, "translation", &translation)) {
            return false;
        }

        Matrix s;
        Matrix r;
        Matrix t;
        if (scale.size() == 3) {
            s = createScale(scale[0], scale[1], scale[2]);
        }

        if (rotation.size() == 4) {
            r = createFromQuaternion(rotation[0], rotation[1], rotation[2], rotation[3]);
        }

        if (translation.size() == 3) {
            t = createTranslation(translation[0], translation[1], translation[2]);
        }

        *out = multiply(multiply(s, r), t);
        return true;
    }

    // An accessor's elements of `components` numbers: floats, or unsigned
    // integers (normalized when the accessor says so).
    bool vectors(int32_t index, int32_t components, std::vector<float>* out) {
        const JsonValue* accessors = root_->member("accessors");
        const JsonValue* accessor = accessors ? accessors->element(index) : nullptr;
        if (!accessor) {
            setError(error_, "glTF accessor out of range");
            return false;
        }

        if (accessor->member("sparse")) {
            setError(error_, "glTF sparse accessors aren't read");
            return false;
        }

        int32_t count = 0;
        bool present = false;
        if (!memberInt(accessor, "count", &count, &present)) {
            return false;
        }

        int32_t type = kFloat;
        present = false;
        if (!memberInt(accessor, "componentType", &type, &present)) {
            return false;
        }

        const bool normalized = accessor->member("normalized") != nullptr &&
                                accessor->member("normalized")->isTrue();

        int32_t size = 0;
        if (type == kUnsignedByte) {
            size = 1;
        } else if (type == kUnsignedShort) {
            size = 2;
        } else if (type == kUnsignedInt || type == kFloat) {
            size = 4;
        } else {
            setError(error_, "glTF component type not read");
            return false;
        }

        int32_t offset = 0;
        int32_t length = 0;
        int32_t stride = 0;
        int32_t view = 0;
        present = false;
        if (!memberInt(accessor, "bufferView", &view, &present)) {
            return false;
        }

        if (present) {
            if (!viewBounds(view, &offset, &length, &stride)) {
                return false;
            }
        }

        int32_t byteOffset = 0;
        present = false;
        if (!memberInt(accessor, "byteOffset", &byteOffset, &present)) {
            return false;
        }

        offset += byteOffset;
        if (stride == 0) {
            stride = size * components;
        }

        if (count > 0 && (int64_t)offset + (int64_t)(count - 1) * stride + (int64_t)size * components > binSize_) {
            setError(error_, "glTF accessor past the end");
            return false;
        }

        out->resize((size_t)count * (size_t)components);
        for (int32_t i = 0; i < count; i++) {
            for (int32_t c = 0; c < components; c++) {
                const int64_t at = (int64_t)offset + (int64_t)i * stride + (int64_t)c * size;
                float value = 0.0f;
                if (type == kFloat) {
                    value = binF32(bin_, (int32_t)at);
                } else if (type == kUnsignedByte) {
                    const uint8_t raw = bin_[(size_t)at];
                    value = normalized ? (float)raw / 255.0f : (float)raw;
                } else if (type == kUnsignedShort) {
                    const uint16_t raw = binU16(bin_, (int32_t)at);
                    value = normalized ? (float)raw / 65535.0f : (float)raw;
                } else {
                    value = (float)binU32(bin_, (int32_t)at);
                }

                (*out)[(size_t)(i * components + c)] = value;
            }
        }

        return true;
    }

    // A buffer view's offset, length and stride.
    bool viewBounds(int32_t index, int32_t* outOffset, int32_t* outLength, int32_t* outStride) {
        const JsonValue* views = root_->member("bufferViews");
        const JsonValue* view = views ? views->element(index) : nullptr;
        if (!view) {
            setError(error_, "glTF buffer view out of range");
            return false;
        }

        int32_t buffer = 0;
        bool present = false;
        if (!memberInt(view, "buffer", &buffer, &present)) {
            return false;
        }

        if (buffer != 0) {
            setError(error_, "glTF data outside the GLB's buffer");
            return false;
        }

        int32_t offset = 0;
        int32_t length = 0;
        int32_t stride = 0;
        present = false;
        if (!memberInt(view, "byteOffset", &offset, &present)) {
            return false;
        }

        present = false;
        if (!memberInt(view, "byteLength", &length, &present)) {
            return false;
        }

        present = false;
        if (!memberInt(view, "byteStride", &stride, &present)) {
            return false;
        }

        if (offset < 0 || length < 0 || (int64_t)offset + length > binSize_) {
            setError(error_, "glTF buffer view past the end");
            return false;
        }

        *outOffset = offset;
        *outLength = length;
        *outStride = stride;
        return true;
    }

    // An embedded image's bytes (external files aren't read).
    bool imageBytes(const JsonValue* image, std::vector<uint8_t>* out) {
        const JsonValue* viewValue = image->member("bufferView");
        int32_t view = 0;
        if (!viewValue || !viewValue->asInt(&view)) {
            setError(error_, "glTF image not embedded");
            return false;
        }

        const JsonValue* typeValue = image->member("mimeType");
        if (typeValue && typeValue->isString() && typeValue->text != kPngType) {
            setError(error_, "glTF image is not PNG");
            return false;
        }

        int32_t offset = 0;
        int32_t length = 0;
        int32_t stride = 0;
        if (!viewBounds(view, &offset, &length, &stride)) {
            return false;
        }

        out->assign(bin_ + offset, bin_ + offset + length);
        return true;
    }

    // A member's numbers.
    bool floats(const JsonValue* element, const char* name, std::vector<float>* out) {
        out->clear();
        const JsonValue* value = element ? element->member(name) : nullptr;
        if (!value || value->kind != JsonValue::Kind::Array) {
            return true;
        }

        for (size_t i = 0; i < value->items.size(); i++) {
            float number = 0.0f;
            if (!value->items[i].asFloat(&number)) {
                setError(error_, "glTF number is not a number");
                return false;
            }

            out->push_back(number);
        }

        return true;
    }

    const JsonValue* root_;
    const uint8_t* bin_;
    int32_t binSize_;
    std::string* error_ = nullptr;
};

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

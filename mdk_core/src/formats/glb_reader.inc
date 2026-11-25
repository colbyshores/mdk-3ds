// glb_reader.inc -- the glTF reader: the JSON's nodes, meshes, accessors and images into a Scene.
// Part of glb.cpp's single translation unit (see its header): pasted inside
// its anonymous namespace after the file-local constants, which it uses.
// No include guards, no includes of its own; the order is load-bearing.

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

#pragma once

// ============================================================================
// glb.h — glTF 2.0 binary files (.glb), the subset mods use.
//
// THE C# ORIGINAL is src/Mdk.Formats/Glb.cs. This is a 1:1 port.
//
// WHAT IS READ: named nodes with triangle meshes (positions, first UVs,
// indices), materials of a base colour and an embedded PNG. The node
// transforms (a matrix, or scale/rotation/translation) are applied, with the
// parents' included; the normals and the rest are ignored.
//
// WHAT IS WRITTEN: one node per mesh, no transform.
//
// THE COORDINATES STAY glTF's (Y up); the callers convert.
//
// THE STRUCTURE:
//   "glTF" u32 version 2, u32 length │ u32 length "JSON" text │ u32 length
//   "BIN\0" data   (little-endian, 4-aligned)
//   node ─► mesh ─► primitives ─► accessors (POSITION, TEXCOORD_0, indices)
//   ─► bufferViews ─► BIN
//        └► material ─► pbrMetallicRoughness: baseColorFactor,
//        baseColorTexture ─► image (PNG in a bufferView)
//
// THE MATRIX CONVENTION IS THE TRICKY PART. glTF's matrix is COLUMN-major,
// and System.Numerics' is ROW-major with row vectors, so the C#'s
// `new Matrix4x4(m[0], m[1], ...)` puts glTF's COLUMN 0 in the
// System.Numerics matrix's ROW 0: the matrix is TRANSPOSED on the way in.
// The same convention is used here, so a position comes out where the C#
// puts it.
//
// THE ERRORS ARE REPORTED, NOT THROWN. The C# throws InvalidDataException
// sixteen times; here each of them is a false return with the C#'s own
// message, so the caller can tell a broken file from a missing one.
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

namespace mdk {
namespace formats {

// glTF 2.0 binary files.
class Glb {
public:
    // A material: its name, its base colour (linear RGBA as written) and its
    // PNG, if it has one.
    struct Material {
        std::string name;
        float colour[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        // The embedded PNG's bytes; empty when the material has no image.
        std::vector<uint8_t> png;
    };

    // The triangles of one material (-1: none).
    struct Primitive {
        // Three floats a vertex.
        std::vector<float> positions;
        // Two floats a vertex.
        std::vector<float> uvs;
        std::vector<int32_t> indices;
        int32_t material = -1;

        int32_t vertexCount() const { return (int32_t)(positions.size() / 3); }
    };

    // A node's mesh, named after the node (else after the mesh).
    struct Mesh {
        std::string name;
        std::vector<Primitive> primitives;
    };

    struct Scene {
        std::vector<Mesh> meshes;
        std::vector<Material> materials;
    };

    // Reads a glb. FALSE when it is not one, or when it is broken; the
    // reason is in `outError` when that is wanted.
    static bool read(const uint8_t* bytes, int32_t byteCount, Scene* outScene, std::string* outError);

    // Writes a scene as a glb: a node per mesh, positions, UVs and u32
    // indices per primitive, each material's PNG embedded, both faces shown.
    static bool write(const Scene& scene, std::vector<uint8_t>* outBytes, std::string* outError);

private:
    // A material's PNG, or none.
    static bool material(const Scene& scene, int32_t index, Material* outMaterial);
};

} // namespace formats
} // namespace mdk

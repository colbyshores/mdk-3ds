#pragma once

// ============================================================================
// model.h — a model: an object, an alien, a weapon.
//
// A 1:1 port of src/Mdk.Formats/Model.cs.
//
// THE FILE, in the order it is read:
//
//   u32 flags                    (only when the header says there is one)
//   u32 material count, then that many char[16] material names
//   u32 part count               (only when the parts are named)
//   per part:
//     char[12] name, vec3 pivot  (only when the parts are named)
//     u32 vertex count, then that many vec3
//     u32 triangle count, then per triangle:
//       s16 index x3, s16 material, vec2 uv x3, and FOUR SKIPPED BYTES
//     six floats: the part's box (only when the parts are named)
//   six floats: the model's box
//   u32 reference point count, then that many vec3
//
// THE FLAGS WORD IS NOT IN THE FILE. Whether a model starts with one depends
// on where the model came from: the arena's model sections have one, and
// XGHEAD and the fall's models do not. So the CALLER says which, and the
// flags word then says whether the parts are named. Getting this wrong
// shifts every field after it, which is why the caller has to be right
// rather than the parser guessing.
//
// THE BOX IS INTERLEAVED. The six floats are stored as min x, max x, min y,
// max y, min z, max z -- not as two vectors. Reading them as two vectors
// swaps the corners, and the corners are what the game tests against.
//
// THE FOUR SKIPPED BYTES after each triangle's texture coordinates are not
// read by the C# either. The game ignores them; so does this, and the
// reader steps over them so that the next triangle starts in the right
// place.
//
// THE MATERIAL NAMES are sixteen bytes in the file, but the game keeps only
// the first ten characters of each. The truncation is the game's, not the
// format's, and it is applied here so the C++ and the C# agree.
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

#include "formats/vector_types.h"

namespace mdk {
namespace formats {

// A box in MDK coordinates: the minimum and the maximum corner.
struct Box {
    Vec3f min;
    Vec3f max;
};

// A model (an object, an alien, a weapon) from an arena's models section or
// a level's CMI.
class Model {
public:
    // Whether the model starts with a flags word (absent in XGHEAD and the
    // fall's models).
    enum class Header { Flags, NoFlags };

    // The parts layout when there is no flags word.
    enum class Parts { Single, Named };

    // The number of characters the game keeps of a material name.
    static const int32_t kMaterialNameLength = 10;

    // A material of the model.
    struct Material {
        // The name as the file has it: sixteen bytes, NUL-padded.
        char name[17] = {0};

        // The name as the GAME has it: the file's name cut to
        // kMaterialNameLength characters.
        std::string nameString() const;
    };

    // One part of the model.
    struct Part {
        // The part's name: twelve bytes, NUL-padded.
        char name[13] = {0};

        // The part's name as a string, up to the first NUL.
        std::string nameString() const;

        // Unused by the game.
        Vec3f pivot;

        std::vector<Vec3f> vertices;

        // The same layout as Arena.
        std::vector<int32_t> triangleIndices;
        std::vector<int32_t> triangleMaterials;
        std::vector<Vec2f> triangleUvs;

        Box bounds;
    };

    // The model's name, as the caller gave it.
    std::string name;

    std::vector<Material> materials;
    std::vector<Part> partList;
    std::vector<Vec3f> referencePoints;
    Box bounds;

    // Parse a model.
    //
    // Returns false when the data runs out or does not fit, which is where
    // the C# throws: the C#'s exception was the caller's signal that the
    // model is not there, and here the caller gets the same signal as a
    // return value.
    static bool parse(Model* out, const std::string& name, const uint8_t* bytes, int32_t byteCount,
                      int32_t offset, Header header = Header::Flags, Parts parts = Parts::Single);

    // Every part's vertices, in part order.
    std::vector<std::vector<Vec3f>> restPose() const;
};

} // namespace formats
} // namespace mdk

#pragma once

// ============================================================================
// model_animation.h — a vertex animation of a model's parts.
//
// A 1:1 port of src/Mdk.Formats/ModelAnimation.cs.
//
// THE RETAIL LAYOUT:
//
//   f32 speed                  1.0 means 30 frames per second
//   u32 track count
//   u32 frame count
//   u32 track offset[count]    RELATIVE TO THE TRACK TABLE, which is four
//                              bytes into the animation (the speed)
//   f32 root motion[frame][3]  the object's movement in model space
//   u32 reference point count
//   f32 reference point[point][frame][3]
//
// THE 1996 DEMO'S LAYOUT (ParseBeta) has no speed, and its track offsets are
// relative to the animation itself:
//
//   u32 track count
//   u32 frame count
//   u32 track offset[count]
//   f32 root motion[frame][3]
//   f32 bounds[frame][6]       the model's box in each frame, SKIPPED
//   u32 reference point count
//   f32 reference point[point][frame][3]
//
// THE TRACKS themselves are read when the animation is baked, because how
// many vertices a track has depends on the MODEL it animates.
//
// THE TRACK'S OWN HEADER decides its kind, and the decision is a QUIRK worth
// spelling out: the float at the track's offset 16 holds the delta scale,
// and the track is a MATRIX track only when that float is exactly zero --
// the C# tests the float's BITS with the sign bit masked off, so +0.0 and
// -0.0 are the two values that choose the matrix form. Every other value is
// a delta track whose scale is that float.
//
// A matrix track is rigid: shifts, the base vertices, and a 3x4 s16 matrix
// per frame. A delta track is the base vertices and then, for the frames the
// file names, an s8 delta per vertex added to the previous frame.
//
// THE FILE IS BORROWED. The tracks are read from it when the animation is
// baked, so the buffer must outlive the animation.
// ============================================================================

#include <stdint.h>

#include <map>
#include <string>
#include <vector>

#include "formats/model.h"
#include "formats/vector_types.h"

namespace mdk {
namespace formats {

// A vertex animation of a model's parts.
class ModelAnimation {
public:
    // The animation's name, as the caller gave it.
    std::string name;

    // 1.0 means 30 frames per second.
    float speed = 1.0f;

    int32_t frameCount = 0;

    // Per frame, the object's movement in model space.
    std::vector<Vec3f> rootMotion;

    // Per reference point, its place in each frame.
    std::vector<std::vector<Vec3f>> referencePoints;

    // Parse a retail animation. Returns false when the data runs out.
    static bool parse(ModelAnimation* out, const std::string& name, const uint8_t* bytes, int32_t byteCount,
                      int32_t offset);

    // Parse a 1996 demo animation. Returns false when the data runs out.
    static bool parseBeta(ModelAnimation* out, const std::string& name, const uint8_t* bytes, int32_t byteCount,
                          int32_t offset);

    // Whether this animation moves `model`: one of its tracks names a part
    // of the model (a level's load bakes these pairs).
    bool animates(const Model& model) const;

    // Per frame, every part's vertices: frames[frame][part][vertex]. Parts
    // without a track keep their model vertices.
    std::vector<std::vector<std::vector<Vec3f>>> bake(const Model& model) const;

private:
    // The track table: a lower case part name to its offset in the file.
    //
    // The order never shows: the animation is looked up by the MODEL's
    // parts, not the other way round.
    std::map<std::string, int32_t> tracks_;

    // The file the tracks are read from. BORROWED: the caller owns it and it
    // must outlive the animation.
    const uint8_t* bytes_ = nullptr;
    int32_t byteCount_ = 0;

    // The layout: the retail one, or the 1996 demo's.
    bool beta_ = false;

    // The frames of one track, decoded.
    std::vector<std::vector<Vec3f>> decodeTrack(int32_t offset, int32_t vertexCount) const;
    std::vector<std::vector<Vec3f>> decodeDeltaTrack(int32_t offset, int32_t vertexCount, float scale) const;
    std::vector<std::vector<Vec3f>> decodeBetaTrack(int32_t offset, int32_t vertexCount, float scale) const;
    std::vector<std::vector<Vec3f>> decodeMatrixTrack(int32_t offset, int32_t vertexCount) const;
};

} // namespace formats
} // namespace mdk

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
// THE TRACKS' BYTES ARE BORROWED. The tracks are read when the animation is
// baked, so the buffer must outlive the animation. parse() borrows the buffer
// it was given; an MTO arena then hands every animation a COPY of its models
// section and rebase()s it there (Arena::parse), so an arena's animations
// borrow only the arena that owns them, never the level's MTO file
// (docs/ARENA_STREAMING_SPEC.md Phase 1: the file is not kept resident).
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

    // Point the tracks at `bytes`, a copy of the parsed buffer's
    // [shift, shift + byteCount) range. False, changing nothing, when a
    // track's header would fall outside it.
    bool rebase(const uint8_t* bytes, int32_t byteCount, int32_t shift);

    // Whether every track's header lies in [from, to) of the parsed buffer.
    bool tracksWithin(int32_t from, int32_t to) const;

    // Whether this animation moves `model`: one of its tracks names a part
    // of the model (a level's load bakes these pairs).
    bool animates(const Model& model) const;

    // One part's track for a model, DECODED AS FAR AS ASKED: `frames` whole
    // frames of `vertexCount` vertices so far, one after another in ONE buffer
    // (not one vector a frame -- on the Old 3DS the allocations were the bake's
    // cost). A track decodes in frame order (a delta track's frame is the one
    // before it plus its deltas), and decodeTo() goes on from where the last
    // call stopped, so frames nobody asks for are never decoded. `ended` once
    // there is no further frame: the animation's last, or the track's data ran
    // out -- the frames past `frames` then keep the model's own vertices, as a
    // part with no track (frames 0, ended) does.
    struct DecodedTrack {
        int32_t vertexCount = 0;
        int32_t frames = 0;
        std::vector<Vec3f> vertices;
        bool ended = true;
        // Where decoding stands: the kind, the reader's place for the next
        // frame's record, and what the next frame is made from.
        enum Kind { Delta, Beta, Matrix };
        Kind kind = Delta;
        int32_t at = 0;
        int32_t next = 0;                // a delta track's next frame with a record
        float scale = 0.0f;              // a delta or demo track's delta scale
        float rotationScale = 0.0f;      // a matrix track's two precisions
        float positionScale = 0.0f;
        std::vector<Vec3f> current;      // a delta or demo track's last frame
        std::vector<Vec3f> base;         // a matrix track's base vertices
    };

    // Every part's track for `model`, in the model's part order, with its
    // header read and nothing past what the header holds decoded.
    std::vector<DecodedTrack> beginTracks(const Model& model) const;

    // Decode `track` until it holds frame `frame` or has ended.
    void decodeTo(DecodedTrack* track, int32_t frame) const;

    // One frame of the bake: every part's vertices -- its track's frame
    // (decoded as far as that first), or the model's vertices past the
    // track's end or without one. bake() is every frame of this in order, so
    // a frame built alone is the same frame the whole bake holds.
    void bakeFrame(const Model& model, std::vector<DecodedTrack>* tracks, int32_t frame,
                   std::vector<std::vector<Vec3f>>* out) const;

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

    // A track's header at `offset` (its vertexCount set): its kind, its base
    // vertices, and -- for a delta or demo track -- frame 0.
    void beginTrack(int32_t offset, DecodedTrack* track) const;
    // The track's next frame, or `ended` when there is none.
    void decodeNext(DecodedTrack* track) const;
};

} // namespace formats
} // namespace mdk

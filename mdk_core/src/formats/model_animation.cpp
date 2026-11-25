// ============================================================================
// model_animation.cpp — see model_animation.h.
// ============================================================================

#include "formats/model_animation.h"

#include "formats/ascii_case.h"
#include "formats/bin_reader.h"

namespace mdk {
namespace formats {

namespace {

// The sign bit of the float at the track's offset 16. The C# masks it off
// and asks whether the rest is zero, which is how +0.0 and -0.0 are told
// apart from every other scale.
const uint32_t kSignMask = 0x7FFFFFFF;

// The number of values in a track's 3x4 matrix.
const int32_t kMatrixValues = 12;

// The length of a track's part name.
const int32_t kTrackNameLength = 12;

// The size of one frame's box in the 1996 demo's animations.
const int32_t kBoundsSize = 24;

// The bytes left to read, or -1 when the reader is already past the end.
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

// A byte as the C#'s (sbyte) cast makes it: 128..255 become negative.
int32_t signedByte(uint8_t value) {
    return value < 128 ? (int32_t)value : (int32_t)value - 256;
}

// The C#'s Bin.Ascii: up to the first NUL within `length` bytes. The field
// is CLAMPED to the end of the buffer, so a name that runs off the end of a
// truncated file comes back short rather than throwing.
//
// The caller checks that `at` is inside the buffer: the C#'s AsSpan throws
// when it is not, and the caller turns that into a failed parse.
std::string asciiAt(const uint8_t* bytes, int32_t byteCount, int32_t at, int32_t length) {
    std::string out;
    if (at < 0 || at > byteCount) {
        return out;
    }
    int32_t take = byteCount - at;
    if (take > length) {
        take = length;
    }
    for (int32_t i = 0; i < take; i++) {
        if (bytes[at + i] == 0) {
            break;
        }
        out.push_back((char)bytes[at + i]);
    }
    return out;
}

} // namespace

bool ModelAnimation::parse(ModelAnimation* out, const std::string& name, const uint8_t* bytes,
                           int32_t byteCount, int32_t offset) {
    if (!out || !bytes || offset < 0 || offset > byteCount) {
        return false;
    }

    ModelAnimation animation;
    animation.name = name;
    animation.bytes_ = bytes;
    animation.byteCount_ = byteCount;
    animation.beta_ = false;

    BinReader reader(bytes, byteCount, offset);
    if (!fits(reader, 4)) {
        return false;
    }
    animation.speed = reader.f32();

    if (!fits(reader, 4)) {
        return false;
    }
    const uint32_t trackCount = reader.u32();

    if (!fits(reader, 4)) {
        return false;
    }
    animation.frameCount = (int32_t)reader.u32();
    if (animation.frameCount < 0) {
        return false;
    }

    if (!fits(reader, (int32_t)trackCount * 4)) {
        return false;
    }
    for (uint32_t i = 0; i < trackCount; i++) {
        const uint32_t relative = reader.u32();
        // The track offsets are relative to the TRACK TABLE, which is four
        // bytes into the animation: the speed is in front of it.
        const int32_t trackOffset = offset + 4 + (int32_t)relative;
        if (trackOffset < 0 || trackOffset > byteCount) {
            return false;
        }
        const std::string trackName = asciiAt(bytes, byteCount, trackOffset, kTrackNameLength);
        animation.tracks_[toLowerAscii(trackName)] = trackOffset;
    }

    if (!fits(reader, animation.frameCount * 12)) {
        return false;
    }
    animation.rootMotion.reserve(animation.frameCount);
    for (int32_t f = 0; f < animation.frameCount; f++) {
        animation.rootMotion.push_back(readVec3(&reader));
    }

    if (!fits(reader, 4)) {
        return false;
    }
    const uint32_t pointCount = reader.u32();
    if (!fits(reader, (int32_t)pointCount * animation.frameCount * 12)) {
        return false;
    }
    for (uint32_t i = 0; i < pointCount; i++) {
        std::vector<Vec3f> points;
        points.reserve(animation.frameCount);
        for (int32_t f = 0; f < animation.frameCount; f++) {
            points.push_back(readVec3(&reader));
        }
        animation.referencePoints.push_back(points);
    }

    *out = animation;
    return true;
}

bool ModelAnimation::rebase(const uint8_t* bytes, int32_t byteCount, int32_t shift) {
    if (!bytes || byteCount <= 0) {
        return false;
    }
    // A track's header is 20 bytes (its name, then the float that picks its
    // kind); decodeTrack refuses one that does not fit, so this refuses the
    // same, BEFORE anything moves.
    for (std::map<std::string, int32_t>::const_iterator it = tracks_.begin(); it != tracks_.end(); ++it) {
        const int32_t at = it->second - shift;
        if (at < 0 || at + 20 > byteCount) {
            return false;
        }
    }
    for (std::map<std::string, int32_t>::iterator it = tracks_.begin(); it != tracks_.end(); ++it) {
        it->second -= shift;
    }
    bytes_ = bytes;
    byteCount_ = byteCount;
    return true;
}

bool ModelAnimation::tracksWithin(int32_t from, int32_t to) const {
    for (std::map<std::string, int32_t>::const_iterator it = tracks_.begin(); it != tracks_.end(); ++it) {
        if (it->second < from || it->second + 20 > to) {
            return false;
        }
    }
    return true;
}

bool ModelAnimation::parseBeta(ModelAnimation* out, const std::string& name, const uint8_t* bytes,
                               int32_t byteCount, int32_t offset) {
    if (!out || !bytes || offset < 0 || offset > byteCount) {
        return false;
    }

    ModelAnimation animation;
    animation.name = name;
    animation.bytes_ = bytes;
    animation.byteCount_ = byteCount;
    animation.beta_ = true;

    BinReader reader(bytes, byteCount, offset);
    if (!fits(reader, 4)) {
        return false;
    }
    const uint32_t trackCount = reader.u32();

    if (!fits(reader, 4)) {
        return false;
    }
    animation.frameCount = (int32_t)reader.u32();
    if (animation.frameCount < 0) {
        return false;
    }

    if (!fits(reader, (int32_t)trackCount * 4)) {
        return false;
    }
    for (uint32_t i = 0; i < trackCount; i++) {
        const uint32_t relative = reader.u32();
        // The demo's track offsets are relative to the ANIMATION, not to
        // the track table: there is no speed in front of them.
        const int32_t trackOffset = offset + (int32_t)relative;
        if (trackOffset < 0 || trackOffset > byteCount) {
            return false;
        }
        const std::string trackName = asciiAt(bytes, byteCount, trackOffset, kTrackNameLength);
        animation.tracks_[toLowerAscii(trackName)] = trackOffset;
    }

    if (!fits(reader, animation.frameCount * 12)) {
        return false;
    }
    animation.rootMotion.reserve(animation.frameCount);
    for (int32_t f = 0; f < animation.frameCount; f++) {
        animation.rootMotion.push_back(readVec3(&reader));
    }

    // The demo keeps the model's box in every frame. The retail VM does not
    // read it, and neither does this; the reader steps over it.
    reader.skip(animation.frameCount * kBoundsSize);

    if (!fits(reader, 4)) {
        return false;
    }
    const uint32_t pointCount = reader.u32();
    if (!fits(reader, (int32_t)pointCount * animation.frameCount * 12)) {
        return false;
    }
    for (uint32_t i = 0; i < pointCount; i++) {
        std::vector<Vec3f> points;
        points.reserve(animation.frameCount);
        for (int32_t f = 0; f < animation.frameCount; f++) {
            points.push_back(readVec3(&reader));
        }
        animation.referencePoints.push_back(points);
    }

    *out = animation;
    return true;
}

bool ModelAnimation::animates(const Model& model) const {
    for (size_t i = 0; i < model.partList.size(); i++) {
        if (tracks_.find(toLowerAscii(model.partList[i].nameString())) != tracks_.end()) {
            return true;
        }
    }
    return false;
}

std::vector<ModelAnimation::DecodedTrack> ModelAnimation::beginTracks(const Model& model) const {
    std::vector<DecodedTrack> tracks(model.partList.size());
    for (size_t i = 0; i < model.partList.size(); i++) {
        tracks[i].vertexCount = (int32_t)model.partList[i].vertices.size();
        const std::map<std::string, int32_t>::const_iterator found =
            tracks_.find(toLowerAscii(model.partList[i].nameString()));
        if (found != tracks_.end()) {
            beginTrack(found->second, &tracks[i]);
        }
    }
    return tracks;
}

void ModelAnimation::decodeTo(DecodedTrack* track, int32_t frame) const {
    while (!track->ended && track->frames <= frame) {
        decodeNext(track);
    }
}

void ModelAnimation::bakeFrame(const Model& model, std::vector<DecodedTrack>* tracks, int32_t frame,
                               std::vector<std::vector<Vec3f>>* out) const {
    out->clear();
    out->reserve(model.partList.size());
    for (size_t p = 0; p < model.partList.size(); p++) {
        DecodedTrack* track = p < tracks->size() ? &(*tracks)[p] : nullptr;
        if (track != nullptr) {
            decodeTo(track, frame);
        }
        if (track != nullptr && frame < track->frames) {
            const size_t first = (size_t)frame * (size_t)track->vertexCount;
            out->push_back(std::vector<Vec3f>(track->vertices.begin() + (long)first,
                                              track->vertices.begin() +
                                                  (long)(first + (size_t)track->vertexCount)));
        } else {
            // The part has no track, or its track ran out: it keeps its model vertices.
            out->push_back(model.partList[p].vertices);
        }
    }
}

std::vector<std::vector<std::vector<Vec3f>>> ModelAnimation::bake(const Model& model) const {
    std::vector<DecodedTrack> tracks = beginTracks(model);
    std::vector<std::vector<std::vector<Vec3f>>> frames((size_t)frameCount);
    for (int32_t f = 0; f < frameCount; f++) {
        bakeFrame(model, &tracks, f, &frames[(size_t)f]);
    }
    return frames;
}

namespace {

// One decoded frame onto the end of a track's buffer.
void appendFrame(const std::vector<Vec3f>& vertices, ModelAnimation::DecodedTrack* track) {
    track->vertices.insert(track->vertices.end(), vertices.begin(), vertices.end());
    track->frames++;
}

} // namespace

// THE THREE TRACK LAYOUTS, read a frame at a time (decodeNext); each early end
// below is where the whole-track decode used to return with the frames it had.
//
// A delta track: the base vertices, then records of `s16 frame, s8 delta[n][3]`
// (ending with frame -1) adding `delta * scale` to the previous frame.
//
// A 1996 demo track: the base vertices, then `s8 delta[n][3]` for every next
// frame. There is no frame number: the deltas are in step.
//
// A matrix track (rigid parts): shifts, the base vertices, then a 3x4 s16 matrix
// per frame.
void ModelAnimation::beginTrack(int32_t offset, DecodedTrack* track) const {
    if (!bytes_ || offset < 0 || offset + 20 > byteCount_) {
        return;
    }
    const int32_t vertexCount = track->vertexCount;
    const uint32_t scaleBits = binU32(bytes_, offset + 16);
    if (beta_ || (scaleBits & kSignMask) != 0) {
        track->kind = beta_ ? DecodedTrack::Beta : DecodedTrack::Delta;
        track->scale = binF32(bytes_, offset + 16);
        BinReader reader(bytes_, byteCount_, offset + 20);
        if (!fits(reader, vertexCount * 12)) {
            return;
        }
        track->current.reserve((size_t)vertexCount);
        for (int32_t v = 0; v < vertexCount; v++) {
            track->current.push_back(readVec3(&reader));
        }
        track->vertices.reserve((size_t)vertexCount * (size_t)(frameCount > 0 ? frameCount : 1));
        appendFrame(track->current, track);
        if (track->kind == DecodedTrack::Delta) {
            if (!fits(reader, 2)) {
                return;
            }
            track->next = reader.s16();
        }
        track->at = reader.pos();
        track->ended = false;
        return;
    }

    track->kind = DecodedTrack::Matrix;
    if (offset + 22 > byteCount_) {
        return;
    }
    // The two bytes after the scale are the rotation and the position
    // precisions: the matrix values are scaled by 0x8000 shifted right by
    // them.
    //
    // THE SHIFT IS MASKED TO FIVE BITS, because that is what the C#'s
    // shift operator does with a count of 32 or more. Without the mask a
    // byte of 32 would be undefined behaviour in C++ where the C# gives a
    // plain shift of zero.
    const int32_t rotationBits = bytes_[offset + 20] & 31;
    const int32_t positionBits = bytes_[offset + 21] & 31;
    track->rotationScale = 1.0f / (float)(0x8000 >> rotationBits);
    track->positionScale = 1.0f / (float)(0x8000 >> positionBits);

    BinReader reader(bytes_, byteCount_, offset + 22);
    if (!fits(reader, vertexCount * 12)) {
        return;
    }
    track->base.reserve((size_t)vertexCount);
    for (int32_t v = 0; v < vertexCount; v++) {
        track->base.push_back(readVec3(&reader));
    }
    track->vertices.reserve((size_t)vertexCount * (size_t)(frameCount > 0 ? frameCount : 1));
    track->at = reader.pos();
    track->ended = false;
}

void ModelAnimation::decodeNext(DecodedTrack* track) const {
    const int32_t vertexCount = track->vertexCount;
    const int32_t f = track->frames;
    if (f >= frameCount) {
        track->ended = true;
        return;
    }
    BinReader reader(bytes_, byteCount_, track->at);

    if (track->kind == DecodedTrack::Matrix) {
        if (!fits(reader, kMatrixValues * 2)) {
            track->ended = true;
            return;
        }
        float m[kMatrixValues];
        for (int32_t i = 0; i < kMatrixValues; i++) {
            m[i] = (float)reader.s16();
        }
        const float rotationScale = track->rotationScale;
        const float positionScale = track->positionScale;
        for (int32_t v = 0; v < vertexCount; v++) {
            const Vec3f b = track->base[(size_t)v];
            track->vertices.push_back(Vec3f{
                (m[0] * b.x + m[1] * b.y + m[2] * b.z) * rotationScale + m[3] * positionScale,
                (m[4] * b.x + m[5] * b.y + m[6] * b.z) * rotationScale + m[7] * positionScale,
                (m[8] * b.x + m[9] * b.y + m[10] * b.z) * rotationScale + m[11] * positionScale,
            });
        }
        track->frames++;
        track->at = reader.pos();
        return;
    }

    // A delta track's frame without a record repeats the one before it; a demo
    // track has a record for every frame.
    const bool record = track->kind == DecodedTrack::Beta || track->next == f;
    if (record) {
        const int32_t recordBytes =
            track->kind == DecodedTrack::Beta ? vertexCount * 3 : vertexCount * 3 + 2;
        if (!fits(reader, recordBytes)) {
            track->ended = true;
            return;
        }
        const float scale = track->scale;
        std::vector<Vec3f>& vertices = track->current;
        for (int32_t v = 0; v < vertexCount; v++) {
            const float dx = (float)signedByte(reader.u8());
            const float dy = (float)signedByte(reader.u8());
            const float dz = (float)signedByte(reader.u8());
            vertices[(size_t)v].x += dx * scale;
            vertices[(size_t)v].y += dy * scale;
            vertices[(size_t)v].z += dz * scale;
        }
        if (track->kind == DecodedTrack::Delta) {
            track->next = reader.s16();
        }
        track->at = reader.pos();
    }
    appendFrame(track->current, track);
}

} // namespace formats
} // namespace mdk

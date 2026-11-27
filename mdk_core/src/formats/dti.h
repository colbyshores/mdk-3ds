#pragma once

// ============================================================================
// dti.h — LEVELn.DTI: the level's settings, its arena list and its palette.
//
// A 1:1 port of src/Mdk.Formats/Dti.cs.
//
// THE FILE:
//
//     block 0  settings: the start, the sky's fields, the GLASS1-4 colours
//     block 2  the arenas and corridors, with their object records
//     block 3  u32 arena colour count, then the 256-colour palette
//     block 4  the sky panorama(s)
//
// The block offsets are in a table at 4 + 0x10, and they are RELATIVE to
// the file WITHOUT its first 4 bytes -- so the absolute offset of a block
// is 4 + the table entry.
//
// THE SKY is a panorama, not a dome: a 2D image scrolled by the camera's
// yaw. Its width is the wrap width plus 4 EXTRA PIXELS, so that the
// panorama can wrap round without a seam.
//
// THE RECORDS hold a quirk that must be preserved: a record's "box end" is
// read as three floats and then the reader is rewound 12 bytes and the same
// 12 bytes are read as a NAME. The fans and wind zones keep their far
// corner in the name's bytes.
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

#include "formats/palette.h"
#include "formats/texture.h"

namespace mdk {
namespace formats {

// A level's settings file.
class Dti {
public:
    // The number of glass colours.
    static const int32_t kGlassCount = 4;

    // The panorama's rows hold 4 extra pixels for wrapping.
    static const int32_t kSkyWrapExtra = 4;

    // An object record in an arena.
    struct Record {
        uint32_t type = 0;
        int32_t id = 0;
        float angle = 0.0f;
        float position[3] = {0.0f, 0.0f, 0.0f};
        float boxEnd[3] = {0.0f, 0.0f, 0.0f};
        char name[13] = {0};
    };

    // An arena or a corridor. `pitch` is the camera pitch in degrees, and
    // it is POSITIVE when the level looks down.
    struct ArenaEntry {
        std::string name;
        float pitch = 0.0f;
        std::vector<Record> records;
    };

    Dti() = default;

    // Parse a DTI from a buffer. The buffer is borrowed.
    static Dti parse(const uint8_t* bytes, int32_t byteCount);

    const Palette& palette() const { return palette_; }
    Palette& mutablePalette() { return palette_; }

    // The index in arenas() of the starting arena.
    int32_t startArena() const { return startArena_; }

    // MDK coordinates. The angle is in degrees, and 90 faces +Y.
    const float* startPosition() const { return startPosition_; }
    float startAngle() const { return startAngle_; }

    // The sky panorama. The first skyWrapWidth() columns cover 360 degrees.
    const Texture& sky() const { return sky_; }
    int32_t skyWrapWidth() const { return skyWrapWidth_; }
    int32_t skyHorizonRow() const { return skyHorizonRow_; }
    int32_t skyOffset() const { return skyOffset_; }
    int32_t skyTopColor() const { return skyTopColor_; }
    int32_t skyBottomColor() const { return skyBottomColor_; }

    // The panorama the mirrors show: the second one in levels 5 and 6, and
    // the sky itself everywhere else.
    const Texture& mirrorSky() const { return mirrorSky_; }
    // True when mirrorSky() is the level's own second panorama, not a copy of
    // the sky (the C# `dti.MirrorSky != dti.Sky`, Viewer.CreatePanorama).
    bool hasOwnMirrorSky() const { return ownMirrorSky_; }

    // The RGBA8 of GLASS1-GLASS4.
    const std::vector<std::vector<uint32_t>>& glass() const { return glass_; }

    const std::vector<ArenaEntry>& arenas() const { return arenas_; }

    // The setters. The 1996 demo's level settings are TEXT files, and the
    // demo builds a Dti by hand rather than reading one, so the fields have
    // to be settable from outside. The C# has them as public fields, and
    // these are the same fields with the same names.
    void setStartArena(int32_t arena) { startArena_ = arena; }
    void setStartPosition(const float position[3]) {
        startPosition_[0] = position[0];
        startPosition_[1] = position[1];
        startPosition_[2] = position[2];
    }
    void setStartAngle(float angle) { startAngle_ = angle; }
    void setSkyTopColor(int32_t color) { skyTopColor_ = color; }
    void setSkyBottomColor(int32_t color) { skyBottomColor_ = color; }
    void setSkyHorizonRow(int32_t row) { skyHorizonRow_ = row; }
    void setSkyWrapWidth(int32_t width) { skyWrapWidth_ = width; }
    void setSky(const Texture& sky) { sky_ = sky; }
    void setMirrorSky(const Texture& sky) { mirrorSky_ = sky; }
    void setGlass(const std::vector<std::vector<uint32_t>>& glass) { glass_ = glass; }
    void setArenas(const std::vector<ArenaEntry>& arenas) { arenas_ = arenas; }

private:
    // The sky panorama strip at `offset`. The width is the wrap width plus
    // the extra pixels the wrap needs.
    Texture panorama(const uint8_t* bytes, int32_t byteCount, int32_t offset, int32_t height) const;

    // An arena's object records.
    std::vector<Record> parseRecords(const uint8_t* bytes, int32_t byteCount, int32_t offset) const;

    Palette palette_;
    int32_t startArena_ = 0;
    float startPosition_[3] = {0.0f, 0.0f, 0.0f};
    float startAngle_ = 0.0f;
    Texture sky_;
    int32_t skyWrapWidth_ = 0;
    int32_t skyHorizonRow_ = 0;
    int32_t skyOffset_ = 0;
    int32_t skyTopColor_ = 0;
    int32_t skyBottomColor_ = 0;
    Texture mirrorSky_;
    bool ownMirrorSky_ = false;
    std::vector<std::vector<uint32_t>> glass_;
    std::vector<ArenaEntry> arenas_;
};

} // namespace formats
} // namespace mdk

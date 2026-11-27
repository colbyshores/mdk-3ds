// ============================================================================
// dti.cpp — see dti.h.
// ============================================================================

#include "formats/dti.h"

#include <string.h>

#include "formats/bin_reader.h"

namespace mdk {
namespace formats {

namespace {

// The block table sits at 4 + 0x10, and the block offsets in it are
// RELATIVE to the file without its first 4 bytes.
const int32_t kBlockTable = 4 + 0x10;

int32_t blockOf(const uint8_t* bytes, int32_t byteCount, int32_t index) {
    const int32_t at = kBlockTable + index * 4;
    if (at < 0 || at + 4 > byteCount) {
        return -1;
    }
    return 4 + (int32_t)binU32(bytes, at);
}

} // namespace

Dti Dti::parse(const uint8_t* bytes, int32_t byteCount) {
    Dti dti;

    // The level's base palette, at block 3 plus a u32 colour count.
    const int32_t paletteBlock = blockOf(bytes, byteCount, 3);
    if (paletteBlock >= 0 && paletteBlock + 4 + Palette::kSize * 3 <= byteCount) {
        dti.palette_ = Palette::fromRgb(bytes + paletteBlock + 4, Palette::kSize * 3);
    }

    // Block 0: the settings.
    const int32_t settingsBlock = blockOf(bytes, byteCount, 0);
    if (settingsBlock >= 0 && settingsBlock + 4 <= byteCount) {
        BinReader reader(bytes, byteCount, settingsBlock);
        dti.startArena_ = (int32_t)reader.u32();
        reader.vec3(dti.startPosition_);
        dti.startAngle_ = reader.f32();
        dti.skyTopColor_ = (int32_t)reader.u32();
        dti.skyBottomColor_ = (int32_t)reader.u32();
        dti.skyHorizonRow_ = (int32_t)reader.u32();
        dti.skyOffset_ = (int32_t)reader.u32();
        dti.skyWrapWidth_ = (int32_t)reader.u32();
        const int32_t skyHeight = (int32_t)reader.u32();
        // Positive when the level has a second panorama, for the mirrors.
        const int32_t secondSkyTop = reader.s32();
        reader.s32();
        for (int32_t i = 0; i < kGlassCount; i++) {
            std::vector<uint32_t> glass;
            glass.push_back(reader.u32());
            glass.push_back(reader.u32());
            glass.push_back(reader.u32());
            glass.push_back(reader.u32());
            dti.glass_.push_back(glass);
        }

        // Block 4: the sky panorama(s).
        const int32_t skyBlock = blockOf(bytes, byteCount, 4);
        if (skyBlock >= 0) {
            dti.sky_ = dti.panorama(bytes, byteCount, skyBlock, skyHeight);
            const int32_t mirrorBlock = skyBlock + dti.sky_.width() * dti.sky_.height();
            dti.ownMirrorSky_ = secondSkyTop > 0;
            dti.mirrorSky_ = (secondSkyTop > 0)
                ? dti.panorama(bytes, byteCount, mirrorBlock, skyHeight)
                : dti.sky_;
        }
    }

    // Block 2: the arenas and corridors.
    const int32_t arenaBlock = blockOf(bytes, byteCount, 2);
    if (arenaBlock >= 0 && arenaBlock + 4 <= byteCount) {
        BinReader reader(bytes, byteCount, arenaBlock);
        const uint32_t count = reader.u32();
        for (uint32_t i = 0; i < count; i++) {
            ArenaEntry entry;
            char name[9];
            reader.name(name, 8);
            entry.name = name;
            // The records' offset is relative to the file WITHOUT its first
            // 4 bytes, like every other offset in the file.
            const int32_t recordsOffset = 4 + (int32_t)reader.u32();
            entry.pitch = reader.f32();
            entry.records = dti.parseRecords(bytes, byteCount, recordsOffset);
            dti.arenas_.push_back(entry);
        }
    }

    return dti;
}

Texture Dti::panorama(const uint8_t* bytes, int32_t byteCount, int32_t offset, int32_t height) const {
    Texture texture;
    texture.setName("SKY");
    const int32_t width = skyWrapWidth_ + kSkyWrapExtra;
    texture.setWidth(width);
    texture.setHeight(height);
    if (offset < 0 || width <= 0 || height <= 0) {
        return texture;
    }
    const int32_t size = width * height;
    if (offset + size > byteCount) {
        return texture;
    }
    texture.setIndices(bytes + offset, size);
    return texture;
}

std::vector<Dti::Record> Dti::parseRecords(const uint8_t* bytes, int32_t byteCount, int32_t offset) const {
    std::vector<Record> records;
    if (offset < 0 || offset + 4 > byteCount) {
        return records;
    }
    BinReader reader(bytes, byteCount, offset);
    const uint32_t count = reader.u32();
    for (uint32_t i = 0; i < count; i++) {
        Record record;
        record.type = reader.u32();
        record.id = reader.s32();
        record.angle = reader.f32();
        reader.vec3(record.position);
        // The box end is read as three floats and then the reader is
        // REWOUND 12 bytes, and the same 12 bytes are read as the name. The
        // fans and the wind zones keep their far corner in the name's bytes.
        // The rewind is a NEGATIVE skip, and it is deliberate.
        reader.vec3(record.boxEnd);
        reader.skip(-12);
        reader.name(record.name, 12);
        records.push_back(record);
    }
    return records;
}

} // namespace formats
} // namespace mdk

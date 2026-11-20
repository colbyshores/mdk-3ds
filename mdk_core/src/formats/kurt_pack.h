#pragma once

// ============================================================================
// kurt_pack.h -- Kurt's compressed frames in ONE file (romfs:/kurt/kurt.pak).
//
// WHY ONE FILE. The set (EnhancedToggles::kurtCompressed) used to be 408 loose
// .t3x files under romfs:/kurt, every one of a level's 360 opened and read at
// level load and then held on the main heap: measured on the Old 3DS,
// 1,122,760 bytes read in 4,557 ms, and those bytes kept for the level. The
// pack is the same .t3x bytes behind one index, so a level load is one open,
// one read of the header, and one read of the index and the START SET together
// (the start set is laid out contiguously at the front of the data); the other
// animations are read later, one contiguous range per animation, off the main
// thread (KurtSprite's stream).
//
// WRITTEN BY mdk_3ds/tools/kurt_pack.py (run by kurt_etc1_convert.py), which is
// the format's other half: change one, change both, and the host tests
// (tests/formats/kurt_pack_tests.cpp) round-trip the writer through this
// reader.
//
// THE FILE, little-endian throughout:
//
//   header (kHeaderBytes = 32)
//     0  char[4] magic "KPAK"
//     4  u16     version (kVersion: 2 since the entry carries the image size)
//     6  u16     header bytes (32)
//     8  u32     entry count
//     12 u32     entry bytes (44)
//     16 u32     data offset: the first frame's bytes (index end, aligned)
//     20 u32     start end: the end of the start set's bytes. The start set
//                (KurtSprite's K_STILL, K_FALL, K_LAND, K_IDLE, the chute's
//                K_CHUTE and K_CHUTEC, the jump's K_JUMP and K_RJMP: what
//                level load makes and keeps)
//                is [data offset, start end); == data offset when there is none
//     24 u32     file bytes: the whole file's size (a truncation is caught
//                against the file's real size before anything is read past it)
//     28 u32     alignment of every frame's offset (kAlign)
//   index (entry count x kEntryBytes = 44), sorted start set first, then by
//   animation, frame and level, so an animation's frames are contiguous:
//     0  char[24] frame name, NUL-padded ("K_RUN_12", the HD pack's key)
//     24 u8       level: 0 every level, else the one level whose palette shows
//                 the frame differently (K_LOOKD_0 on the GOG data)
//     25 u8       flags (kFlagStartSet)
//     26 u16      reserved, 0
//     28 u16      width, 30 u16 height: the frame's own size (the texture is
//                 the power-of-two pad of it)
//     32 u32      offset of the .t3x bytes (from the start of the file)
//     36 u32      size of the .t3x bytes
//     40 u16      image width, 42 u16 image height: the size of the picture
//                 INSIDE the texture, at its top-left. The frame's own size
//                 unless the converter fitted it to 256x256 (a frame whose pad
//                 would pass that: the chute's), then smaller, never larger. The
//                 runtime still draws the frame at width x height, through the
//                 image's rectangle (pica::compressedFrameRect), so its size on
//                 screen does not change
//   zero padding to the data offset, then each frame's .t3x at its offset,
//   zero-padded to the next kAlign boundary.
//
// A BAD HEADER rejects the whole pack (every frame takes the 16-bit path); a
// BAD ENTRY rejects that entry only (its frame takes the 16-bit path), and the
// first one is named for the log.
// ============================================================================

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace mdk {
namespace formats {

class KurtPack {
public:
    static constexpr uint16_t kVersion = 2;
    static constexpr uint32_t kHeaderBytes = 32;
    static constexpr uint32_t kEntryBytes = 44;
    static constexpr uint32_t kNameBytes = 24;
    static constexpr uint32_t kAlign = 16;
    // Bounds no honest pack comes near (408 frames; the largest is 6 KB): what
    // they refuse is a corrupt count or size, before it sizes an allocation.
    static constexpr uint32_t kMaxEntries = 4096;
    static constexpr uint32_t kMaxFrameBytes = 1u << 20;
    static constexpr uint32_t kMaxFileBytes = 64u << 20;
    static constexpr uint8_t kFlagStartSet = 1;

    enum class Status {
        Ok,
        TooSmall,      // fewer bytes than a header (or than the index it names)
        BadMagic,
        BadVersion,
        BadLayout,     // header/entry sizes, alignment, or offsets out of order
        Truncated,     // the file is shorter than the header says it is
    };

    struct Header {
        uint16_t version = 0;
        uint32_t entryCount = 0;
        uint32_t dataOffset = 0;
        uint32_t startEnd = 0;
        uint32_t fileBytes = 0;
        uint32_t align = 0;
        // Where the index ends: header + count x entry.
        uint32_t indexEnd() const { return kHeaderBytes + entryCount * kEntryBytes; }
    };

    struct Entry {
        std::string name;
        uint8_t level = 0;
        uint8_t flags = 0;
        uint16_t width = 0;
        uint16_t height = 0;
        uint32_t offset = 0;
        uint32_t size = 0;
        // The image's size inside the texture (0 < imageWidth <= width, the same
        // for the height): the frame's own size unless it was fitted to 256x256.
        uint16_t imageWidth = 0;
        uint16_t imageHeight = 0;
        // False when the entry failed validation: its frame is drawn from the
        // index texture. The entry is still listed, so a level's own copy that
        // is bad is not replaced by the every-level one (its colours differ).
        bool valid = false;
    };

    // The header's 32 bytes, checked against the file's real size. On Ok `out`
    // holds the header; the index is the next count x 40 bytes.
    static Status parseHeader(const uint8_t* bytes, size_t size, uint32_t actualFileBytes,
                              Header* out);

    // The index (`size` bytes from offset kHeaderBytes) for a header parseHeader
    // accepted. Only a short index is an error here; each entry is validated on
    // its own (Entry::valid).
    static Status parseIndex(const Header& header, const uint8_t* index, size_t size,
                             KurtPack* out);

    // A whole pack in memory (the tests' form): header and index together.
    static Status parse(const uint8_t* bytes, size_t size, KurtPack* out);

    const Header& header() const { return header_; }
    const std::vector<Entry>& entries() const { return entries_; }
    // Entries that failed validation, and the first of them (-1: none) and why.
    int32_t badEntries() const { return badEntries_; }
    int32_t firstBadEntry() const { return firstBad_; }
    const char* firstBadReason() const { return firstBadReason_; }

    // The entry for a frame name on `level`: the level's own copy first, then
    // the every-level one. -1 when there is none -- or when the level's own
    // copy is listed but invalid (falling back to the every-level copy would
    // draw the wrong palette's colours). The caller still checks `valid`.
    int32_t find(const std::string& name, int32_t level) const;

    static const char* statusName(Status status);

private:
    Header header_;
    std::vector<Entry> entries_;
    int32_t badEntries_ = 0;
    int32_t firstBad_ = -1;
    const char* firstBadReason_ = "";
};

} // namespace formats
} // namespace mdk

#pragma once

// ============================================================================
// fti.h — MISC/MDKFONT.FTI: interface texts, fonts, palette and sounds.
//
// A 1:1 port of src/Mdk.Formats/Fti.cs.
//
// THE FILE:
//
//   u32 size, u32 count, then per entry:
//     char[8] name, u32 offset (from 4)
//
// The offsets are SORTED, and each entry's size runs to the next offset, or
// to the end of the file for the last one.
//
// The texts (OPT0-OPT4, OM_*, KM_*) are NUL-terminated and use a two
// character "\n" for a newline. SYS_PAL is the interface's 64 colours,
// FONTBIG and FONTSML are the fonts, and SND_PUSH is a WAV.
//
// ARCHIVEINDEX IS HERE, NOT IN ITS OWN FILE, because the C# keeps it in
// Fti.cs: the FTI and the BNI share it, and it is the BNI's directory that
// has twelve-character names where the FTI's have eight.
//
// THE INDEX OWNS THE BYTES. The C#'s ArchiveIndex holds the byte[] and
// appends to it, and the objects the archive hands out -- a sprite
// animation, whose frames are decoded on first use -- read from that array
// later. So the array has to stay alive and still be the same array, and
// the C++'s equivalent is a vector the index owns.
//
// The cost is that reading an archive copies the file once. The move
// overloads below take the caller's buffer instead, so the game can hand
// over the file it just read without a second copy of it.
// ============================================================================

#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

namespace mdk {
namespace formats {

// The directory shared by the FTI and the BNI: `u32 size, u32 count`, then
// per entry a fixed-length name and a `u32 offset` (from 4). The sizes run
// to the next offset.
class ArchiveIndex {
public:
    struct Entry {
        int32_t offset = 0;
        int32_t size = 0;
    };

    // Read an index over `bytes`. `nameLength` is the length of the entry
    // names: eight for the FTI, twelve for the BNI. The bytes are copied.
    //
    // Returns false when the data runs out.
    static bool read(ArchiveIndex* out, const uint8_t* bytes, int32_t byteCount, int32_t nameLength);

    // Read an index over `bytes`, taking the caller's buffer over. No copy.
    static bool read(ArchiveIndex* out, std::vector<uint8_t>&& bytes, int32_t nameLength);

    // The bytes the index is over.
    const std::vector<uint8_t>& bytes() const { return bytes_; }
    int32_t byteCount() const { return (int32_t)bytes_.size(); }

    // The entries, in the order the file has them: by offset. A name that
    // appears twice keeps the LAST one's offset and size, and its place in
    // the list, which is what the C#'s dictionary does.
    const std::vector<std::pair<std::string, Entry>>& entries() const { return entries_; }

    // The entry with this name, or null.
    const Entry* find(const std::string& name) const;

    // Adds or replaces an entry, its bytes after the others.
    //
    // THE BYTES MOVE. Anything that borrows them -- a sprite animation from
    // the archive -- must not be used after this. The C# has the same
    // hazard in a different shape: its Add replaces the byte array, and the
    // animations keep reading the old one.
    void add(const std::string& name, const uint8_t* data, int32_t size);

    // The entry's bytes. Returns false when there is no such entry.
    bool getBytes(const std::string& name, std::vector<uint8_t>* out) const;

private:
    std::vector<uint8_t> bytes_;

    // The entries, by offset. A vector, not a map, because the C#'s
    // dictionary keeps the order the names were added in and overwrites a
    // name's value in place; this does the same.
    std::vector<std::pair<std::string, Entry>> entries_;
};

// MISC/MDKFONT.FTI: interface texts, fonts, palette and sounds.
class Fti {
public:
    // Read an FTI. The bytes are copied. Returns false when the data runs
    // out.
    static bool parse(Fti* out, const uint8_t* bytes, int32_t byteCount);

    // Read an FTI, taking the caller's buffer over. No copy.
    static bool parse(Fti* out, std::vector<uint8_t>&& bytes);

    const std::vector<uint8_t>& bytes() const { return index_.bytes(); }

    const ArchiveIndex& index() const { return index_; }

    bool has(const std::string& name) const;

    // The entry's bytes. Returns false when there is no such entry.
    bool getBytes(const std::string& name, std::vector<uint8_t>* out) const;

    // A text entry as bytes (the fonts' character set, the "\n" escapes
    // kept), or empty.
    std::vector<uint8_t> getTextBytes(const std::string& name) const;

    // A text entry, or `fallback`. The two character "\n" becomes a
    // newline.
    std::string getText(const std::string& name, const std::string& fallback = std::string()) const;

private:
    ArchiveIndex index_;
};

} // namespace formats
} // namespace mdk

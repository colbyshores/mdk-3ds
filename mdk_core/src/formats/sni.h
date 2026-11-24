#pragma once

// ============================================================================
// sni.h — an SNI archive: TRAVERSE.SNI, LEVELnS.SNI, LEVELnO.SNI.
//
// A 1:1 port of src/Mdk.Formats/Sni.cs.
//
// THE FILE:
//
//   header, u32 count, then per entry:
//     char[12] name, u16 flags, u16 volume, u32 offset (from 4), u32 length
//
// Flag 1 loops, flag 2 marks music. Most entries are RIFF WAV; in
// LEVELnO.SNI the corridors' (CHMO_n...) world geometry.
//
// THE 1996 DEMO'S ARCHIVES have no header: the count is at 4 instead of
// 0x14, and their names are in lower case, so they are stored in UPPER CASE
// here, as the C# does.
//
// TWO WAYS TO HOLD ONE (docs/ARENA_STREAMING_SPEC.md Phase 5). parse() keeps
// the whole file, as the C# does. open() keeps the DIRECTORY ONLY and reads an
// entry's bytes from the file when they are asked for -- which is what the
// original does with LEVELnO.SNI: its directory stays in memory (0x57f378,
// count 0x57f374) and a music track or a corridor is read from the open file
// when its arena loads (0x431590 / 0x4317d8, 0x431914). A level's archives are
// 4.4-5.8 MB of which a frame plays a few hundred KB, so on the 3DS the bank
// opens them. Both give the same entries and the same bytes; the format tests
// compare every entry of every retail archive both ways.
//
// THE ARCHIVE CANNOT BE COPIED. The sprite animations it hands out read its
// bytes when their frames are decoded, which happens on first use, and a
// copy of the archive would have its own bytes while the animations still
// pointed at the original's.
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

#include <map>

#include "formats/mdk_data.h"
#include "formats/sprite_animation.h"

namespace mdk {
namespace formats {

// An SNI archive.
class Sni {
public:
    // The flag of a looping sound.
    static const int32_t kFlagLoop = 1;

    // The flag of a music track.
    static const int32_t kFlagMusic = 2;

    // More than any entry of an archive is (the largest retail one is a 512 KB
    // music track; the original refuses a track pair past 0x7d000). An opened
    // archive knows its file only by its directory, so a read of more than
    // this is refused BEFORE a buffer is sized for it: with no exceptions, an
    // allocation a lying directory asks for is an abort.
    static const int32_t kMaxStreamedEntryBytes = 8 << 20;

    // One entry of the archive.
    struct Entry {
        // The name: twelve bytes, NUL-padded. The 1996 demo's names are
        // stored in upper case.
        char name[13] = {0};

        int32_t offset = 0;
        // The entry's length -- of a sound or a corridor. A SPRITE ANIMATION's
        // is 0xFFFFFFFF in the file (-1 here): its size is the u32 the entry
        // starts with (see Sni::animation).
        int32_t length = 0;
        int32_t flags = 0;
        int32_t volume = 0;

        // Whether the entry's bytes begin "RIFF" (a WAV). Read once, when the
        // archive is parsed or opened, so asking costs no I/O.
        bool riff = false;

        // The name as a string, up to the first NUL.
        std::string nameString() const;
    };

    // Read an SNI. The bytes are copied. Returns false when the data runs
    // out.
    static bool parse(Sni* out, const uint8_t* bytes, int32_t byteCount);

    // Read an SNI, taking the caller's buffer over. No copy.
    static bool parse(Sni* out, std::vector<uint8_t>&& bytes);

    // Open a retail archive BY ITS DIRECTORY: only the entries are resident,
    // and getBytes() / animation() read the file. The archive keeps its own
    // copy of `data` (a file-system pointer and a folder name), so only the
    // FileSystem has to outlive it. False when the file cannot be read or is
    // not an SNI.
    static bool open(Sni* out, const MdkData& data, const std::string& path);

    // Whether this archive was open()ed (its bytes are in the file, not here).
    bool streamed() const { return streamed_; }
    // An open()ed archive's file, resolved (empty for a parsed one): the music
    // prefetch hands it to a file thread with an entry's range.
    const std::string& path() const { return path_; }

    // Read exactly `length` bytes of an entry starting `at` bytes into it (the
    // bank's stepped prefetch). False past the entry or on a short read.
    bool readEntry(const Entry& entry, int32_t at, int32_t length, std::vector<uint8_t>* out) const;

    // Reads that failed since the archive was opened.
    int32_t readFailures() const { return readFailures_; }

    // Read an archive of the 1996 demo. The C# keeps this internal: it is
    // for the demo's files.
    static bool parseBeta(Sni* out, const uint8_t* bytes, int32_t byteCount);
    static bool parseBeta(Sni* out, std::vector<uint8_t>&& bytes);

    // The default constructor: parse() takes the SNI as an out parameter,
    // so one has to be creatable.
    Sni() = default;

    Sni(const Sni&) = delete;
    Sni& operator=(const Sni&) = delete;
    Sni(Sni&&) = default;
    Sni& operator=(Sni&&) = default;

    // The whole file's bytes; EMPTY for an archive that was open()ed.
    const std::vector<uint8_t>& bytes() const { return bytes_; }

    // The entries, in file order.
    const std::vector<Entry>& entries() const { return entries_; }

    // Is the entry a RIFF WAV?
    bool isSound(const Entry& entry) const;

    // The entry's bytes.
    bool getBytes(const Entry& entry, std::vector<uint8_t>* out) const;

    // A sprite animation stored in the archive (LEVEL4S.SNI's K_SURF).
    // Returns false when there is no such entry, or when it is a sound.
    bool animation(const std::string& name, SpriteAnimation* out) const;

private:
    // Read the directory. `directory` is where the count is.
    static bool read(Sni* out, std::vector<uint8_t>&& bytes, int32_t directory, bool beta);

    std::vector<uint8_t> bytes_;
    std::vector<Entry> entries_;

    // An open()ed archive: where its bytes are.
    bool streamed_ = false;
    MdkData data_;
    std::string path_;   // resolved once (MdkData::pathOf)
    mutable int32_t readFailures_ = 0;
    // The entries a sprite animation was made from: an animation reads its
    // bytes when its frames are decoded, so an open()ed archive keeps those
    // entries (and only those) for its life. A map node's vector never moves.
    mutable std::map<std::string, std::vector<uint8_t>> held_;
};

} // namespace formats
} // namespace mdk

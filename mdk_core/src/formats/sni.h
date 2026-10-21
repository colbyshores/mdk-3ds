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
// THE ARCHIVE CANNOT BE COPIED. The sprite animations it hands out read its
// bytes when their frames are decoded, which happens on first use, and a
// copy of the archive would have its own bytes while the animations still
// pointed at the original's.
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

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

    // One entry of the archive.
    struct Entry {
        // The name: twelve bytes, NUL-padded. The 1996 demo's names are
        // stored in upper case.
        char name[13] = {0};

        int32_t offset = 0;
        int32_t length = 0;
        int32_t flags = 0;
        int32_t volume = 0;

        // The name as a string, up to the first NUL.
        std::string nameString() const;
    };

    // Read an SNI. The bytes are copied. Returns false when the data runs
    // out.
    static bool parse(Sni* out, const uint8_t* bytes, int32_t byteCount);

    // Read an SNI, taking the caller's buffer over. No copy.
    static bool parse(Sni* out, std::vector<uint8_t>&& bytes);

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
};

} // namespace formats
} // namespace mdk

#pragma once

// ============================================================================
// sound_bank.h — a level's sounds by name, from its SNI archives.
//
// A 1:1 port of src/Mdk.Game/Audio/SoundBank.cs: TRAVERSE.SNI, LEVELnS.SNI and
// LEVELnO.SNI (later archives win), looked up by the exact name a script asks
// for. The C# caches `null` for a name it could not load, so a missing name is
// remembered too — a repeated lookup of a name the level does not have costs
// one map probe, not a re-scan of three archives.
//
// METADATA AND SAMPLES. The C# Entry carries the decoded `Sound` (Sound.FromPcm)
// and the script-visible half of a sound is its existence, its default volume
// (the SNI entry's, 0-0x7FFF) and whether it loops (SNI flag 1). What the mixer
// and the platform backend need is both halves, so the port keeps both: the
// metadata by value in the Entry, and `sound` as a pointer to the bank's own
// decoded copy. The C# gets the same sharing for free because a Sound is a heap
// object there; here the bank owns a map of them and a map node never moves, so
// the pointer is good for the bank's life and copying an Entry stays free.
//
// The geometry the backend needs — channels, sample rate and frame count — is
// computed exactly as Sound.FromPcm divides the data chunk (8-bit: one byte per
// sample; otherwise two, over the even part of the data), then divided by the
// channels as Sound.Frames does. It is kept beside the pointer rather than
// reached through it because the voice table reads it every frame.
//
// A name whose WAV will not parse is NOT found, as in the C# (`Wav.Parse`
// returns null and Get caches null).
//
// THE MEMORY, HONESTLY, AND MEASURED. Decoding level 3's three archives to float is
// 3,234,976 samples = 12.34 MB from 4,390,060 bytes on disk (72 sounds; counted by
// the oracle probe, not estimated) — against a 3DS with 16 MB of FCRAM. Nothing
// calls preload() on the 3DS today, and the ndsp batch has to decide whether to keep
// the file's 8/16-bit bytes and convert on read, decode on demand with a budget, or
// stream; that is recorded as an open item, not solved by pretending the numbers are
// smaller.
// ============================================================================

#include <stdint.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "audio/sound.h"
#include "formats/sni.h"

namespace mdk {
namespace formats {
class MdkData;
} // namespace formats

namespace audio {

class SoundBank {
public:
    // SoundMixer.Entry: the sound's samples and metadata and its default volume.
    struct Entry {
        int32_t volume = 0;  // 0-0x7FFF, from the SNI entry
        bool looping = false;
        int32_t channels = 0;
        int32_t sampleRate = 0;
        int32_t frames = 0;  // samples per channel (Sound.Frames)
        // The decoded samples, owned by the bank that made this Entry. Null only
        // for an Entry that was never filled from an archive.
        const Sound* sound = nullptr;
    };

    // An Entry's `sound` points into THIS bank's `sounds_`, so a bank that was copied
    // or moved would go on handing out pointers into the one that was left behind.
    // Nothing copies one today — every user holds a bank by pointer or as a local —
    // and deleting the copy is what keeps that true instead of hoping it stays true.
    SoundBank() = default;
    SoundBank(const SoundBank&) = delete;
    SoundBank& operator=(const SoundBank&) = delete;

    // The three archives a level plays from. FALSE when one cannot be read or
    // parsed (the C# throws there).
    static bool forLevel(SoundBank* out, const formats::MdkData& data, int32_t level);

    // A bank over archives the caller already has (the 1996 demo's).
    static bool withArchives(SoundBank* out, std::vector<formats::Sni>&& archives);

    // Get(name): the sound, or false when the level has no such sound. The name
    // is matched exactly, as the C# does — a script that asks for a lower-case
    // name the archives do not carry gets nothing.
    bool get(const std::string& name, Entry* out);

    // A sprite animation stored with the sounds (the snowboard's K_SURF), or
    // false. Later archives win, exactly as the C# SoundBank.Animation does.
    bool animation(const std::string& name, formats::SpriteAnimation* out);

    // Convert every sound of the archives now (a level's load: none while
    // playing). Here that means reading every WAV header.
    void preload();

    int32_t archiveCount() const { return (int32_t)archives_.size(); }

private:
    // Look the name up in the archives, newest first.
    bool load(const std::string& name, Entry* out);

    std::vector<formats::Sni> archives_;
    std::map<std::string, Entry> cache_;
    // The decoded samples, one per cached name. A std::map node never moves, so
    // the Entry's `sound` pointer stays good however many names are added after
    // it — which is the sharing the C# gets from Sound being a reference type.
    std::map<std::string, Sound> sounds_;
    std::set<std::string> missing_;
};

} // namespace audio
} // namespace mdk

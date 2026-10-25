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
// METADATA ONLY. The C# Entry carries the decoded PCM (Sound.FromPcm). The
// script-visible half of a sound is its existence, its default volume (the SNI
// entry's, 0-0x7FFF) and whether it loops (SNI flag 1); the samples belong to
// the platform audio backend, which is not in mdk_core yet. What the port does
// keep is the geometry the backend will need and the mixer can already use:
// channels, sample rate and frame count, computed exactly as Sound.FromPcm
// divides them (8-bit: one byte per sample; otherwise two, over the even part
// of the data chunk).
//
// A name whose WAV will not parse is NOT found, as in the C# (`Wav.Parse`
// returns null and Get caches null).
// ============================================================================

#include <stdint.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "formats/sni.h"

namespace mdk {
namespace formats {
class MdkData;
} // namespace formats

namespace audio {

class SoundBank {
public:
    // SoundMixer.Entry: the sound's metadata and its default volume.
    struct Entry {
        int32_t volume = 0;  // 0-0x7FFF, from the SNI entry
        bool looping = false;
        int32_t channels = 0;
        int32_t sampleRate = 0;
        int32_t frames = 0;  // samples per channel (Sound.Frames)
    };

    // The three archives a level plays from. FALSE when one cannot be read or
    // parsed (the C# throws there).
    static bool forLevel(SoundBank* out, const formats::MdkData& data, int32_t level);

    // A bank over archives the caller already has (the 1996 demo's).
    static bool withArchives(SoundBank* out, std::vector<formats::Sni>&& archives);

    // Get(name): the sound, or false when the level has no such sound. The name
    // is matched exactly, as the C# does — a script that asks for a lower-case
    // name the archives do not carry gets nothing.
    bool get(const std::string& name, Entry* out);

    // Convert every sound of the archives now (a level's load: none while
    // playing). Here that means reading every WAV header.
    void preload();

    int32_t archiveCount() const { return (int32_t)archives_.size(); }

private:
    // Look the name up in the archives, newest first.
    bool load(const std::string& name, Entry* out);

    std::vector<formats::Sni> archives_;
    std::map<std::string, Entry> cache_;
    std::set<std::string> missing_;
};

} // namespace audio
} // namespace mdk

// ============================================================================
// sound_bank.cpp — a level's sounds by name (port of SoundBank.cs).
// ============================================================================

#include "audio/sound_bank.h"

#include <stdio.h>

#include "formats/mdk_data.h"
#include "formats/wav.h"

namespace mdk {
namespace audio {

bool SoundBank::forLevel(SoundBank* out, const formats::MdkData& data, int32_t level) {
    char prefix[96];
    snprintf(prefix, sizeof(prefix), "TRAVERSE/LEVEL%d/LEVEL%d", (int)level, (int)level);
    const std::string paths[3] = {
        std::string("TRAVERSE/TRAVERSE.SNI"),
        std::string(prefix) + "S.SNI",
        std::string(prefix) + "O.SNI",
    };

    std::vector<formats::Sni> archives;
    for (int i = 0; i < 3; i++) {
        std::vector<uint8_t> bytes;
        if (!data.read(paths[i], &bytes)) {
            return false;
        }
        formats::Sni archive;
        if (!formats::Sni::parse(&archive, std::move(bytes))) {
            return false;
        }
        archives.push_back(std::move(archive));
    }
    return withArchives(out, std::move(archives));
}

bool SoundBank::withArchives(SoundBank* out, std::vector<formats::Sni>&& archives) {
    out->archives_ = std::move(archives);
    out->cache_.clear();
    out->sounds_.clear();
    out->missing_.clear();
    out->pins_.clear();
    out->recent_.clear();
    return true;
}

bool SoundBank::get(const std::string& name, Entry* out) {
    std::map<std::string, Entry>::const_iterator found = cache_.find(name);
    if (found != cache_.end()) {
        touch(name);
        if (out != nullptr) {
            *out = found->second;
        }
        return true;
    }
    if (missing_.find(name) != missing_.end()) {
        return false;
    }

    Entry entry;
    if (!load(name, &entry)) {
        missing_.insert(name);
        return false;
    }
    cache_[name] = entry;
    touch(name);
    evictOverBudget(name);
    if (out != nullptr) {
        *out = entry;
    }
    return true;
}

void SoundBank::touch(const std::string& name) {
    for (std::deque<std::string>::iterator it = recent_.begin(); it != recent_.end(); ++it) {
        if (*it == name) {
            recent_.erase(it);
            break;
        }
    }
    recent_.push_back(name);
}

void SoundBank::retain(const std::string& name) {
    if (sounds_.find(name) != sounds_.end()) {
        pins_[name]++;
    }
}

void SoundBank::release(const std::string& name) {
    std::map<std::string, int32_t>::iterator found = pins_.find(name);
    if (found == pins_.end()) {
        return;
    }
    if (--found->second <= 0) {
        pins_.erase(found);
    }
}

int32_t SoundBank::pins(const std::string& name) const {
    std::map<std::string, int32_t>::const_iterator found = pins_.find(name);
    return found != pins_.end() ? found->second : 0;
}

void SoundBank::evictOverBudget(const std::string& keep) {
    if (budgetBytes_ == 0) {
        return;
    }
    while (decodedBytes() > budgetBytes_) {
        std::string victim;
        for (size_t i = 0; i < recent_.size(); i++) {
            const std::string& name = recent_[i];
            if (name != keep && pins(name) == 0 && sounds_.find(name) != sounds_.end()) {
                victim = name;
                break;
            }
        }
        if (victim.empty()) {
            return;  // everything resident is pinned or just decoded
        }
        sounds_.erase(victim);
        cache_.erase(victim);
        for (std::deque<std::string>::iterator it = recent_.begin(); it != recent_.end(); ++it) {
            if (*it == victim) {
                recent_.erase(it);
                break;
            }
        }
    }
}

void SoundBank::preload() {
    for (size_t i = 0; i < archives_.size(); i++) {
        const std::vector<formats::Sni::Entry>& entries = archives_[i].entries();
        for (size_t e = 0; e < entries.size(); e++) {
            if (archives_[i].isSound(entries[e])) {
                Entry unused;
                get(entries[e].nameString(), &unused);
            }
        }
    }
}

size_t SoundBank::decodedSamples() const {
    size_t n = 0;
    for (std::map<std::string, Sound>::const_iterator it = sounds_.begin(); it != sounds_.end();
         ++it) {
        n += it->second.samples.size();
    }
    return n;
}

size_t SoundBank::decodedBytes() const {
    // The element size is read from the store itself, so this number is the
    // store's width and not a comment about it: a store that changed width
    // changes this without anything having to remember to update it.
    size_t n = 0;
    for (std::map<std::string, Sound>::const_iterator it = sounds_.begin(); it != sounds_.end();
         ++it) {
        n += it->second.samples.size() * sizeof(Sound::Samples::value_type);
    }
    return n;
}

size_t SoundBank::archiveBytes() const {
    size_t n = 0;
    for (size_t i = 0; i < archives_.size(); i++) {
        n += archives_[i].bytes().size();
    }
    return n;
}

bool SoundBank::animation(const std::string& name, formats::SpriteAnimation* out) {
    if (!out) {
        return false;
    }
    // Newest archive first, exactly as the C# SoundBank.Animation does.
    for (int i = (int)archives_.size() - 1; i >= 0; i--) {
        if (archives_[(size_t)i].animation(name, out)) {
            return true;
        }
    }
    return false;
}

// Newest archive first, as the C#'s `for (var i = archives.Count - 1; ...)`.
// An entry of that name that is not a sound is skipped and the search continues
// in the older archives — it does not end the lookup. The winning entry is found
// in the walk and decoded after it, so the frame-count division (runtime
// divisor: the channel count) runs once per name, not inside the archive loop.
bool SoundBank::load(const std::string& name, Entry* out) {
    const formats::Sni::Entry* found = nullptr;
    std::vector<uint8_t> bytes;
    for (int i = (int)archives_.size() - 1; i >= 0; i--) {
        const formats::Sni& candidate = archives_[i];
        const std::vector<formats::Sni::Entry>& entries = candidate.entries();
        const formats::Sni::Entry* entry = nullptr;
        for (size_t e = 0; e < entries.size(); e++) {
            if (entries[e].nameString() == name) {
                entry = &entries[e];
                break;
            }
        }
        if (entry == nullptr || !candidate.isSound(*entry)) {
            continue;
        }
        if (!candidate.getBytes(*entry, &bytes)) {
            continue;
        }
        found = entry;
        break;
    }
    if (found == nullptr) {
        return false;
    }

    formats::Wav wav;
    if (!formats::Wav::parse(bytes.data(), (int32_t)bytes.size(), &wav)) {
        return false;  // C#: Wav.Parse null here means Get caches null
    }

    out->volume = found->volume;
    out->looping = (found->flags & formats::Sni::kFlagLoop) != 0;
    out->channels = wav.channels();
    out->sampleRate = wav.sampleRate();
    out->frames = wav.frames();

    // Sound.FromPcm, into the bank's own copy so every Entry of this name points
    // at one decode. The C# does the same by handing out the same reference.
    Sound& sound = sounds_[name];
    soundFromPcm(wav.data(), wav.dataSize(), wav.channels(), wav.sampleRate(),
                 wav.bitsPerSample(), out->looping, &sound);
    out->sound = &sound;
    return true;
}

} // namespace audio
} // namespace mdk

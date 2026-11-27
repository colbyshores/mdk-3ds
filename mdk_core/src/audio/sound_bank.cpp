// ============================================================================
// sound_bank.cpp — a level's sounds by name (port of SoundBank.cs).
// ============================================================================

#include "audio/sound_bank.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "formats/mdk_data.h"
#include "formats/wav.h"

namespace mdk {
namespace audio {

namespace {

// An arena may name at most this many sounds (the original registers 16,
// 0x4310cc); a count past it is not walked.
const uint32_t kMaxArenaSounds = 64;
// The SNI layout the arenas' sounds are gathered into (formats/sni.h): the
// directory's count at 0x14, then 24-byte entries (name[12], flags u16,
// volume u16, offset - 4, length).
const int32_t kSniDirectory = 0x14;
const int32_t kSniEntryBytes = 24;
const int32_t kArenaDirectoryEntryBytes = 12;   // name[8], offset

uint32_t le32(const std::vector<uint8_t>& b, size_t at) {
    return (uint32_t)b[at] | (uint32_t)b[at + 1] << 8 | (uint32_t)b[at + 2] << 16 | (uint32_t)b[at + 3] << 24;
}

void put32(std::vector<uint8_t>* b, size_t at, uint32_t v) {
    (*b)[at] = (uint8_t)v;
    (*b)[at + 1] = (uint8_t)(v >> 8);
    (*b)[at + 2] = (uint8_t)(v >> 16);
    (*b)[at + 3] = (uint8_t)(v >> 24);
}

bool inArchives(const std::vector<formats::Sni>& archives, const std::string& name) {
    for (size_t a = 0; a < archives.size(); a++) {
        const std::vector<formats::Sni::Entry>& entries = archives[a].entries();
        for (size_t e = 0; e < entries.size(); e++) {
            // As SoundBank.Get: an entry that is not a sound (a sprite
            // animation) leaves the name to the arenas.
            if (entries[e].nameString() == name && archives[a].isSound(entries[e])) {
                return true;
            }
        }
    }
    return false;
}

// THE ARENAS' OWN SOUNDS (upstream 683a53ef, SoundBank.ArenaSounds). Some sounds
// live in the arenas of LEVELnO.MTO and in no SNI archive -- 85 names across the
// six levels: LEVEL7's barricade soldiers' SING and TAUNT1 (DANT_2), LEVEL3's
// tower boss's gun, LEVEL4's submarine -- and without them they played nothing.
// The ones whose names no archive has (the first arena's when two share a name)
// are read here, at the level's load, and gathered into one more SNI-shaped
// archive (no name in it is in the others, so the search order does not
// matter): played once, at full volume, as the C#
// plays them. Only their own bytes are read (the MTO is 10 MB): 185-566 KB a
// level, measured on the retail data. False (no archive) when the MTO cannot be
// read; the level's other sounds play as before.
bool arenaSounds(const formats::MdkData& data, int32_t level, const std::vector<formats::Sni>& archives,
                 formats::Sni* out) {
    char relative[96];
    snprintf(relative, sizeof(relative), "TRAVERSE/LEVEL%d/LEVEL%dO.MTO", (int)level, (int)level);
    const std::string path = data.pathOf(relative);
    std::vector<uint8_t> head;
    if (path.empty() || !data.readRangeAt(path, kSniDirectory, 4, &head)) {
        return false;
    }
    const uint32_t arenaCount = le32(head, 0);
    std::vector<uint8_t> directory;
    if (arenaCount == 0 || arenaCount > 256 ||
        !data.readRangeAt(path, kSniDirectory + 4, (int32_t)arenaCount * kArenaDirectoryEntryBytes, &directory)) {
        return false;
    }

    struct Found {
        std::string name;
        int32_t offset;
        int32_t length;
    };
    std::vector<Found> found;
    for (uint32_t a = 0; a < arenaCount; a++) {
        // Arena::parse's layout: its size field, then the buffer the offsets are
        // relative to, whose first word is the models section's offset; the
        // section's counts are 4 bytes into it.
        const uint32_t arena = le32(directory, (size_t)a * kArenaDirectoryEntryBytes + 8);
        std::vector<uint8_t> word;
        if (arena > 0x7FFFFF00u || !data.readRangeAt(path, (int32_t)arena + 4, 4, &word)) {
            continue;
        }
        const uint64_t base = (uint64_t)arena + 4 + le32(word, 0) + 4;
        std::vector<uint8_t> counts;
        if (base > 0x7FFFFF00u || !data.readRangeAt(path, (int32_t)base, 12, &counts)) {
            continue;
        }
        const uint64_t animations = le32(counts, 0);
        const uint64_t models = le32(counts, 4);
        const uint32_t sounds = le32(counts, 8);
        const uint64_t table = base + 12 + 12 * (animations + models);
        std::vector<uint8_t> entries;
        if (sounds == 0 || sounds > kMaxArenaSounds || table > 0x7FFFFF00u ||
            !data.readRangeAt(path, (int32_t)table, (int32_t)sounds * kSniEntryBytes, &entries)) {
            continue;
        }
        for (uint32_t i = 0; i < sounds; i++) {
            const size_t at = (size_t)i * kSniEntryBytes;
            char name[13] = {0};
            memcpy(name, entries.data() + at, 12);
            const uint64_t offset = base + le32(entries, at + 16);
            const uint32_t length = le32(entries, at + 20);
            bool taken = inArchives(archives, name);
            for (size_t f = 0; f < found.size() && !taken; f++) {
                taken = found[f].name == name;
            }
            if (taken || length == 0 || length > (uint32_t)formats::Sni::kMaxStreamedEntryBytes ||
                offset > 0x7FFFFF00u) {
                continue;
            }
            Found sound;
            sound.name = name;
            sound.offset = (int32_t)offset;
            sound.length = (int32_t)length;
            found.push_back(sound);
        }
    }
    if (found.empty()) {
        return false;
    }

    // The archive: its directory, then each sound's bytes as read.
    size_t size = (size_t)kSniDirectory + 4 + found.size() * kSniEntryBytes;
    for (size_t f = 0; f < found.size(); f++) {
        size += (size_t)found[f].length;
    }
    std::vector<uint8_t> bytes(size, 0);
    put32(&bytes, kSniDirectory, (uint32_t)found.size());
    size_t at = (size_t)kSniDirectory + 4 + found.size() * kSniEntryBytes;
    std::vector<uint8_t> wav;
    for (size_t f = 0; f < found.size(); f++) {
        const size_t entry = (size_t)kSniDirectory + 4 + f * kSniEntryBytes;
        memcpy(bytes.data() + entry, found[f].name.c_str(), found[f].name.size() < 12 ? found[f].name.size() : 12);
        bytes[entry + 14] = 0xFF;   // volume 0x7FFF: full (SoundMixer.FullVolume)
        bytes[entry + 15] = 0x7F;
        put32(&bytes, entry + 16, (uint32_t)(at - 4));   // an SNI offset counts from byte 4
        put32(&bytes, entry + 20, (uint32_t)found[f].length);
        if (!data.readRangeAt(path, found[f].offset, found[f].length, &wav)) {
            return false;
        }
        memcpy(bytes.data() + at, wav.data(), wav.size());
        at += wav.size();
    }
    return formats::Sni::parse(out, std::move(bytes));
}

} // namespace

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
        formats::Sni archive;
        if (i == 2) {
            // LEVELnO.SNI: the music and the corridors, by its directory (see
            // the header).
            if (!formats::Sni::open(&archive, data, paths[i])) {
                return false;
            }
        } else {
            std::vector<uint8_t> bytes;
            if (!data.read(paths[i], &bytes)) {
                return false;
            }
            if (!formats::Sni::parse(&archive, std::move(bytes))) {
                return false;
            }
        }
        archives.push_back(std::move(archive));
    }
    // The arenas' own sounds, for the names no archive has.
    formats::Sni arenas;
    size_t arenaBytes = 0;
    if (arenaSounds(data, level, archives, &arenas)) {
        arenaBytes = arenas.bytes().size();
        archives.push_back(std::move(arenas));
    }
    if (!withArchives(out, std::move(archives))) {
        return false;
    }
    out->arenaSoundBytes_ = arenaBytes;
    return true;
}

bool SoundBank::withArchives(SoundBank* out, std::vector<formats::Sni>&& archives) {
    out->archives_ = std::move(archives);
    out->arenaSoundBytes_ = 0;
    out->cache_.clear();
    out->sounds_.clear();
    out->missing_.clear();
    out->readFailures_.clear();
    out->pins_.clear();
    out->recent_.clear();
    out->stagingDrop();   // a read in flight cancelled first
    out->staging_ = Staging();
    out->stats_ = Stats();
    return true;
}

void SoundBank::addStreamedMusic(const std::string& name, int32_t sampleRate, int32_t frames) {
    Entry entry;
    entry.volume = 0;
    entry.looping = true;
    entry.music = true;
    size_t archive = 0;
    const formats::Sni::Entry* found = nullptr;
    if (find(name, &archive, &found)) {
        entry.volume = found->volume;
        entry.looping = (found->flags & formats::Sni::kFlagLoop) != 0;
    }
    entry.channels = 1;
    entry.sampleRate = sampleRate;
    entry.frames = frames;
    Sound& stand = streamedSounds_[name];
    stand = Sound();
    stand.channels = 1;
    stand.sampleRate = sampleRate;
    entry.sound = &stand;
    streamed_[name] = entry;
}

bool SoundBank::getMusic(const std::string& name, Entry* out) {
    std::map<std::string, Entry>::const_iterator found = streamed_.find(name);
    if (found == streamed_.end()) {
        return get(name, out);
    }
    if (out != nullptr) {
        *out = found->second;
    }
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
    bool readFailed = false;
    if (!load(name, &entry, &readFailed)) {
        if (readFailed) {
            noteReadFailure(name);   // tried again next time, up to the limit
        } else {
            missing_.insert(name);
        }
        return false;
    }
    readFailures_.erase(name);   // the limit is on failures in a row
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
        stats_.evictions++;
        for (std::deque<std::string>::iterator it = recent_.begin(); it != recent_.end(); ++it) {
            if (*it == victim) {
                recent_.erase(it);
                break;
            }
        }
    }
}

size_t SoundBank::releaseMusicExcept(const std::string* keep, int32_t count) {
    size_t freed = 0;
    for (std::map<std::string, Entry>::iterator it = cache_.begin(); it != cache_.end();) {
        const std::string& name = it->first;
        bool kept = !it->second.music || pins(name) != 0;
        for (int32_t k = 0; !kept && keep && k < count; k++) {
            kept = keep[k] == name;
        }
        if (kept) {
            ++it;
            continue;
        }
        std::map<std::string, Sound>::iterator sound = sounds_.find(name);
        if (sound != sounds_.end()) {
            freed += sound->second.samples.size() * sizeof(Sound::Samples::value_type);
            sounds_.erase(sound);
        }
        for (std::deque<std::string>::iterator r = recent_.begin(); r != recent_.end(); ++r) {
            if (*r == name) {
                recent_.erase(r);
                break;
            }
        }
        stats_.musicReleased++;
        cache_.erase(it++);
    }
    return freed;
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

void SoundBank::effectNames(std::vector<std::string>* out) const {
    out->clear();
    std::set<std::string> seen;
    for (size_t i = 0; i < archives_.size(); i++) {
        const std::vector<formats::Sni::Entry>& entries = archives_[i].entries();
        for (size_t e = 0; e < entries.size(); e++) {
            if (!archives_[i].isSound(entries[e])) {
                continue;
            }
            const std::string name = entries[e].nameString();
            if (name.empty() || !seen.insert(name).second || streamsMusic(name)) {
                continue;
            }
            // The entry get() would decode (newest archive wins), not this one.
            size_t archive = 0;
            const formats::Sni::Entry* winner = nullptr;
            if (find(name, &archive, &winner) && (winner->flags & formats::Sni::kFlagMusic) == 0) {
                out->push_back(name);
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
// in the older archives -- it does not end the lookup.
bool SoundBank::find(const std::string& name, size_t* archive,
                     const formats::Sni::Entry** entry) const {
    for (int i = (int)archives_.size() - 1; i >= 0; i--) {
        const formats::Sni& candidate = archives_[(size_t)i];
        const std::vector<formats::Sni::Entry>& entries = candidate.entries();
        const formats::Sni::Entry* found = nullptr;
        for (size_t e = 0; e < entries.size(); e++) {
            if (entries[e].nameString() == name) {
                found = &entries[e];
                break;
            }
        }
        if (found == nullptr || !candidate.isSound(*found)) {
            continue;
        }
        // The C# takes the entry's bytes here (GetBytes cannot fail for a
        // parsed archive); an entry whose range is outside the file is skipped
        // as before.
        // A streamed entry is all directory: one longer than any entry is
        // (Sni::kMaxStreamedEntryBytes) is skipped the same way, before the
        // prefetch reserves a buffer of its length.
        if (found->offset < 0 || found->length < 0 ||
            (!candidate.streamed() &&
             (int64_t)found->offset + found->length > (int64_t)candidate.bytes().size()) ||
            (candidate.streamed() && found->length > formats::Sni::kMaxStreamedEntryBytes)) {
            continue;
        }
        *archive = (size_t)i;
        *entry = found;
        return true;
    }
    return false;
}

bool SoundBank::prefetch(const std::string& name) {
    if (name.empty() || name == staging_.name || cache_.find(name) != cache_.end() ||
        missing_.find(name) != missing_.end() || streamsMusic(name)) {
        return false;
    }
    size_t archive = 0;
    const formats::Sni::Entry* entry = nullptr;
    if (!find(name, &archive, &entry)) {
        return false;
    }
    if (!staging_.name.empty()) {
        stats_.prefetchDrops++;
    }
    stagingDrop();
    staging_.name = name;
    staging_.archive = archive;
    staging_.entry = *entry;
    staging_.bytes.reserve((size_t)entry->length);
    stats_.prefetches++;
    return true;
}

void SoundBank::noteReadFailure(const std::string& name) {
    if (++readFailures_[name] >= kMaxReadFailures) {
        missing_.insert(name);
    }
}

void SoundBank::stagingDrop() {
    if (staging_.readTag >= 0 && reader_.rangeActive()) {
        reader_.cancel(reader_.user, staging_.readTag);
    }
    staging_.readTag = -1;
    staging_.name.clear();
    std::vector<uint8_t>().swap(staging_.bytes);
    staging_.decoding = false;
    staging_.sound = Sound();
}

bool SoundBank::stagingRead(int32_t limit) {
    const int32_t have = (int32_t)staging_.bytes.size();
    int32_t want = staging_.entry.length - have;
    if (limit > 0 && want > limit) {
        want = limit;
    }
    if (want <= 0) {
        return true;
    }
    std::vector<uint8_t> chunk;
    if (!archives_[staging_.archive].readEntry(staging_.entry, have, want, &chunk)) {
        stats_.readFailures++;
        stagingDrop();
        return false;
    }
    staging_.bytes.insert(staging_.bytes.end(), chunk.begin(), chunk.end());
    return true;
}

bool SoundBank::stagingReadAsync(int32_t step) {
    const formats::Sni::Entry& entry = staging_.entry;
    const int32_t have = (int32_t)staging_.bytes.size();
    if (staging_.readTag < 0) {
        // Ask for the step. Refused (the reader's music slot is still busy with a
        // read this bank cancelled): asked again on the next call.
        const int32_t tag = nextReadTag_;
        if (reader_.requestRange(reader_.user, tag, archives_[staging_.archive].path().c_str(),
                                 (uint32_t)(entry.offset + have), (uint32_t)step)) {
            staging_.readTag = tag;
            nextReadTag_ = (nextReadTag_ + 1) & 0x7FFFFFFF;
        }
        return true;
    }
    uint8_t* bytes = nullptr;
    uint32_t size = 0;
    bool ok = false;
    if (!reader_.poll(reader_.user, staging_.readTag, &bytes, &size, &ok)) {
        return true;   // still reading
    }
    staging_.readTag = -1;
    if (!ok || bytes == nullptr || size != (uint32_t)step) {
        free(bytes);
        stats_.readFailures++;
        stagingDrop();
        return false;
    }
    staging_.bytes.insert(staging_.bytes.end(), bytes, bytes + size);
    free(bytes);
    stats_.prefetchSteps++;
    stats_.prefetchReads++;
    return true;
}

bool SoundBank::stagingDecodeStep() {
    if (!staging_.decoding) {
        // The first slice finds the data chunk (parse()'s walk, no copy).
        if (!formats::Wav::locate(staging_.bytes.data(), (int32_t)staging_.bytes.size(),
                                  &staging_.format, &staging_.dataAt, &staging_.dataSize)) {
            missing_.insert(staging_.name);   // as a get() whose WAV will not parse
            stagingDrop();
            return false;
        }
        staging_.decoding = true;
        staging_.samples =
            pcmSampleCount(staging_.dataSize, staging_.format.bitsPerSample());
        // The store's room at once, its elements a slice at a time: no
        // whole-track zero fill, and no reallocation while it fills.
        staging_.sound.samples.reserve((size_t)staging_.samples);
    }
    const int32_t done = (int32_t)staging_.sound.samples.size();
    int32_t count = staging_.samples - done;
    if (count > kDecodeStepSamples) {
        count = kDecodeStepSamples;
    }
    if (count > 0) {
        staging_.sound.samples.resize((size_t)(done + count));
        pcmToSamples(staging_.bytes.data() + staging_.dataAt, staging_.format.bitsPerSample(), done,
                     count, staging_.sound.samples.data() + done);
        stats_.decodeSteps++;
    }
    if (done + count < staging_.samples) {
        return true;
    }
    // The last slice: the sound goes into the bank, as decode() puts it.
    const std::string name = staging_.name;
    const formats::Sni::Entry& entry = staging_.entry;
    Entry out;
    out.volume = entry.volume;
    out.looping = (entry.flags & formats::Sni::kFlagLoop) != 0;
    out.music = (entry.flags & formats::Sni::kFlagMusic) != 0;
    out.channels = staging_.format.channels();
    out.sampleRate = staging_.format.sampleRate();
    Sound& sound = sounds_[name];
    sound.samples.swap(staging_.sound.samples);
    sound.channels = out.channels;
    sound.sampleRate = out.sampleRate;
    sound.looping = out.looping;
    // Sound.Frames, which is Wav::frames() of the same samples (once per sound).
    out.frames = sound.frames();
    out.sound = &sound;
    stats_.decodes++;
    stats_.decodedSampleCount += (int64_t)sound.samples.size();
    stagingDrop();
    readFailures_.erase(name);
    cache_[name] = out;
    touch(name);
    evictOverBudget(name);
    return false;
}

bool SoundBank::prefetchStep() {
    if (staging_.name.empty()) {
        return false;
    }
    const int32_t left = staging_.entry.length - (int32_t)staging_.bytes.size();
    if (left <= 0) {
        return stagingDecodeStep();
    }
    const int32_t step = left > kPrefetchTailBytes ? kPrefetchStepBytes : left;
    const std::string reading = staging_.name;
    const formats::Sni& archive = archives_[staging_.archive];
    if (reader_.rangeActive() && archive.streamed() &&
        (int64_t)staging_.entry.offset + staging_.entry.length <= (int64_t)0x7FFFFFFF) {
        if (!stagingReadAsync(step)) {
            noteReadFailure(reading);
            return false;
        }
        // The landed bytes' frame is not also the first decode slice's.
        return true;
    }
    stats_.prefetchSteps++;
    if (!stagingRead(step)) {
        // Abandoned, and counted against the name: a later get() reads it
        // whole, or finds it missing once the limit is reached.
        noteReadFailure(reading);
        return false;
    }
    return true;
}

bool SoundBank::load(const std::string& name, Entry* out, bool* readFailed) {
    std::vector<uint8_t> bytes;
    formats::Sni::Entry entry;
    if (name == staging_.name) {
        // The sound is wanted before its prefetch is done: finish it now, on this
        // thread -- the read still in flight on the reader dropped, the rest read
        // here, and the whole decoded at once (a slice already made is thrown
        // away; this is the last resort, and it is counted).
        const int64_t left = (int64_t)staging_.entry.length - (int64_t)staging_.bytes.size();
        stats_.prefetchStalls++;
        stats_.prefetchStallBytes += left;
        if (staging_.readTag >= 0 && reader_.rangeActive()) {
            reader_.cancel(reader_.user, staging_.readTag);
            staging_.readTag = -1;
        }
        if (!stagingRead(0)) {
            *readFailed = true;
            return false;
        }
        entry = staging_.entry;
        bytes.swap(staging_.bytes);
        stagingDrop();
    } else {
        size_t archive = 0;
        const formats::Sni::Entry* found = nullptr;
        if (!find(name, &archive, &found)) {
            return false;
        }
        entry = *found;
        if (!archives_[archive].getBytes(entry, &bytes)) {
            if (archives_[archive].streamed()) {
                stats_.readFailures++;
                *readFailed = true;
            }
            return false;
        }
        if (archives_[archive].streamed()) {
            stats_.demandReads++;
            stats_.demandBytes += entry.length;
        }
    }
    return decode(name, entry, bytes, out);
}

// The winning entry is decoded after the walk, so the frame-count division
// (runtime divisor: the channel count) runs once per name, not inside the
// archive loop.
bool SoundBank::decode(const std::string& name, const formats::Sni::Entry& entry,
                       const std::vector<uint8_t>& bytes, Entry* out) {
    formats::Wav wav;
    if (!formats::Wav::parse(bytes.data(), (int32_t)bytes.size(), &wav)) {
        return false;  // C#: Wav.Parse null here means Get caches null
    }

    out->volume = entry.volume;
    out->looping = (entry.flags & formats::Sni::kFlagLoop) != 0;
    out->music = (entry.flags & formats::Sni::kFlagMusic) != 0;
    out->channels = wav.channels();
    out->sampleRate = wav.sampleRate();
    out->frames = wav.frames();

    // Sound.FromPcm, into the bank's own copy so every Entry of this name points
    // at one decode. The C# does the same by handing out the same reference.
    Sound& sound = sounds_[name];
    soundFromPcm(wav.data(), wav.dataSize(), wav.channels(), wav.sampleRate(),
                 wav.bitsPerSample(), out->looping, &sound);
    out->sound = &sound;
    stats_.decodes++;
    stats_.decodedSampleCount += (int64_t)sound.samples.size();
    return true;
}

} // namespace audio
} // namespace mdk

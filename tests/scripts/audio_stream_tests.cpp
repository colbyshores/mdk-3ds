// audio_stream_tests.cpp -- see audio_stream_tests.h.

#include "audio_stream_tests.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <string>
#include <utility>
#include <vector>

#include "audio/dsp_adpcm.h"
#include "audio/level_music.h"
#include "audio/sound_bank.h"
#include "audio/sound_mixer.h"
#include "formats/file_system.h"
#include "formats/mdk_data.h"
#include "formats/sni.h"
#include "formats/sprite_animation.h"
#include "game/async_read.h"

namespace {

using mdk::audio::LevelMusic;
using mdk::audio::SoundBank;
using mdk::audio::SoundMixer;
using mdk::formats::MdkData;
using mdk::formats::Sni;

int failures = 0;
long long checks = 0;

void check(bool ok, const char* what, const std::string& detail = std::string()) {
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL AudioStream: %s%s%s\n", what, detail.empty() ? "" : " -- ", detail.c_str());
    }
}

void expect(const char* what, long long got, long long want) {
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL AudioStream: %s: got %lld, want %lld\n", what, got, want);
    }
}

void expectFloat(const char* what, float got, float want) {
    checks++;
    if (memcmp(&got, &want, sizeof(float)) != 0) {
        failures++;
        printf("FAIL AudioStream: %s: got %.9g, want %.9g\n", what, (double)got, (double)want);
    }
}

std::vector<std::string> archivePaths(int level) {
    char prefix[96];
    snprintf(prefix, sizeof(prefix), "TRAVERSE/LEVEL%d/LEVEL%d", level, level);
    std::vector<std::string> paths;
    paths.push_back("TRAVERSE/TRAVERSE.SNI");
    paths.push_back(std::string(prefix) + "S.SNI");
    paths.push_back(std::string(prefix) + "O.SNI");
    return paths;
}

// ---------------------------------------------------------------------------
// 1. An opened archive is the parsed archive, entry for entry and byte for byte.
// ---------------------------------------------------------------------------
void testAnOpenedArchiveIsTheWholeFile(const MdkData& data) {
    long long entriesSeen = 0;
    long long animationsSeen = 0;
    for (int level = 3; level <= 8; level++) {
        const std::vector<std::string> paths = archivePaths(level);
        for (size_t p = level == 3 ? 0 : 1; p < paths.size(); p++) {
            std::vector<uint8_t> bytes;
            if (!data.read(paths[p], &bytes)) {
                check(false, "the archive reads", paths[p]);
                continue;
            }
            Sni whole;
            Sni opened;
            check(Sni::parse(&whole, bytes.data(), (int32_t)bytes.size()), "the archive parses", paths[p]);
            check(Sni::open(&opened, data, paths[p]), "the archive opens", paths[p]);
            check(!whole.streamed() && opened.streamed(), "only the opened one is streamed");
            expect("an opened archive holds no file bytes", (long long)opened.bytes().size(), 0);
            expect("the same entry count", (long long)opened.entries().size(),
                   (long long)whole.entries().size());
            if (opened.entries().size() != whole.entries().size()) {
                continue;
            }
            for (size_t e = 0; e < whole.entries().size(); e++) {
                const Sni::Entry& a = whole.entries()[e];
                const Sni::Entry& b = opened.entries()[e];
                entriesSeen++;
                const bool same = strcmp(a.name, b.name) == 0 && a.offset == b.offset &&
                                  a.length == b.length && a.flags == b.flags &&
                                  a.volume == b.volume &&
                                  whole.isSound(a) == opened.isSound(b);
                check(same, "the entry's directory fields and kind agree", paths[p] + " " + a.name);
                std::vector<uint8_t> wholeBytes;
                std::vector<uint8_t> openedBytes;
                const bool gotWhole = whole.getBytes(a, &wholeBytes);
                const bool gotOpened = opened.getBytes(b, &openedBytes);
                check(gotWhole == gotOpened && wholeBytes == openedBytes,
                      "the entry's bytes agree", paths[p] + " " + a.name);
                // A ranged read is that range of the entry, and nothing past it.
                if (gotOpened && a.length >= 16) {
                    std::vector<uint8_t> part;
                    check(opened.readEntry(b, 5, 9, &part) && part.size() == 9 &&
                              memcmp(part.data(), wholeBytes.data() + 5, 9) == 0,
                          "a ranged read is that range", a.name);
                    check(!opened.readEntry(b, a.length - 4, 5, &part),
                          "a read past the entry is refused", a.name);
                    check(!whole.readEntry(a, a.length - 4, 5, &part),
                          "... by the whole-file form too", a.name);
                }
                // A sprite animation (LEVEL4S.SNI's K_SURF, LEVEL6S.SNI's slides):
                // the same frames. The directory marks one with a length of
                // 0xFFFFFFFF; nothing else is asked to be one (a corridor read
                // as an animation is garbage in either form).
                if (a.length != -1) {
                    continue;
                }
                check(!gotWhole && !gotOpened, "an animation's entry has no byte length", a.name);
                mdk::formats::SpriteAnimation fromWhole;
                mdk::formats::SpriteAnimation fromOpened;
                const bool animWhole = whole.animation(a.name, &fromWhole);
                const bool animOpened = opened.animation(a.name, &fromOpened);
                check(animWhole && animOpened, "an animation is one in both forms", a.name);
                if (animWhole && animOpened) {
                    expect("the animation's frame count", fromOpened.frameCount(),
                           fromWhole.frameCount());
                    int32_t framesSame = 0;
                    for (int32_t f = 0; f < fromWhole.frameCount(); f++) {
                        mdk::formats::SpriteAnimation::Frame x;
                        mdk::formats::SpriteAnimation::Frame y;
                        const bool gx = fromWhole.frame(f, &x);
                        const bool gy = fromOpened.frame(f, &y);
                        // Both must DECODE: two refusals are not "the same frame"
                        // (a frame guard that refused everything passed this once).
                        if (gx && gy && x.hotspotX == y.hotspotX && x.hotspotY == y.hotspotY &&
                            x.image.width() == y.image.width() &&
                            x.image.height() == y.image.height() &&
                            x.image.width() > 0 && x.image.height() > 0 &&
                            x.image.indexCount() == y.image.indexCount() &&
                            memcmp(x.image.indices(), y.image.indices(),
                                   (size_t)x.image.indexCount()) == 0) {
                            framesSame++;
                        }
                    }
                    if (fromWhole.frameCount() > 0 && framesSame == fromWhole.frameCount()) {
                        animationsSeen++;
                    }
                    expect("every frame decodes the same", framesSame, fromWhole.frameCount());
                }
            }
            expect("no read failed", opened.readFailures(), 0);
        }
    }
    check(entriesSeen > 150, "the retail archives were walked");
    expect("the six retail sprite animations were compared", animationsSeen, 6);

    Sni none;
    check(!Sni::open(&none, data, "TRAVERSE/NO_SUCH.SNI"), "a missing archive does not open");
}

// The whole-file bank the C# has: three parsed archives.
bool wholeBank(SoundBank* out, const MdkData& data, int level) {
    const std::vector<std::string> paths = archivePaths(level);
    std::vector<Sni> archives;
    for (size_t p = 0; p < paths.size(); p++) {
        std::vector<uint8_t> bytes;
        Sni archive;
        if (!data.read(paths[p], &bytes) || !Sni::parse(&archive, std::move(bytes))) {
            return false;
        }
        archives.push_back(std::move(archive));
    }
    return SoundBank::withArchives(out, std::move(archives));
}

bool sameEntry(const SoundBank::Entry& a, const SoundBank::Entry& b) {
    return a.volume == b.volume && a.looping == b.looping && a.channels == b.channels &&
           a.sampleRate == b.sampleRate && a.frames == b.frames && a.sound && b.sound &&
           a.sound->samples == b.sound->samples && a.sound->channels == b.sound->channels &&
           a.sound->sampleRate == b.sound->sampleRate && a.sound->looping == b.sound->looping;
}

// Every name the level's archives carry.
std::vector<std::string> levelNames(const MdkData& data, int level) {
    std::vector<std::string> names;
    const std::vector<std::string> paths = archivePaths(level);
    for (size_t p = 0; p < paths.size(); p++) {
        std::vector<uint8_t> bytes;
        Sni archive;
        if (data.read(paths[p], &bytes) && Sni::parse(&archive, std::move(bytes))) {
            for (size_t e = 0; e < archive.entries().size(); e++) {
                names.push_back(archive.entries()[e].nameString());
            }
        }
    }
    return names;
}

// ---------------------------------------------------------------------------
// 2. The level's bank (music archive opened) answers as the whole-file bank.
// ---------------------------------------------------------------------------
void testTheLevelsBankIsTheWholeFilesBank(const MdkData& data) {
    long long sounds = 0;
    for (int level = 3; level <= 8; level++) {
        SoundBank streamed;
        SoundBank whole;
        check(SoundBank::forLevel(&streamed, data, level), "the level's bank loads");
        check(wholeBank(&whole, data, level), "the whole-file bank loads");
        expect("three archives", streamed.archiveCount(), 3);
        // The music archive is the big one and it is not resident.
        check(streamed.archiveBytes() * 3 < whole.archiveBytes(),
              "the level's bank holds less than a third of the archives' bytes");
        const std::vector<std::string> names = levelNames(data, level);
        for (size_t i = 0; i < names.size(); i++) {
            SoundBank::Entry a;
            SoundBank::Entry b;
            const bool gotA = streamed.get(names[i], &a);
            const bool gotB = whole.get(names[i], &b);
            check(gotA == gotB, "a name is a sound in both banks or in neither", names[i]);
            if (gotA && gotB) {
                sounds++;
                check(sameEntry(a, b), "the sound decodes the same", names[i]);
            }
        }
        expect("the same number decoded", streamed.soundsDecoded(), whole.soundsDecoded());
        expect("no read failed", streamed.stats().readFailures, 0);
        // What was read on demand is exactly the opened archive's sounds: the
        // level's music (5 tracks on level 5, 9 on level 8).
        {
            Sni music;
            check(Sni::open(&music, data, archivePaths(level)[2]), "the music archive opens");
            int32_t tracks = 0;
            for (size_t e = 0; e < music.entries().size(); e++) {
                tracks += music.isSound(music.entries()[e]) ? 1 : 0;
            }
            check(tracks >= 4, "the level has music");
            expect("the music, and only the music, was read on demand",
                   streamed.stats().demandReads, tracks);
        }
        expect("the whole-file bank read nothing on demand", whole.stats().demandReads, 0);
    }
    check(sounds > 400, "the retail sounds were compared");
}

// The steps the original's rule gives a read of `length` bytes.
int32_t stepsFor(int32_t length) {
    int32_t steps = 0;
    while (length > 0) {
        length -= length > SoundBank::kPrefetchTailBytes ? SoundBank::kPrefetchStepBytes : length;
        steps++;
    }
    return steps;
}

int32_t lengthOf(const MdkData& data, const std::string& path, const std::string& name) {
    Sni archive;
    if (!Sni::open(&archive, data, path)) {
        return -1;
    }
    for (size_t e = 0; e < archive.entries().size(); e++) {
        if (archive.entries()[e].nameString() == name) {
            return archive.entries()[e].length;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// 3. The prefetch: a step a call, decoded on the last, finished at once when
//    the sound is wanted early.
// ---------------------------------------------------------------------------
void testThePrefetchReadsAStepACall(const MdkData& data) {
    const int32_t h1 = lengthOf(data, "TRAVERSE/LEVEL3/LEVEL3O.SNI", "H1");
    const int32_t h3 = lengthOf(data, "TRAVERSE/LEVEL3/LEVEL3O.SNI", "H3");
    check(h1 > 0x40000 && h3 > 0x40000, "level 3's tracks are there and are many steps long");
    // The rule itself, on numbers: one step up to 0x10000, 0xf768 a step above.
    expect("a 0x10000-byte sound is one step", stepsFor(0x10000), 1);
    expect("0x10001 bytes are two", stepsFor(0x10001), 2);
    expect("H1's steps", stepsFor(h1), (h1 - 0x10000 + 0xf768 - 1) / 0xf768 + 1);

    SoundBank whole;
    check(wholeBank(&whole, data, 3), "the whole-file bank loads");
    SoundBank::Entry want;
    check(whole.get("H1", &want), "H1 is a sound");

    // --- a prefetch that runs to its end ---
    {
        SoundBank bank;
        check(SoundBank::forLevel(&bank, data, 3), "the bank loads");
        check(bank.prefetch("H1"), "the prefetch begins");
        check(bank.prefetching() == "H1", "and says what it reads");
        check(!bank.prefetch("H1"), "asking again begins nothing");
        int32_t steps = 0;
        bool more = true;
        while (more && steps < 1000) {
            expect("nothing is decoded before the last step", bank.soundsDecoded(), 0);
            more = bank.prefetchStep();
            steps++;
        }
        // The reads by the original's rule, then the decode a slice a step.
        const int32_t slices =
            ((int32_t)want.sound->samples.size() + SoundBank::kDecodeStepSamples - 1) /
            SoundBank::kDecodeStepSamples;
        check(slices > 4, "a track decodes in several slices, not one");
        expect("the read steps are the original's rule", bank.stats().prefetchSteps,
               stepsFor(h1));
        expect("then the decode's slices", bank.stats().decodeSteps, slices);
        expect("and nothing else", steps, stepsFor(h1) + slices);
        expect("no read went to a reader (none is lent)", bank.stats().prefetchReads, 0);
        expect("the sound is decoded by the last step", bank.soundsDecoded(), 1);
        check(bank.prefetching().empty(), "the prefetch is over");
        check(!bank.prefetchStep(), "a step with nothing in flight does nothing");
        SoundBank::Entry got;
        check(bank.get("H1", &got) && sameEntry(got, want), "the prefetched sound is the whole file's");
        expect("playing it read nothing more", bank.stats().demandReads, 0);
        expect("and waited for nothing", bank.stats().prefetchStalls, 0);
        check(!bank.prefetch("H1"), "a decoded sound is not prefetched again");
        check(!bank.prefetch("NO_SUCH_SOUND"), "nor a name the level has not got");
        check(!bank.prefetch(""), "nor no name");
        check(!bank.prefetch("CHMO_1"), "nor an entry that is not a sound (a corridor)");
    }

    // --- wanted before its prefetch is done: finished at once, and counted ---
    {
        SoundBank bank;
        check(SoundBank::forLevel(&bank, data, 3), "the bank loads");
        check(bank.prefetch("H1"), "the prefetch begins");
        check(bank.prefetchStep() && bank.prefetchStep(), "two steps, more to come");
        SoundBank::Entry got;
        check(bank.get("H1", &got) && sameEntry(got, want), "the early get is the whole file's");
        expect("one stall", bank.stats().prefetchStalls, 1);
        expect("of the bytes that were left", bank.stats().prefetchStallBytes,
               (long long)h1 - 2 * SoundBank::kPrefetchStepBytes);
        expect("which is not a demand read", bank.stats().demandReads, 0);
        check(bank.prefetching().empty() && !bank.prefetchStep(), "the prefetch is over");
    }

    // --- one staging buffer: a new prefetch drops the one in flight ---
    {
        SoundBank bank;
        check(SoundBank::forLevel(&bank, data, 3), "the bank loads");
        check(bank.prefetch("H1") && bank.prefetchStep(), "H1 is in flight");
        check(bank.prefetch("H3"), "H3 takes the buffer");
        expect("H1 was dropped", bank.stats().prefetchDrops, 1);
        check(bank.prefetching() == "H3", "H3 is what is read");
        int32_t steps = 0;
        while (bank.prefetchStep() && steps < 1000) {
            steps++;
        }
        expect("H3 took its own read steps from the start", bank.stats().prefetchSteps,
               1 + stepsFor(h3));
        expect("only H3 is decoded", bank.soundsDecoded(), 1);
        SoundBank::Entry h1Entry;
        check(bank.get("H1", &h1Entry) && sameEntry(h1Entry, want),
              "the dropped sound still plays, read whole");
        expect("as a demand read", bank.stats().demandReads, 1);
        expect("of its length", bank.stats().demandBytes, (long long)h1);
    }

    // --- a sound of a RESIDENT archive prefetches the same way (CORRIDOR) ---
    {
        SoundBank bank;
        check(SoundBank::forLevel(&bank, data, 3), "the bank loads");
        check(bank.prefetch("CORRIDOR"), "the corridor track prefetches");
        int32_t steps = 0;
        while (bank.prefetchStep() && steps < 1000) {
            steps++;
        }
        SoundBank::Entry got;
        SoundBank::Entry wantCorridor;
        check(bank.get("CORRIDOR", &got) && whole.get("CORRIDOR", &wantCorridor) &&
                  sameEntry(got, wantCorridor),
              "and is the whole file's");
        expect("with no demand read", bank.stats().demandReads, 0);
    }
}

// ---------------------------------------------------------------------------
// 3b. A track no arena in memory plays is given back; a playing one, a kept
//     one and every effect stay.
// ---------------------------------------------------------------------------
void testMusicNobodyNeedsIsReleased(const MdkData& data) {
    SoundBank bank;
    SoundBank whole;
    check(SoundBank::forLevel(&bank, data, 3) && wholeBank(&whole, data, 3), "the banks load");
    SoundBank::Entry h1;
    SoundBank::Entry h3;
    SoundBank::Entry h5;
    SoundBank::Entry door;
    check(bank.get("H1", &h1) && bank.get("H3", &h3) && bank.get("H5", &h5) &&
              bank.get("DOOR", &door),
          "three tracks and an effect decode");
    check(h1.music && h3.music && h5.music && !door.music, "the tracks are music, the door is not");
    expect("four decoded", bank.soundsDecoded(), 4);
    const size_t before = bank.decodedBytes();
    const size_t h5Bytes = h5.sound->samples.size() * sizeof(mdk::audio::Sound::Samples::value_type);
    check(h5Bytes > 400000, "H5 is a track's worth of samples");

    bank.retain("H1");   // a voice plays it
    const std::string keep[1] = {"H3"};
    const size_t freed = bank.releaseMusicExcept(keep, 1);
    expect("exactly H5's samples were given back", (long long)freed, (long long)h5Bytes);
    expect("the store shrank by that", (long long)bank.decodedBytes(), (long long)(before - h5Bytes));
    expect("three stay decoded", bank.soundsDecoded(), 3);
    expect("one track released", bank.stats().musicReleased, 1);
    SoundBank::Entry again;
    check(bank.get("H1", &again) && again.sound == h1.sound, "the playing track did not move");
    check(bank.get("H3", &again) && again.sound == h3.sound, "nor the kept one");
    check(bank.get("DOOR", &again) && again.sound == door.sound, "nor the effect");
    const int32_t reads = bank.stats().demandReads;
    SoundBank::Entry want;
    check(bank.get("H5", &again) && whole.get("H5", &want) && sameEntry(again, want),
          "the released track plays again, the same");
    expect("by reading it again", bank.stats().demandReads, reads + 1);

    // Nothing kept and nothing pinned: every track goes, no effect does.
    bank.release("H1");
    bank.releaseMusicExcept(nullptr, 0);
    expect("only the effect is left", bank.soundsDecoded(), 1);
    check(bank.get("DOOR", &again) && again.sound == door.sound, "and it is the door");
    expect("no eviction was counted for it (that is the budget's)", bank.stats().evictions, 0);
}

// ---------------------------------------------------------------------------
// SYNTHETIC ARCHIVES. The retail files cannot show everything the code decides:
// no name is in two of a level's archives, every track's flags are 3, no entry
// is exactly 0x10000 bytes, and no file is broken. These are built here.
// ---------------------------------------------------------------------------
struct Item {
    std::string name;
    int32_t flags;
    std::vector<uint8_t> bytes;
    uint32_t lengthOverride;   // 0: the bytes' own length
};

void put32(std::vector<uint8_t>* out, uint32_t value) {
    for (int i = 0; i < 4; i++) {
        out->push_back((uint8_t)(value >> (8 * i)));
    }
}

void put16(std::vector<uint8_t>* out, uint32_t value) {
    out->push_back((uint8_t)value);
    out->push_back((uint8_t)(value >> 8));
}

// A mono 8-bit WAV of exactly `total` bytes whose samples are `fill`, then a ramp.
std::vector<uint8_t> makeWav(int32_t total, uint8_t fill) {
    std::vector<uint8_t> wav;
    const int32_t dataBytes = total - 44;
    wav.insert(wav.end(), {'R', 'I', 'F', 'F'});
    put32(&wav, (uint32_t)(total - 8));
    wav.insert(wav.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put32(&wav, 16);
    put16(&wav, 1);       // PCM
    put16(&wav, 1);       // mono
    put32(&wav, 11025);
    put32(&wav, 11025);
    put16(&wav, 1);
    put16(&wav, 8);
    wav.insert(wav.end(), {'d', 'a', 't', 'a'});
    put32(&wav, (uint32_t)dataBytes);
    for (int32_t i = 0; i < dataBytes; i++) {
        wav.push_back((uint8_t)(fill + (i & 0x3F)));
    }
    return wav;
}

Item item(const std::string& name, int32_t flags, std::vector<uint8_t> bytes,
          uint32_t lengthOverride = 0) {
    Item it;
    it.name = name;
    it.flags = flags;
    it.bytes = std::move(bytes);
    it.lengthOverride = lengthOverride;
    return it;
}

// The retail layout: 0x14 bytes of header, the count, 24-byte entries (name[12],
// u16 flags, u16 volume, u32 offset FROM BYTE 4, u32 length), then the data.
std::vector<uint8_t> makeSni(const std::vector<Item>& items, uint32_t countOverride = 0) {
    std::vector<uint8_t> out(0x14, 0);
    put32(&out, countOverride ? countOverride : (uint32_t)items.size());
    uint32_t at = 0x18 + (uint32_t)items.size() * 24;
    for (size_t i = 0; i < items.size(); i++) {
        char name[12] = {0};
        strncpy(name, items[i].name.c_str(), 12);
        out.insert(out.end(), name, name + 12);
        put16(&out, (uint32_t)items[i].flags);
        put16(&out, 0x7FFF);
        put32(&out, at - 4);
        put32(&out, items[i].lengthOverride ? items[i].lengthOverride
                                            : (uint32_t)items[i].bytes.size());
        at += (uint32_t)items[i].bytes.size();
    }
    for (size_t i = 0; i < items.size(); i++) {
        out.insert(out.end(), items[i].bytes.begin(), items[i].bytes.end());
    }
    return out;
}

// A private folder for files this test writes, and the data seen through it.
std::string g_scratch;

bool writeFile(const std::string& name, const std::vector<uint8_t>& bytes) {
    FILE* file = fopen((g_scratch + "/" + name).c_str(), "wb");
    if (!file) {
        return false;
    }
    const size_t wrote = bytes.empty() ? 0 : fwrite(bytes.data(), 1, bytes.size(), file);
    fclose(file);
    return wrote == bytes.size();
}

// A file system whose RANGE reads can be made to fail: the host's own, with one
// function replaced. `g_rangeReadsLeft` reads succeed, then every one fails
// (-1: never fail). `g_rangeReads` counts the attempts.
int g_rangeReadsLeft = -1;
int g_rangeReads = 0;
// The largest read anything asked for: a guard against a lying directory is
// only a guard if the read it would have made is never REQUESTED (the platform
// sizes its buffer before it reads, and with no exceptions that is an abort).
int32_t g_largestRangeRead = 0;
const mdk::formats::FileSystem* g_realSystem = nullptr;

bool countedReadRange(void* user, const char* path, int32_t offset, int32_t length,
                      std::vector<uint8_t>* out) {
    g_rangeReads++;
    if (length > g_largestRangeRead) {
        g_largestRangeRead = length;
    }
    if (g_rangeReadsLeft == 0) {
        return false;
    }
    if (g_rangeReadsLeft > 0) {
        g_rangeReadsLeft--;
    }
    return g_realSystem->readRange(user, path, offset, length, out);
}

// ---------------------------------------------------------------------------
// 3c. What the retail archives cannot show: precedence, flags, the step rule at
//     its boundary, the budget with the prefetch, and broken input.
// ---------------------------------------------------------------------------
void testSyntheticArchives(const mdk::formats::FileSystem* system) {
    const MdkData scratch(system, g_scratch);

    // --- later archives win; a non-sound of the name does not end the lookup ---
    {
        std::vector<Item> older;
        older.push_back(item("DUP", 0, makeWav(300, 10)));
        older.push_back(item("SHADOWED", 0, makeWav(300, 20)));
        older.push_back(item("LOOPFX", 1, makeWav(300, 30)));    // loop, not music
        older.push_back(item("ONESHOT", 2, makeWav(300, 40)));   // music, not loop
        older.push_back(item("PASTEND", 0, makeWav(300, 60)));
        std::vector<Item> newer;
        newer.push_back(item("DUP", 0, makeWav(300, 200)));
        newer.push_back(item("SHADOWED", 0, std::vector<uint8_t>(64, 0x55)));   // not a WAV
        newer.push_back(item("PASTEND", 0, makeWav(300, 50), 100000));          // lies about its length
        newer.push_back(item("PASTONLY", 0, makeWav(300, 55), 100000));
        Sni a;
        Sni b;
        const std::vector<uint8_t> olderBytes = makeSni(older);
        const std::vector<uint8_t> newerBytes = makeSni(newer);
        check(Sni::parse(&a, olderBytes.data(), (int32_t)olderBytes.size()) &&
                  Sni::parse(&b, newerBytes.data(), (int32_t)newerBytes.size()),
              "the synthetic archives parse");
        std::vector<Sni> archives;
        archives.push_back(std::move(a));
        archives.push_back(std::move(b));
        SoundBank bank;
        check(SoundBank::withArchives(&bank, std::move(archives)), "a bank over the two");
        SoundBank::Entry got;
        check(bank.get("DUP", &got) && got.sound && got.sound->samples.size() == 256 &&
                  got.sound->samples[0] == (int16_t)((200 - 128) * 256),
              "a name in both archives is the LATER archive's sound");
        check(bank.get("SHADOWED", &got) && got.sound &&
                  got.sound->samples[0] == (int16_t)((20 - 128) * 256),
              "a non-sound of the name in the later archive does not end the lookup");
        check(bank.get("LOOPFX", &got) && got.looping && !got.music,
              "flag 1 alone: loops, and is not music");
        check(bank.get("ONESHOT", &got) && got.music && !got.looping,
              "flag 2 alone: music, and does not loop");
        // The later archive's PASTEND lies about its length; it is skipped, and
        // the lookup goes on to the older archive's, as the whole-file bank's
        // `if (!getBytes) continue;` always did.
        check(bank.get("PASTEND", &got) && got.sound &&
                  got.sound->samples[0] == (int16_t)((60 - 128) * 256),
              "an entry that runs past its file is skipped, not the end of the lookup");
        check(!bank.get("PASTONLY", &got), "and a name with no other entry is not a sound");

        // A prefetch in flight does not survive the bank being given new archives.
        check(bank.prefetch("DUP") == false, "a decoded sound is not prefetched");
    }

    // --- the step rule at its boundary, through a RESIDENT and a STREAMED archive ---
    {
        std::vector<Item> items;
        items.push_back(item("EXACT", 2, makeWav(0x10000, 3)));       // one step
        items.push_back(item("OVER", 2, makeWav(0x10001, 7)));        // 0xf768, then the rest
        items.push_back(item("THREE", 2, makeWav(0xf768 + 0x10001, 11)));
        items.push_back(item("NOTWAV", 2, std::vector<uint8_t>{'R', 'I', 'F', 'F', 1, 2, 3, 4, 5, 6, 7, 8}));
        items.push_back(item("NOTWAV2", 2, std::vector<uint8_t>{'R', 'I', 'F', 'F', 8, 7, 6, 5, 4, 3, 2, 1}));
        const std::vector<uint8_t> bytes = makeSni(items);
        check(writeFile("STEPS.SNI", bytes), "the archive is written");

        // The reference: every sound decoded with no prefetch at all.
        SoundBank reference;
        {
            Sni whole;
            check(Sni::parse(&whole, bytes.data(), (int32_t)bytes.size()), "it parses");
            std::vector<Sni> one;
            one.push_back(std::move(whole));
            SoundBank::withArchives(&reference, std::move(one));
        }
        for (int form = 0; form < 2; form++) {
            const char* which = form == 0 ? "resident" : "streamed";
            Sni archive;
            const bool opened = form == 0
                                    ? Sni::parse(&archive, bytes.data(), (int32_t)bytes.size())
                                    : Sni::open(&archive, scratch, "STEPS.SNI");
            check(opened && archive.streamed() == (form == 1), "the archive in this form", which);
            std::vector<Sni> one;
            one.push_back(std::move(archive));
            SoundBank bank;
            SoundBank::withArchives(&bank, std::move(one));
            const char* names[3] = {"EXACT", "OVER", "THREE"};
            const int32_t wantSteps[3] = {1, 2, 3};
            for (int n = 0; n < 3; n++) {
                check(bank.prefetch(names[n]), "the prefetch begins", names[n]);
                const int32_t readBefore = bank.stats().prefetchSteps;
                int32_t steps = 1;
                while (bank.prefetchStep() && steps < 100) {
                    steps++;
                }
                expect(names[n], bank.stats().prefetchSteps - readBefore, wantSteps[n]);
                SoundBank::Entry got;
                SoundBank::Entry want;
                check(bank.get(names[n], &got) && reference.get(names[n], &want) &&
                          sameEntry(got, want),
                      "a sound read in steps is the sound read whole", which);
            }
            expect("nothing was read on demand", bank.stats().demandReads, 0);

            // A prefetched entry that is not a WAV: missing from then on, and
            // never read again.
            check(bank.prefetch("NOTWAV"), "the bad entry's prefetch begins (it says RIFF)");
            check(bank.prefetchStep() && !bank.prefetchStep(),
                  "its one read step, then the decode finds no WAV and ends it");
            SoundBank::Entry bad;
            const int32_t demandBefore = bank.stats().demandReads;
            check(!bank.get("NOTWAV", &bad), "a WAV that does not parse is not a sound");
            expect("and it was not read again to find that out", bank.stats().demandReads,
                   demandBefore);
            check(!bank.prefetch("NOTWAV"), "nor is it prefetched again");
            // The same through get(): read ONCE, found not to be a WAV, and
            // remembered -- the C#'s cached null. (A read that FAILS is the
            // other case, and is not remembered: testReadsThatFail.)
            const int readsBefore = g_rangeReads;
            check(!bank.get("NOTWAV2", &bad), "a bad WAV asked for is not a sound");
            const int readsForOne = g_rangeReads - readsBefore;
            expect("it took the read of its entry, in the streamed form", readsForOne,
                   form == 1 ? 1 : 0);
            check(!bank.get("NOTWAV2", &bad) && !bank.get("NOTWAV2", &bad), "nor the next times");
            expect("which read nothing", g_rangeReads - readsBefore, readsForOne);

            // A bank given new archives forgets the prefetch that was in flight.
            check(bank.get("THREE", nullptr), "THREE is decoded");
        }

        // The budget and the prefetch together: the sound a prefetch lands evicts
        // the least recently gotten one, as a get() would.
        {
            Sni archive;
            check(Sni::open(&archive, scratch, "STEPS.SNI"), "the archive opens");
            std::vector<Sni> one;
            one.push_back(std::move(archive));
            SoundBank bank;
            SoundBank::withArchives(&bank, std::move(one));
            SoundBank::Entry got;
            check(bank.get("EXACT", &got), "EXACT is decoded");
            bank.setDecodedBudgetBytes(bank.decodedBytes() + 1000);
            check(bank.prefetch("OVER"), "OVER is prefetched");
            while (bank.prefetchStep()) {
            }
            expect("the prefetched sound evicted the older one", bank.soundsDecoded(), 1);
            expect("counted as an eviction", bank.stats().evictions, 1);
            check(bank.get("OVER", &got) && bank.stats().demandReads == 1,
                  "and it is OVER that stayed (one demand read: EXACT's own)");

            // withArchives drops a prefetch in flight.
            check(bank.prefetch("THREE") && bank.prefetchStep(), "THREE is in flight");
            Sni again;
            check(Sni::open(&again, scratch, "STEPS.SNI"), "the archive opens again");
            std::vector<Sni> fresh;
            fresh.push_back(std::move(again));
            SoundBank::withArchives(&bank, std::move(fresh));
            check(bank.prefetching().empty() && !bank.prefetchStep(),
                  "new archives: no prefetch is in flight");
        }
    }

    // --- broken directories are refused before anything is sized by them ---
    {
        std::vector<Item> items;
        items.push_back(item("A", 0, makeWav(300, 1)));
        check(writeFile("MANY.SNI", makeSni(items, 5000)), "an archive claiming 5000 entries");
        Sni many;
        g_largestRangeRead = 0;
        check(!Sni::open(&many, scratch, "MANY.SNI"), "is not opened");
        check(g_largestRangeRead <= 0x18, "and its 120,000-byte directory was never asked for");

        // An "animation" (length 0xFFFFFFFF) whose size word is absurd.
        std::vector<Item> anim;
        std::vector<uint8_t> body;
        put32(&body, 64u << 20);     // the size word: 64 MiB
        put32(&body, 2);
        anim.push_back(item("K_BAD", 0, body, 0xFFFFFFFFu));
        check(writeFile("ANIM.SNI", makeSni(anim)), "an archive with an absurd animation");
        Sni bad;
        check(Sni::open(&bad, scratch, "ANIM.SNI"), "opens (its directory is fine)");
        mdk::formats::SpriteAnimation animation;
        g_largestRangeRead = 0;
        check(!bad.animation("K_BAD", &animation), "and the animation is refused");
        check(g_largestRangeRead <= 4, "having asked for its size word and nothing else");
        expect("as a counted failure", bad.readFailures(), 1);

        // An entry longer than any entry is: refused before a buffer is sized.
        std::vector<Item> huge;
        huge.push_back(item("HUGE", 2, makeWav(300, 1), 64u << 20));
        huge.push_back(item("HUGER", 2, makeWav(300, 1), 0x7FFFFF00u));
        check(writeFile("HUGE.SNI", makeSni(huge)), "an archive with a 64 MiB and a 2 GB entry");
        Sni big;
        check(Sni::open(&big, scratch, "HUGE.SNI"), "opens");
        std::vector<uint8_t> none;
        g_largestRangeRead = 0;
        check(!big.getBytes(big.entries()[0], &none) && none.empty() &&
                  !big.getBytes(big.entries()[1], &none) && none.empty(),
              "and both entries are refused");
        expect("with no read asked for at all", g_largestRangeRead, 0);
        std::vector<Sni> one;
        one.push_back(std::move(big));
        SoundBank bank;
        SoundBank::withArchives(&bank, std::move(one));
        SoundBank::Entry got;
        check(!bank.prefetch("HUGE") && !bank.prefetch("HUGER"),
              "the bank does not prefetch them (no 64 MiB staging buffer is reserved)");
        check(!bank.get("HUGE", &got) && !bank.get("HUGER", &got), "nor play them");
        expect("still with no read asked for", g_largestRangeRead, 0);
    }
}

// ---------------------------------------------------------------------------
// 3d. Reads that fail: counted, retried a bounded number of times, never a
//     permanent silence from one bad read, and never a misclassified archive.
// ---------------------------------------------------------------------------
void testReadsThatFail(const MdkData& data) {
    g_realSystem = mdk::formats::fileSystem();
    mdk::formats::FileSystem failing = *g_realSystem;
    failing.readRange = countedReadRange;
    const MdkData flaky(&failing, data.dir());
    g_rangeReadsLeft = -1;
    g_rangeReads = 0;

    // An archive whose KIND reads fail does not open: it must not come back as
    // "sixteen entries, none of them a sound" (the level would then parse its
    // music as corridors).
    {
        Sni sni;
        g_rangeReadsLeft = 2;   // the header and the directory, then nothing
        check(!Sni::open(&sni, flaky, "TRAVERSE/LEVEL3/LEVEL3O.SNI"),
              "an archive that cannot be classified does not open");
        g_rangeReadsLeft = 0;
        check(!Sni::open(&sni, flaky, "TRAVERSE/LEVEL3/LEVEL3O.SNI"),
              "nor one whose header cannot be read");
        SoundBank none;
        check(!SoundBank::forLevel(&none, flaky, 3), "and the level's bank does not load over it");
        g_rangeReadsLeft = -1;
    }

    SoundBank whole;
    check(wholeBank(&whole, data, 3), "the reference bank loads");
    SoundBank::Entry want;

    // A read failure of the archive itself is counted by the archive.
    {
        Sni sni;
        check(Sni::open(&sni, flaky, "TRAVERSE/LEVEL3/LEVEL3O.SNI"), "the archive opens");
        std::vector<uint8_t> bytes;
        g_rangeReadsLeft = 0;
        check(!sni.getBytes(sni.entries()[0], &bytes), "a failed read is a failed getBytes");
        expect("and the archive counted it", sni.readFailures(), 1);
        g_rangeReadsLeft = -1;
        check(sni.getBytes(sni.entries()[0], &bytes), "the next read works");
    }

    // A prefetch that fails midway is abandoned; the sound is NOT lost: when the
    // reads work again it is read and plays.
    {
        SoundBank bank;
        check(SoundBank::forLevel(&bank, flaky, 3), "the bank loads");
        check(bank.prefetch("H3") && bank.prefetchStep(), "H3: one good step");
        g_rangeReadsLeft = 0;
        check(!bank.prefetchStep(), "the next step fails and ends the prefetch");
        check(bank.prefetching().empty(), "the staging is dropped");
        expect("one read failure", bank.stats().readFailures, 1);
        expect("nothing decoded", bank.soundsDecoded(), 0);
        check(!bank.prefetchStep(), "and there is nothing left to step");
        g_rangeReadsLeft = -1;
        SoundBank::Entry got;
        check(bank.get("H3", &got) && whole.get("H3", &want) && sameEntry(got, want),
              "the card works again: the track is read and is the whole file's");
    }

    // A sound whose reads KEEP failing is given up after kMaxReadFailures, and
    // from then on costs no read at all.
    {
        SoundBank bank;
        check(SoundBank::forLevel(&bank, flaky, 3), "the bank loads");
        g_rangeReadsLeft = 0;
        SoundBank::Entry got;
        for (int32_t attempt = 1; attempt <= SoundBank::kMaxReadFailures; attempt++) {
            const int before = g_rangeReads;
            check(!bank.get("H1", &got), "a failing read is no sound");
            expect("and it was an attempt", g_rangeReads - before, 1);
        }
        const int before = g_rangeReads;
        check(!bank.get("H1", &got) && !bank.get("H1", &got), "after the limit: still no sound");
        expect("and no more reads", g_rangeReads - before, 0);
        check(!bank.prefetch("H1"), "nor a prefetch");
        expect("the failures were counted", bank.stats().readFailures,
               SoundBank::kMaxReadFailures);
        g_rangeReadsLeft = -1;
        check(!bank.get("H1", &got), "given up is given up (until the level reloads)");
        expect("with no read", g_rangeReads - before, 0);
        // ONE failure is not the limit: another track fails once and then plays.
        g_rangeReadsLeft = 0;
        check(!bank.get("H5", &got), "H5 fails once");
        g_rangeReadsLeft = -1;
        check(bank.get("H5", &got) && whole.get("H5", &want) && sameEntry(got, want),
              "and plays the next time it is asked for");
        // A name the level has not got is missing at once, with no read at all.
        const int beforeMissing = g_rangeReads;
        check(!bank.get("NO_SUCH_SOUND", &got) && !bank.get("NO_SUCH_SOUND", &got),
              "a name the archives do not carry is not a sound");
        expect("and costs no read", g_rangeReads - beforeMissing, 0);
    }
    g_rangeReadsLeft = -1;
}

// ---------------------------------------------------------------------------
// 3d'. THE LENT READER (the 3DS's music channel): the prefetch's streamed steps
//     are ASKED FOR and taken back, never read on the calling thread; the decode
//     follows a slice a step; a read that fails, a drop, an early get() and the
//     bank's end each leave nothing in flight. The test plays the reader's
//     thread: a read is held until land() serves it.
// ---------------------------------------------------------------------------
struct FakeRead {
    int32_t tag = -1;
    std::string path;
    uint32_t offset = 0;
    uint32_t length = 0;
    bool landed = false;
    bool ok = false;
    bool cancelled = false;
    bool taken = false;
};

struct FakeReader {
    std::vector<FakeRead> reads;   // every request, in order
    int32_t cancels = 0;
    int32_t refusals = 0;
    bool refuse = false;           // the music slot is busy: requests fail
};

// The read the reader is holding (asked for, not landed, not cancelled), or null.
FakeRead* fakePending(FakeReader* reader) {
    for (size_t i = 0; i < reader->reads.size(); i++) {
        FakeRead& read = reader->reads[i];
        if (!read.landed && !read.cancelled) {
            return &read;
        }
    }
    return nullptr;
}

bool fakeRequestRange(void* user, int32_t tag, const char* path, uint32_t offset,
                      uint32_t length) {
    FakeReader* reader = static_cast<FakeReader*>(user);
    if (reader->refuse || fakePending(reader) != nullptr) {
        reader->refusals++;
        return false;   // one read in flight, as render.h's music channel
    }
    FakeRead read;
    read.tag = tag;
    read.path = path;
    read.offset = offset;
    read.length = length;
    reader->reads.push_back(read);
    return true;
}

bool fakePoll(void* user, int32_t tag, uint8_t** bytes, uint32_t* size, bool* ok) {
    FakeReader* reader = static_cast<FakeReader*>(user);
    for (size_t i = 0; i < reader->reads.size(); i++) {
        FakeRead& read = reader->reads[i];
        if (read.tag != tag || !read.landed || read.cancelled || read.taken) {
            continue;
        }
        read.taken = true;
        *ok = false;
        *bytes = nullptr;
        *size = 0;
        if (!read.ok) {
            return true;
        }
        // The thread's read: the host's own, of exactly the range asked for.
        FILE* file = fopen(read.path.c_str(), "rb");
        uint8_t* block = static_cast<uint8_t*>(malloc(read.length));
        if (file && block && fseek(file, (long)read.offset, SEEK_SET) == 0 &&
            fread(block, 1, read.length, file) == read.length) {
            *bytes = block;
            *size = read.length;
            *ok = true;
        } else {
            free(block);
        }
        if (file) {
            fclose(file);
        }
        return true;
    }
    return false;
}

void fakeCancel(void* user, int32_t tag) {
    FakeReader* reader = static_cast<FakeReader*>(user);
    reader->cancels++;
    for (size_t i = 0; i < reader->reads.size(); i++) {
        if (reader->reads[i].tag == tag) {
            reader->reads[i].cancelled = true;
        }
    }
}

// The held read lands (ok or failed). False when nothing is held.
bool fakeLand(FakeReader* reader, bool ok) {
    FakeRead* read = fakePending(reader);
    if (read == nullptr) {
        return false;
    }
    read->landed = true;
    read->ok = ok;
    return true;
}

mdk::AsyncRead lend(FakeReader* reader) {
    mdk::AsyncRead read;
    read.requestRange = fakeRequestRange;
    read.poll = fakePoll;
    read.cancel = fakeCancel;
    read.user = reader;
    return read;
}

void testTheLentReaderTakesTheReads(const MdkData& data) {
    g_realSystem = mdk::formats::fileSystem();
    mdk::formats::FileSystem failing = *g_realSystem;
    failing.readRange = countedReadRange;
    const MdkData flaky(&failing, data.dir());
    g_rangeReadsLeft = -1;

    SoundBank whole;
    check(wholeBank(&whole, data, 3), "the reference bank loads");
    SoundBank::Entry want;
    check(whole.get("H1", &want), "H1 is a sound");
    const int32_t h1 = lengthOf(data, "TRAVERSE/LEVEL3/LEVEL3O.SNI", "H1");

    // --- a prefetch with a lent reader: no read on this thread at all ---
    {
        FakeReader reader;
        SoundBank bank;
        check(SoundBank::forLevel(&bank, flaky, 3), "the bank loads");
        bank.setReader(lend(&reader));
        g_rangeReadsLeft = 0;   // from here, any read on this thread FAILS
        check(bank.prefetch("H1"), "the prefetch begins");
        int32_t calls = 0;
        bool more = true;
        bool heldOnce = false;
        while (more && calls < 1000) {
            more = bank.prefetchStep();
            calls++;
            if (fakePending(&reader) != nullptr) {
                // A held read holds the prefetch: steps go by and nothing moves.
                const int32_t stepsBefore = bank.stats().prefetchSteps;
                check(bank.prefetchStep(), "a held read is still in flight");
                calls++;
                heldOnce = heldOnce || bank.stats().prefetchSteps == stepsBefore;
                expect("nothing is decoded while it reads", bank.soundsDecoded(), 0);
                fakeLand(&reader, true);
            }
        }
        g_rangeReadsLeft = -1;
        check(heldOnce, "the reads were held and the prefetch waited for them");
        expect("every step went to the reader", bank.stats().prefetchReads, stepsFor(h1));
        expect("by the original's rule", (long long)reader.reads.size(), stepsFor(h1));
        expect("and was counted as a step", bank.stats().prefetchSteps, stepsFor(h1));
        expect("no read failed (none was made here)", bank.stats().readFailures, 0);
        // The ranges: back to back, each the rule's length, the last the tail.
        uint32_t next = reader.reads.empty() ? 0 : reader.reads[0].offset;
        bool contiguous = true;
        uint32_t total = 0;
        for (size_t i = 0; i < reader.reads.size(); i++) {
            contiguous = contiguous && reader.reads[i].offset == next &&
                         reader.reads[i].length <= (uint32_t)SoundBank::kPrefetchTailBytes;
            next += reader.reads[i].length;
            total += reader.reads[i].length;
        }
        check(contiguous, "the ranges are back to back and each one step long");
        expect("and are exactly the entry", (long long)total, h1);
        check(bank.stats().decodeSteps > 4, "the decode took slices");
        SoundBank::Entry got;
        check(bank.get("H1", &got) && sameEntry(got, want), "the sound is the whole file's");
        expect("playing it read nothing more", bank.stats().demandReads, 0);
        expect("and waited for nothing", bank.stats().prefetchStalls, 0);
    }

    // --- a refused request is asked again; a failed read ends the prefetch ---
    {
        FakeReader reader;
        SoundBank bank;
        check(SoundBank::forLevel(&bank, data, 3), "the bank loads");
        bank.setReader(lend(&reader));
        reader.refuse = true;
        check(bank.prefetch("H3") && bank.prefetchStep() && bank.prefetchStep(),
              "refused requests keep the prefetch in flight");
        expect("asked each time", reader.refusals, 2);
        reader.refuse = false;
        check(bank.prefetchStep() && fakePending(&reader) != nullptr, "then it is asked for");
        fakeLand(&reader, false);
        check(!bank.prefetchStep(), "a read that fails ends the prefetch");
        check(bank.prefetching().empty(), "the staging is dropped");
        expect("one read failure", bank.stats().readFailures, 1);
        SoundBank::Entry got;
        SoundBank::Entry wantH3;
        check(bank.get("H3", &got) && whole.get("H3", &wantH3) && sameEntry(got, wantH3),
              "the track still plays, read when it is played");
        expect("as a demand read (the last resort, counted)", bank.stats().demandReads, 1);
    }

    // --- a new prefetch, an early get() and the bank's end cancel the read ---
    {
        FakeReader reader;
        {
            SoundBank bank;
            check(SoundBank::forLevel(&bank, data, 3), "the bank loads");
            bank.setReader(lend(&reader));
            check(bank.prefetch("H1") && bank.prefetchStep(), "H1 is asked for");
            check(bank.prefetch("H3"), "H3 takes the staging");
            expect("H1's read was cancelled", reader.cancels, 1);
            check(bank.prefetchStep() && fakePending(&reader) != nullptr &&
                      fakePending(&reader)->tag != reader.reads[0].tag,
                  "H3's read is asked under a tag of its own");
            fakeLand(&reader, true);
            check(bank.prefetchStep(), "its first step lands");
            check(bank.prefetchStep() && fakePending(&reader) != nullptr, "the second is asked");
            SoundBank::Entry got;
            SoundBank::Entry wantH3;
            check(bank.get("H3", &got) && whole.get("H3", &wantH3) && sameEntry(got, wantH3),
                  "H3 wanted early is the whole file's");
            expect("a stall", bank.stats().prefetchStalls, 1);
            expect("which cancelled the read in flight", reader.cancels, 2);
            check(bank.prefetching().empty() && !bank.prefetchStep(), "nothing is left in flight");
            check(bank.prefetch("H5") && bank.prefetchStep() && fakePending(&reader) != nullptr,
                  "H5 is in flight when the level ends");
        }
        expect("the bank's end cancelled it", reader.cancels, 3);
    }

    // --- every track of every level: the sliced decode is the whole decode ---
    long long tracks = 0;
    for (int level = 3; level <= 8; level++) {
        SoundBank lent;
        SoundBank reference;
        check(SoundBank::forLevel(&lent, data, level) && wholeBank(&reference, data, level),
              "the banks load");
        FakeReader reader;
        lent.setReader(lend(&reader));
        Sni music;
        check(Sni::open(&music, data, archivePaths(level)[2]), "the music archive opens");
        for (size_t e = 0; e < music.entries().size(); e++) {
            if (!music.isSound(music.entries()[e])) {
                continue;
            }
            const std::string name = music.entries()[e].nameString();
            check(lent.prefetch(name), "the track prefetches", name);
            int32_t calls = 0;
            while (lent.prefetchStep() && calls < 1000) {
                fakeLand(&reader, true);
                calls++;
            }
            SoundBank::Entry got;
            SoundBank::Entry ref;
            check(lent.get(name, &got) && reference.get(name, &ref) && sameEntry(got, ref),
                  "a track decoded in slices is the track decoded whole", name);
            tracks++;
        }
        expect("nothing was read on demand", lent.stats().demandReads, 0);
        expect("nor stalled", lent.stats().prefetchStalls, 0);
    }
    check(tracks > 30, "every level's music was compared");
}

// ---------------------------------------------------------------------------
// 3e. A mixer that dies gives its voices' pins back (a respawn destroys the
//     runtime's mixer; the bank outlives it).
// ---------------------------------------------------------------------------
void testADeadMixerHoldsNoPins(const MdkData& data) {
    SoundBank bank;
    check(SoundBank::forLevel(&bank, data, 3), "the bank loads");
    for (int life = 0; life < 3; life++) {
        {
            SoundMixer mixer(&bank);
            check(mixer.playMusic("H1") != 0, "the music plays");
            check(mixer.playLooped("DOOR") != 0, "and a looping effect");
            expect("the track is pinned while it plays", bank.pins("H1"), 1);
            expect("the effect too", bank.pins("DOOR"), 1);
        }
        expect("a dead mixer leaves no pin on the track", bank.pins("H1"), 0);
        expect("nor on the effect", bank.pins("DOOR"), 0);
    }
    const size_t before = bank.decodedBytes();
    const size_t freed = bank.releaseMusicExcept(nullptr, 0);
    check(freed > 400000 && bank.decodedBytes() == before - freed,
          "so the track of an arena Kurt died in can be given back");
}

// ---------------------------------------------------------------------------
// 4. LevelMusic, against LevelMusic.cs.
// ---------------------------------------------------------------------------
void testTheMusicFollowsKurtsArena(const MdkData& data) {
    SoundBank bank;
    check(SoundBank::forLevel(&bank, data, 3), "the bank loads");
    SoundMixer mixer(&bank);

    LevelMusic::NameMap tracks;
    tracks.push_back(std::make_pair(std::string("HMO_1"), std::string("H1")));
    tracks.push_back(std::make_pair(std::string("HMO_2"), std::string("H3")));
    tracks.push_back(std::make_pair(std::string("HMO_3"), std::string("NONE")));
    tracks.push_back(std::make_pair(std::string("HMO_4"), std::string("H5")));
    tracks.push_back(std::make_pair(std::string("CHMO_9"), std::string("")));
    LevelMusic music(&mixer, &tracks);

    // LevelMusic.cs: FramesPerSecond = 1000f / 34f; FadeIn = 0x80 * that;
    // FadeOut = 0x100 * that.
    const float fps = 1000.0f / 34.0f;
    const float fadeIn = (float)0x80 * fps;
    const float fadeOut = (float)0x100 * fps;
    expectFloat("FadeIn is the C#'s", LevelMusic::kFadeIn, fadeIn);
    expectFloat("FadeOut is the C#'s", LevelMusic::kFadeOut, fadeOut);
    const float dt = 1.0f / 60.0f;

    // TrackOf: the arena's own, CORRIDOR for one without (or with an empty one).
    check(music.trackOf("HMO_1") == "H1", "an arena's track is the table's");
    check(music.trackOf("CHMO_1") == "CORRIDOR", "an arena the table has not got plays CORRIDOR");
    check(music.trackOf("CHMO_9") == "CORRIDOR", "and so does one with an empty track");
    check(music.trackOf("HMO_3") == "NONE", "NONE is a name like any other here");

    // Enter(null/empty): no change.
    music.enter("");
    check(music.playing().empty() && mixer.voices().empty(), "outside every arena nothing starts");

    // Enter(HMO_1): H1 from silence.
    music.enter("HMO_1");
    check(music.playing() == "H1", "Kurt's arena's track plays");
    expectFloat("from volume 0", music.volume(), 0.0f);
    expect("on one voice", (long long)mixer.voices().size(), 1);
    check(mixer.voices().size() == 1 && mixer.voices()[0].name == "H1", "which is H1's");
    const int32_t h1Voice = mixer.voices().empty() ? 0 : mixer.voices()[0].id;
    check(bank.pins("H1") == 1, "and its samples are pinned while it plays");

    // Update: _current.Volume = Min(Volume + FadeIn * delta, FullVolume), applied
    // as (int)Max(volume, 0).
    music.update(dt);
    float want = 0.0f + fadeIn * dt;
    expectFloat("one frame of the fade in", music.volume(), want);
    expect("is the voice's volume, truncated", mixer.voices()[0].volume, (int32_t)want);
    // Enter(the same arena): nothing.
    music.enter("HMO_1");
    expect("the same arena starts nothing", (long long)mixer.voices().size(), 1);
    for (int i = 0; i < 600; i++) {
        music.update(dt);
    }
    expectFloat("the fade in stops at full volume", music.volume(), (float)SoundMixer::kFullVolume);
    expect("and so does the voice", mixer.voices()[0].volume, SoundMixer::kFullVolume);

    // Enter(a corridor): CORRIDOR in, H1 fading from where it was.
    music.enter("CHMO_1");
    check(music.playing() == "CORRIDOR" && music.fading() == "H1", "a corridor: CORRIDOR in, H1 out");
    expectFloat("the new track starts at 0", music.volume(), 0.0f);
    expectFloat("the old one keeps its volume", music.fadingVolume(), (float)SoundMixer::kFullVolume);
    expect("two voices", (long long)mixer.voices().size(), 2);
    music.update(dt);
    expectFloat("the fade out is FadeOut a second", music.fadingVolume(),
                (float)SoundMixer::kFullVolume - fadeOut * dt);
    expectFloat("beside the fade in", music.volume(), fadeIn * dt);

    // Back to the fading track: they swap, and nothing new starts.
    const int32_t nextBefore = mixer.nextVoiceId();
    const float corridorVolume = music.volume();
    const float h1Volume = music.fadingVolume();
    music.enter("HMO_1");
    check(music.playing() == "H1" && music.fading() == "CORRIDOR", "back: the two swap");
    expectFloat("H1 goes on where it was", music.volume(), h1Volume);
    expectFloat("and CORRIDOR fades from where it was", music.fadingVolume(), corridorVolume);
    expect("no voice was started", mixer.nextVoiceId(), nextBefore);
    check(mixer.isVoicePlaying(h1Voice), "H1 is the voice it always was");

    // The fading track stops below 0: `if (_fading.Volume < 0f && Name != null)`.
    music.update(dt);
    check(music.fading().empty(), "CORRIDOR (one frame of volume) is below 0 and stopped");
    expect("one voice again", (long long)mixer.voices().size(), 1);
    check(!mixer.isPlaying("CORRIDOR"), "and it is H1's");

    // NONE: not a sound, so no voice; H1 fades out under it.
    music.enter("HMO_3");
    check(music.playing() == "NONE" && music.fading() == "H1", "NONE: H1 fades under silence");
    expect("NONE has no voice", (long long)mixer.voices().size(), 1);

    // A third track while one is fading: `if (_fading.Voice != 0)
    // StopVoice(_fading.Voice)` -- the fading one is stopped at once.
    //   enter HMO_3: fading = H1 (voice), current = NONE (0)
    //   enter HMO_2: fading.Voice != 0 -> stop H1; fading = NONE; current = H3
    //   enter HMO_4: fading.Voice == 0;            fading = H3;   current = H5
    music.enter("HMO_2");
    check(music.playing() == "H3" && music.fading() == "NONE", "H3 in; NONE is what 'fades'");
    check(!mixer.isVoicePlaying(h1Voice) && !mixer.isPlaying("H1"),
          "H1 was fading when H3 came in, so it was stopped at once");
    expect("H1's pin is released", bank.pins("H1"), 0);
    music.enter("HMO_4");
    check(music.playing() == "H5" && music.fading() == "H3", "H5 in, H3 out");
    check(mixer.isPlaying("H3") && mixer.isPlaying("H5"), "H3 and H5 are the two voices");
    expect("two voices", (long long)mixer.voices().size(), 2);
}

// The fading track stops BELOW zero, not at it: `if (_fading.Volume < 0f && ...)`.
// FadeOut is exactly twice FadeIn (0x100 against 0x80 of the same rate), so a
// track that faded IN for two frames fades OUT to exactly 0.0 in one -- and at
// exactly 0 the C# leaves it playing for one more frame.
void testTheFadeStopsBelowZeroNotAtIt(const MdkData& data) {
    SoundBank bank;
    check(SoundBank::forLevel(&bank, data, 3), "the bank loads");
    SoundMixer mixer(&bank);
    LevelMusic::NameMap tracks;
    tracks.push_back(std::make_pair(std::string("A"), std::string("H1")));
    tracks.push_back(std::make_pair(std::string("B"), std::string("H3")));
    // A name twice: GetValueOrDefault on a dictionary built by assignment is the
    // LAST value written.
    tracks.push_back(std::make_pair(std::string("TWICE"), std::string("H5")));
    tracks.push_back(std::make_pair(std::string("TWICE"), std::string("H6")));
    LevelMusic music(&mixer, &tracks);
    check(music.trackOf("TWICE") == "H6", "a name written twice is its last value");

    const float dt = 1.0f / 60.0f;
    music.enter("A");
    music.update(dt);
    music.update(dt);
    const float two = music.volume();
    expectFloat("two frames in is twice one frame", two, LevelMusic::kFadeIn * dt + LevelMusic::kFadeIn * dt);
    expectFloat("which is one frame of the fade out", two, LevelMusic::kFadeOut * dt);
    music.enter("B");
    music.update(dt);
    expectFloat("one frame out: exactly zero", music.fadingVolume(), 0.0f);
    check(music.fading() == "H1" && mixer.isPlaying("H1"),
          "at exactly zero the track is still fading (the C# stops it BELOW zero)");
    music.update(dt);
    check(music.fading().empty() && !mixer.isPlaying("H1"), "and the next frame stops it");
}

// The ambient loop under the music (the 1996 demo's Cmi.ArenaAmbience):
// PlayAmbience replaces the last one, plays it LOOPED, and an arena without one
// is silence.
void testTheAmbienceFollowsTheArena(const MdkData& data) {
    SoundBank bank;
    check(SoundBank::forLevel(&bank, data, 3), "the bank loads");
    SoundMixer mixer(&bank);
    LevelMusic::NameMap tracks;
    LevelMusic::NameMap ambience;
    ambience.push_back(std::make_pair(std::string("A"), std::string("DOOR")));
    ambience.push_back(std::make_pair(std::string("B"), std::string("DOOR")));
    ambience.push_back(std::make_pair(std::string("C"), std::string("MAD")));
    LevelMusic music(&mixer, &tracks, &ambience);
    SoundBank::Entry door;
    check(bank.get("DOOR", &door) && !door.looping, "DOOR is a one-shot in its archive");

    music.enter("A");
    check(mixer.isPlaying("DOOR"), "the arena's ambience plays");
    // CORRIDOR is the music of an arena with no track; the ambience is the other voice.
    int32_t doorVoice = 0;
    for (size_t i = 0; i < mixer.voices().size(); i++) {
        if (mixer.voices()[i].name == "DOOR") {
            doorVoice = mixer.voices()[i].id;
        }
    }
    check(doorVoice != 0, "on a voice of its own");
    // LOOPED even though its entry is not: a one-shot of DOOR's length would have
    // ended long before this, a loop is still playing.
    for (int i = 0; i < 600; i++) {
        mixer.device().update(1.0f / 60.0f);
        mixer.update(1.0f / 60.0f);
    }
    check(mixer.isVoicePlaying(doorVoice), "it loops (ten seconds on, a one-shot would be over)");

    const int32_t next = mixer.nextVoiceId();
    music.enter("B");
    check(mixer.isVoicePlaying(doorVoice) && mixer.nextVoiceId() == next,
          "the same ambience in the next arena is not restarted");
    music.enter("C");
    check(!mixer.isVoicePlaying(doorVoice) && mixer.isPlaying("MAD"),
          "a different one replaces it");
    music.enter("D");
    check(!mixer.isPlaying("MAD") && !mixer.isPlaying("DOOR"), "an arena without one is silence");

    // No ambience table (the retail game): nothing but the music is ever started.
    SoundMixer plain(&bank);
    LevelMusic retail(&plain, &tracks);
    retail.enter("A");
    expect("retail: the music's voice and no other", (long long)plain.voices().size(), 1);
}


// THE 3DS'S STREAMED MUSIC (SoundBank::addStreamedMusic, getMusic): a registered
// track answers the music bus with the files' geometry and no samples, is never
// read, decoded or prefetched, keeps the archives' script-visible half, and a
// name the archives lack (a soundtrack's extra track) exists for the music bus
// only. The effects' lookup (get) is untouched, and LevelMusic plays the same.
void testStreamedMusicIsNeverRead(const MdkData& data) {
    SoundBank bank;
    check(SoundBank::forLevel(&bank, data, 3), "the bank loads");
    bank.addStreamedMusic("H1", 32000, 1383680);
    bank.addStreamedMusic("AMB_DRIP", 32000, 2969920);
    SoundBank::Entry entry;
    check(bank.getMusic("H1", &entry), "a streamed track answers the music bus");
    expect("at the files' rate", entry.sampleRate, 32000);
    expect("and length", entry.frames, 1383680);
    expect("mono", entry.channels, 1);
    check(entry.looping && entry.music, "looping music, as LEVEL3O.SNI flags it");
    check(entry.sound != nullptr && entry.sound->samples.empty(),
          "with a sample-less stand-in the voice table can bind");
    check(bank.getMusic("AMB_DRIP", &entry) && entry.looping && entry.volume == 0,
          "a track the archives lack plays looping at volume 0");
    check(!bank.get("AMB_DRIP", nullptr), "but is no sound of the archives for anything else");
    check(!bank.prefetch("H1"), "a streamed track is never prefetched");
    expect("nothing decoded", (long long)bank.decodedBytes(), 0);
    expect("nothing read", bank.stats().demandReads, 0);
    check(bank.getMusic("H10", &entry) && entry.sound != nullptr && !entry.sound->samples.empty(),
          "an unregistered track is the bank's, decoded as before");
    const long long decodedBefore = (long long)bank.decodedBytes();

    SoundMixer mixer(&bank);
    LevelMusic::NameMap tracks;
    tracks.push_back(std::make_pair(std::string("HMO_1"), std::string("H1")));
    tracks.push_back(std::make_pair(std::string("HMO_9"), std::string("AMB_DRIP")));
    LevelMusic music(&mixer, &tracks);
    music.enter("HMO_1");
    check(music.playing() == "H1" && mixer.voices().size() == 1, "LevelMusic starts it");
    music.enter("HMO_9");
    check(music.playing() == "AMB_DRIP" && music.fading() == "H1" && mixer.voices().size() == 2,
          "and crossfades into the extra track");
    for (int i = 0; i < 600; i++) {
        music.update(1.0f / 60.0f);
        mixer.update(1.0f / 60.0f);
    }
    check(music.fading().empty() && mixer.voices().size() == 1, "the faded track stops");
    expect("the streamed tracks decoded nothing", (long long)bank.decodedBytes(), decodedBefore);
}

// THE DSP-ADPCM HISTORY (audio/dsp_adpcm.h): decoding a track in pieces, each
// piece started from the history the last one ended with, gives the samples a
// decode in one piece gives -- the per-buffer context the 3DS hands the DSP.
void testAdpcmHistoryCarriesAcrossBuffers() {
    std::vector<uint8_t> header(96, 0);
    const uint32_t samples = 14 * 50;
    header[0] = 0; header[1] = 0; header[2] = (uint8_t)(samples >> 8); header[3] = (uint8_t)samples;
    header[8] = 0; header[9] = 0; header[10] = 0x7D; header[11] = 0x00;  // 32000 Hz
    const int16_t coefs[16] = {2048, 0, 3600, -1600, 1900, -800, 0, 0,
                               4000, -2000, 1000, 500, -1000, 300, 2500, -1300};
    for (int i = 0; i < 16; i++) {
        header[0x1C + i * 2] = (uint8_t)((uint16_t)coefs[i] >> 8);
        header[0x1D + i * 2] = (uint8_t)coefs[i];
    }
    mdk::audio::DspAdpcmHeader head;
    check(mdk::audio::DspAdpcm::parseHeader(header.data(), 96, &head), "the header parses");
    expect("its length", head.samples, samples);
    expect("its rate", head.sampleRate, 32000);
    expect("a coefficient", head.coefs[3], -1600);
    std::vector<uint8_t> frames(50 * 8);
    uint32_t seed = 12345;
    for (size_t i = 0; i < frames.size(); i++) {
        seed = seed * 1103515245u + 12345u;
        frames[i] = (uint8_t)(seed >> 16);
        if (i % 8 == 0) {
            frames[i] &= 0x7B;  // a predictor pair 0-7, a scale 0-11
        }
    }
    std::vector<int16_t> whole(samples), pieces(samples);
    mdk::audio::DspAdpcmContext a, b;
    mdk::audio::DspAdpcm::decode(frames.data(), 50, head.coefs, &a, whole.data());
    const int cut[] = {0, 7, 8, 30, 50};
    for (int k = 0; k + 1 < 5; k++) {
        mdk::audio::DspAdpcmContext start = b;
        mdk::audio::DspAdpcm::decode(frames.data() + cut[k] * 8, cut[k + 1] - cut[k], head.coefs,
                                     &start, pieces.data() + cut[k] * 14);
        mdk::audio::DspAdpcmContext carried = b;
        mdk::audio::DspAdpcm::advance(frames.data() + cut[k] * 8, cut[k + 1] - cut[k], head.coefs,
                                      &carried);
        check(carried.history0 == start.history0 && carried.history1 == start.history1,
              "advance carries the history decode does");
        b = carried;
    }
    check(whole == pieces, "a track decoded in buffers is the track decoded whole");
    std::vector<uint8_t> bad = header;
    bad[0x0F] = 1;
    check(!mdk::audio::DspAdpcm::parseHeader(bad.data(), 96, &head), "a PCM header is refused");
}

} // namespace

namespace mdk {
namespace test {

int runAudioStreamTests(const formats::MdkData& data) {
    failures = 0;
    checks = 0;
    testAnOpenedArchiveIsTheWholeFile(data);
    testTheLevelsBankIsTheWholeFilesBank(data);
    testThePrefetchReadsAStepACall(data);
    testMusicNobodyNeedsIsReleased(data);
    {
        // The files the synthetic cases write: a private folder under TMPDIR.
        const char* tmp = getenv("TMPDIR");
        char folder[512];
        snprintf(folder, sizeof(folder), "%s/mdk-audio-stream-XXXXXX", tmp && tmp[0] ? tmp : "/tmp");
        if (mkdtemp(folder) == nullptr) {
            check(false, "a scratch folder for the synthetic archives");
        } else {
            g_scratch = folder;
            g_realSystem = mdk::formats::fileSystem();
            mdk::formats::FileSystem counting = *g_realSystem;
            counting.readRange = countedReadRange;
            g_rangeReadsLeft = -1;
            testSyntheticArchives(&counting);
            const char* files[] = {"STEPS.SNI", "MANY.SNI", "ANIM.SNI", "HUGE.SNI"};
            for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
                remove((g_scratch + "/" + files[i]).c_str());
            }
            remove(folder);
        }
    }
    testReadsThatFail(data);
    testTheLentReaderTakesTheReads(data);
    testADeadMixerHoldsNoPins(data);
    testTheMusicFollowsKurtsArena(data);
    testTheFadeStopsBelowZeroNotAtIt(data);
    testTheAmbienceFollowsTheArena(data);
    testStreamedMusicIsNeverRead(data);
    testAdpcmHistoryCarriesAcrossBuffers();
    if (failures == 0) {
        printf("  audio stream: %lld checks\n", checks);
    }
    return failures;
}

} // namespace test
} // namespace mdk

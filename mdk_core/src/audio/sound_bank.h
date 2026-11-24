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
// THE MEMORY, HONESTLY, AND MEASURED. `decodedSamples()`, `decodedBytes()` and
// `archiveBytes()` are the instrument, and `tick1 mixsize <level>` prints them from
// the store itself: level 3's three archives are 4,390,060 bytes on disk and decode
// to 3,234,976 samples (72 sounds) = 6,469,952 bytes in the int16 store, level 4 is
// 7,878,536 and level 8 is 10,002,084, with the archive blobs resident on top of
// that (10.86 / 13.78 / 15.82 MB fully decoded). What they are NOT is a verdict
// against a ceiling: the budget this bank has to live inside is the C++ heap the
// build asks for, `__heap_size = 0x01800000` = 24 MiB, read out of the shipped
// ELF's crt0 header, and the region the kernel actually grants
// (`osGetMemRegionSize(MEMREGION_APPLICATION)`) has never been read by anything in
// this repository. sound.h's memory section carries all three figures and says
// which is measured, which is documented and which is missing; do not restate a
// fourth. Nothing calls preload() on the 3DS today, and the console still has to
// choose between decode-on-demand with a budget and streaming; the store half of
// that choice is made (sound.h) and the budget half is recorded as an open item,
// not solved by pretending the numbers are smaller.
//
// WHICH ARCHIVE IS WHERE (docs/ARENA_STREAMING_SPEC.md Phase 5). forLevel()
// holds TRAVERSE.SNI and LEVELnS.SNI whole -- the level's effects, 0.8-1.5 MB,
// any of which a frame may start with no warning -- and OPENS LEVELnO.SNI by its
// directory (formats::Sni::open): that file is the level's MUSIC, 7-9 tracks of
// about half a megabyte (3.3-4.5 MB), plus the corridors, and the original never
// holds it either. It keeps the directory (0x57f378) and one 512,000-byte music
// buffer (0x491b40; "Sounds %s %s overflow buffersize" past 0x7d000), and reads
// an arena's track into it when the arena LOADS (0x4192c4 -> 0x431590), a step
// a frame (0x4193a4 -> 0x4317d8: 0xf768 bytes while more than 0x10000 remain,
// then the rest), so the track is in memory by the time Kurt walks in.
//
// prefetch() / prefetchStep() are that: begin reading a named sound, read one
// step per call, then DECODE IT A SLICE PER CALL (kDecodeStepSamples) once the
// last step has landed. A get() of a sound whose prefetch is still in flight
// finishes it at once and counts the stall; a get() of a streamed sound nobody
// prefetched reads it whole (counted as a demand read). Either way the answer is
// the same Entry the whole-file bank gives -- the format tests decode every sound
// of every retail archive both ways.
//
// NO STEP IS A WHOLE TRACK, AND ON THE 3DS NO STEP READS (measured on the Old
// 3DS, level 7's corridor crossing: a step's fopen + seek + 64 KiB read was 11-17
// ms of the main thread for eight frames, and the last step's decode of the
// whole track -- about half a million samples -- 74.9 ms in one frame).
//   - THE READS: a frontend with a file thread lends it (setReader; the 3DS's
//     kChannelMusic), and a streamed archive's step is then ASKED FOR on that
//     thread and taken back by a later call: the frame only appends the landed
//     bytes. One read in flight, one step long, so the thread is never held by a
//     track for more than an ordinary read. Without a reader (the desktop, the
//     host tests) a step reads on the calling thread, as the original does.
//   - THE DECODE: a slice a call, straight out of the read bytes (Wav::locate,
//     pcmToSamples: no copy of the track, no whole-track zero fill), into a store
//     the bank adopts when the last slice is in. On the main thread and not the
//     file thread ON PURPOSE: the decode is CPU, the Old 3DS gives the
//     application one core, and the reader thread runs only in the main thread's
//     waits -- a whole-track decode there would hold the one reader (Kurt's pack,
//     the orig pack's surfaces about to be drawn) for as many frames as it took
//     to find ~60 ms of idle core. A bounded slice on the frame costs a known
//     couple of milliseconds and holds nobody.
// The stall and the demand read stay as the LAST RESORT (a track wanted before
// its prefetch is in): read and decoded at once, and counted.
// ============================================================================

#include <stdint.h>

#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "audio/sound.h"
#include "formats/sni.h"
#include "formats/wav.h"
#include "game/async_read.h"

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
        // SNI flag 2: a music track. Not in the C#'s Entry (nothing there asks);
        // here it is what releaseMusicExcept() goes by.
        bool music = false;
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
    // A read still in flight on a lent reader is cancelled (the thread frees it).
    ~SoundBank() { stagingDrop(); }
    SoundBank(const SoundBank&) = delete;
    SoundBank& operator=(const SoundBank&) = delete;

    // The three archives a level plays from. FALSE when one cannot be read or
    // parsed (the C# throws there).
    static bool forLevel(SoundBank* out, const formats::MdkData& data, int32_t level);

    // A bank over archives the caller already has (the 1996 demo's).
    static bool withArchives(SoundBank* out, std::vector<formats::Sni>&& archives);

    // What the bank has read and waited for (a streamed archive only).
    struct Stats {
        int32_t prefetches = 0;       // prefetch() calls that began a read
        int32_t prefetchSteps = 0;    // steps read by prefetchStep()
        int32_t prefetchReads = 0;    // ... of which on the lent reader's thread
        int32_t decodeSteps = 0;      // decode slices of prefetchStep()
        int32_t prefetchStalls = 0;   // get() had to finish a prefetch in flight
        int64_t prefetchStallBytes = 0;
        int32_t prefetchDrops = 0;    // a prefetch replaced before it finished
        int32_t demandReads = 0;      // get() read a streamed sound nobody prefetched
        int64_t demandBytes = 0;
        int32_t readFailures = 0;     // a read that failed (the sound is then missing)
        int32_t evictions = 0;        // decoded sounds dropped for the budget
        int32_t musicReleased = 0;    // tracks dropped by releaseMusicExcept()
    };

    // The original's music step (0x4317d8): 0xf768 bytes while more than
    // 0x10000 remain, then whatever is left.
    static const int32_t kPrefetchStepBytes = 0xf768;
    static const int32_t kPrefetchTailBytes = 0x10000;
    // The decode's slice: 32768 samples (64 KiB of the int16 store) a call. A
    // track of ~500,000 samples is ~16 slices; the whole-track decode measured
    // 74.9 ms on the Old 3DS with its tail read, so a slice is a few ms at most
    // and less now that it neither copies the WAV nor zero-fills the whole store.
    static const int32_t kDecodeStepSamples = 32768;

    // A READ THAT FAILS IS NOT "THE LEVEL HAS NO SUCH SOUND". The C# caches null
    // for a name its archives do not carry, and so does this (missing_); but a
    // streamed sound can also fail because the card did, and caching THAT would
    // silence a track until the game is restarted. So a read failure is counted
    // per name and the name is tried again the next time something asks for it
    // -- at most this many times, after which it is missing like any other.
    // Nothing asks per frame (the music asks when Kurt's arena changes, a script
    // when it plays), so this is not a retry in the frame loop (AGENTS.md P5).
    static const int32_t kMaxReadFailures = 3;

    // Begin reading `name` a step per prefetchStep(). True when a read began;
    // false when there is nothing to do (already decoded, not a sound of the
    // archives, or known missing). A prefetch still in flight for ANOTHER name
    // is dropped: there is one staging buffer, as the original has one music
    // buffer.
    bool prefetch(const std::string& name);
    // One step of the prefetch in flight: a read step (asked for on the lent
    // reader, or taken back from it once it lands; read here without one), or,
    // once every byte is in, a decode slice. The sound is in the bank when the
    // last slice is. True while more steps remain.
    bool prefetchStep();
    // The file thread the prefetch's streamed reads go to (game/async_read.h,
    // requestRange/poll/cancel; one read in flight). Unset: they read on the
    // calling thread. A read in flight is cancelled when the prefetch is dropped
    // or the bank goes, so the reader's user must outlive the bank.
    void setReader(const AsyncRead& reader) { reader_ = reader; }
    // The sound a prefetch is reading (empty: none).
    const std::string& prefetching() const { return staging_.name; }
    const Stats& stats() const { return stats_; }

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

    // The decoded store's budget, in bytes (0 = unbounded, the shipped
    // default: the C#'s bank keeps everything it decoded). Over the
    // budget, decoding a sound evicts the least recently gotten decoded
    // sounds until it fits -- except pinned ones (below) and the sound
    // just decoded, so a bank whose every resident is pinned stays over
    // budget rather than dropping a sound in use. The 3DS's 24 MiB heap
    // (sound.h's memory section) is why this exists; which budget a
    // frontend sets is its own call.
    void setDecodedBudgetBytes(size_t bytes) { budgetBytes_ = bytes; }
    size_t decodedBudgetBytes() const { return budgetBytes_; }

    // Pin a decoded sound against eviction (a voice is playing it), and
    // release the pin. Counts nest; an unknown name is a no-op.
    void retain(const std::string& name);
    void release(const std::string& name);
    int32_t pins(const std::string& name) const;

    // Drop every decoded MUSIC track (SNI flag 2) that no voice is playing and
    // that is not one of `keep[0..count)`: the spec's "free the decoded samples
    // of an arena when it is deactivated" (docs/ARENA_STREAMING_SPEC.md Phase
    // 5). The frontend keeps the track playing, the one fading out and the one
    // of the arena that is loading, so a level played end to end holds three
    // tracks and not all nine. Effects are never touched (they are the
    // budget's business). Returns the bytes given back. A dropped track that is
    // asked for again is read again, like any other.
    size_t releaseMusicExcept(const std::string* keep, int32_t count);

    int32_t archiveCount() const { return (int32_t)archives_.size(); }

    // How many names have actually decoded (a bank that has only had one lookup
    // answers 1, not the archives' entry count).
    int32_t soundsDecoded() const { return (int32_t)sounds_.size(); }

    // The decoded sample payload: the samples this bank holds and the bytes they
    // occupy (count x the store's element size, so it follows the store's width
    // instead of restating it). The bank is what owns this memory, so it is what
    // can say how big it is — the number a preload budget or an eviction policy
    // has to compare against FCRAM. Payload only: not the vector or map nodes,
    // and not the archive bytes the Sni still holds. Zero for a bank that has
    // decoded nothing, because nothing has been decoded.
    size_t decodedSamples() const;
    size_t decodedBytes() const;

    // The archive blobs that are RESIDENT (an Sni that was parsed owns its
    // bytes and the bank owns its Snis): TRAVERSE.SNI and LEVELnS.SNI for a
    // forLevel() bank, whose music archive is opened by its directory and
    // counts for nothing here. The other half of what the bank costs, beside
    // decodedBytes().
    size_t archiveBytes() const;

private:
    // Look the name up in the archives, newest first: the archive and the
    // entry of the sound that wins, or false.
    bool find(const std::string& name, size_t* archive, const formats::Sni::Entry** entry) const;
    // Read the name's bytes (finishing its prefetch, if one is in flight) and
    // decode them. `*readFailed` says the failure was a READ, not the data.
    bool load(const std::string& name, Entry* out, bool* readFailed);
    // A read of `name` failed: count it, and give the name up at the limit.
    void noteReadFailure(const std::string& name);
    // The entry's WAV bytes into the bank's decoded store.
    bool decode(const std::string& name, const formats::Sni::Entry& entry,
                const std::vector<uint8_t>& bytes, Entry* out);

    // The one prefetch in flight.
    struct Staging {
        std::string name;        // empty: none
        size_t archive = 0;
        formats::Sni::Entry entry;
        std::vector<uint8_t> bytes;   // read so far
        int32_t readTag = -1;    // the step asked of the reader (-1: none)
        // The decode, once every byte is in: the format and the data chunk
        // (Wav::locate), the samples it makes and how many are converted.
        bool decoding = false;
        formats::Wav format;
        int32_t dataAt = 0;
        int32_t dataSize = 0;
        int32_t samples = 0;
        Sound sound;
    };
    // Read up to `limit` more bytes of the staging (0: all that remain) on this
    // thread. False on a read failure, which abandons the prefetch.
    bool stagingRead(int32_t limit);
    // A read step on the lent reader: asked for, or taken back. False on a read
    // failure (the prefetch is abandoned).
    bool stagingReadAsync(int32_t step);
    // One decode slice (the first locates the data chunk). False when the bytes
    // are not a WAV (the name is then missing) or the sound is in.
    bool stagingDecodeStep();
    // The read in flight cancelled, and everything staged freed.
    void stagingDrop();

    // Mark a decoded sound as recently gotten, and evict over-budget
    // residents (least recently gotten first, pinned and `keep` excepted).
    void touch(const std::string& name);
    void evictOverBudget(const std::string& keep);

    size_t budgetBytes_ = 0;
    std::map<std::string, int32_t> pins_;
    std::deque<std::string> recent_;
    std::vector<formats::Sni> archives_;
    std::map<std::string, Entry> cache_;
    // The decoded samples, one per cached name. A std::map node never moves, so
    // the Entry's `sound` pointer stays good however many names are added after
    // it — which is the sharing the C# gets from Sound being a reference type.
    std::map<std::string, Sound> sounds_;
    std::set<std::string> missing_;
    // Read failures per name, until kMaxReadFailures makes the name missing.
    std::map<std::string, int32_t> readFailures_;
    Staging staging_;
    AsyncRead reader_;
    int32_t nextReadTag_ = 0;
    Stats stats_;
};

} // namespace audio
} // namespace mdk

#pragma once

// ============================================================================
// arena_stream.h -- which arena's textures are in memory, and how they get
// there (docs/ARENA_STREAMING_SPEC.md Phase 2; the design and its evidence are
// in docs/CONTINUATION.md, "ARENA STREAMING PHASE 2").
//
// THE ORIGINAL (MDKD3D.EXE, rechecked in Ghidra 2026-11-18) has ONE buffer for
// the level's MTO arenas. arena_switch (0x41970c) drops the previous arena's
// pointers into it and starts reading the new arena's block; arena_load_step
// (0x41983c -> 0x423540) reads 32 KiB of it per frame and only when the last
// chunk is in sets the arena up; BSPShow (0x41a11c) -- a door opening, a
// crossing, the level's start -- loops that step until the stream is done, so
// an arena is never SHOWN half-read; arena_set_neighbour (opcode 223) and a
// type-3 trigger start the same stream and let it run in the background.
//
// THE PORT keeps the same machine for the part of an arena that is big: its
// texture PIXELS (60-72% of a level's MTO). Geometry, collision and models stay
// resident for every arena, so nothing the scripts or the collision compute can
// depend on how far a stream has got: the simulation stays the oracle's.
//
//   update()   once per frame, after the scripts' tick, with what the scripts
//              say: Kurt's arena, the second arena (loaded: behind a door, just
//              left, or preloading), whether the second is ACTIVE, and the
//              arenas the frame DRAWS (ScriptRuntime::drawnArenas).
//              - a different MTO arena in the pair is the new resident one: the
//                old one's pixels go FIRST, then the new one's stream starts
//                (switch) -- the order the original's one buffer forces, and
//                what keeps the peak at one arena;
//              - a stream whose arena is drawn is finished NOW and counted as a
//                stall (BSPShow); otherwise it advances one 32 KiB step;
//              - afterwards no other MTO arena keeps pixels (the trim).
//
//   THE DRAWN LIST IS NOT THE PAIR. The scripts snapshot it in the middle of
//   their tick and the pair is the tick's END, so a teleport out of a script
//   (opcodes 112/173) leaves the frame drawing the arena Kurt just left while
//   the pair already names the one he is in. Whatever is drawn keeps its
//   pixels for that frame, whatever the pair says (review, 2026-11-18).
//
//   A READER OF PIXELS THAT ARE AWAY (Texture::indices through the arena's
//   pixel source) gets them at once: a demand load, counted. It is the backstop
//   for the one retail arena that draws another arena's textures (level 6
//   OLYM_9 borrows three from OLYM_3) and for anything that reads a texture
//   outside a frame loop (tools, tests): correct first, counted so it shows.
//   Those pixels last until the next update()'s trim. Eviction happens ONLY in
//   update(), never under a reader.
//
// TWO MTO ARENAS LOADED TOGETHER cannot happen by walking: all 84 connections
// of levels 3-8 join an arena to a corridor (format test). A teleport can make
// the pair; the original would then thrash its one buffer, and the port keeps
// both and counts the frames (Stats::pairFrames).
//
// THE LEVEL OWNS THE STREAM (LevelData::stream()); the frontends call update().
// A level nobody calls update() on (a tool, a test) still works: every read is
// a demand load and nothing is trimmed.
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

namespace mdk {

namespace formats {
class Arena;
class Texture;
} // namespace formats

class LevelData;

class ArenaStream {
public:
    // One step's read (0x423540: min(remaining, 0x8000)).
    static const int32_t kChunkBytes = 0x8000;

    // An arena whose reads keep failing is left alone after this many, so a bad
    // card is a counted failure and not a retry in every frame (AGENTS.md P5).
    static const int32_t kMaxReadFailures = 3;

    struct Stats {
        int32_t switches = 0;      // arena_switch: a different MTO arena began streaming
        int32_t steps = 0;         // 32 KiB reads, one per update()
        int32_t stalls = 0;        // streams finished at once because the arena was drawn
        int64_t stallBytes = 0;    // ... and the bytes read in those
        int32_t demandLoads = 0;   // a reader found pixels away
        int64_t demandBytes = 0;
        int32_t pairFrames = 0;    // update()s with two MTO arenas loaded (a teleport)
        int32_t readFailures = 0;
        int64_t bytesRead = 0;     // everything read from the file
        int32_t trims = 0;         // arenas whose pixels a trim dropped
        // The most MTO arenas holding pixels at the end of an update(), ever.
        // 1 in walking play; 2 only in a pairFrame or a teleport's frame.
        int32_t maxResidentArenas = 0;
        // The most pixel bytes resident at any moment pixels came in (measured
        // right after an adoption, BEFORE the trim): the transient a switch
        // costs. One arena's worth when the old arena is dropped first.
        int64_t peakPixelBytes = 0;
    };

    ArenaStream() = default;
    // The textures point back at this object: it does not copy or move.
    ArenaStream(const ArenaStream&) = delete;
    ArenaStream& operator=(const ArenaStream&) = delete;

    // Bind to a loaded level: every MTO arena's pixels are dropped and its
    // textures ask this stream when read. LevelData::load calls it.
    void attach(LevelData* level);
    // Unbind (the level is going or being reloaded). The arenas are not touched.
    void reset();

    // The frame's step. See the header. `drawn` is the list the frame draws.
    void update(const std::string& current, const std::string& second, bool secondActive,
                const std::vector<std::string>& drawn);
    // The same with the drawn list the pair implies: Kurt's arena, and the
    // second when it is active. (Tools and tests; a frontend passes the
    // scripts' own list.)
    void update(const std::string& current, const std::string& second, bool secondActive);

    // Read an arena's pixels in now. True when they are resident afterwards
    // (also when they already were). Does not change which arena is resident.
    bool loadNow(const std::string& arena);

    // The MTO arena that has the stream (the original's _g_current_arena), or "".
    const std::string& resident() const { return resident_; }
    // The arena whose stream is still running (the original's 0x573b00), or "".
    const std::string& streaming() const { return streaming_; }
    // The running stream's bytes read / total (0 / 0 when none).
    int32_t streamedBytes() const { return cursor_; }
    int32_t streamTotalBytes() const { return total_; }

    // How many MTO arenas hold pixels now, and how many bytes in all.
    int32_t residentArenas() const;
    int64_t residentBytes() const;

    const Stats& stats() const { return stats_; }

private:
    static void loadThunk(void* user, const formats::Texture* texture);

    // Whether `name` is an arena this stream manages: it came from an MTO block.
    formats::Arena* streamed(const std::string& name) const;
    // Every texture's pixels are in.
    static bool pixelsIn(const formats::Arena* arena);
    void switchTo(const std::string& name);
    // One read of the running stream; completes it when it was the last. False
    // when the read failed (the stream is abandoned).
    bool step();
    // The staged bytes are the arena's whole texture prefix: parse and adopt.
    bool adopt(formats::Arena* arena, const uint8_t* bytes, int32_t byteCount);
    void abandon();
    // Drop the pixels of every MTO arena that is not the resident one, the
    // streaming one, or in kept_.
    void trim();
    bool isKept(const std::string& name) const;
    bool failedTooOften(const std::string& name) const;
    void noteFailure(const std::string& name);

    LevelData* level_ = nullptr;
    std::string resident_;
    std::string streaming_;
    // MTO arenas other than the resident one that keep their pixels this
    // frame: the second of a teleport's pair, and anything else that is drawn.
    std::vector<std::string> kept_;
    std::vector<uint8_t> staging_;   // the running stream's bytes so far
    std::vector<uint8_t> chunk_;     // one read, reused
    int32_t cursor_ = 0;
    int32_t total_ = 0;
    // Pixels came in or the resident arena changed since the last trim: the
    // trim walks every arena's textures, so it runs only when there is
    // something it could find.
    bool trimPending_ = false;
    std::vector<std::pair<std::string, int32_t>> failures_;
    Stats stats_;
};

} // namespace mdk

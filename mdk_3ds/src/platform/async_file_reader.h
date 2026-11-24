#pragma once

// ============================================================================
// async_file_reader.h -- whole files read from the SD card on a worker thread.
//
// THE HD STREAM'S I/O (docs/HD_TEXTURES_3DS_SPEC.md 4.2). An SD read of a
// 100-350 KB .t3x is an FS-service round trip measured in tens of milliseconds
// on hardware; done at the frame boundary it is a hitch per texture. So the
// renderer hands PATHS to this reader and takes back BUFFERS: the worker reads,
// the main thread imports (the import -- a copy into the linear heap -- is the
// only part that has to be on the thread that owns citro3d, AGENTS.md P2).
//
// THE THREAD (AGENTS.md P4, R1, §10):
//   - owned and joined: start() creates it, stop() signals, joins and frees it,
//     and frees every buffer still held; the renderer's destroy calls stop();
//   - FZ/DN at entry (vfpThreadFzDn), though it does no float work;
//   - a small stack (kStackBytes) and no large locals: the path is copied into
//     a fixed slot, the bytes live in a malloc'd buffer;
//   - priority one BELOW the main thread's, on the application core: on the Old
//     3DS it runs while main waits (the frame end's vsync), never instead of it.
//   - synchronisation: one LightLock guards the slots; a LightEvent wakes the
//     worker. No `volatile` stands in for either.
//
// TWO CHANNELS, ONE THREAD. The HD stream (kChannelHd: whole files) and Kurt's
// pack stream (kChannelPack: a byte RANGE of romfs:/kurt/kurt.pak, one
// animation's frames, KurtSprite's stream) share the worker and its slots, so
// no second thread is made for the pack. Each caller polls its own channel only
// -- a finished read is never handed to the other's importer -- and counts only
// its own slots (busy). The pack stream keeps at most one read in flight, so the
// HD stream always has the other slots.
//
// A THIRD CHANNEL, kChannelFile: whole files for the menu screens (the
// slideshow's GIFs, MDK12.FLC; game/async_read.h through render.h's
// mdk_render_read_file_async). Its users take their reads BY TAG (pollTag), so two
// of them never take each other's, and drop one they no longer want (discard):
// a finished read is freed at once, a read in progress when the worker ends it.
// They keep one or two reads in flight, never all the slots.
//
// A FOURTH CHANNEL, kChannelOrig: the orig pack's .t3x files (romfs:/orig, the
// arena textures' ORIGINAL look pre-encoded), whole files, tagged by texture id.
// The renderer asks for them at the frame boundary and imports them there
// (04_textures.inc, pumpPackOriginals); a draw that cannot wait takes its read
// by tag (pollTag) or drops it (discard) and reads the file itself.
//
// WHICH READ GOES NEXT. The worker takes the Requested slot whose channel ranks
// first -- Kurt's pack (0), the menus' files (1), the orig pack (2), the HD
// stream (3) -- and within a rank the oldest request (`order`, stamped by
// queue()). Kurt's animation and a menu's picture are what a player is waiting
// on; an orig texture is a surface about to be drawn; an HD copy only replaces a
// surface that already draws. And the STREAMS (HD, orig) only ask while fewer
// than kStreamSlots slots are busy, so the streams never fill the reader: one
// slot is always left that only Kurt's read or a menu's can take. It is ONE slot
// for the two of them -- shared, first come -- not one each: a menu's read in it
// makes Kurt's wait for the next free slot like any other request.
// ============================================================================

#include <3ds.h>
#include <stddef.h>
#include <stdint.h>

namespace mdk {

class AsyncFileReader {
public:
    static constexpr int32_t kSlots = 4;
    static constexpr size_t kPathMax = 192;
    static constexpr size_t kStackBytes = 16 * 1024;
    static constexpr int32_t kChannelHd = 0;
    static constexpr int32_t kChannelPack = 1;
    static constexpr int32_t kChannelFile = 2;
    static constexpr int32_t kChannelOrig = 3;
    // The slots the streams (kChannelHd, kChannelOrig) may hold between them:
    // one fewer than all, so one slot is left for Kurt's read and the menus'
    // (shared between them, not one each).
    static constexpr int32_t kStreamSlots = kSlots - 1;
    // The largest range read: more than any animation's frames (K_CHUTEC's 12
    // are 163,536 bytes); a corrupt index cannot size a bigger allocation.
    static constexpr uint32_t kMaxRangeBytes = 1u << 20;

    AsyncFileReader() = default;
    AsyncFileReader(const AsyncFileReader&) = delete;
    AsyncFileReader& operator=(const AsyncFileReader&) = delete;

    // Starts the worker. False (and the reader stays unusable) when the thread
    // cannot be created.
    bool start();
    // Stops and joins the worker, then frees every buffer it still holds. Safe
    // to call when start() failed or was never called.
    void stop();

    // Asks for `path` under `tag`, the whole file, on kChannelHd. False when
    // every slot is busy (try again next frame), the path is too long, or the
    // reader is not running.
    bool request(int32_t tag, const char* path);

    // Asks for `length` bytes at `offset` of `path` under `tag` on `channel`.
    // The read succeeds only when all `length` bytes are read: a short read
    // (a truncated file) is ok=false, never a partial buffer. False as request.
    bool requestRange(int32_t channel, int32_t tag, const char* path, uint32_t offset,
                      uint32_t length);

    // Work the worker runs on a whole file's bytes before handing them back
    // (kChannelFile's GIF decode): it returns a malloc'd block and its size, or
    // null (ok=false). The worker frees the bytes it read.
    typedef uint8_t* (*Work)(const uint8_t* bytes, uint32_t size, uint32_t* outSize);
    // Asks for the whole of `path` under `tag` on `channel`, `work` (null: none)
    // run on it on the worker. False as request.
    bool requestFile(int32_t channel, int32_t tag, const char* path, Work work);

    // Takes one finished read on `channel`, if any: its tag, the path it read
    // (into `path`, kPathMax bytes -- a tag can be reused before its old read
    // finishes, and the path is what tells the two apart), and either the bytes
    // (malloc'd; the caller frees them) and their size, or ok=false (the file
    // could not be read). False when nothing on that channel is finished.
    bool poll(int32_t channel, int32_t* tag, char* path, uint8_t** bytes, size_t* size, bool* ok);

    // Takes the finished read under `tag` on `channel`, if it is finished: its
    // bytes (malloc'd; the caller frees them) and size, or ok=false. False while
    // it is not (or there is no such read).
    bool pollTag(int32_t channel, int32_t tag, uint8_t** bytes, size_t* size, bool* ok);

    // Drops every read under `tag` on `channel`: a queued one is forgotten, a
    // finished one's bytes freed, one in progress freed by the worker when it
    // ends. Its slot is free again either way.
    void discard(int32_t channel, int32_t tag);

    bool running() const { return running_; }

    // Slots of `channel` holding a request, a read in progress or an untaken
    // result.
    int32_t busy(int32_t channel);

    // Slots of any channel not free: a request on any channel fails while this
    // is kSlots.
    int32_t busyAll();

    // The worker's loop (the thread's entry, asyncFileReaderThread, calls it
    // after establishing FZ/DN). Not for any other caller.
    void run();

private:
    enum State : uint8_t { Free, Requested, Reading, Done };
    struct Slot {
        State state = Free;
        int32_t channel = kChannelHd;
        int32_t tag = -1;
        // A range read (requestRange): `length` bytes at `offset`. 0: the whole
        // file (request).
        uint32_t offset = 0;
        uint32_t length = 0;
        char path[kPathMax] = {0};
        uint8_t* bytes = nullptr;
        size_t size = 0;
        bool ok = false;
        // discard() on a read in progress: the worker frees it when it ends.
        bool discarded = false;
        Work work = nullptr;
        // When it was asked for (queue()'s nextOrder_): the worker takes the
        // oldest request of the best-ranked channel first.
        uint32_t order = 0;
    };


    bool queue(int32_t channel, int32_t tag, const char* path, uint32_t offset, uint32_t length,
               Work work);

    Slot slots_[kSlots];
    LightLock lock_;
    LightEvent wake_;
    Thread thread_ = nullptr;
    bool running_ = false;
    bool exit_ = false;   // read and written under lock_ only
    uint32_t nextOrder_ = 0;   // read and written under lock_ only
};

} // namespace mdk

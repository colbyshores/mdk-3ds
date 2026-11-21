#pragma once

// ============================================================================
// async_read.h -- a frontend's file reader, lent to a screen that would
// otherwise read a file on the main thread inside one frame (the owner's rule:
// no main-thread stalls; AGENTS.md P5: no unpredictable blocking read inside a
// frame).
//
// The 3DS lends its file-reader thread (platform/async_file_reader.h, through
// render.h's mdk_render_read_file_async / poll / cancel): the main menu's
// slideshow (game/slideshow.h) and MDK12.FLC (game/video_player.h) are read
// there, and the screen shows what it has until the bytes arrive. With no
// reader (null hooks: the desktop, the host tests) the screen reads at once,
// as the C# does.
//
// WORK ON THE READER'S THREAD: a request may name a function the reader runs on
// the bytes before handing them back (the slideshow's GIF decode, formats::Gif::
// decodePacked), so the decode leaves the main thread too. It takes the file's
// bytes and returns a malloc'd block (its size in *outSize) or null for "not
// usable" (ok=false). It must touch no shared state.
//
// One read per tag. poll() takes only the read made under `tag`, so two users
// of one reader never take each other's bytes; cancel() drops it whatever its
// state (a read in progress is freed by the worker when it ends), so a screen
// that closes with a read outstanding leaves nothing behind.
//
// No virtuals, no std::function (AGENTS.md §10): function pointers and a user.
// ============================================================================

#include <stdint.h>

namespace mdk {

typedef uint8_t* (*AsyncWork)(const uint8_t* bytes, uint32_t size, uint32_t* outSize);

struct AsyncRead {
    // Asks for the whole file at `path` (a resolved path) under `tag`, `work`
    // (null: none) run on it on the reader's thread. False when it cannot be
    // queued now (try again on a later frame).
    bool (*request)(void* user, int32_t tag, const char* path, AsyncWork work) = nullptr;
    // The read under `tag`, once finished: its bytes (malloc'd; the caller
    // free()s them) and size, or ok=false with null bytes (the file could not
    // be read). False while it is not finished.
    bool (*poll)(void* user, int32_t tag, uint8_t** bytes, uint32_t* size, bool* ok) = nullptr;
    // Drops the read under `tag`: queued, in progress or finished.
    void (*cancel)(void* user, int32_t tag) = nullptr;
    void* user = nullptr;

    bool active() const { return request != nullptr && poll != nullptr; }
};

} // namespace mdk

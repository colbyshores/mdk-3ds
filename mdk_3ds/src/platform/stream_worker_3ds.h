#pragma once

// ============================================================================
// stream_worker_3ds.h -- the one streaming worker, on core 1, on the Old and the
// New 3DS alike (docs/3DS_ARCHITECTURE.md section 2).
//
// THE LAYOUT. Core 0 runs the game: input, simulation, every Citro3D call, and
// the hand-off of finished assets to the renderer at a frame boundary. Core 1,
// the system core, runs ONE application thread -- this worker -- which does the
// rest: music streaming (audio_dsp_3ds.cpp), every file read the renderer and
// the menus ask for (async_file_reader.cpp, read in chunks) with the work done
// on what it read (the HD copies' LZ11 unpack, the menus' GIF decode), and the
// log's writes (log_3ds.cpp). The New 3DS's cores 2 and 3 are not used; no code
// here asks which model it runs on.
//
// THE ORDER, every pass: the music first (when its 25 ms tick is due, or a
// start, stop or prime asks), then ONE chunk of the best-ranked read
// (AsyncFileReader::kChunkBytes, so no file holds the music up longer than a
// chunk's read), then the log when nothing else is waiting or its lines have
// waited kLogMaxWaitMs. A pass that leaves nothing to read waits for a wake-up
// (streamWorkerWake) or the music's next tick.
//
// CORE 1'S TERMS. The kernel lets the application run one thread there, and
// only after APT_SetAppCpuTimeLimit has granted it a share of that core's time
// (kSyscorePercent, measured: docs/WORK_LEDGER.md 2026-11-26). If the share is
// refused, or the thread cannot be made there, the worker is made on the
// process's default core (-2) and the log says so -- that breaks the layout, so
// it is said, never silent.
//
// LIFETIME AND OWNERSHIP (AGENTS.md P4). main starts the worker after the log
// opens and stops it (signal, join, free) before the log closes. A service is
// ATTACHED while its owner wants it served and DETACHED before its owner frees
// anything the worker reads: attach and detach take the lock the worker holds
// for its whole pass over the services, so after a detach returns the worker
// is not inside that service and will not enter it again. The services' data
// keep their own locks (the reader's slots, the music's requests).
// ============================================================================

#include <3ds.h>
#include <stdint.h>

namespace mdk {

class AsyncFileReader;

// The share of core 1's time asked for (APT_SetAppCpuTimeLimit).
constexpr uint32_t kSyscorePercent = 30;

// Starts the worker on core 1 (asking for the share first). False when no
// thread could be made at all: every service then falls back to its caller
// (the log writes on the calling thread, reads and music do not happen).
bool streamWorkerStart();
// Signals, joins and frees the worker. Every service must be detached first.
void streamWorkerStop();
bool streamWorkerRunning();

// Wakes the worker now (a read queued, a music request posted, log lines).
void streamWorkerWake();

// The services. Main thread only; each returns once the worker is not, and will
// not be, inside the service being detached.
void streamWorkerAttachAudio(bool attached);
void streamWorkerAttachReader(AsyncFileReader* reader);  // null: detach
// (The log needs no attaching: log_3ds.cpp hands its lines to the worker while
// streamWorkerRunning(), and writes them itself otherwise.)

} // namespace mdk

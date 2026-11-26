// ============================================================================
// log_3ds.cpp — the 3DS platform's log file.
//
// See log_3ds.h for why this is a file and not stdout.
// ============================================================================

#include "platform/log_3ds.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "platform/stream_worker_3ds.h"

namespace mdk {

namespace {

// The newlib FILE*, not libctru's FileHandle: libctru installs a devoptab
// for the SD card, so the ordinary stdio calls reach `sdmc:` once fsInit()
// has run.
FILE* g_log = nullptr;
bool g_tried = false;
bool g_fsOpen = false;

// THE WRITES ARE THE STREAMING WORKER'S (measured 2026-11-25, Old 3DS appliance,
// level 8's door walk): a line written and flushed on the calling thread cost
// 10-20 ms of SD time, and a door crossing writes three to five -- 44-80 ms of
// the crossing frame was the log. So log3dsPrintf only copies the line into
// kRingBytes of queue and wakes the streaming worker on core 1
// (platform/stream_worker_3ds.h), which writes and flushes it when no read or
// music work waits (or the lines have waited 100 ms): the lines reach the card
// a moment later, so a crash loses at most the last moment's lines.
//   - order: both the worker and a caller draining a full queue hold
//     g_fileLock while they take bytes from the queue and write them, so the
//     file gets the lines in the order they were queued;
//   - a full queue is written by the caller itself (the line is never dropped);
//   - no worker (not started yet, stopped, or it failed): the caller writes.
constexpr size_t kRingBytes = 16 * 1024;
char g_ring[kRingBytes];
size_t g_ringHead = 0;   // guarded by g_ringLock: the next byte to write to the file
size_t g_ringUsed = 0;   // guarded by g_ringLock
char g_chunk[kRingBytes];  // guarded by g_fileLock: the bytes being written
LightLock g_ringLock;
LightLock g_fileLock;

// Take everything queued and write it. The caller holds g_fileLock.
void drainLocked() {
    LightLock_Lock(&g_ringLock);
    const size_t used = g_ringUsed;
    for (size_t i = 0; i < used; i++) {
        g_chunk[i] = g_ring[(g_ringHead + i) % kRingBytes];
    }
    g_ringHead = (g_ringHead + used) % kRingBytes;
    g_ringUsed = 0;
    LightLock_Unlock(&g_ringLock);
    if (used > 0 && g_log) {
        fwrite(g_chunk, 1, used, g_log);
        fflush(g_log);
    }
}

} // namespace

bool log3dsOpen() {
    if (g_tried) {
        return g_log != nullptr;
    }
    g_tried = true;

    // The SD card's filesystem. A failure is not fatal: the log is a
    // diagnostic, and the game runs on without it.
    if (R_FAILED(fsInit())) {
        return false;
    }
    g_fsOpen = true;

    // The directory may not exist on a fresh SD card. A failure here is
    // harmless: the open below then fails and the log becomes a no-op.
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/mdk", 0777);

    // ROTATE A LOG THAT HAS OUTGROWN THE WIRE. The log is append-only across
    // sessions on purpose (every line is flushed, so a crash keeps what was
    // written), which means it only grows. The console's ATM HTTP server
    // serves it at ~28 KB/s with up to 40 s before the first byte and ignores
    // Range, so a 725 KB log (measured 2026-11-17) took 26-52 s per read and
    // every hardware-loop poll paid it. At boot, and only at boot, a log over
    // kRotateBytes becomes mdk.prev.log, the one before it mdk.prev2.log (two
    // generations, both served under /3ds/mdk/), and this session starts a
    // fresh file. Two, so a timed-out cycle's session survives the NEXT boot's
    // rotation as well. The line below says the rotation and names the rotated
    // size. Failures are harmless: the open appends to whatever is there.
    const long kRotateBytes = 128 * 1024;
    long rotated = 0;
    struct stat st;
    if (stat("sdmc:/3ds/mdk/mdk.log", &st) == 0 && st.st_size > kRotateBytes) {
        remove("sdmc:/3ds/mdk/mdk.prev2.log");
        rename("sdmc:/3ds/mdk/mdk.prev.log", "sdmc:/3ds/mdk/mdk.prev2.log");
        if (rename("sdmc:/3ds/mdk/mdk.log", "sdmc:/3ds/mdk/mdk.prev.log") == 0) {
            rotated = (long)st.st_size;
        }
    }

    g_log = fopen("sdmc:/3ds/mdk/mdk.log", "a");
    if (!g_log) {
        // Fall back to the SD card's root: some emulators' devoptab does
        // not implement mkdir, and a log in the root is still readable.
        g_log = fopen("sdmc:/mdk.log", "a");
    }
    if (!g_log) {
        return false;
    }
    fprintf(g_log, "--- mdk 3ds session ---\n");
    if (rotated > 0) {
        fprintf(g_log, "log: rotated the previous %ld-byte log to mdk.prev.log\n", rotated);
    }
    fflush(g_log);

    LightLock_Init(&g_ringLock);
    LightLock_Init(&g_fileLock);
    return true;
}

void log3dsClose() {
    // Normally the streaming worker is stopped first (main_3ds.cpp). The OOM
    // handler (heap_probe_3ds.cpp) closes with the worker still running -- on
    // either thread -- so the last write and the close hold g_fileLock: a drain
    // the worker is in finishes first, and one it starts after finds no file.
    if (g_log) {
        LightLock_Lock(&g_fileLock);
        drainLocked();
        if (g_log) {
            fprintf(g_log, "--- session end ---\n");
            fflush(g_log);
            fclose(g_log);
            g_log = nullptr;
        }
        LightLock_Unlock(&g_fileLock);
    }
    if (g_fsOpen) {
        fsExit();
        g_fsOpen = false;
    }
}

void log3dsPrintf(const char* format, ...) {
    // The 3DS's own debug channel FIRST. `svcOutputDebugString` is what an
    // emulator forwards to its log, and it costs nothing. The FILE is the
    // fallback for a real console, where the debug channel goes nowhere.
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    svcOutputDebugString(line, (s32)strlen(line));
    svcOutputDebugString("\n", 1);

    if (!g_log) {
        return;
    }
    if (!streamWorkerRunning()) {
        // No worker: what is queued first (a worker stopped with lines left),
        // then this line, on the calling thread.
        LightLock_Lock(&g_fileLock);
        drainLocked();
        fprintf(g_log, "%s\n", line);
        fflush(g_log);
        LightLock_Unlock(&g_fileLock);
        return;
    }
    size_t length = strlen(line);
    line[length++] = '\n';  // vsnprintf left at most 511 bytes, so the newline fits
    for (;;) {
        LightLock_Lock(&g_ringLock);
        if (kRingBytes - g_ringUsed >= length) {
            const size_t tail = (g_ringHead + g_ringUsed) % kRingBytes;
            for (size_t i = 0; i < length; i++) {
                g_ring[(tail + i) % kRingBytes] = line[i];
            }
            g_ringUsed += length;
            LightLock_Unlock(&g_ringLock);
            break;
        }
        LightLock_Unlock(&g_ringLock);
        // Full: write the queue here, then queue the line.
        LightLock_Lock(&g_fileLock);
        drainLocked();
        LightLock_Unlock(&g_fileLock);
    }
    streamWorkerWake();
}

size_t log3dsQueuedBytes() {
    if (!g_log) {
        return 0;
    }
    LightLock_Lock(&g_ringLock);
    const size_t used = g_ringUsed;
    LightLock_Unlock(&g_ringLock);
    return used;
}

size_t log3dsQueueBytes() { return kRingBytes; }

void log3dsDrain() {
    if (!g_log) {
        return;
    }
    LightLock_Lock(&g_fileLock);
    drainLocked();
    LightLock_Unlock(&g_fileLock);
}

} // namespace mdk

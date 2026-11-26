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

#include "platform/side_core_3ds.h"
#include "platform/vfp_fpscr.h"

namespace mdk {

namespace {

// The newlib FILE*, not libctru's FileHandle: libctru installs a devoptab
// for the SD card, so the ordinary stdio calls reach `sdmc:` once fsInit()
// has run.
FILE* g_log = nullptr;
bool g_tried = false;
bool g_fsOpen = false;

// THE WRITER (measured 2026-11-25, Old 3DS appliance, level 8's door walk):
// a line written and flushed on the calling thread cost 10-20 ms of SD time,
// and a door crossing writes three to five -- 44-80 ms of the crossing frame
// was the log. So log3dsPrintf only copies the line into kRingBytes of queue
// and wakes a writer thread, which writes and flushes it at once: the lines
// reach the card a moment later, still one flush per wake-up, so a crash
// loses at most the lines of the last few milliseconds.
//   - order: both the writer and a caller draining a full queue hold
//     g_fileLock while they take bytes from the queue and write them, so the
//     file gets the lines in the order they were queued;
//   - a full queue is written by the caller itself (the line is never dropped);
//   - the thread: below the main thread's priority, off its core when the
//     console has one to give (side_core_3ds.h, never the system core: that
//     one slot is the music's), its own small stack, joined by log3dsClose,
//     which then writes what is left on the calling thread.
constexpr size_t kRingBytes = 16 * 1024;
constexpr size_t kWriterStackBytes = 8 * 1024;
char g_ring[kRingBytes];
size_t g_ringHead = 0;   // guarded by g_ringLock: the next byte to write to the file
size_t g_ringUsed = 0;   // guarded by g_ringLock
char g_chunk[kRingBytes];  // guarded by g_fileLock: the bytes being written
LightLock g_ringLock;
LightLock g_fileLock;
LightEvent g_wake;
bool g_exit = false;     // guarded by g_ringLock
Thread g_writer = nullptr;
int32_t g_writerCore = -2;

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

// THE WRITER'S ENTRY, a plain function so the R1 gate (tools/runner/gates_vfp.py)
// resolves the routine threadCreate names and reads its body.
void logWriterThread(void*) {
    // R1, AT THE WORKER'S ENTRY. Flush-to-zero and default-NaN are MDK's chosen
    // numerical mode, and FPSCR is per thread: the main thread's state is not
    // evidence of this one's. This thread does no float work (it writes bytes
    // the callers formatted), but decaying values can reach subnormals in any
    // float code a later change adds here, and modern libctru already
    // initialises the mode on its supported thread entry -- so this is the
    // defensive, read-back form the contract asks every float-capable thread to
    // carry. (A failed read-back has nowhere to be said but this log, and saying
    // it here would queue a line from the writer itself: it is not said.)
    (void)vfpThreadFzDn();
    for (;;) {
        LightEvent_Wait(&g_wake);
        LightLock_Lock(&g_fileLock);
        drainLocked();
        LightLock_Unlock(&g_fileLock);
        LightLock_Lock(&g_ringLock);
        const bool exit = g_exit;
        LightLock_Unlock(&g_ringLock);
        if (exit) {
            break;
        }
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
    LightEvent_Init(&g_wake, RESET_ONESHOT);
    g_exit = false;
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    // Two below the main thread (the HD reader is one below): on the main
    // thread's core it runs while main waits for the frame's vsync.
    const s32 writerPriority = priority < 0x3E ? priority + 2 : priority;
    g_writerCore = offMainCore3ds(false);
    g_writer = threadCreate(logWriterThread, nullptr, kWriterStackBytes, writerPriority,
                            g_writerCore, false);
    if (!g_writer && g_writerCore != -2) {
        g_writerCore = -2;
        g_writer = threadCreate(logWriterThread, nullptr, kWriterStackBytes, writerPriority, -2,
                                false);
    }
    if (g_writer) {
        log3dsPrintf("log: writer thread on core %d (priority 0x%X): lines reach the card from it",
                     (int)g_writerCore, (unsigned)writerPriority);
    } else {
        log3dsPrintf("log: writer thread FAILED: every line is written by its caller");
    }
    return true;
}

void log3dsClose() {
    if (g_writer) {
        LightLock_Lock(&g_ringLock);
        g_exit = true;
        LightLock_Unlock(&g_ringLock);
        LightEvent_Signal(&g_wake);
        threadJoin(g_writer, U64_MAX);
        threadFree(g_writer);
        g_writer = nullptr;
    }
    if (g_log) {
        drainLocked();  // the writer is gone: no lock is contended

        fprintf(g_log, "--- session end ---\n");
        fflush(g_log);
        fclose(g_log);
        g_log = nullptr;
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
    if (!g_writer) {
        fprintf(g_log, "%s\n", line);
        fflush(g_log);
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
    LightEvent_Signal(&g_wake);
}

} // namespace mdk

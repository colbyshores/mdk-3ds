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

namespace mdk {

namespace {

// The newlib FILE*, not libctru's FileHandle: libctru installs a devoptab
// for the SD card, so the ordinary stdio calls reach `sdmc:` once fsInit()
// has run.
FILE* g_log = nullptr;
bool g_tried = false;
bool g_fsOpen = false;

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
    return true;
}

void log3dsClose() {
    if (g_log) {
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
    fprintf(g_log, "%s\n", line);
    fflush(g_log);
}

} // namespace mdk

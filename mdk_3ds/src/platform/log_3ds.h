#pragma once

// ============================================================================
// log_3ds.h — the 3DS platform's log file.
//
// WHY A FILE AND NOT stdout: a 3DS application's stdout goes to the console
// applet, and an emulator (mandarine) does not forward it to the terminal.
// A log file on the SD card is the only place a host-side test harness can
// read, so it is a TEST INFRASTRUCTURE FILE, not a convenience.
//
// It goes to `sdmc:/3ds/mdk/mdk.log` -- the SD card, NOT the RomFS. The
// RomFS is read-only (AGENTS.md P5), and the SD card is where a 3DS
// application's save data lives too.
//
// The log is APPEND-mode across runs, and each run writes a banner, so
// several emulator runs can be concatenated and read as one timeline.
// ============================================================================

#include <stdarg.h>

namespace mdk {

// Open the log. Returns false when the SD card is not writable (a normal
// condition in an emulator without an SD image) -- the log then becomes a
// no-op and the game runs on.
bool log3dsOpen();

// Close the log. Safe to call when the open failed.
void log3dsClose();

// Write one line. A no-op when the log is not open.
void log3dsPrintf(const char* format, ...);

} // namespace mdk

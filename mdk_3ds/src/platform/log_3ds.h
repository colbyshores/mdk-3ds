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
//
// The printf format attribute is load-bearing, not decoration. The tree has 72
// call sites across three files and nothing used to check them: the gate that
// adjudicates log arguments reads ONE refusal line in main_3ds.cpp, so a
// dropped argument, a star width with no width argument, or an `int32_t` handed
// to `%d` anywhere else in the tree wrote stack garbage -- or a wrong unit --
// into the file the test harness reads, and nothing noticed. With the attribute
// the compiler checks every call site; the call sites carry `(int)` casts where
// the value is an `int32_t`, which on this target is `long int`, and those casts
// are width-preserving, so no log byte and no instruction changed (measured:
// `mdk.elf` is bit-identical with and without them). See
// tests/scripts/arm_compile_check.sh, which compiles the shipped sources with
// -Werror=format so the diagnostic is a gate and not a notice, and keeps mutants
// that must NOT compile. What the compiler cannot see is WHICH quantity a figure
// claims to be, and that stays the gate's job (check 17 of main_3ds_flow_test.sh).
void log3dsPrintf(const char* format, ...) __attribute__((format(printf, 1, 2)));

} // namespace mdk

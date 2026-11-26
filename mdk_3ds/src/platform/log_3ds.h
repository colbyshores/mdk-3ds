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
#include <stddef.h>
#include <stdint.h>

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
// adjudicates log arguments reads ONE refusal line in main_3ds.cpp, so a dropped
// argument or a star width with no width argument anywhere else in the tree read
// whatever the call happened to leave in the next slot and wrote it into the file
// the test harness reads, and nothing noticed.
//
// Two things make that diagnostic a gate rather than a notice, and they are
// different: (1) `mdk_3ds/Makefile` compiles every shipped source with
// `-Werror=format`, so a mismatched call FAILS `make -C mdk_3ds` -- a gate this
// loop runs -- and that is the half that enforces the tree; (2)
// tests/scripts/arm_compile_check.sh keeps four mutants that must NOT compile and
// one honest call that must, so the attribute cannot be deleted silently. That
// script is a runner a batch invokes and quotes, NOT part of the loop's gate chain
// (review-23 F1); the enforcement lives in the Makefile.
//
// With the attribute the compiler checks every call site; the call sites carry
// `(int)` casts where the value is an `int32_t`, which on this target is
// `long int`. Those 23 sites were standard-level undefined behaviour, NOT a
// corrupted byte: `int` and `long int` are both 4 bytes and occupy the same
// varargs slot here (`__INT32_TYPE__ long int`, `__SIZEOF_INT__ ==
// __SIZEOF_LONG__ == 4`), so the printed bytes were always right and the cast is
// a correctness-of-type fix, not a data-repair one (review-23 F2). The casts are
// width-preserving, so no log byte and no instruction changed (measured:
// `mdk.elf` is bit-identical with and without them, and bit-identical again with
// `-Werror=format` added to the shipped build).
//
// What the compiler cannot see is WHICH quantity a figure claims to be, and that
// stays the gate's job (check 17 of main_3ds_flow_test.sh): a call can be
// format-clean and still print a corner count where the sentence says bytes.
void log3dsPrintf(const char* format, ...) __attribute__((format(printf, 1, 2)));

// THE STREAMING WORKER'S SIDE (platform/stream_worker_3ds.h): the bytes queued,
// the queue's size, and the write of everything queued (the worker calls it;
// so does log3dsClose once the worker is gone).
size_t log3dsQueuedBytes();
size_t log3dsQueueBytes();
void log3dsDrain();

} // namespace mdk

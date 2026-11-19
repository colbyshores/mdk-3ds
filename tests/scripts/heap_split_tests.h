#pragma once

// ============================================================================
// heap_split_tests.h -- the 3DS heap split (mdk_3ds/src/platform/
// heap_split_policy.h), checked on the host.
//
// Where each expected value came from:
//   * the New 3DS row is the owner's Luma crash dump 41 (2026-11-18): maxCommit
//     0x07C00000, currentCommit 0x000F5000 read off its stack, and the main
//     heap libctru then asked for, 0x0630B000 (r3), which overflowed the
//     96 MiB heap area -- the defect this pins;
//   * the original-3DS row is the boot log's `main: heap split` on the
//     appliance (main 41,041,920 = avail - 24 MiB), which must not move;
//   * the area bounds are libctru <3ds/os.h>'s OS_HEAP_AREA_BEGIN/END.
// Nothing here was read off heap_split_policy.cpp's output.
// ============================================================================

namespace mdk {
namespace test {

int runHeapSplitTests();

} // namespace test
} // namespace mdk

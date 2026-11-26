#pragma once

// ============================================================================
// heap_split_policy.h -- how this title divides the application memory region
// between newlib's heap (main) and the GPU's linear heap. Pure arithmetic, no
// libctru, so the host suite pins it (tests/scripts/heap_split_tests.cpp);
// heap_split.cpp's __system_allocateHeaps is the only caller on the console.
//
// THE RULE. The linear heap gets kLinearHeapBytes and main gets the rest -- the
// split heap_split.cpp has always asked for -- EXCEPT that main can never be
// larger than the virtual area it is mapped into (OS_HEAP_AREA_BEGIN 0x08000000
// to OS_HEAP_AREA_END 0x0E000000, 96 MiB). Whatever main cannot hold goes to
// the linear heap instead.
//
// WHY THE CAP. A New 3DS running homebrew in extended memory gives the process
// 124 MiB (maxCommit 0x07C00000). 124 MiB less the image less 24 MiB of linear
// asked for a 99 MiB main heap, svcControlMemory refused it, and libctru's
// guard called svcBreak before main(): the owner's Luma crash dump 41
// (2026-11-18, PC in svcBreak, LR in __system_allocateHeaps after the MEMOP_ALLOC,
// r3 = 0x0630B000). libctru's own default never hits this because it caps the
// linear heap at 32 MiB from the other side; the fixed 24 MiB linear did.
//
// On an original 3DS (about 62-64 MiB) the cap never binds: linear is exactly
// kLinearHeapBytes. On the New 3DS's 124 MiB the cap binds and linear gets the
// rest (~28 MiB either way: before 2026-11-25 it was ~27 MiB there by the cap).
// ============================================================================

#include <stdint.h>

namespace mdk {

// The GPU heap: one arena's working set, the sky, Kurt's frames, the audio and
// the HD window. 30 MiB since 2026-11-26 (owner; 28 since 2026-11-25, 24
// before): the HD window -- Kurt's arena, its neighbours and the arenas it names
// (docs/3DS_ARCHITECTURE.md 4) -- is at most 8.79 MiB at 512 (level 8, CGUNT_9)
// against 8.92 MiB of room in 28, too thin to keep; 30 gives 2.1 MiB of margin.
// The 2 MiB come out of the main heap, which leaves 33.74 MB on the Old 3DS
// against a level-8 main peak of 29.11 MB (Old 3DS, 2026-11-26, halo-shadow-hw
// l8auto-3, music streamed as DSP-ADPCM): 4.6 MB of margin. The owner's
// condition: a level-8 walk confirms the main heap's minimum before this merges.
const uint32_t kLinearHeapBytes = 30u * 1024u * 1024u;

// OS_HEAP_AREA_END - OS_HEAP_AREA_BEGIN (libctru <3ds/os.h>).
const uint32_t kMainHeapAreaBytes = 0x0E000000u - 0x08000000u;

struct HeapSplit {
    uint32_t main;    // bytes for newlib (MEMOP_ALLOC at OS_HEAP_AREA_BEGIN)
    uint32_t linear;  // bytes for the GPU (MEMOP_ALLOC_LINEAR)
    bool fits;        // false: the region cannot hold the linear heap at all
};

// `remaining` is the process's free commit (maxCommit - currentCommit), rounded
// down to a 4 KiB page by the caller as libctru does.
HeapSplit heapSplitFor(uint32_t remaining);

} // namespace mdk

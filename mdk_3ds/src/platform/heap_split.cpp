// ============================================================================
// heap_split.cpp — the main-heap / linear-heap split for this title.
//
// WHY THIS FILE EXISTS. libctru's default __system_allocateHeaps splits the
// application region in HALF when neither size is pre-set (decoded from
// libctru's allocateHeaps.o: with heap == 0 and linear == 0 it computes
// linear = avail/2, rounded down to 4 KiB, and heap = avail - linear). On the
// original 3DS that gives ~31.1 MiB of newlib heap and ~32.7 MiB of linear
// heap; MDK measured `mem[pre-audio]: cap=32657408 used=28084224` on level 7
// and then died allocating the 4,059,388-byte LEVEL7O.SNI, because the level's
// parsed arenas and the level's sprite archives are resident in the MAIN heap.
//
// MDK's GPU side is the opposite of its malloc side: textures are kept as
// 8-bit palette indices in system RAM and expanded on demand into RGBA8 under
// an LRU (SPEC.md §"The chosen 3DS texture strategy"), so the linear heap holds
// one arena's working set, the sky panorama and the animated strips, not a
// level's worth of expanded colours. 24 MiB is far above that working set and
// still leaves ~8 MiB of margin against the old 32.7 MiB.
//
// HOW. libctru 2.7.0's __system_allocateHeaps is WEAK (symbol "W" in
// allocateHeaps.o), and so are the two size globals ("V"). Until 2026-11-18
// this file only overrode the globals (heap 0, linear 24 MiB: "linear gets this
// much, main the rest") and let libctru's default allocator run. That broke on
// a New 3DS in extended memory: 124 MiB less 24 MiB of linear asked for a
// 99 MiB main heap, which does not fit the 96 MiB heap area, and libctru's
// svcBreak guard fired before main() (Luma crash dump 41; heap_split_policy.h
// has the registers). The globals cannot express "24 MiB, unless main would
// overflow its area" -- they are read before any of our code runs -- so the
// allocator itself is overridden here, as a line-for-line copy of libctru
// 2.7.0's (recovered from its disassembly in mdk.elf, allocateHeaps.c lines
// 19-80) with the split taken from mdk::heapSplitFor instead.
//
// THE TRAP an override has to respect: it owns mappableInit() and the
// fake_heap_start/end wiring too, and omitting mappableInit data-aborts in
// gspInit at boot (Obsidian
// 30-architecture/patterns/3ds-adaptive-linear-heap-and-mappableinit-trap.md).
// Both are below, in libctru's order.
//
// The split is logged at boot and linearSpaceFree() is logged with every heap
// probe (main_3ds.cpp), so a hardware run shows both sides. An original-3DS
// region is ~62.3 MiB free after the image, where the cap never binds and the
// split is the one every earlier measurement used; on a region too small for
// the linear heap the svcBreak guard fires rather than silently starving main.
// ============================================================================

#include <stdint.h>

#include "platform/heap_split_policy.h"

// NOT <3ds/env.h>: it declares the size globals with C++ linkage inside its
// static inline accessors, which collides with the C linkage libctru actually
// exports. The definitions below are the authoritative (unmangled) symbols.
extern "C" {
#include <3ds/types.h>
#include <3ds/result.h>
#include <3ds/svc.h>
#include <3ds/os.h>
#include <3ds/allocator/mappable.h>
}

extern "C" {

// Written by __system_allocateHeaps below before anything reads them
// (envGetHeapSize, linearSpaceFree and the texture code read them later).
u32 __ctru_heap_size = 0;
u32 __ctru_linear_heap_size = 0;

// Defined (bss) by libctru's allocateHeaps.o, which is still linked for them.
extern u32 __ctru_heap;
extern u32 __ctru_linear_heap;

// newlib's sbrk window.
extern char* fake_heap_start;
extern char* fake_heap_end;

void __system_allocateHeaps(void) {
    Result rc;

    // The resource limit object for this process.
    Handle reslimit = 0;
    rc = svcGetResourceLimit(&reslimit, CUR_PROCESS_HANDLE);
    if (R_FAILED(rc)) {
        svcBreak(USERBREAK_PANIC);
    }

    // Total and used commit.
    s64 maxCommit = 0;
    s64 currentCommit = 0;
    ResourceLimitType reslimitType = RESLIMIT_COMMIT;
    svcGetResourceLimitLimitValues(&maxCommit, reslimit, &reslimitType, 1);
    svcGetResourceLimitCurrentValues(&currentCommit, reslimit, &reslimitType, 1);
    svcCloseHandle(reslimit);

    // What is left, page-aligned, split by this title's rule.
    const u32 remaining = (u32)(maxCommit - currentCommit) & ~0xFFFu;
    const mdk::HeapSplit split = mdk::heapSplitFor(remaining);
    if (!split.fits) {
        svcBreak(USERBREAK_PANIC);
    }
    __ctru_heap_size = split.main;
    __ctru_linear_heap_size = split.linear;

    // The application heap.
    rc = svcControlMemory(&__ctru_heap, OS_HEAP_AREA_BEGIN, 0x0, __ctru_heap_size, MEMOP_ALLOC,
                          (MemPerm)(MEMPERM_READ | MEMPERM_WRITE));
    if (R_FAILED(rc)) {
        svcBreak(USERBREAK_PANIC);
    }

    // The linear heap.
    rc = svcControlMemory(&__ctru_linear_heap, 0x0, 0x0, __ctru_linear_heap_size,
                          MEMOP_ALLOC_LINEAR, (MemPerm)(MEMPERM_READ | MEMPERM_WRITE));
    if (R_FAILED(rc)) {
        svcBreak(USERBREAK_PANIC);
    }

    // The mappable allocator (gspInit needs it).
    mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);

    // newlib's heap.
    fake_heap_start = (char*)__ctru_heap;
    fake_heap_end = fake_heap_start + __ctru_heap_size;
}

} // extern "C"

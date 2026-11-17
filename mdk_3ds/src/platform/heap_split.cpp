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
// HOW. __ctru_heap_size and __ctru_linear_heap_size are STRONG-overridable
// (libctru defines them weak, symbols "V" in allocateHeaps.o). The default
// allocator reads both BEFORE it splits (the `cmp r3, #0` / `cmp r1, #0`
// branches): heap == 0 with linear != 0 means "give the linear heap this much
// and the rest to main". So these two globals move ~7.5 MiB from the GPU heap
// to the malloc heap without reimplementing the allocator -- which matters,
// because an __system_allocateHeaps override owns mappableInit() too and
// omitting it data-aborts in gspInit at boot
// (Obsidian 30-architecture/patterns/3ds-adaptive-linear-heap-and-mappableinit-trap.md).
// The default path still runs: it does the svcControlMemory mapping, the
// mappableInit(), and the fake_heap_start/end wiring.
//
// The split is logged at boot and linearSpaceFree() is logged with every heap
// probe (main_3ds.cpp), so a hardware run shows both sides. An original-3DS
// region is ~62.3 MiB free after the image; on a region too small for this
// split libctru's own svcBreak guard fires rather than silently starving main.
// ============================================================================

#include <stdint.h>

// NOT <3ds/env.h>: it declares these two globals with C++ linkage inside its
// static inline accessors, which collides with the C linkage libctru actually
// exports. The definitions below are the authoritative (unmangled) symbols.
#include <3ds/types.h>

extern "C" {

// 0 = "let libctru compute it": with a non-zero linear size the default
// allocator assigns main = avail - linear, so this stays adaptive to the
// console's actual application region instead of hard-coding a main size.
u32 __ctru_heap_size = 0;

// 24 MiB for the GPU heap; the surplus (~7.5 MiB over libctru's half split on
// an original 3DS) goes to newlib. See the header comment for why the GPU
// side does not need the default half.
u32 __ctru_linear_heap_size = 24 * 1024 * 1024;

} // extern "C"

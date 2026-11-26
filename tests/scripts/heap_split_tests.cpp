// ============================================================================
// heap_split_tests.cpp -- see heap_split_tests.h.
// ============================================================================

#include "heap_split_tests.h"

#include <stdio.h>

#include "platform/heap_split_policy.h"

using namespace mdk;

namespace {

int failures = 0;

void expect(const char* what, long long got, long long want) {
    if (got != want) {
        printf("  FAIL: %s was %lld, expected %lld\n", what, got, want);
        failures++;
    }
}

const unsigned kMiB = 1024u * 1024u;

// The area libctru maps the main heap into (<3ds/os.h>).
const unsigned kAreaBytes = 0x0E000000u - 0x08000000u;

void testTheNew3dsRegionFromCrashDump41() {
    // maxCommit - currentCommit, page-aligned, as libctru computes it.
    const unsigned remaining = (0x07C00000u - 0x000F5000u) & ~0xFFFu;
    // What the old split asked for, and why it failed.
    expect("the old split's main heap (dump 41's r3)", remaining - 24u * kMiB, 0x0630B000);
    expect("which overflowed the 96 MiB area", (remaining - 24u * kMiB) > kAreaBytes, 1);

    const HeapSplit split = heapSplitFor(remaining);
    expect("the New 3DS region fits", split.fits, 1);
    // With 30 MiB of linear (2026-11-26) main is 93.04 MiB: inside the area, so
    // the cap does not bind on this region (it still guards larger ones,
    // testTheBoundaries).
    expect("main fits the area", split.main <= kAreaBytes, 1);
    expect("main is the rest", split.main, remaining - 30u * kMiB);
    expect("linear is 30 MiB", split.linear, 30u * kMiB);
    expect("nothing is lost", (long long)split.main + split.linear, remaining);
    expect("linear is page-aligned", split.linear & 0xFFFu, 0);
}

void testTheOriginal3dsSplit() {
    // The appliance's boot log (2026-11-25): avail=65200128, which 24 MiB of
    // linear split into main 40,034,304. With 30 MiB, 6 MiB moves to linear.
    const unsigned remaining = 65200128u;
    const HeapSplit split = heapSplitFor(remaining);
    expect("the original 3DS fits", split.fits, 1);
    expect("linear is 30 MiB", split.linear, 30u * kMiB);
    expect("main is the rest, as the boot log will say it", split.main, 33742848);
    // The level-8 main peak measured on the Old 3DS with music streamed as
    // DSP-ADPCM (2026-11-26, halo-shadow-hw l8auto-3, `mem[loop]` peak): 29,113,408.
    // (The 30.63 MB of 4d9264f3 decoded music into main memory.)
    expect("main keeps 4.5 MB above the measured level-8 peak",
           split.main > 29113408u + 4500000u, 1);
}

void testTheBoundaries() {
    HeapSplit split = heapSplitFor(kAreaBytes + 30u * kMiB);
    expect("main exactly fills the area", split.main, kAreaBytes);
    expect("with exactly 30 MiB of linear", split.linear, 30u * kMiB);

    split = heapSplitFor(kAreaBytes + 30u * kMiB + 0x1000u);
    expect("one page more: main still the area", split.main, kAreaBytes);
    expect("one page more: the page is linear's", split.linear, 30u * kMiB + 0x1000u);

    split = heapSplitFor(30u * kMiB);
    expect("a region of exactly the linear heap fits", split.fits, 1);
    expect("with no main heap", split.main, 0);

    split = heapSplitFor(30u * kMiB - 0x1000u);
    expect("a region smaller than the linear heap does not fit", split.fits, 0);

    // libctru's largest region (the New 3DS's 178 MiB dev mode): main capped.
    split = heapSplitFor(178u * kMiB);
    expect("the largest region: main capped", split.main, kAreaBytes);
    expect("the largest region: linear the rest", split.linear, 178u * kMiB - kAreaBytes);
}

} // namespace

namespace mdk {
namespace test {

int runHeapSplitTests() {
    failures = 0;
    testTheNew3dsRegionFromCrashDump41();
    testTheOriginal3dsSplit();
    testTheBoundaries();
    return failures;
}

} // namespace test
} // namespace mdk

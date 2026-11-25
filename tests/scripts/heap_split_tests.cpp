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
    // With 28 MiB of linear (2026-11-25) main is 95.04 MiB: inside the area, so
    // the cap does not bind on this region any more (it still guards larger ones,
    // testTheBoundaries). The New 3DS's linear goes from 27.04 MiB to 28.
    expect("main fits the area", split.main <= kAreaBytes, 1);
    expect("main is the rest", split.main, remaining - 28u * kMiB);
    expect("linear is 28 MiB", split.linear, 28u * kMiB);
    expect("nothing is lost", (long long)split.main + split.linear, remaining);
    expect("linear is page-aligned", split.linear & 0xFFFu, 0);
}

void testTheOriginal3dsSplit() {
    // The appliance's boot log (2026-11-25): avail=65200128, which 24 MiB of
    // linear split into main 40,034,304. With 28 MiB, 4 MiB moves to linear.
    const unsigned remaining = 65200128u;
    const HeapSplit split = heapSplitFor(remaining);
    expect("the original 3DS fits", split.fits, 1);
    expect("linear is 28 MiB", split.linear, 28u * kMiB);
    expect("main is the rest, as the boot log will say it", split.main, 35840000);
    // The highest main use measured in play (level 8, base 4d9264f3): 30,633,712.
    expect("main keeps 5 MB above the measured in-play peak",
           split.main > 30633712u + 5000000u, 1);
}

void testTheBoundaries() {
    HeapSplit split = heapSplitFor(kAreaBytes + 28u * kMiB);
    expect("main exactly fills the area", split.main, kAreaBytes);
    expect("with exactly 28 MiB of linear", split.linear, 28u * kMiB);

    split = heapSplitFor(kAreaBytes + 28u * kMiB + 0x1000u);
    expect("one page more: main still the area", split.main, kAreaBytes);
    expect("one page more: the page is linear's", split.linear, 28u * kMiB + 0x1000u);

    split = heapSplitFor(28u * kMiB);
    expect("a region of exactly the linear heap fits", split.fits, 1);
    expect("with no main heap", split.main, 0);

    split = heapSplitFor(28u * kMiB - 0x1000u);
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

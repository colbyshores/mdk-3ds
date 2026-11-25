// ============================================================================
// heap_probe_3ds.cpp -- see heap_probe_3ds.h.
// ============================================================================

#include "platform/heap_probe_3ds.h"

#include <3ds.h>
#include <malloc.h>
#include <stdlib.h>

#include "platform/log_3ds.h"

namespace mdk {

namespace {

// PHASE 0 HEAP PROBE (arena streaming). The number that matters is the
// newlib heap's, not the application region's: libctru allocates the whole
// application region at boot (heap_split.cpp explains the split), so
// osGetMemRegionFree(APPLICATION) reads 0 at every point in the run. That is
// a statement about the reservation, not about headroom. mallinfo() reports
// the allocator itself:
//   arena    bytes sbrk'd from the heap reservation so far
//   used     bytes currently handed out (uordblks)
//   free     free space inside that arena (fordblks)
// `peak` is the run's high-water mark of `used`, which is what an arena
// budget has to fit under. `lin` is the GPU heap's free bytes
// (linearSpaceFree), the other half of the split: a texture allocation fails
// with invisible geometry, not an abort, so it is logged beside the malloc
// numbers on every probe. No probe on this path allocates (log3dsPrintf
// writes through a static FILE buffer, not newlib's heap).
static uint32_t g_heapPeak = 0;

} // namespace

void heapProbe(const char* tag) {
    struct mallinfo mi = mallinfo();
    const uint32_t used = (uint32_t)mi.uordblks;
    if (used > g_heapPeak) {
        g_heapPeak = used;
    }
    mdk::log3dsPrintf("mem[%s]: cap=%u arena=%u used=%u free=%u peak=%u lin=%u",
                      tag, (unsigned)envGetHeapSize(), (unsigned)mi.arena, (unsigned)used,
                      (unsigned)mi.fordblks, (unsigned)g_heapPeak, (unsigned)linearSpaceFree());
}

// With -fno-exceptions libstdc++'s operator new calls this and then aborts,
// so the handler is the only place that can name the allocation that ran
// the heap dry. Without it an OOM is a silent exit with no last line.
void onHeapExhausted() {
    heapProbe("OOM");
    mdk::log3dsPrintf("FATAL: operator new failed -- the 24 MiB heap is exhausted");
    mdk::log3dsClose();
    abort();
}

} // namespace mdk

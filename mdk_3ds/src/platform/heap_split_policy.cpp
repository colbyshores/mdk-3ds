// ============================================================================
// heap_split_policy.cpp -- see heap_split_policy.h.
// ============================================================================

#include "platform/heap_split_policy.h"

namespace mdk {

HeapSplit heapSplitFor(uint32_t remaining) {
    HeapSplit split;
    split.main = 0;
    split.linear = 0;
    split.fits = remaining >= kLinearHeapBytes;
    if (!split.fits) {
        return split;
    }
    split.linear = kLinearHeapBytes;
    split.main = remaining - kLinearHeapBytes;
    if (split.main > kMainHeapAreaBytes) {
        // The surplus stays page-aligned: remaining and the area size both are.
        split.linear += split.main - kMainHeapAreaBytes;
        split.main = kMainHeapAreaBytes;
    }
    return split;
}

} // namespace mdk

#pragma once

// ============================================================================
// heap_probe_3ds.h -- the boot log's memory lines, and the out-of-memory handler.
// ============================================================================

namespace mdk {

// One `mem[tag]:` line: the newlib heap (arena, used, free, the run's peak) and
// the GPU heap's free bytes. Allocates nothing.
void heapProbe(const char* tag);
// std::set_new_handler's target: names the exhausted heap in the log, then aborts.
void onHeapExhausted();

} // namespace mdk

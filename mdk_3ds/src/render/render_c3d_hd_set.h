#pragma once

// ============================================================================
// render_c3d_hd_set.h -- the 3DS backend's answer about the HD window
// (mdk_render_set_hd_window, docs/3DS_ARCHITECTURE.md 4), for the lines
// main_3ds.cpp logs when a wait ends and at the level's end.
//
// Not part of the shared seam (render/render.h): the Linux backend has no
// resident set. A plain struct copy -- no I/O, no allocation.
// ============================================================================

#include <stdint.h>

#include "render/render.h"

struct MdkHdSetState {
    int32_t pinned = 0;          // the set's copies
    int32_t resident = 0;        // ... of them resident
    uint32_t residentBytes = 0;  // ... and their bytes
    int32_t givenUp = 0;         // copies given up (a failed read or import)
    int32_t topCap = 0;          // the smallest cap in the set (0: no level dropped)
    uint32_t drawnMissing = 0;   // draws of an HD-only texture with no copy (must stay 0)
    int32_t imported = 0;        // HD copies imported since the renderer started
};

void mdk_render_hd_set_state(MdkRenderer r, MdkHdSetState* out);

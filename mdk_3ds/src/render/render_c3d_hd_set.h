#pragma once

// ============================================================================
// render_c3d_hd_set.h -- the 3DS backend's answer about the level's resident
// HD set (mdk_render_pin_hd_set, docs/HD_TEXTURES_3DS_SPEC.md 3.2), for the
// one-line summary main_3ds.cpp logs when the loading screen's hold ends.
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
};

void mdk_render_hd_set_state(MdkRenderer r, MdkHdSetState* out);

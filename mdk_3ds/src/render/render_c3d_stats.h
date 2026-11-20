#pragma once

// ============================================================================
// render_c3d_stats.h -- the 3DS backend's FIRST-USE counters, for the harness.
//
// Not part of the shared seam (render/render.h): the Linux backend has no such
// counters and the game does not read them. The 3DS frontend's PROFILE build
// samples them once a frame (platform/harness_3ds.h, the arena-entry table) so
// a slow frame around an arena change says what it made: textures created on
// first draw, HD copies imported. All cumulative since mdk_render_create; the
// caller takes differences. A plain struct copy -- no I/O, no allocation.
// ============================================================================

#include <stdint.h>

#include "render/render.h"

struct MdkC3dFirstUse {
    int32_t texturesMade = 0;   // textures made (any store, compressed included)
    int32_t hdImported = 0;     // HD copies imported (docs/HD_TEXTURES_3DS_SPEC.md)
    int32_t hdEvicted = 0;
};

void mdk_render_c3d_first_use(MdkRenderer r, MdkC3dFirstUse* out);

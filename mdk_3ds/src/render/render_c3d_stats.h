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
    int32_t texturesFirstMade = 0;
    int32_t texturesRemade = 0;
    int32_t hdReads = 0;
    int64_t hdReadBytes = 0;
    int32_t hdImported = 0;     // HD copies imported (docs/HD_TEXTURES_3DS_SPEC.md)
    int32_t hdEvicted = 0;
    int32_t texturesEvicted = 0;     // GPU copies the LRU freed (made again on next draw)
    uint64_t textureMakeTicks = 0;   // svcGetSystemTick spent making textures
    uint64_t hdImportTicks = 0;      // ... and importing HD copies
    // THE ORIG PACK (04_textures.inc pumpPackOriginals): packed originals
    // imported at a frame boundary from the reader thread's read; made by a draw
    // that could not wait and READ THE FILE on the main thread (keep near zero)
    // and the ticks those took (PROFILE builds); made by a draw from a read the
    // reader had already finished (an import in the frame, no read); and files
    // that could not be made.
    int32_t packImported = 0;
    int32_t packSync = 0;
    uint64_t packSyncTicks = 0;
    int32_t packLanded = 0;
    int32_t packFailed = 0;
};

void mdk_render_c3d_first_use(MdkRenderer r, MdkC3dFirstUse* out);

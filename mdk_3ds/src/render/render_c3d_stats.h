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
    // THE HD RULE'S PROOF (docs/HD_TEXTURES_3DS_SPEC.md 3.1): with
    // enableHdTextures on, a surface the HD pack covers is drawn from its HD
    // copy and nothing else. hdCoveredSdMade counts the standard-definition GPU
    // copies made (an expansion or a packed original, any path) for a texture
    // with an HD copy linked while the toggle is on -- the number that must stay
    // 0. hdLateTextures: textures whose first draw that wanted their HD copy
    // found it not resident; hdLateDraws every such draw; hdBlankDraws those of
    // them that drew nothing (no original resident to stand in). hdResidentPeak:
    // the most HD bytes resident at once.
    int32_t hdCoveredSdMade = 0;
    int32_t hdLateTextures = 0;
    int32_t hdLateDraws = 0;
    int32_t hdBlankDraws = 0;
    uint32_t hdResidentPeak = 0;
    // HOW LATE: for each copy a draw wanted before it was resident, the frames
    // from that first draw to its import -- the count, their sum and the worst,
    // with the worst one's file and the frame it was first wanted, and the first
    // eight (wanted frame, frames late) in order.
    int32_t hdLateLanded = 0;
    int32_t hdLateFramesSum = 0;
    int32_t hdLateFramesMax = 0;
    int32_t hdLateWorstFrame = -1;
    char hdLateWorst[64] = {0};
    int32_t hdLateRows[8][2] = {};
    // THE MARGIN, for the copies that were in time: at each texture's first draw
    // that wanted its HD copy, for a copy that landed in play (after the
    // loading screen's wait), the frames since that copy landed. The smallest (with its file and frame) is how much
    // slower the reads could have been before that surface popped in; under30
    // counts the first draws with less than 30 frames (half a second) to spare.
    int32_t hdSlackMin = -1;
    int32_t hdSlackMinFrame = -1;
    int32_t hdSlackUnder30 = 0;
    int32_t hdSlackFirsts = 0;
    char hdSlackWorst[64] = {0};
    // THE LEVEL'S RESIDENT SET (mdk_render_pin_hd_set): its copies, how many are
    // resident and their bytes, and the ones given up (a failed read or import).
    int32_t hdPinned = 0;
    int32_t hdPinnedResident = 0;
    uint32_t hdPinnedBytes = 0;
    int32_t hdPinFailed = 0;
    int32_t hdTopCap = 0;
    // Stamps (bullet holes) skipped on HD-only surfaces (the HD rule).
    int32_t hdStampsSkipped = 0;
};

void mdk_render_c3d_first_use(MdkRenderer r, MdkC3dFirstUse* out);

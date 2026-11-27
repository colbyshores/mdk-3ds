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

// THE TEXTURE CENSUS (docs/3DS_ARCHITECTURE.md 2 and 5): what core 0 made and
// read for textures, cumulative since the renderer started; the frontend logs
// the differences over a level's load and its play. "Made" is a GPU copy built
// on this core from indices or RGBA (an expansion, a streamed picture), not a
// compressed import.
struct MdkTextureCensus {
    int32_t made = 0;               // textures made (each store's count, summed)
    uint64_t madeBytes = 0;         // ... their GPU bytes
    uint64_t makeTicks = 0;         // ... core-0 ticks making them (SYSCLOCK_ARM11)
    int32_t packImported = 0;       // packed originals imported at a frame boundary
    uint64_t packImportTicks = 0;   // ... core-0 ticks importing them
    int32_t packMainReads = 0;      // packed originals READ on core 0 (must stay 0 in play)
    int32_t compressedImported = 0; // every compressed import (packs, Kurt)
};

void mdk_render_texture_census(MdkRenderer r, MdkTextureCensus* out);
// One log line: what core 0 made and read for textures between two snapshots
// (`what`: "level load", "level play", "fall", "stream"). Cold.
void mdk_render_log_texture_census(const char* what, const MdkTextureCensus& from,
                                   const MdkTextureCensus& to);
// Every chunked expansion still pending or deferred, finished now (between
// frames only: the level load's end, behind the loading screen). Returns how
// many were finished.
int32_t mdk_render_finish_expansions(MdkRenderer r);
// While a phase is named ("load", "play": a string literal; null for none) each
// make on core 0 is logged with it, up to a cap a phase.
void mdk_render_texture_census_log(MdkRenderer r, const char* phase);

#pragma once

// ============================================================================
// c3d_sun_bake.h -- the Citro3D backend's one 3DS-only entry: the level's BAKED
// light (EnhancedToggles::lightBaked; render/sun_bake.h has the files, and
// mdk_3ds/tools/sun_bake/ makes them at build time).
//
// NOT IN THE SEAM (render/render.h) ON PURPOSE. The PC has no use for it -- its
// light and shadow map are the PC's own look -- and the seam carries what the
// GAME needs.
// main_3ds.cpp loads the bake at level load, BEFORE the first arena mesh is made
// (mdk_render_create_mesh gives every mesh the bake knows its lightmap uvs as it
// is made, the arenas that stream in later included), and logs the report.
// ============================================================================

#include <stdint.h>

#include "render/render.h"

struct MdkSunBakeReport {
    bool loaded = false;          // the bake is in and its texture made
    const char* why = "";         // when not: what stopped it
    int32_t atlasWidth = 0;
    int32_t atlasHeight = 0;
    const char* format = "";      // "L4" / "L8"
    float texelSize = 0.0f;       // world units a texel
    uint32_t fileBytes = 0;
    uint32_t texelBytes = 0;      // the GPU texture (linear heap)
    uint32_t residentBytes = 0;   // charts, keys and the depth map (main heap)
    int32_t charts = 0;           // the grey blocks included
    int32_t blocks = 0;           // the 4x4 grey blocks
    int32_t keys = 0;
    int32_t depthWidth = 0;
    int32_t depthHeight = 0;
    int32_t loadMs = 0;
    // The meshes made since the load that took lightmap uvs.
    int32_t meshes = 0;
    int32_t triangles = 0;        // their triangles
    int32_t keyed = 0;            // sides the bake knew (a DoubleSided triangle has two)
    uint32_t uvBytes = 0;         // the side-data buffers' linear-heap bytes
    int32_t uvMs = 0;             // the time they took
};

// Read `path` (romfs:/sunbake/levelN.sbk) and import `atlasPath` (its .t3x) as
// the grey shadow map, and keep what the meshes and the objects need. Cold:
// level load only. False when there is no bake, it does not parse or its memory
// cannot be had (`out->why`); the frame then lights the static world in real
// time, as with the toggle off.
bool mdk_c3d_load_sun_bake(MdkRenderer r, const char* path, const char* atlasPath,
                           MdkSunBakeReport* out);

// The report as it stands (the load's, and the meshes made since).
void mdk_c3d_sun_bake_report(MdkRenderer r, MdkSunBakeReport* out);

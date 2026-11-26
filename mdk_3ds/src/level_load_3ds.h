#pragma once

// ============================================================================
// level_load_3ds.h -- the level's cold setup that is not a pack: the music
// prefetch's reads lent to the renderer's file thread, the light rigs, the sky.
// ============================================================================

#include <stdint.h>

#include <utility>
#include <vector>

#include "formats/arena.h"
#include "formats/dti.h"
#include "formats/mdk_data.h"
#include "game/font_view.h"
#include "game/hud_view.h"
#include "game/level_data.h"
#include "game/level_light.h"
#include "render/render.h"

namespace mdk {

// The light rig for one arena, computed once and kept in `cache`.
Lighting cachedLighting(std::vector<std::pair<const formats::Arena*, Lighting>>& cache,
                        const LevelData& level, const formats::Arena* arena);

// One draw of the sky dome: a texture over a range of the dome's mesh.
struct SkyRange {
    int32_t texture = -1;
    int32_t first = 0;
    int32_t count = 0;
};
// The sky (level_load_3ds.cpp): with `hd` (the HD toggle) its slices come from
// the build's de-dithered ETC1 copies, romfs:/orig/sky/level<L>_<k>.t3x, when
// they are there, and from the palette expansion otherwise.
int32_t buildSky3ds(MdkRenderer renderer, const formats::Dti& dti, int32_t level, bool hd,
                    MdkMeshId* skyMesh, SkyRange* ranges);
// Gives back what buildSky3ds made (the HD toggle changed: the sky is made again).
void releaseSky3ds(MdkRenderer renderer, MdkMeshId* skyMesh, SkyRange* ranges, int32_t* count);

// The pause menu's fonts (MISC/MDKFONT.FTI), also handed to the HUD's messages.
bool loadMenuFonts3ds(MdkRenderer renderer, const formats::MdkData& data, FontView* menuBig,
                      FontView* menuSmall, HudView* hud);

// The diagnostics after "data: NOT FOUND".
void logDataNotFound3ds();

} // namespace mdk

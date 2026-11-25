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

// The music prefetch's reads (audio/sound_bank.h's setReader): `user` is the
// MdkRenderer whose file thread reads them.
bool musicReadRequest(void* user, int32_t tag, const char* path, uint32_t offset,
                      uint32_t length);
bool musicReadPoll(void* user, int32_t tag, uint8_t** bytes, uint32_t* size, bool* ok);
void musicReadCancel(void* user, int32_t tag);

// The light rig for one arena, computed once and kept in `cache`.
Lighting cachedLighting(std::vector<std::pair<const formats::Arena*, Lighting>>& cache,
                        const LevelData& level, const formats::Arena* arena);

// One draw of the sky dome: a texture over a range of the dome's mesh.
struct SkyRange {
    int32_t texture = -1;
    int32_t first = 0;
    int32_t count = 0;
};
int32_t buildSky3ds(MdkRenderer renderer, const formats::Dti& dti, MdkMeshId* skyMesh,
                    SkyRange* ranges);

// The pause menu's fonts (MISC/MDKFONT.FTI), also handed to the HUD's messages.
bool loadMenuFonts3ds(MdkRenderer renderer, const formats::MdkData& data, FontView* menuBig,
                      FontView* menuSmall, HudView* hud);

// The diagnostics after "data: NOT FOUND".
void logDataNotFound3ds();

} // namespace mdk

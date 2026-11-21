#pragma once

// ============================================================================
// sky_modes.h — how the scripts want the background drawn (C# Level/SkyModes.cs).
//
// The scripts' sky mode (0x574304, opcode 202 set_574304; sky_draw 0x475b4c,
// godot-mdk level.gd `show_sky`): 0 the sky, 1 black, anything else nothing --
// the last frame stays, e.g. level 3's closed HMO_3. The C# Viewer turns it into
// the renderer's Backdrop and the frame's clear colour every frame
// (Viewer.Background, Viewer.cs:1105); both frontends do the same with these.
//
// HEADER ONLY, like formats/ascii_case.h: two pure functions and the DTI's sky
// colour, and a header needs no object file to collide with in the 3DS build
// (which keys objects by `notdir`).
// ============================================================================

#include <stdint.h>

#include "formats/dti.h"
#include "render/render_types.h"

namespace mdk {
namespace sky_modes {

// C# SkyModes.Shown / Black.
constexpr int32_t kShown = 0;
constexpr int32_t kBlack = 1;

// C# SkyModes.BackdropOf: Sky in mode 0, Clear in mode 1, Keep otherwise.
inline Backdrop backdropOf(int32_t mode) {
    if (mode == kShown) {
        return Backdrop::Sky;
    }
    if (mode == kBlack) {
        return Backdrop::Clear;
    }
    return Backdrop::Keep;
}

// C# SkyModes.ClearOf: the clear colour, black in mode 1, else `sky` (above the
// panorama). 0xAARRGGBB, the seam's clear-colour layout (render.h MdkFrameParams).
inline uint32_t clearOf(int32_t mode, uint32_t sky) {
    return mode == kBlack ? 0xFF000000u : sky;
}

// C# Viewer.SkyColour (Viewer.cs:1112): the colour above the sky panorama -- the
// DTI palette's entry SkyTopColor, opaque. 0xAARRGGBB.
inline uint32_t skyColour(const formats::Dti& dti) {
    const int32_t index = dti.skyTopColor();
    if (index < 0 || index >= formats::Palette::kSize) {
        return 0xFF000000u;
    }
    const uint8_t* rgba = dti.palette().rgba() + index * 4;
    return 0xFF000000u | ((uint32_t)rgba[0] << 16) | ((uint32_t)rgba[1] << 8) | (uint32_t)rgba[2];
}

} // namespace sky_modes
} // namespace mdk

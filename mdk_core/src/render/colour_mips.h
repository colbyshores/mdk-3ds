#pragma once

// ============================================================================
// colour_mips.h -- the enhanced look's colour textures on the CPU, ported
// 1:1 from the C#'s ColourMips.cs: palette indices expanded to RGBA8
// (index 0 clear), then a mip chain per frame by box filter.
//
// Colours are premultiplied by their cover in effect: index 0 expands to
// (0,0,0,0) and every other index to alpha 255, so filtering weighs texels
// by cover and clear ones add no dark fringe.
// ============================================================================

#include <cstdint>
#include <vector>

namespace mdk {

// Mip levels from width x height down to 1 x 1.
int32_t colourLevels(int32_t width, int32_t height);

// Bytes of one frame's whole chain (all levels).
int32_t colourChainSize(int32_t width, int32_t height);

// Indices through an RGBA8 palette (paletteBytes long); index 0, or one
// whose colour would run past the palette, is clear. Writes count*4 bytes.
void colourExpand(const uint8_t* indices, int32_t count, const uint8_t* palette,
                  int32_t paletteBytes, uint8_t* outRgba);

// Fill the levels after the first in a chain buffer whose first level is
// already written (colourExpand into the chain's start).
void colourFillChain(uint8_t* chain, int32_t width, int32_t height);

// One texture's whole expansion: frames stacked, each frame's chain
// contiguous (chainSize * frames bytes).
std::vector<uint8_t> colourChain(const uint8_t* indices, int32_t width, int32_t height,
                                 int32_t frames, const uint8_t* palette,
                                 int32_t paletteBytes);

} // namespace mdk

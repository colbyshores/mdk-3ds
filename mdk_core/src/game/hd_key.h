#pragma once

// ============================================================================
// hd_key.h -- the names of a mod's replacement images, ported 1:1 from
// src/Mdk.Game/HdTextures/HdKey.cs (and HdImage.cs's record).
//
// An HD image's name is a hash of what the texture shows through a palette
// (its size, frames, indices, and the colours of the indices it uses).
// Arenas whose palettes differ only in colours a texture doesn't use share
// its image; a changed texture or colour makes another key, so a cache made
// from other game files is never used (stale).
//
//   SHA-256(width, height, frames, indices, RGB of each used index 1-255)
//     -> first 8 bytes, lower-case hex
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

namespace mdk {

namespace formats {
class Palette;
class Texture;
} // namespace formats

// An image replacing a texture for the renderer: `frames` frames of
// width x height stacked downwards, RGBA8 premultiplied (as the renderer's
// colour textures are).
struct HdImage {
    int32_t width = 0;
    int32_t height = 0;  // one frame's height
    int32_t frames = 1;
    std::vector<uint8_t> rgba;
};

// The key naming `texture` seen through `palette` (16 hex digits).
std::string hdKeyOf(const formats::Texture& texture, const formats::Palette& palette);

} // namespace mdk

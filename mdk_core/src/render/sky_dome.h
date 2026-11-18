#pragma once

// ============================================================================
// sky_dome.h -- the sky panorama as GEOMETRY, for a backend with no fragment
// shader (the 3DS's PICA200).
//
// The oracle draws the sky as a full-screen pass that maps every pixel's view
// direction to a panorama cell (shaders/panorama.hlsli):
//
//     ppr     = wrap_width / TAU                    (pixels per radian)
//     azimuth = atan2(dir.x, dir.y)
//     column  = (azimuth * ppr + offset) wrapped into [0, wrap_width)
//     row     = horizon_row - asin(dir.z) * ppr
//     rows < 0 take top_colour, rows >= height take bottom_colour
//
// Column is LINEAR in azimuth and row is LINEAR in elevation, so a sphere
// tessellated in (column, row) space with each vertex's direction made by the
// INVERSE of those formulas reproduces the mapping exactly at every vertex and,
// along each edge, linearly in angle. The cells are small (about 32 columns x
// 30 rows), so the remaining interpolation error is a small fraction of a
// panorama cell. The host test checks every vertex against the oracle's own
// formula (the oracle's mapping lives in tests/scripts/sky_dome_tests.cpp, in
// libm, so the reference is not the code under test).
//
// THE PICA ALSO CANNOT HOLD THE PANORAMA IN ONE TEXTURE: the shipped skies are
// wrap_width = 1800 columns, and a PICA texture is at most 1024 wide. The
// columns are cut into slices of at most kSkySliceMax, each its own index
// texture and its own vertex range, cut exactly at a slice boundary so no
// triangle spans two textures. Each slice's texture also carries ONE NEIGHBOUR
// COLUMN on each side (the panorama's previous and next columns, wrapped), so a
// bilinear tap at a slice's edge -- the 3DS's Enhanced Mode filtering -- reads
// the panorama's own neighbour instead of wrapping onto the slice's far edge: a
// one-texel seam at every boundary otherwise. A nearest tap never reaches them.
//
// AND INDEX 0 IS NOT TRANSPARENT IN THE SKY. The scene's expansion makes palette
// index 0 transparent (the palette shader's discard), but the sky shader uses
// index 0 as a colour. Each slice's index-0 texels are therefore remapped to
// the palette entry nearest (in RGB) to entry 0, which is exact whenever the
// palette repeats that colour and the nearest available colour otherwise; the
// build reports how many texels were remapped and the distance.
//
// Cold code: built once per level load. Nothing here runs per frame.
// ============================================================================

#include <stdint.h>

#include <vector>

#include "render/render_types.h"

namespace mdk {

namespace formats {
class Texture;
}

static constexpr int32_t kSkySliceMax = 1024;   // the PICA's widest texture
static constexpr int32_t kSkyColumnStep = 32;   // columns per cell (at most)
static constexpr int32_t kSkyRowStep = 30;      // rows per cell (at most)
static constexpr int32_t kSkyCapRings = 6;      // rings from the band to a pole
static constexpr float kSkyRadius = 10000.0f;   // inside the camera's 20000 far

// One vertical slice of the panorama: its own index texture.
struct SkySlice {
    int32_t firstColumn = 0;        // the first panorama column the slice DRAWS
    int32_t width = 0;              // how many columns it draws
    // The texture: the drawn columns plus one neighbour column on each side, so
    // storedWidth = width + 2 <= kSkySliceMax; stored column 0 is panorama column
    // firstColumn - 1 (wrapped) and the last is firstColumn + width (wrapped).
    int32_t storedWidth = 0;
    std::vector<uint8_t> indices;   // storedWidth * height, row-major, index 0 remapped
    int32_t first = 0;              // its triangles in SkyDome::vertices
    int32_t count = 0;
};

struct SkyDome {
    // The mesh: positions on a sphere of kSkyRadius around the origin (draw it
    // with the camera's position as the world translation), uv normalised over
    // the slice's STORED width and height (the backend scales to its padding).
    std::vector<Vertex> vertices;
    std::vector<SkySlice> slices;
    int32_t height = 0;
    // The two caps, each drawn with a flat colour (the palette's RGB, opaque).
    int32_t topFirst = 0, topCount = 0;
    int32_t bottomFirst = 0, bottomCount = 0;
    uint8_t topRgb[3] = {0, 0, 0};
    uint8_t bottomRgb[3] = {0, 0, 0};
    // The index-0 remap, reported.
    int32_t remappedTexels = 0;
    int32_t remapTarget = 0;
    int32_t remapDistance = 0;   // squared RGB distance from entry 0 to the target
};

// Builds the dome for a level's sky. Returns false (and leaves *out empty) when
// the inputs cannot describe a sky: no texture, a wrap width below one cell, or
// a texture narrower than the wrap width.
bool buildSkyDome(const formats::Texture& sky, const uint8_t* paletteRgba, int32_t wrapWidth,
                  int32_t horizonRow, int32_t offset, int32_t topColour, int32_t bottomColour,
                  SkyDome* out);

} // namespace mdk

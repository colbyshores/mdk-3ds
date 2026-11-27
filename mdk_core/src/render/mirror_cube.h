#pragma once

// ============================================================================
// mirror_cube.h -- the mirrors' panorama as CUBE MAPS, for a backend with no
// fragment shader (the 3DS's PICA200).
//
// THE ORACLE'S MIRROR (shaders/mirror.hlsl; the material is
// render/material_resolver.cpp's MIRRLOW...MIRRHIGH, 990-1010) reflects
// nothing: each pixel shows the level's MIRROR panorama (Dti::mirrorSky) in the
// direction from the camera to the surface, `row_shift` rows lower or higher,
// the rows WRAPPING over the panorama's height:
//
//     d       = normalize(world - camera)
//     column  = (atan2(d.x, d.y) * ppr + offset) wrapped into [0, wrap_width)
//     row     = floor(horizon_row - asin(d.z) * ppr)
//     texel   = panorama[column, (row + row_shift) mod height]
//
// That is a function of the DIRECTION alone, one per row shift -- which is
// exactly what a cube map holds. The PICA's texture unit 0 looks a cube map up
// by a per-pixel direction, and `world - camera` is linear in the world
// position, so the rasteriser's perspective-correct interpolation of it is
// the oracle's per-pixel direction. The backend draws the mirror's own
// triangles once, with its own program (mdk_3ds/shaders/mdk_mirror.v.pica):
// no CPU work per frame, no extra pass, nothing but the mirror's pixels.
//
// ONE CUBE PER ROW SHIFT THE LEVEL'S ARENAS USE. A row shift is not a rotation
// of the direction (the rows wrap), so each is its own picture; mirrorRowShifts
// finds the ones a level's arenas carry (the retail levels: LEVEL3 1, LEVEL5 3,
// LEVEL6 5, LEVEL8 1, none in LEVEL4 and LEVEL7), so every ROOM's mirror is
// exact. Models are not counted: LEVEL6's XF (the level's CMI) and XBSHARK
// (OLYM_10) carry nearly every value from 995 to 1010, a few triangles each,
// and a cube apiece would be 17 of them (3.2 MiB). Their draws take the
// nearest made shift instead (pica::mirrorCubeFor): at most 20 rows off.
//
// THE FACE CONVENTION is the PICA's cube lookup (mirrorCubeDirection): faces
// in GPU_TEXFACE order (+X, -X, +Y, -Y, +Z, -Z), the major-axis selection and
// the (s, t) of each face as OpenGL defines them, and t = 1 at a face's FIRST
// memory row (the PICA's t runs from the last row: pica_matrix.h,
// sceneUvFromTop). tests/scripts/mirror_cube_tests.cpp holds the inverse here
// against that forward lookup, written out independently.
//
// THE RESOLUTION is kMirrorCubeSize texels a face edge: 64-128 texels a radian
// (centre to edge of a face), against the panorama's wrap_width / 2pi (143 in
// LEVEL5/6, 286 elsewhere). The quantisation is the cube's, not the
// panorama's: a coarser picture in the mirror, never a different one.
//
// Cold code: built once per level load. Nothing here runs per frame.
// ============================================================================

#include <stdint.h>

#include <vector>

#include "render/render_types.h"

namespace mdk {

namespace formats {
class Arena;
class Texture;
}

static constexpr int32_t kMirrorCubeSize = 128;   // texels a face edge (a power of two)
static constexpr int32_t kMirrorCubeFaces = 6;
// MIRRLOW...MIRRHIGH: at most this many distinct row shifts.
static constexpr int32_t kMirrorRowShiftsMax = 21;

// The distinct row shifts of the mirror triangles in `arenas` (the colours
// 990-1010 -- a negative material value or a PEN_n name -- MaterialResolver's
// rowShift), ascending, into `shifts` (kMirrorRowShiftsMax entries). Returns
// how many.
int32_t mirrorRowShifts(const std::vector<const formats::Arena*>& arenas, int32_t* shifts);

// The direction (not normalised) the PICA's cube lookup samples face `face`'s
// texel (x, y) at, y counted from the face's first memory row.
Vec3 mirrorCubeDirection(int32_t face, int32_t x, int32_t y, int32_t size);

// The panorama cell each texel of one face shows before the row shift: the
// column in [0, wrapWidth) and the row wrapped into [0, height) (the oracle's
// rows wrap; the shift is mirrorCubeFace's). `columns` and `rows` hold
// size * size entries, row-major.
void mirrorCubeCells(int32_t face, int32_t size, int32_t wrapWidth, int32_t horizonRow,
                     int32_t offset, int32_t height, int16_t* columns, int16_t* rows);

// One face's palette indices for one row shift, from its cells (rows in
// [0, height), as mirrorCubeCells makes them): the panorama's index at
// (column, (row + rowShift) mod height). `count` cells. False (and nothing
// written) when the panorama is not readable, or the shift is a whole height
// or more (a mirror's is at most 80 rows).
bool mirrorCubeFace(const formats::Texture& panorama, const int16_t* columns,
                    const int16_t* rows, int32_t count, int32_t rowShift, uint8_t* out);

} // namespace mdk

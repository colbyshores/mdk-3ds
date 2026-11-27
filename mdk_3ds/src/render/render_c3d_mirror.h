#pragma once

// ============================================================================
// render_c3d_mirror.h -- the 3DS backend's MIRROR CUBES (render/mirror_cube.h):
// the mirror panorama, one cube map per row shift the level uses, made at
// level load and sampled by every Pass::Mirror draw through the mirrors'
// programs (mdk_3ds/shaders/mdk_mirror.v.pica, and mdk_mirror.g.pica for a
// command list already in geometry mode).
//
// Not part of the shared seam (render/render.h): the Linux backend draws the
// oracle's per-pixel mirror program instead (mdk_linux/shaders/mirror.frag).
//
// A mirror draw samples the cube of its row shift, or the nearest made one (a
// model's value no arena uses: pica::mirrorCubeFor). With no cube at all, or
// with the mirrors toggle off (Enhanced Mode enableMirrors, as the Linux
// backend reads it), it is SKIPPED -- never drawn as its placeholder material,
// the resolver's white (the owner's "white panels", 2026-11-27).
// ============================================================================

#include <stdint.h>

#include "render/render.h"

// Makes the cube for `rowShift` (rows; MaterialResolver's shift): kMirrorCubeSize
// texels a face, RGB565, linear heap. Faces arrive afterwards. Returns its slot,
// or -1 (no room in the table, the shift already has one, or the linear heap
// refused it -- logged). Between frames only (the level load).
int32_t mdk_render_create_mirror_cube(MdkRenderer r, int32_t rowShift);
// One face (GPU_TEXFACE order: +X -X +Y -Y +Z -Z) of a slot's cube, from
// kMirrorCubeSize^2 palette indices (mirrorCubeFace) through `paletteRgba` (256
// RGBA entries). The cube is sampled once all six have arrived.
void mdk_render_set_mirror_cube_face(MdkRenderer r, int32_t slot, int32_t face,
                                     const uint8_t* indices, const uint8_t* paletteRgba);
// Gives every cube back (the level's end): freed now, or at the next frame
// boundary when the GPU may still be reading one (AGENTS.md P3).
void mdk_render_release_mirror_cubes(MdkRenderer r);
// The cubes' linear bytes (the level-load log).
uint32_t mdk_render_mirror_cube_bytes(MdkRenderer r);

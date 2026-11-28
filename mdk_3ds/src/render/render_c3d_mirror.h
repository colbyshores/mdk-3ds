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

// Makes the cube for `rowShift` (rows; MaterialResolver's shift), `size` texels a
// face (a power of two), linear heap: ETC1 for a BAKED cube (`etc1`: the build's
// kMirrorBakedSize faces, mdk_render_set_mirror_cube_face_etc1), else RGB565 (the
// runtime build at kMirrorCubeSize, mdk_render_set_mirror_cube_face). Faces
// arrive afterwards. Returns its slot, or -1 (no room in the table, the shift
// already has one, or the linear heap refused it -- logged). Between frames only
// (the level load).
int32_t mdk_render_create_mirror_cube(MdkRenderer r, int32_t rowShift, int32_t size, bool etc1);
// One face (GPU_TEXFACE order: +X -X +Y -Y +Z -Z) of a runtime (RGB565) cube,
// from size^2 palette indices (mirrorCubeFace) through `paletteRgba` (256 RGBA
// entries). The cube is sampled once all six have arrived.
void mdk_render_set_mirror_cube_face(MdkRenderer r, int32_t slot, int32_t face,
                                     const uint8_t* indices, const uint8_t* paletteRgba);
// One face of a baked (ETC1) cube: `bytes` bytes of ETC1 blocks already in the
// PICA's tiled order (the build's file, mirror_cube.h). False when the slot is
// not an ETC1 cube or the size is not the face's.
bool mdk_render_set_mirror_cube_face_etc1(MdkRenderer r, int32_t slot, int32_t face,
                                          const uint8_t* blocks, uint32_t bytes);
// Gives every cube back (the level's end): freed now, or at the next frame
// boundary when the GPU may still be reading one (AGENTS.md P3).
void mdk_render_release_mirror_cubes(MdkRenderer r);
// The cubes' linear bytes (the level-load log).
uint32_t mdk_render_mirror_cube_bytes(MdkRenderer r);

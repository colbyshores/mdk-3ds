// render_seam_stubs.cpp -- the smallest fake of the renderer seam for
// the format tests: the font and menu units under test call three seam
// functions (upload the atlas, emit a canvas quad), and this binary
// tests their LOGIC, not a backend. The stubs record nothing and
// report success with id 0; a test that needs pixels belongs in the
// frame harness, not here.
//
// The mods tests (mods_tests.cpp) do need the surface cache's side of
// the seam, so the palette/RGBA creations return distinct ids and the
// colour replacement records its call for them to read.

#include <string>
#include <vector>

#include "render/render.h"

// The last colour replacement the surface cache registered, for
// mods_tests.cpp's Lit-hook case.
int mdk_test_replaceCalls = 0;
int32_t mdk_test_replaceTexture = -1;
int32_t mdk_test_replacePalette = -1;
int32_t mdk_test_replaceFrames = 0;
int32_t mdk_test_replaceWidth = 0;
int32_t mdk_test_replaceHeight = 0;
int mdk_test_replaceFirstByte = -1;

// The arena views' side of the seam (stream_tests.cpp): counts, so a test can
// say what is on the GPU after a walk -- meshes and index textures made and
// released, palettes, and the mesh draws queued.
int mdk_test_indexTextures = 0;
int mdk_test_texturesReleased = 0;
int mdk_test_palettes = 0;
int mdk_test_palettesReleased = 0;
int mdk_test_meshes = 0;
int mdk_test_meshesReleased = 0;
int mdk_test_meshDraws = 0;
// Index textures made from a null pixel pointer: each would be a texture made
// of pixels that are away. Counted over EVERY creation, not sampled.
int mdk_test_nullIndexTextures = 0;
// How many of the next mesh creations fail (-1), for the views' retry.
int mdk_test_failMeshCreates = 0;

MdkTextureId mdk_render_create_index_texture(MdkRenderer, int32_t, int32_t, const uint8_t* indices,
                                             const uint8_t*) {
    mdk_test_indexTextures++;
    if (indices == nullptr) {
        mdk_test_nullIndexTextures++;
    }
    return 0;
}

MdkTextureId mdk_render_create_streamed_texture(MdkRenderer r, int32_t w, int32_t h,
                                                const uint8_t* px, const uint8_t* paletteRgba) {
    return mdk_render_create_index_texture(r, w, h, px, paletteRgba);
}

// No compressed HD pack here: the index texture (render.h).
MdkTextureId mdk_render_create_hd_image(MdkRenderer r, int32_t w, int32_t h, const uint8_t* px,
                                        const uint8_t* paletteRgba, const char*) {
    return mdk_render_create_index_texture(r, w, h, px, paletteRgba);
}

int mdk_test_prepareCalls = 0;
void mdk_render_prepare_texture(MdkRenderer, MdkTextureId) {
    mdk_test_prepareCalls++;
}

int32_t mdk_render_settle_streams(MdkRenderer, int32_t) {
    return 0;
}

int32_t mdk_render_pin_hd_set(MdkRenderer, const char* const*, const int32_t*, int32_t) {
    return 0;
}

int32_t mdk_render_hd_set_pending(MdkRenderer) {
    return 0;
}

void mdk_render_release_texture(MdkRenderer, MdkTextureId) {
    mdk_test_texturesReleased++;
}

// SurfaceCache::refresh's call (a bullet hole changed a texture's pixels), and
// the packed original's fallback (a file the backend could not make): counted,
// with the last call's id and size, for mods_tests.cpp's packed-original cases.
int mdk_test_updateCalls = 0;
MdkTextureId mdk_test_updateTexture = -1;
int32_t mdk_test_updateWidth = 0;
int32_t mdk_test_updateHeight = 0;
void mdk_render_update_texture(MdkRenderer, MdkTextureId texture, int32_t width, int32_t height,
                               const uint8_t*) {
    mdk_test_updateCalls++;
    mdk_test_updateTexture = texture;
    mdk_test_updateWidth = width;
    mdk_test_updateHeight = height;
}

void mdk_render_release_palette(MdkRenderer, MdkPaletteId) {
    mdk_test_palettesReleased++;
}

MdkMeshId mdk_render_create_mesh(MdkRenderer, const mdk::Vertex*, int32_t) {
    if (mdk_test_failMeshCreates > 0) {
        mdk_test_failMeshCreates--;
        return -1;
    }
    return 100 + mdk_test_meshes++;
}

void mdk_render_release_mesh(MdkRenderer, MdkMeshId) {
    mdk_test_meshesReleased++;
}

// The end of a level's torn arena (LevelView::tear): the dynamic mesh it swaps to
// and the per-frame rewrites of it, with the last rewrite's corners kept so the
// test can read what was uploaded.
int mdk_test_dynamicMeshes = 0;
int mdk_test_meshUpdates = 0;
std::vector<mdk::Vertex> mdk_test_lastUpdate;

MdkMeshId mdk_render_create_dynamic_mesh(MdkRenderer, int32_t) {
    mdk_test_dynamicMeshes++;
    return 100 + mdk_test_meshes++;
}

void mdk_render_update_mesh(MdkRenderer, MdkMeshId, const mdk::Vertex* vertices, int32_t count) {
    mdk_test_meshUpdates++;
    mdk_test_lastUpdate.assign(vertices, vertices + count);
}

void mdk_render_draw_mesh(MdkRenderer, const mdk::MeshDraw*) {
    mdk_test_meshDraws++;
}

MdkTextureId mdk_render_create_rgba_texture(MdkRenderer, int32_t, int32_t, const uint8_t*) {
    return 3;
}

MdkTextureId mdk_render_create_compressed_texture(MdkRenderer, int32_t, int32_t, const uint8_t*,
                                                  int32_t) {
    return -1;
}

// The packed original's side (mods_tests.cpp): what create_compressed_file
// answers (-1 by default: no compressed textures, as before, so the other
// tests are unchanged), how often it was called, and which id wants its index
// pixels (mdk_render_texture_needs_pixels; -1: none).
MdkTextureId mdk_test_compressedFileId = -1;
int mdk_test_compressedFileCalls = 0;
MdkTextureId mdk_test_needsPixels = -1;
// The HD file the last create_compressed_file was handed ("" for none).
std::string mdk_test_compressedFileHd;
MdkTextureId mdk_render_create_compressed_file(MdkRenderer, int32_t, int32_t, int32_t, const char*,
                                               const char* hdPath, const uint8_t*) {
    mdk_test_compressedFileCalls++;
    mdk_test_compressedFileHd = hdPath ? hdPath : "";
    return mdk_test_compressedFileId;
}

bool mdk_render_texture_needs_pixels(MdkRenderer, MdkTextureId texture) {
    return texture >= 0 && texture == mdk_test_needsPixels;
}

int32_t mdk_render_texture_bytes(MdkRenderer, MdkTextureId) {
    return 0;
}

bool mdk_render_read_range_async(MdkRenderer, int32_t, const char*, uint32_t, uint32_t) {
    return false;
}

bool mdk_render_poll_range_read(MdkRenderer, int32_t*, uint8_t**, uint32_t*, bool*) {
    return false;
}

bool mdk_render_has_file_reader(MdkRenderer) {
    return false;
}

bool mdk_render_read_file_async(MdkRenderer, int32_t, const char*, MdkFileWork) {
    return false;
}

bool mdk_render_poll_file_read(MdkRenderer, int32_t, uint8_t**, uint32_t*, bool*) {
    return false;
}

void mdk_render_cancel_file_read(MdkRenderer, int32_t) {}


MdkPaletteId mdk_render_create_palette(MdkRenderer, const uint8_t*) {
    mdk_test_palettes++;
    return 7;
}

void mdk_render_canvas_quad(MdkRenderer, const mdk::CanvasQuad*) {}

void mdk_render_mark_canvas(MdkRenderer, MdkTextureId) {}
void mdk_render_mark_unmipped(MdkRenderer, MdkTextureId) {}

void mdk_render_replace_colour(MdkRenderer, MdkTextureId texture, MdkPaletteId palette,
                               int32_t frames, int32_t width, int32_t height,
                               const uint8_t* rgba) {
    mdk_test_replaceCalls++;
    mdk_test_replaceTexture = texture;
    mdk_test_replacePalette = palette;
    mdk_test_replaceFrames = frames;
    mdk_test_replaceWidth = width;
    mdk_test_replaceHeight = height;
    mdk_test_replaceFirstByte = rgba ? (int)rgba[0] : -1;
}

void mdk_render_replace_image(MdkRenderer, MdkTextureId, MdkPaletteId, int32_t, int32_t,
                              const uint8_t*) {}

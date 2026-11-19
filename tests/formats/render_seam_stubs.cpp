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

void mdk_render_release_texture(MdkRenderer, MdkTextureId) {
    mdk_test_texturesReleased++;
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

void mdk_render_draw_mesh(MdkRenderer, const mdk::MeshDraw*) {
    mdk_test_meshDraws++;
}

// The prewarm's cost question (render/surface_cache.h): the 3DS's arithmetic
// is not this binary's business, only that a texture costs something.
int32_t mdk_render_index_texture_bytes(MdkRenderer, int32_t width, int32_t height) {
    return width > 0 && height > 0 ? width * height * 4 : 0;
}

MdkTextureId mdk_render_create_rgba_texture(MdkRenderer, int32_t, int32_t, const uint8_t*) {
    return 3;
}

MdkPaletteId mdk_render_create_palette(MdkRenderer, const uint8_t*) {
    mdk_test_palettes++;
    return 7;
}

void mdk_render_canvas_quad(MdkRenderer, const mdk::CanvasQuad*) {}

int mdk_test_compressedCalls = 0;
MdkTextureId mdk_test_compressedTexture = -1;
std::string mdk_test_compressedPath;
void mdk_render_replace_compressed(MdkRenderer, MdkTextureId texture, const char* path) {
    mdk_test_compressedCalls++;
    mdk_test_compressedTexture = texture;
    mdk_test_compressedPath = path ? path : "";
}

int mdk_test_compressedImageCalls = 0;
std::string mdk_test_compressedImagePath;
void mdk_render_replace_image_compressed(MdkRenderer, MdkTextureId, const char* path) {
    mdk_test_compressedImageCalls++;
    mdk_test_compressedImagePath = path ? path : "";
}

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

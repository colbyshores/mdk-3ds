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

MdkTextureId mdk_render_create_index_texture(MdkRenderer, int32_t, int32_t, const uint8_t*,
                                             const uint8_t*) {
    return 0;
}

MdkTextureId mdk_render_create_rgba_texture(MdkRenderer, int32_t, int32_t, const uint8_t*) {
    return 3;
}

MdkPaletteId mdk_render_create_palette(MdkRenderer, const uint8_t*) {
    return 7;
}

void mdk_render_canvas_quad(MdkRenderer, const mdk::CanvasQuad*) {}

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

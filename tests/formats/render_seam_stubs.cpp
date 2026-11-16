// render_seam_stubs.cpp -- the smallest fake of the renderer seam for
// the format tests: the font and menu units under test call three seam
// functions (upload the atlas, emit a canvas quad), and this binary
// tests their LOGIC, not a backend. The stubs record nothing and
// report success with id 0; a test that needs pixels belongs in the
// frame harness, not here.

#include "render/render.h"

MdkTextureId mdk_render_create_index_texture(MdkRenderer, int32_t, int32_t, const uint8_t*,
                                             const uint8_t*) {
    return 0;
}

void mdk_render_canvas_quad(MdkRenderer, const mdk::CanvasQuad*) {}

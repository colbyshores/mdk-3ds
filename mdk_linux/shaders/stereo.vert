// stereo.vert -- the fullscreen triangle for the stereo composite.
//
// One screen-filling triangle from gl_VertexIndex, the same shape the sky
// pass uses: (-1,-1), (3,-1), (-1,3) covers the screen. uv runs (0,0) at the
// top-left to (1,1) at the bottom-right, the C# stereo shader's convention.

#version 450

layout(location = 0) out vec2 uv;

void main() {
    uint id = gl_VertexIndex;
    vec2 clip = vec2(id == 1u ? 3.0 : -1.0, id == 2u ? 3.0 : -1.0);
    gl_Position = vec4(clip, 0.0, 1.0);
    // Vulkan's NDC y runs DOWN the framebuffer (the HLSL original's
    // 0.5 - clip.y * 0.5 assumed it runs up), so uv.y grows with clip.y:
    // uv (0,0) is the framebuffer's top-left, the texture's first row.
    uv = vec2(clip.x * 0.5 + 0.5, clip.y * 0.5 + 0.5);
}

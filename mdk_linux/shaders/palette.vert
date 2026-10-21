// palette.vert — the GLSL port of shaders/palette.hlsl's vs_main.
//
// The matrix convention is the ENGINE's: row-major, row-vector
// (clip = p * M, camera_builder.h). GLSL reads the uploaded floats as
// column-major, so `p * M` in GLSL is exactly the row-vector product --
// the same result the C# gets from mul(view_projection, p) with HLSL's
// column-major cbuffer default. Do not "simplify" this to M * p: that is
// the transpose, and it mirrors the picture.

#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_colour;   // R8G8B8A8_UNORM, R in the low byte

layout(push_constant) uniform Push {
    mat4 view_projection;
    vec4 colour;
    int textured;      // 0 flat colour, 1 index+palette, 2 RGBA texture
    int frame_count;
    int frame;
    int unused_;
} pc;

layout(location = 0) out vec2 uv;
layout(location = 1) out vec4 colour;

void main() {
    gl_Position = vec4(in_position, 1.0) * pc.view_projection;
    uv = in_uv;
    colour = in_colour;
}

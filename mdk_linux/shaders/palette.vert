// palette.vert — the GLSL port of shaders/palette.hlsl's vs_main.
//
// The matrix convention is the ENGINE's: row-major, row-vector
// (clip = p * M, camera_builder.h). GLSL reads an uploaded mat4 as
// COLUMN-major, so the matrix the shader holds is that matrix transposed,
// and the column-vector product `M * p` IS the row-vector product p * M.
// That is exactly what the C# gets from `mul(view_projection,
// float4(position, 1.0))` with DXC's column-major matrix packing for
// SPIR-V. Writing it the other way round (`p * M`) is the transpose: it
// puts every point behind its own near plane and the level vanishes.

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
    gl_Position = pc.view_projection * vec4(in_position, 1.0);
    uv = in_uv;
    colour = in_colour;
}

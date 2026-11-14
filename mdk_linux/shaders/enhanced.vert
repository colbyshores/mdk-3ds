// enhanced.vert -- the GLSL port of shaders/enhanced.hlsl's vs_main.
//
// Row-major row-vector matrices uploaded verbatim, as everywhere in this
// tree: GLSL's column-major mat4 is the engine matrix transposed and
// `M * p` is the row-vector product.

#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec2 in_uv;

layout(push_constant) uniform Push {
    mat4 transform;
    mat4 world;
} pc;

layout(location = 0) out vec2 uv;
layout(location = 1) out vec3 world_position;

void main() {
    gl_Position = pc.transform * vec4(in_position, 1.0);
    uv = in_uv;
    world_position = (pc.world * vec4(in_position, 1.0)).xyz;
}

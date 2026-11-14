// gamma.frag -- the GLSL port of shaders/gamma.hlsl.
//
// The display's gamma (Renderer.Gamma.cs): the finished frame brightened
// or darkened before it is shown, the blacks and whites kept:
// colour^(1/gamma). Uses stereo.vert's fullscreen triangle.

#version 450

layout(set = 0, binding = 0) uniform sampler2D frame_texture;

layout(push_constant) uniform Gamma {
    // x: 1 / gamma.
    vec4 gamma;
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_colour;

void main() {
    vec3 colour = textureLod(frame_texture, uv, 0.0).rgb;
    out_colour = vec4(pow(colour, vec3(pc.gamma.x)), 1.0);
}

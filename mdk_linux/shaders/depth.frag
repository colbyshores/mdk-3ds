// depth.frag -- the GLSL port of shaders/depth.hlsl.
//
// The enhanced look's depth passes (the sun's shadow map here): depth
// only; palette index 0 is a hole, as in palette.hlsl. The push block is
// the palette pipeline's, so the palette descriptor sets bind unchanged.

#version 450

layout(binding = 0) uniform sampler2D index_texture;

layout(push_constant) uniform Push {
    mat4 view_projection;
    vec4 colour;
    int textured;
    int frame_count;
    int frame;
    int unused_;
} pc;

layout(location = 0) in vec2 uv;

void main() {
    if (pc.textured == 0) {
        return;
    }
    vec2 u = uv;
    if (pc.frame_count > 1) {
        u.y = (fract(u.y) + float(pc.frame)) / float(pc.frame_count);
    }
    int index = int(round(texture(index_texture, u).r * 255.0));
    if (index == 0) {
        discard;
    }
}

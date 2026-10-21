// palette.frag — the GLSL port of shaders/palette.hlsl's ps_main.
//
// Paletted surfaces in the original look: palette indices (R8) looked up
// in a 256x1 palette, nearest texels, index 0 transparent (discard).
// Flat-coloured surfaces skip the lookup. The vertex colour multiplies the
// result. The animated frames are stacked vertically:
// uv.y = (fract(uv.y) + frame) / frame_count.
//
// `textured` is the C# `textured` flag widened with one mode: the C++
// LevelView hands flat colours and canvas images to this shader as RGBA
// textures (the 3DS needs them that way), so mode 2 samples RGBA directly
// and skips the palette. The pixels are identical either way.

#version 450

layout(binding = 0) uniform sampler2D index_texture;
layout(binding = 1) uniform sampler2D palette_texture;

layout(push_constant) uniform Push {
    mat4 view_projection;
    vec4 colour;
    int textured;
    int frame_count;
    int frame;
    int unused_;
} pc;

layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 colour;

layout(location = 0) out vec4 out_colour;

void main() {
    if (pc.textured == 0) {
        out_colour = pc.colour * colour;
        return;
    }

    vec2 u = uv;
    if (pc.frame_count > 1) {
        u.y = (fract(u.y) + float(pc.frame)) / float(pc.frame_count);
    }

    if (pc.textured == 2) {
        out_colour = texture(index_texture, u) * pc.colour * colour;
        return;
    }

    int index = int(round(texture(index_texture, u).r * 255.0));
    if (index == 0) {
        discard;
    }

    out_colour = vec4(texelFetch(palette_texture, ivec2(index, 0), 0).rgb, 1.0)
                 * pc.colour * colour;
}

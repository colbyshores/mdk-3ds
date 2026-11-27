// mirror.frag — the GLSL port of shaders/mirror.hlsl's ps_main + panorama.hlsli.
//
// No reflection: the pixel shows the mirror panorama where the sky behind the
// surface would be (the same azimuth/elevation mapping as sky.frag), row_shift
// rows lower or higher. Unlike the sky, the rows WRAP (there are no top and
// bottom colours), and the lookup is always nearest -- the oracle's mirror has
// no filtered path. Opaque, and never discarded (mirror.hlsl:36).

#version 450

layout(binding = 0) uniform sampler2D panorama_texture;  // R8 indices
layout(binding = 1) uniform sampler2D palette_texture;   // RGBA8 256x1

layout(push_constant) uniform Push {
    mat4 view_projection;
    vec4 pano;          // wrap_width, horizon_row, offset, height
    vec4 camera_shift;  // camera position xyz, row_shift
} pc;

layout(location = 0) in vec3 world;
layout(location = 0) out vec4 out_colour;

const float TAU = 6.28318530718;

void main() {
    vec3 dir = normalize(world - pc.camera_shift.xyz);
    // panorama_pixel (panorama.hlsli): the column, and the row rounded down.
    float pixels_per_radian = pc.pano.x / TAU;
    float azimuth = atan(dir.x, dir.y);
    float elevation = asin(clamp(dir.z, -1.0, 1.0));
    float column = mod(mod(azimuth * pixels_per_radian + pc.pano.z, pc.pano.x) + pc.pano.x, pc.pano.x);
    float row = floor(pc.pano.y - elevation * pixels_per_radian);
    // HLSL's fmod keeps the sign of the dividend, so the oracle's double fmod
    // is a true modulo; GLSL's mod already is one.
    int wrappedRow = int(mod(row + pc.camera_shift.w, pc.pano.w));
    int w = int(pc.pano.x);
    int wrappedColumn = ((int(column) % w) + w) % w;
    int index = int(round(texelFetch(panorama_texture, ivec2(wrappedColumn, wrappedRow), 0).r * 255.0));
    out_colour = vec4(texelFetch(palette_texture, ivec2(index, 0), 0).rgb, 1.0);
}

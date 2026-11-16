// enhanced.frag -- the GLSL port of shaders/enhanced.hlsl.
//
//   mode 1 lit:    albedo x exposure x (hemisphere + sun x N.L x shadow +
//                  point lights), tone mapped, then the haze
//   mode 2 sprite: albedo, then the haze; no light
//   mode 3 canvas: the 2D canvas: its index texture filtered by hand
//                  (palette_filtered below), edges blended by their cover
//   mode 4 image:  the canvas's images replaced by mods: their colour
//                  texture, blended like the canvas
//
// The shadow term is 1.0 in this slice: the sun shadow pass is the next
// p6 sub-batch, and `shadow.w` (shadows on) is honoured by that slice
// binding a real map. Normals are flat, from the world position's screen
// derivatives, turned to the camera -- the C#'s own construction.

#version 450

layout(set = 0, binding = 0) uniform sampler2DArray colour_texture;

layout(set = 0, binding = 1) uniform FragmentUniforms {
    vec4 colour;
    int textured;
    int frame_count;
    int frame;
    int mode;
    mat4 sun_matrix;
    vec4 sun;        // xyz direction, w exposure
    vec4 sun_colour;
    vec4 sky;        // w: tone knee
    vec4 ground;
    vec4 camera;
    vec4 haze;       // rgb sRGB colour, a density
    vec4 shadow;     // x size, y depth bias, z normal offset, w shadows on
} u;

layout(set = 0, binding = 2) uniform PointLights {
    vec4 light_count;
    vec4 lights[32];
} pl;

layout(set = 0, binding = 3) uniform sampler2D shadow_map;
layout(set = 0, binding = 4) uniform sampler2D index_texture;
layout(set = 0, binding = 5) uniform sampler2D palette_texture;

layout(location = 0) in vec2 uv;
layout(location = 1) in vec3 world_position;
layout(location = 0) out vec4 out_colour;

const float GAMMA = 2.2;
const float HALF_COVER = 0.5;
const int MAX_LIGHTS = 16;

vec3 to_linear(vec3 c) {
    return pow(max(c, vec3(0.0)), vec3(GAMMA));
}

vec3 to_srgb(vec3 c) {
    return pow(max(c, vec3(0.0)), vec3(1.0 / GAMMA));
}

vec3 tonemap(vec3 c, float knee) {
    float room = 1.0 - knee;
    vec3 shoulder = 1.0 - room * exp(-(c - knee) / room);
    return mix(c, shoulder, step(knee, c));
}

vec3 point_lights(vec3 position, vec3 normal) {
    vec3 sum = vec3(0.0);
    int count = int(pl.light_count.x);
    for (int i = 0; i < count; i++) {
        vec4 at = pl.lights[i * 2];
        vec3 to = at.xyz - position;
        float d2 = dot(to, to);
        float r2 = at.w * at.w;
        if (d2 >= r2) {
            continue;
        }
        float fade = 1.0 - d2 / r2;
        float facing = clamp(dot(normal, to * inversesqrt(max(d2, 1e-4))), 0.0, 1.0);
        sum += pl.lights[i * 2 + 1].rgb * facing * fade * fade;
    }
    return sum;
}

// How much sunlight reaches a point: 2x2 shadow map texels compared,
// then blended; fades out towards the map's edge (enhanced.hlsl).
float sunlight(vec3 position, vec3 normal) {
    if (u.shadow.w == 0.0) {
        return 1.0;
    }
    vec4 clip = u.sun_matrix * vec4(position + normal * u.shadow.z, 1.0);
    vec2 ndc = clip.xy / clip.w;
    float edge = max(abs(ndc.x), abs(ndc.y));
    if (edge >= 1.0 || clip.z >= 1.0) {
        return 1.0;
    }
    vec2 texel = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * u.shadow.x - 0.5;
    ivec2 cell = ivec2(floor(texel));
    vec2 f = fract(texel);
    int last = int(u.shadow.x) - 1;
    float depth = clip.z - u.shadow.y;
    float lit[4];
    for (int i = 0; i < 4; i++) {
        ivec2 at = clamp(cell + ivec2(i % 2, i / 2), ivec2(0), ivec2(last));
        lit[i] = depth <= texelFetch(shadow_map, at, 0).r ? 1.0 : 0.0;
    }
    float light = mix(mix(lit[0], lit[1], f.x), mix(lit[2], lit[3], f.x), f.y);
    const float fade_width = 0.1;
    return mix(light, 1.0, clamp((edge - (1.0 - fade_width)) / fade_width, 0.0, 1.0));
}

// palette_filtered.hlsli with EDGE_CLEAR, the canvas's edges: bilinear
// filtering by hand. Each of the four nearest texels goes through the
// palette first (indices can't be blended), index 0 transparent; animated
// textures keep to their frame (frames stacked vertically). Out of the
// texture: transparent. The result is premultiplied by its coverage.
vec4 palette_texel(ivec2 cell, ivec2 frame_size, int frame) {
    if (cell.x < 0 || cell.y < 0 || cell.x >= frame_size.x || cell.y >= frame_size.y) {
        return vec4(0.0);
    }
    ivec2 wrapped = ivec2(((cell.x % frame_size.x) + frame_size.x) % frame_size.x,
                          ((cell.y % frame_size.y) + frame_size.y) % frame_size.y +
                              frame * frame_size.y);
    int index = int(round(texelFetch(index_texture, wrapped, 0).r * 255.0));
    return index == 0 ? vec4(0.0)
                      : vec4(texelFetch(palette_texture, ivec2(index, 0), 0).rgb, 1.0);
}

// The filtered colour at uv (0-1 over one frame), premultiplied.
vec4 palette_filtered(vec2 uv, int frame_count, int frame) {
    ivec2 size = textureSize(index_texture, 0);
    int count = max(frame_count, 1);
    ivec2 frame_size = ivec2(size.x, size.y / count);
    int current = count > 1 ? frame : 0;
    vec2 position = uv * vec2(frame_size) - 0.5;
    ivec2 cell = ivec2(floor(position));
    vec2 f = fract(position);
    vec4 top = mix(palette_texel(cell, frame_size, current),
                   palette_texel(cell + ivec2(1, 0), frame_size, current), f.x);
    vec4 bottom = mix(palette_texel(cell + ivec2(0, 1), frame_size, current),
                      palette_texel(cell + ivec2(1, 1), frame_size, current), f.x);
    return mix(top, bottom, f.y);
}

// Interleaved gradient noise (Jimenez 2014), the dither.hlsli port.
vec3 dither(vec3 c, vec2 pixel) {
    float noise = fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
    return c + (noise - 0.5) / 255.0;
}

void main() {
    // Derivatives before any discard.
    vec3 position = world_position;
    vec3 normal = normalize(cross(dFdx(position), dFdy(position)));
    if (dot(normal, u.camera.xyz - position) < 0.0) {
        normal = -normal;
    }

    // Sampled before any discard (mips need the derivatives).
    vec4 texel = vec4(1.0);
    vec4 colours = texture(colour_texture, vec3(uv, u.frame_count > 1 ? u.frame : 0));
    if (u.textured != 0) {
        texel = u.mode == 3 ? palette_filtered(uv, u.frame_count, u.frame) : colours;
    }

    // The canvas blends its edges; the rest cut them where half covered
    // (premultiplied -> straight).
    if (u.mode == 3 || u.mode == 4) {
        out_colour = vec4(texel.a > 0.0 ? texel.rgb / texel.a : texel.rgb, texel.a) * u.colour;
        return;
    }

    if (texel.a < HALF_COVER) {
        discard;
    }

    vec3 albedo = to_linear(texel.rgb / texel.a * u.colour.rgb);
    vec3 lit = albedo;
    if (u.mode == 1) {
        float facing = max(dot(normal, -u.sun.xyz), 0.0);
        vec3 hemisphere = mix(u.ground.rgb, u.sky.rgb, normal.z * 0.5 + 0.5);
        vec3 light = hemisphere + u.sun_colour.rgb * facing * sunlight(position, normal) +
                     point_lights(position, normal);
        lit = tonemap(albedo * u.sun.w * light, u.sky.w);
    }

    float fog = 1.0 - exp(-u.haze.a * length(position - u.camera.xyz));
    out_colour = vec4(dither(to_srgb(mix(lit, to_linear(u.haze.rgb), fog)), gl_FragCoord.xy),
                      u.colour.a);
}

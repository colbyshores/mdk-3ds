// post.frag -- the GLSL port of shaders/post.hlsl with
// shaders/screen.hlsli and shaders/dither.hlsli: the lit scene times
// the blurred occlusion, plus the glow (the scene's blurred mips,
// screen-blended), dithered. Uses stereo.vert's fullscreen triangle
// (uv (0,0) at the framebuffer's top-left, the oracle's convention);
// pixel coordinates come from gl_FragCoord with y flipped to that
// origin. The occlusion blur helpers are occlusion.frag's, repeated
// because the two passes are separate programs in the oracle too
// (screen.hlsli is included by both).

#version 450

layout(set = 0, binding = 0) uniform sampler2D scene_texture;
layout(set = 0, binding = 1) uniform sampler2D depth_texture;
layout(set = 0, binding = 2) uniform sampler2D occlusion_texture;

layout(set = 0, binding = 3) uniform Post {
    mat4 clip_to_view;
    mat4 view_to_clip;
    // xy: the frame's size in pixels; zw: one pixel in uv.
    vec4 screen;
    // x: radius (units); y: strength; z: the haze's density per unit.
    vec4 occlusion;
    // x: bloom; y: threshold; z: intensity; w: the blurred mips.
    vec4 glow;
} u;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_colour;

const float GAMMA = 2.2;
const float FAR = 1.0;
const int NOISE_TILE = 4;
// How far off a pixel's plane a neighbour may be, per unit of distance,
// and still be blurred in.
const float SAME_SURFACE = 0.02;

vec3 to_linear(vec3 c) {
    return pow(max(c, vec3(0.0)), vec3(GAMMA));
}

vec3 to_srgb(vec3 c) {
    return pow(max(c, vec3(0.0)), vec3(1.0 / GAMMA));
}

// Interleaved gradient noise (Jimenez 2014), the dither.hlsli port.
vec3 dither(vec3 c, vec2 pixel) {
    float noise = fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
    return c + (noise - 0.5) / 255.0;
}

float depth_at(vec2 at_uv) {
    ivec2 pixel = clamp(ivec2(at_uv * u.screen.xy), ivec2(0, 0), ivec2(u.screen.xy) - 1);
    return texelFetch(depth_texture, pixel, 0).r;
}

vec3 point_at(vec2 at_uv) {
    vec4 p = u.clip_to_view * vec4(at_uv.x * 2.0 - 1.0, 1.0 - at_uv.y * 2.0,
                                   depth_at(at_uv), 1.0);
    return p.xyz / p.w;
}

// The nearer of two differences (no normal across a depth edge).
vec3 nearer(vec3 a, vec3 b) {
    return dot(a, a) < dot(b, b) ? a : b;
}

// The surface's normal at p, from the depth buffer, turned to the camera.
vec3 normal_at(vec2 at_uv, vec3 p) {
    vec3 dx = nearer(point_at(at_uv + vec2(u.screen.z, 0.0)) - p,
                     p - point_at(at_uv - vec2(u.screen.z, 0.0)));
    vec3 dy = nearer(point_at(at_uv + vec2(0.0, u.screen.w)) - p,
                     p - point_at(at_uv - vec2(0.0, u.screen.w)));
    vec3 n = normalize(cross(dx, dy));
    return dot(n, p) > 0.0 ? -n : n;
}

// The occlusion averaged over the 4 x 4 pixels around (one of each noise
// turn), leaving out those off p's plane (another surface: no shade
// across edges).
float blurred_occlusion(vec2 at_uv, vec3 p, vec2 pixel) {
    vec3 n = normal_at(at_uv, p);
    float reach = SAME_SURFACE * length(p);
    ivec2 at = ivec2(pixel);
    ivec2 last = ivec2(u.screen.xy) - 1;
    float sum = 0.0;
    float count = 0.0;
    for (int y = -NOISE_TILE / 2; y < NOISE_TILE / 2; y++) {
        for (int x = -NOISE_TILE / 2; x < NOISE_TILE / 2; x++) {
            ivec2 q = clamp(at + ivec2(x, y), ivec2(0, 0), last);
            vec2 quv = (vec2(q) + 0.5) * u.screen.zw;
            if (depth_at(quv) >= FAR || abs(dot(point_at(quv) - p, n)) > reach) {
                continue;
            }
            sum += texelFetch(occlusion_texture, q, 0).r;
            count += 1.0;
        }
    }
    return count > 0.0 ? sum / count : texelFetch(occlusion_texture, at, 0).r;
}

void main() {
    vec2 pixel = vec2(gl_FragCoord.x, u.screen.y - gl_FragCoord.y);
    vec3 colour = to_linear(textureLod(scene_texture, uv, 0.0).rgb);

    // The sky (nothing in the depth buffer) has no occlusion.
    if (depth_at(uv) < FAR) {
        colour *= blurred_occlusion(uv, point_at(uv), pixel);
    }

    vec3 blurred = to_linear((textureLod(scene_texture, uv, u.glow.w).rgb +
                              textureLod(scene_texture, uv, u.glow.w + 2.0).rgb) * 0.5);
    vec3 shine = clamp((blurred * u.glow.x + max(blurred - u.glow.y, vec3(0.0))) * u.glow.z,
                       vec3(0.0), vec3(1.0));
    colour = 1.0 - (1.0 - colour) * (1.0 - shine);
    out_colour = vec4(dither(to_srgb(colour), pixel), 1.0);
}

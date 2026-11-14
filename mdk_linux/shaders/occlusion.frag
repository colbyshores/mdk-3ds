// occlusion.frag -- the GLSL port of shaders/occlusion.hlsl with
// shaders/screen.hlsli: Alchemy ambient occlusion from the camera's
// depth, noisy (12 samples a pixel on a disc in the surface's plane,
// turned per pixel in a 4x4 ordered pattern), faded by the haze. The
// post pass blurs it. Uses stereo.vert's fullscreen triangle: uv is
// (0,0) at the framebuffer's top-left, the oracle's own convention, so
// the clip reconstruction below is the HLSL's word for word. Pixel
// coordinates come from gl_FragCoord with y flipped to that same
// top-left origin (Vulkan's gl_FragCoord.y runs from the bottom).

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

const float TAU = 6.28318530718;
const float GOLDEN_ANGLE = 2.39996323;
const float FAR = 1.0;
const int SAMPLES = 12;
const int NOISE_TILE = 4;

// A 4 x 4 Bayer matrix: neighbours turn far apart.
const float ORDERED[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0,
                                    3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);

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

float ambient_occlusion(vec2 at_uv, vec3 p, vec2 pixel) {
    vec3 n = normal_at(at_uv, p);

    // A disc in the plane, turned per pixel in a 4 x 4 ordered pattern to
    // spread the samples: the post pass's 4 x 4 blur sees every turn once.
    ivec2 cell = ivec2(pixel) % NOISE_TILE;
    float noise = (ORDERED[cell.y * NOISE_TILE + cell.x] + 0.5) /
                  float(NOISE_TILE * NOISE_TILE);
    vec3 t = normalize(cross(n, abs(n.z) < 0.9 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0)));
    vec3 b = cross(n, t);
    float radius = u.occlusion.x;
    float bias = 0.02 * radius;
    float epsilon = 0.01 * radius * radius;
    float sum = 0.0;
    for (int i = 0; i < SAMPLES; i++) {
        float angle = noise * TAU + float(i) * GOLDEN_ANGLE;
        vec3 s = p + (t * cos(angle) + b * sin(angle)) * (radius * (float(i) + noise) / float(SAMPLES));
        vec4 clip = u.view_to_clip * vec4(s, 1.0);
        if (clip.w <= 0.0) {
            continue;
        }
        vec2 at = vec2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
        if (any(lessThan(at, vec2(0.0))) || any(greaterThan(at, vec2(1.0))) ||
            depth_at(at) >= FAR) {
            continue;
        }
        vec3 v = point_at(at) - p;
        float vv = dot(v, v);
        float near = clamp(1.0 - vv / (4.0 * radius * radius), 0.0, 1.0);
        sum += max(dot(v, n) - bias, 0.0) / (vv + epsilon) * radius * near;
    }
    return clamp(1.0 - u.occlusion.y * sum / float(SAMPLES), 0.0, 1.0);
}

void main() {
    // The sky (nothing in the depth buffer) has no occlusion.
    if (depth_at(uv) >= FAR) {
        out_colour = vec4(1.0, 1.0, 1.0, 1.0);
        return;
    }
    vec2 pixel = vec2(gl_FragCoord.x, u.screen.y - gl_FragCoord.y);
    vec3 p = point_at(uv);
    float clear = exp(-u.occlusion.z * length(p));
    float shade = mix(1.0, ambient_occlusion(uv, p, pixel), clear);
    out_colour = vec4(shade, shade, shade, 1.0);
}

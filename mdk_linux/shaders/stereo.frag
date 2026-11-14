// stereo.frag -- the GLSL port of shaders/stereo.hlsl.
//
// The stereo composite (Renderer.Stereo.cs): the two eyes' frames (drawn
// whole, each through its own camera) into one frame:
//
//   side by side:   the left eye in the left half, the right eye in the right half
//   interlaced:     even rows one eye, odd rows the other
//   top and bottom: the left eye in the top half, the right eye in the bottom half
//   VR viewer:      side by side, each half bent inward (barrel)
//
// mode.y exchanges the eyes (crossview, the reversed interlaced/tab modes).

#version 450

layout(set = 0, binding = 0) uniform sampler2D left_texture;
layout(set = 0, binding = 1) uniform sampler2D right_texture;

layout(push_constant) uniform Stereo {
    // x: 0 side by side, 1 interlaced, 2 top and bottom, 3 VR viewer;
    // y: 1 the eyes exchanged; z: an eye's aspect (width / height);
    // w: the frame height in pixels (interlaced row parity).
    vec4 mode;
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_colour;

// The VR viewer's lens constants, from the C# shader (K1, K2).
const float K1 = 0.22;
const float K2 = 0.24;

float distortion(float r2) {
    return 1.0 + K1 * r2 + K2 * r2 * r2;
}

void main() {
    bool swapped = pc.mode.y > 0.5;
    if (pc.mode.x > 2.5) {
        float aspect = pc.mode.z;
        vec2 local = vec2(fract(uv.x * 2.0), uv.y);
        vec2 q = (local - 0.5) * 2.0 * vec2(aspect, 1.0);
        vec2 source = q * distortion(dot(q, q)) / distortion(aspect * aspect);
        vec2 suv = source / (2.0 * vec2(aspect, 1.0)) + 0.5;
        if (any(lessThan(suv, vec2(0.0))) || any(greaterThan(suv, vec2(1.0)))) {
            out_colour = vec4(0.0, 0.0, 0.0, 1.0);
            return;
        }
        bool left_eye = uv.x < 0.5;
        out_colour = left_eye ? textureLod(left_texture, suv, 0.0)
                              : textureLod(right_texture, suv, 0.0);
        return;
    }

    if (pc.mode.x < 0.5) {
        vec2 suv = vec2(fract(uv.x * 2.0), uv.y);
        bool left_eye = (uv.x < 0.5) != swapped;
        out_colour = left_eye ? textureLod(left_texture, suv, 0.0)
                              : textureLod(right_texture, suv, 0.0);
        return;
    }

    if (pc.mode.x < 1.5) {
        // Rows count from the TOP (uv.y * height), the C# shader's
        // SV_Position convention; gl_FragCoord.y counts from the bottom and
        // would put the other eye on every row.
        bool even = (int(uv.y * pc.mode.w) & 1) == 0;
        bool left_eye = even != swapped;
        out_colour = left_eye ? textureLod(left_texture, uv, 0.0)
                              : textureLod(right_texture, uv, 0.0);
        return;
    }

    vec2 suv = vec2(uv.x, fract(uv.y * 2.0));
    bool left_eye = (uv.y < 0.5) != swapped;
    out_colour = left_eye ? textureLod(left_texture, suv, 0.0)
                          : textureLod(right_texture, suv, 0.0);
}

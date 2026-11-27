// mirror.vert — the GLSL port of shaders/mirror.hlsl's vs_main.
//
// The mirrors (MIRRLOW...MIRRHIGH) reflect nothing: each pixel shows the
// mirror panorama in the direction from the camera to the surface, so the
// vertex hands the fragment its WORLD position (the oracle's `output.world =
// input.position`: the arena's vertices are world positions, and the depth
// pull lives in the matrix only, so the direction is the unpulled surface's).
//
// The push block is the 96-byte range every pipeline here shares
// (01_types.inc MirrorPush): the matrix is uploaded exactly as palette.vert's
// (row-major, row-vector; GLSL's `M * v` is then the row-vector product).

#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_colour;

layout(push_constant) uniform Push {
    mat4 view_projection;
    vec4 pano;          // wrap_width, horizon_row, offset, height
    vec4 camera_shift;  // camera position xyz, row_shift
} pc;

layout(location = 0) out vec3 world;

void main() {
    gl_Position = pc.view_projection * vec4(in_position, 1.0);
    world = in_position;
}

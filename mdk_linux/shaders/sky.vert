// sky.vert — the GLSL port of shaders/sky.hlsl's vs_main.
//
// One screen-filling triangle from gl_VertexIndex. Each vertex carries the
// world direction its clip position looks along: the clip-to-direction matrix
// (the camera's rotation and projection, inverted) turns a clip point into a
// ray. The matrix is uploaded in the COLUMN-vector form the C# uses for the sky
// (HLSL mul(clip_to_direction, v)), which is the transpose of the row-vector
// matrices the mesh pipeline uses -- so here it is `M * v`, NOT `v * M`.

#version 450

layout(push_constant) uniform Push {
    mat4 clip_to_direction;
    vec4 pano;        // wrap_width, horizon_row, offset, height
    ivec2 topbottom;  // top_colour, bottom_colour (palette indices)
    float row_shift;
    int sampling;
} pc;

layout(location = 0) out vec3 direction;

void main() {
    uint id = gl_VertexIndex;
    // (-1,-1), (3,-1), (-1,3): covers the screen.
    vec2 clip = vec2(id == 1u ? 3.0 : -1.0, id == 2u ? 3.0 : -1.0);
    gl_Position = vec4(clip, 0.0, 1.0);
    vec4 d = pc.clip_to_direction * vec4(clip, 1.0, 1.0);
    direction = d.xyz / d.w;
}

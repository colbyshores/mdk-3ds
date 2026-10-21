// sky.frag — the GLSL port of shaders/sky.hlsl's ps_main + panorama.hlsli.
//
// The panorama is a 2D backdrop (not a dome): the azimuth (counted from +Y
// towards +X) picks the column, the elevation picks the row, horizon_row is at
// eye level, and above/below the panorama the screen is the top/bottom palette
// colours. The panorama is paletted: an R8 index texture looked up in a 256x1
// palette, exactly like the level's surfaces. The original look samples
// nearest; the enhanced look (sampling == 1) filters the 4 nearest pixels.

#version 450

layout(binding = 0) uniform sampler2D panorama_texture;  // R8 indices
layout(binding = 1) uniform sampler2D palette_texture;   // RGBA8 256x1

layout(push_constant) uniform Push {
    mat4 clip_to_direction;
    vec4 pano;        // wrap_width, horizon_row, offset, height
    ivec2 topbottom;  // top_colour, bottom_colour (palette indices)
    float row_shift;
    int sampling;
} pc;

layout(location = 0) in vec3 direction;
layout(location = 0) out vec4 out_colour;

const float TAU = 6.28318530718;

// Where in the panorama a direction looks (columns and rows, not rounded).
vec2 panorama_position(vec3 dir) {
    float pixels_per_radian = pc.pano.x / TAU;
    float azimuth = atan(dir.x, dir.y);
    float elevation = asin(clamp(dir.z, -1.0, 1.0));
    float column = mod(mod(azimuth * pixels_per_radian + pc.pano.z, pc.pano.x) + pc.pano.x, pc.pano.x);
    return vec2(column, pc.pano.y - elevation * pixels_per_radian);
}

int panorama_index(int column, int row) {
    int w = int(pc.pano.x);
    int wrapped = ((column % w) + w) % w;
    return int(round(texelFetch(panorama_texture, ivec2(wrapped, row), 0).r * 255.0));
}

vec3 sky_pixel(int column, int row) {
    int index;
    if (row < 0) {
        index = pc.topbottom.x;
    } else if (row >= int(pc.pano.w)) {
        index = pc.topbottom.y;
    } else {
        index = panorama_index(column, row);
    }
    return texelFetch(palette_texture, ivec2(index, 0), 0).rgb;
}

void main() {
    vec3 dir = normalize(direction);
    if (pc.sampling == 1) {
        vec2 position = panorama_position(dir) - 0.5;
        ivec2 cell = ivec2(floor(position));
        vec2 f = fract(position);
        vec3 top = mix(sky_pixel(cell.x, cell.y), sky_pixel(cell.x + 1, cell.y), f.x);
        vec3 bottom = mix(sky_pixel(cell.x, cell.y + 1), sky_pixel(cell.x + 1, cell.y + 1), f.x);
        out_colour = vec4(mix(top, bottom, f.y), 1.0);
        return;
    }
    vec2 position = panorama_position(dir);
    out_colour = vec4(sky_pixel(int(position.x), int(floor(position.y))), 1.0);
}

// level_light.cpp -- see level_light.h. The arithmetic is the C#'s own,
// operation for operation (cold path: level load and arena changes, so
// powf here is the oracle's MathF.Pow, not a LUT candidate).

#include "game/level_light.h"

#include "formats/arena.h"
#include "formats/dti.h"
#include "formats/palette.h"
#include "formats/texture.h"

#include <cmath>

namespace mdk {
namespace {

// ArenaShape's thresholds.
const float kHorizontal = 0.5f;
const float kCoveredShare = 0.5f;

// EnhancedLook's constants.
const float kSunWeakest = 0.4f;
const float kSunStrongest = 1.0f;
const float kBrightSky = 0.3f;
const float kSunSaturation = 0.25f;
const float kOpenAbove = 0.6f;
const float kOpenBelow = 0.35f;
const float kAmbientSaturation = 0.3f;
const float kCoveredAbove = 1.0f;
const float kCoveredBelow = 0.7f;
const float kCoveredSaturation = 0.15f;
const float kSunlitShare = 0.7f;
const float kMeanLight = 1.15f;
const float kShadowDistance = 300.0f;
const float kGlowBloom = 0.0f;
const float kHazeDensity = 0.0002f;

// SkyLight's gamma and luminance weights.
const float kSkyGamma = 2.2f;

struct Face {
    Vec3 normal;
    float area;
};

// A drawn triangle's outward normal and area; false when it has none or
// only blocks Kurt (ArenaShape.Face: the outward normal is the negated
// cross product -- MDK winds a floor's corners clockwise from above).
bool faceOf(const formats::Arena& arena, int32_t triangle, Face* out) {
    if (arena.clipFlag() != 0 &&
        (arena.triangleFlags()[(size_t)triangle] & arena.clipFlag()) != 0) {
        return false;
    }
    const std::vector<formats::Vec3f>& vertices = arena.vertices();
    const std::vector<int32_t>& indices = arena.triangleIndices();
    const formats::Vec3f& a = vertices[(size_t)indices[(size_t)triangle * 3]];
    const formats::Vec3f& b = vertices[(size_t)indices[(size_t)triangle * 3 + 1]];
    const formats::Vec3f& c = vertices[(size_t)indices[(size_t)triangle * 3 + 2]];
    const Vec3 e1{b.x - a.x, b.y - a.y, b.z - a.z};
    const Vec3 e2{c.x - a.x, c.y - a.y, c.z - a.z};
    const Vec3 cross{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z,
                     e1.x * e2.y - e1.y * e2.x};
    const float length = std::sqrt(cross.x * cross.x + cross.y * cross.y + cross.z * cross.z);
    if (length <= 0.0f) {
        return false;
    }
    out->normal = Vec3{-cross.x / length, -cross.y / length, -cross.z / length};
    out->area = length * 0.5f;
    return true;
}

Vec3 linearOf(const formats::Palette& palette, int32_t index) {
    const uint8_t* rgba = palette.rgba();
    const Vec3 srgb{rgba[(size_t)index * 4] / 255.0f, rgba[(size_t)index * 4 + 1] / 255.0f,
                    rgba[(size_t)index * 4 + 2] / 255.0f};
    return Vec3{std::pow(srgb.x, kSkyGamma), std::pow(srgb.y, kSkyGamma),
                std::pow(srgb.z, kSkyGamma)};
}

// The mean linear colour of sky rows [first, end) over the full turn's
// columns (SkyLight.Mean).
Vec3 skyMean(const formats::Dti& dti, int32_t first, int32_t end) {
    const formats::Texture& sky = dti.sky();
    const int32_t columns = dti.skyWrapWidth() < sky.width() ? dti.skyWrapWidth() : sky.width();
    Vec3 sum{0.0f, 0.0f, 0.0f};
    for (int32_t row = first; row < end; row++) {
        for (int32_t column = 0; column < columns; column++) {
            const Vec3 linear = linearOf(dti.palette(), sky.indices()[(size_t)row * (size_t)sky.width() + (size_t)column]);
            sum.x += linear.x;
            sum.y += linear.y;
            sum.z += linear.z;
        }
    }
    const float count = (float)((end - first) * columns > 1 ? (end - first) * columns : 1);
    return Vec3{sum.x / count, sum.y / count, sum.z / count};
}

float clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

Vec3 scale(const Vec3& v, float s) {
    return Vec3{v.x * s, v.y * s, v.z * s};
}

} // namespace

Cover arenaCover(const formats::Arena& arena) {
    float floors = 0.0f;
    float ceilings = 0.0f;
    for (int32_t t = 0; t < arena.triangleCount(); t++) {
        Face face;
        if (!faceOf(arena, t, &face)) {
            continue;
        }
        floors += face.normal.z > kHorizontal ? face.area : 0.0f;
        ceilings += face.normal.z < -kHorizontal ? face.area : 0.0f;
    }
    return ceilings > 0.0f && ceilings >= floors * kCoveredShare ? Cover::Covered : Cover::Open;
}

float arenaMeanUp(const formats::Arena& arena) {
    float sum = 0.0f;
    float total = 0.0f;
    for (int32_t t = 0; t < arena.triangleCount(); t++) {
        Face face;
        if (!faceOf(arena, t, &face)) {
            continue;
        }
        sum += face.normal.z * face.area;
        total += face.area;
    }
    return total > 0.0f ? sum / total : 0.0f;
}

float arenaMeanFacing(const formats::Arena& arena, const Vec3& light) {
    const float len = std::sqrt(light.x * light.x + light.y * light.y + light.z * light.z);
    if (len <= 0.0f) {
        return 0.0f;
    }
    const Vec3 towards{-light.x / len, -light.y / len, -light.z / len};
    float sum = 0.0f;
    float total = 0.0f;
    for (int32_t t = 0; t < arena.triangleCount(); t++) {
        Face face;
        if (!faceOf(arena, t, &face)) {
            continue;
        }
        const float dot = face.normal.x * towards.x + face.normal.y * towards.y +
                          face.normal.z * towards.z;
        sum += (dot > 0.0f ? dot : 0.0f) * face.area;
        total += face.area;
    }
    return total > 0.0f ? sum / total : 0.0f;
}

float skyLuminance(const Vec3& colour) {
    return colour.x * 0.2126f + colour.y * 0.7152f + colour.z * 0.0722f;
}

Vec3 skyTint(const Vec3& colour, float saturation) {
    const float luminance = skyLuminance(colour);
    if (luminance < 1e-4f) {
        return Vec3{1.0f, 1.0f, 1.0f};
    }
    const Vec3 hue{colour.x / luminance, colour.y / luminance, colour.z / luminance};
    // Vector3.Lerp(One, hue, s): the oracle's arithmetic is
    // fmaf(from, 1 - t, to * t) (measured in docs/OPEN_ITEMS.md for
    // FrameBlend.Lerp; the same runtime shape applies here).
    return Vec3{std::fmaf(1.0f, 1.0f - saturation, hue.x * saturation),
                std::fmaf(1.0f, 1.0f - saturation, hue.y * saturation),
                std::fmaf(1.0f, 1.0f - saturation, hue.z * saturation)};
}

SkyColours skyLightOf(const formats::Dti& dti) {
    const int32_t height = dti.sky().height();
    int32_t horizon = dti.skyHorizonRow();
    horizon = horizon < 0 ? 0 : (horizon > height ? height : horizon);
    SkyColours out;
    out.sky = horizon > 0 ? skyMean(dti, 0, horizon)
                          : linearOf(dti.palette(), dti.skyTopColor());
    out.ground = horizon < height ? skyMean(dti, horizon, height)
                                  : linearOf(dti.palette(), dti.skyBottomColor());
    return out;
}

Vec3 sunDirection() {
    /* @vfp-exempt R6 — the sun direction is a constant of the enhanced
     * look's rig (it decides every lit face's brightness), and the C#
     * builds it from MathF.Sin/Cos; the fastSin/fastCos LUT is not
     * bit-identical to them, so routing this through the LUT would move
     * the rig off the oracle's bits (pinned in lighting_tests.cpp).
     * Computed once per lighting query, never per frame. measured n/a.
     * Verified 2026-11-14. */
    // EnhancedLook.Sun(-55, 35): Godot (x, y, z) is MDK (x, -z, y).
    const float p = -55.0f * 3.14159265358979323846f / 180.0f;
    const float y = 35.0f * 3.14159265358979323846f / 180.0f;
    const Vec3 godot{-std::cos(p) * std::sin(y), std::sin(p), -std::cos(p) * std::cos(y)};
    return Vec3{godot.x, -godot.z, godot.y};
}

Lighting enhancedLighting(const formats::Dti& dti, const formats::Arena& arena) {
    const SkyColours colours = skyLightOf(dti);
    const Cover cover = arenaCover(arena);
    Vec3 sun{0.0f, 0.0f, 0.0f};
    Vec3 sky;
    Vec3 ground;
    if (cover == Cover::Open) {
        const float strength = kSunWeakest + (kSunStrongest - kSunWeakest) *
                                                 clamp01(skyLuminance(colours.sky) / kBrightSky);
        sun = scale(skyTint(colours.sky, kSunSaturation), strength);
        sky = scale(skyTint(colours.sky, kAmbientSaturation), kOpenAbove);
        ground = scale(skyTint(colours.ground, kAmbientSaturation), kOpenBelow);
    } else {
        const Vec3 tint = skyTint(colours.ground, kCoveredSaturation);
        sky = scale(tint, kCoveredAbove);
        ground = scale(tint, kCoveredBelow);
    }

    const float up = arenaMeanUp(arena) * 0.5f + 0.5f;
    const Vec3 hemi{std::fmaf(ground.x, 1.0f - up, sky.x * up),
                    std::fmaf(ground.y, 1.0f - up, sky.y * up),
                    std::fmaf(ground.z, 1.0f - up, sky.z * up)};
    const float facing = arenaMeanFacing(arena, sunDirection());
    const Vec3 mean{hemi.x + sun.x * facing * kSunlitShare,
                    hemi.y + sun.y * facing * kSunlitShare,
                    hemi.z + sun.z * facing * kSunlitShare};
    const float lum = skyLuminance(mean);
    // MathF.Max(luminance, float.Epsilon): .NET's Epsilon is the smallest
    // denormal (1.4e-45), not FLT_MIN.
    const float exposure = kMeanLight / (lum > 1.401298464e-45f ? lum : 1.401298464e-45f);

    const uint8_t* rgba = dti.palette().rgba();
    const int32_t at = dti.skyBottomColor() * 4;
    Lighting out;
    out.enabled = true;
    out.sun_direction = sunDirection();
    out.sun = Colour{sun.x, sun.y, sun.z, 1.0f};
    out.sky = Colour{sky.x, sky.y, sky.z, 1.0f};
    out.ground = Colour{ground.x, ground.y, ground.z, 1.0f};
    out.exposure = exposure;
    out.shadows = cover == Cover::Open;
    out.shadow_distance = kShadowDistance;
    out.glow = kGlowBloom;
    out.haze_colour = Colour{rgba[(size_t)at] / 255.0f, rgba[(size_t)at + 1] / 255.0f,
                             rgba[(size_t)at + 2] / 255.0f, 1.0f};
    out.haze_density = kHazeDensity;
    return out;
}

} // namespace mdk

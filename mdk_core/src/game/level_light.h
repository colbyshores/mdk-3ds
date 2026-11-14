#pragma once

// ============================================================================
// level_light.h -- the enhanced look's light, ported 1:1 from the C#:
// SkyLight.cs (the sky panorama's mean colours), ArenaShape.cs (an arena's
// cover and mean facing) and EnhancedLook.cs (the arena's Lighting rig).
//
// The rig is shared DATA (SPEC.md 5.3): the Vulkan backend consumes it for
// the enhanced look, the Citro3D backend ignores it. Nothing here renders.
// ============================================================================

#include "render/render_types.h"

namespace mdk {
namespace formats {
class Arena;
class Dti;
} // namespace formats

// Whether an arena is under a roof or under the sky (ArenaShape.Cover).
enum class Cover { Open, Covered };

// An arena's cover: ceilings (normal down past 0.5) over at least half the
// floors' area make a roof (ArenaShape.CoverOf).
Cover arenaCover(const formats::Arena& arena);

// The faces' mean up component, area-weighted (-1 all ceilings, 1 all
// floors; ArenaShape.MeanUp).
float arenaMeanUp(const formats::Arena& arena);

// How much the faces turn to a light shining ALONG `light`, area-weighted
// mean of max(dot(normal, -normalize(light)), 0) (ArenaShape.MeanFacing).
float arenaMeanFacing(const formats::Arena& arena, const Vec3& light);

// The sky panorama's mean linear colours above and below its horizon
// (SkyLight.Of).
struct SkyColours {
    Vec3 sky;
    Vec3 ground;
};
SkyColours skyLightOf(const formats::Dti& dti);

// A colour's hue at luminance 1, blended towards white by (1 - saturation)
// (SkyLight.Tint).
Vec3 skyTint(const Vec3& colour, float saturation);

// Rec. 709 luminance of a linear colour (SkyLight.Luminance).
float skyLuminance(const Vec3& colour);

// The way the enhanced look's sunlight goes, in MDK coordinates
// (EnhancedLook.SunDirection: the Godot light at pitch -55, yaw 35).
Vec3 sunDirection();

// One arena's full lighting rig (EnhancedLook.Lighting). `enabled` is set;
// shadows on exactly when the arena is open.
Lighting enhancedLighting(const formats::Dti& dti, const formats::Arena& arena);

} // namespace mdk

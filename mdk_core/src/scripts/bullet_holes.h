#pragma once

// ============================================================================
// bullet_holes.h — special_130's bullet holes (0x45d140; PORT_GAPS package #16).
//
// A 1:1 port of src/Mdk.Game/Scripts/BulletHoles.cs (itself godot-mdk's
// script_runtime.gd stamp_bullet_hole). The hole sprite (BHOLE, BHOLE2 without
// gore) is copied onto the texture of the face a sniper round hit, centred on
// the hit's texel, its transparent pixels left out. Textures are shared: every
// object using one gets the hole.
//
//   hit point --> nearest face (barycentric weights) --> UV --> hole pixels
//                                                              into frame 0 (wrapping)
//
// THE NEAREST FACE IS THE C#'S APPROXIMATION, KEPT ON PURPOSE. The original keeps
// the face its round's test found; the C# (the oracle) finds the textured face of
// the hit part nearest to the point instead, and this port does exactly that --
// golden/L*_holes_*.txt pin it bit for bit, so "improving" it is a divergence.
//
// WHO CALLS IT. The retail scripts run special_130 from arena-script objects when a
// sniper round hits them (if_hit_part ANY, gosub special_130): L5 MUSE_2 / MUSE_5,
// L6 OLYM_10's XB2, L8 GUNT_10's XGUNTAM. ScriptRuntime::stampBulletHole (the C#'s
// StampBulletHole, in bullet_holes.cpp) puts the hit in the model's frame, finds
// the face and the UV, and calls stamp(). The C#'s rounds stamp nothing on a WALL
// (SniperRounds.MoveStraight): an arena wall never gets a hole, here or there.
//
// COLD: once per sniper hit on such an object.
// ============================================================================

#include <stdint.h>

#include <vector>

#include "formats/model.h"
#include "formats/texture.h"
#include "render/render_types.h"

namespace mdk {
namespace scripts {
namespace bullet_holes {

// C# BulletHoles.Hole / GorelessHole: the sprites archive's images.
extern const char* const kHole;
extern const char* const kGorelessHole;

// C# BulletHoles.Face: a face of a part and the weights of its corners at the point.
struct Face {
    int32_t triangle = 0;
    Vec3 weights{};
};

// The C#'s Func<int, bool> textured: whether a material index of the model has a
// texture (a plain function pointer and its state, AGENTS.md section 10).
typedef bool (*Textured)(void* user, int32_t material);

// C# BulletHoles.Nearest: the face nearest to `point` (model space) among those
// `textured` accepts; false (the C#'s null) when there is none.
bool nearest(const formats::Model::Part& part, const std::vector<formats::Vec3f>& vertices,
             const Vec3& point, Textured textured, void* user, Face* out);

// C# BulletHoles.Stamp: copies `hole` onto frame 0 of `texture` centred on (u, v)
// (texels), wrapping at its edges; index 0 is transparent. False, changing
// nothing, when the texture's pixels are not all there (a stream that could not
// read them: the C# has no such case).
bool stamp(formats::Texture* texture, float u, float v, const formats::Texture& hole);

} // namespace bullet_holes
} // namespace scripts
} // namespace mdk

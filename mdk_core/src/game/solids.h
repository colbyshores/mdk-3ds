#pragma once

// ============================================================================
// solids.h — Kurt against the solid objects of his arena.
//
// A 1:1 port of src/Mdk.Game/Collision/Solids.cs (damp_collide_move 0x465e34,
// object pass; damp_platform_floor 0x41d2c4). Each visible part of an object is
// a box; only the XY of a walk is changed. Platforms (0x100, no 0x10) are stood
// on: a ray from 3 above the feet to 3 below meets the top of one of their boxes.
//
//      part box grown by Kurt's box     segment A ──► B (box centres, XY slabs, z only rejects)
//   ┌───────────────────────┐
//   │                       │ ◄──── A·──────────► B     first hit: B = entry, the rest slid along
//   │                       │                           the face; later hits: B = entry
//   └───────────────────────┘
//
// The C#'s owner is an `object`; every owner the oracle makes is an MdkObject
// (ScriptRuntime.SolidsWithin), so it is that pointer here, opaque to the game
// layer. Objects live in the runtime's deque and are never erased, so the
// pointer stays valid for the runtime's life.
// ============================================================================

#include <vector>

#include "formats/model.h"
#include "game/vec_util.h"

namespace mdk {
namespace scripts {
struct MdkObject;
}

namespace solids {

// A wall, a platform (a wall stood on), or a floor only (standable, else passed
// through) -- C# Solids.Footing.
enum Footing { kWall, kPlatform, kFloor };

// C# Solids.Solid: a part's box in the world, its object, and whether Kurt may
// stand on it.
struct Solid {
    formats::Box box;
    scripts::MdkObject* owner;
    Footing footing;
};

// C# Solids.PlatformReach: a ray from 3 above the feet to 3 below finds the platform.
const float kPlatformReach = 3.0f;

// C# Solids.Walk: the walk from box centre a to b against the boxes (grown by
// Kurt's half extents `half`). Writes the new end and returns true, or returns
// false when no box was hit. Boxes Kurt is already inside are ignored.
bool walk(Vec a, Vec b, Vec half, const std::vector<Solid>& solids, Vec* end);

// C# Solids.Platform: the top of the highest platform under the feet, within
// reach of the ray from 3 above to `bottom` (at least 3 below). False for none.
bool platform(Vec feet, float bottom, const std::vector<Solid>& solids, float* top,
              scripts::MdkObject** owner);

// C# Solids.Touching (the list overload): the owners of the boxes (grown by
// `half` and twice the gap) that hold the box centre `a`, each added once.
void touching(Vec a, Vec half, const std::vector<Solid>& solids,
              std::vector<scripts::MdkObject*>* owners);

} // namespace solids
} // namespace mdk

#pragma once

// ============================================================================
// fall_missile.h — a missile of the fall (fall_missile.gd; launch 0x412568,
// each frame 0x411ee8).
//
// THE C# ORIGINAL is src/Mdk.Game/Fall/FallMissile.cs, ported operation for
// operation. It is pure simulation (the class draws nothing: FallView reads its
// position, velocity and trail), so all of it is here.
//
//   Free (60 ticks) ──► Homing ──(5 units above Kurt)──► Passed (60 ticks) ──► gone
//
// It flies free for 60 ticks, then homes on a point below Kurt; once it is more
// than 5 units above him it has passed (M_PASS): it stops homing, flies on and
// goes 60 ticks later.
//
// THE TRAIL is the C#'s List<Vector3> kept to its last 32 points, here a fixed
// array: the C# appends one point per tick and drops the oldest past 32, which
// is the same last-32 window without growing anything per frame (AGENTS.md §10).
//
// Plain data with non-virtual members: no templates, no exceptions (AGENTS.md §10).
// ============================================================================

#include <stdint.h>

#include "formats/vector_types.h"
#include "util/random.h"

namespace mdk {
namespace fall {

class FallMissile {
public:
    // FallMissile.Event: what happened this frame.
    enum Event { EventNone, EventPassed, EventGone };

    // FallMissile.TrailPoints.
    static constexpr int32_t kTrailPoints = 32;

    // The C# constructor: launched from the origin in a random direction,
    // climbing, with a random aim offset of ±spread. Three draws, in this order:
    // the direction, the offset's x, the offset's y.
    FallMissile(float spread, Random* random);

    // FallMissile.Update: moves the missile for a frame, Kurt being at `kurt`.
    Event update(float dt, int32_t ticks, const formats::Vec3f& kurt);

    // FallMissile.Position / Velocity (public fields in the C#).
    formats::Vec3f position;
    formats::Vec3f velocity;

    // FallMissile.Trail: the last points, oldest first, for the smoke trail.
    int32_t trailCount() const { return trailCount_; }
    const formats::Vec3f* trail() const { return trail_; }
    // FallMissile.TrailHeadings: the velocity at each trail point; the ribbon
    // there keeps the missile's turn (0x4392a4, upstream 04c25852).
    const formats::Vec3f* trailHeadings() const { return headings_; }

private:
    enum Phase { Free, Homing, Passed };

    // FallMissile.Home: steers towards a point below Kurt until 5 units above him.
    Event home(const formats::Vec3f& kurt, float dt);

    // The aim's offset from Kurt (x, y).
    float offsetX_ = 0.0f;
    float offsetY_ = 0.0f;
    formats::Vec3f trail_[kTrailPoints];
    formats::Vec3f headings_[kTrailPoints];
    int32_t trailCount_ = 0;
    Phase phase_ = Free;
    // Ticks left free, or since it passed (a float in the C#).
    float ticks_ = 0.0f;
};

} // namespace fall
} // namespace mdk

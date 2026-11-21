#pragma once

// ============================================================================
// air_strike.h — Bones' air strike (sniper ammo type 5, SW_BONES; 0x4641ac,
// 0x43fa0c, 0x43deac). A 1:1 port of src/Mdk.Game/Scripts/AirStrike.cs (itself
// a port of godot-mdk's air_strike.gd, docs/gameplay.md "Sniper mode").
//
//   start (50 behind Kurt, arena top + 30) ─► before ─► over the target (+32) ─► after ─► end
//                                             teeth: 9 X_TOOTH grenades, one every 15 units
//   last two levels: one strike only, diving into the target (450 damage within 80)
//
// The point in the crosshair (within 5000 units, with open sky 1000 units above
// it) is the target. X_STRIKE flies a 5-point curve at 150 units/s.
//
// The runtime owns one (ScriptRuntime::airStrike(), the C#'s
// ScriptRuntime.AirStrike), updates it once a tick after the sniper rounds
// (ScriptRuntime.cs:957), and Kurt's clip calls it through fireSniper when the
// strike is the selected ammo (ScriptRuntime.cs:1284). Call plays the
// full-screen strike first (ScriptRuntime::playStrikeScene, strike_scene.h).
// The frontends' sniper overlay asks findTarget for its strike-target bit
// (SniperView.Aim, SniperView.cs:81-88).
//
// EXACTNESS. Every float operation is the C#'s, in its order; the System.Numerics
// spellings are the oracle host's (.NET 10): Lerp is fused (std::fma, as
// sniper_rounds.cpp's), Dot/Length sum left to right, Normalize divides. The
// heading is libm atan2 (the strike's yaw is gameplay: the teeth spawn facing
// it). golden/L*_strike_*.txt pins it bit for bit. Cold path: one strike, a
// few calls per tick.
// ============================================================================

#include <math.h>
#include <stdint.h>

#include <vector>

#include "render/render_types.h"

namespace mdk {
namespace scripts {

class ScriptRuntime;
struct MdkObject;

// C# BezierPath (AirStrike.cs:244): a path through points with Bézier handles of
// a quarter of the neighbours' distance (Godot's Curve3D as the Godot port builds
// it), sampled by length.
class BezierPath {
public:
    BezierPath() {}
    // C# BezierPath(points).
    explicit BezierPath(const std::vector<Vec3>& points);

    // C# Length: the last cumulative length.
    float length() const { return lengths_.back(); }
    // C# Sample: the point `offset` along the path.
    Vec3 sample(float offset) const;
    // C# ClosestOffset: the offset of the path's point nearest to `point`.
    float closestOffset(const Vec3& point) const;

private:
    static Vec3 cubic(const Vec3& p0, const Vec3& p1, const Vec3& p2, const Vec3& p3, float t);

    std::vector<Vec3> points_;
    std::vector<float> lengths_;
};

class AirStrike {
public:
    explicit AirStrike(ScriptRuntime& runtime) : runtime_(runtime) {}
    AirStrike(const AirStrike&) = delete;
    AirStrike& operator=(const AirStrike&) = delete;

    // C# UsedUp: the one strike of the last two levels was used (0x57440b).
    bool usedUp = false;

    // C# IsActive: a strike is out (its X_STRIKE alive).
    bool isActive() const;

    // C# FindTarget (0x4641ac): the point in the view from `eye` a strike can hit --
    // within 5000 units, with open sky 1000 units above it. False if there's none.
    bool findTarget(const Vec3& eye, const Vec3& forward, Vec3* target) const;

    // C# Call: calls the strike on the point in the view. False (and Kurt blows a
    // raspberry) without a target, while a strike is out or when it's used up.
    bool call(const Vec3& eye, const Vec3& forward);

    // C# Update: moves the strike by `ticks`.
    void update(float ticks);

    // The C#'s private state, read by the golden's dump (tick1 dump strike).
    MdkObject* strike() const { return strike_; }
    bool hasCurve() const { return hasCurve_; }
    float curveLength() const { return curve_.length(); }
    float offset() const { return offset_; }
    float targetOffset() const { return targetOffset_; }
    float diveOffset() const { return diveOffset_; }
    int32_t teeth() const { return teeth_; }
    bool diving() const { return diving_; }

private:
    // C# Flight: over the target dropping teeth, or diving into it.
    enum Flight { Over, Dive };

    void start(const Vec3& target, Flight flight);
    Vec3 clearPoint(const Vec3& from, const Vec3& over, const Vec3& target) const;
    void crash(MdkObject* strike);
    void dropTooth(MdkObject* strike);

    ScriptRuntime& runtime_;
    // The C#'s _strike: a pointer into the runtime's deque (stable, never vacated).
    MdkObject* strike_ = nullptr;
    // The C#'s nullable _curve.
    BezierPath curve_;
    bool hasCurve_ = false;
    float offset_ = 0.0f;
    float targetOffset_ = 0.0f;
    float diveOffset_ = INFINITY;  // C# float.PositiveInfinity
    int32_t teeth_ = 0;
    bool diving_ = false;
};

} // namespace scripts
} // namespace mdk

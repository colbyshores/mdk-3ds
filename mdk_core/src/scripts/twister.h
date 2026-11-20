#pragma once

// ============================================================================
// twister.h — a twister of the tornado item (0x40741c) and its ribbon.
//
// A port of src/Mdk.Game/Scripts/Twister.cs and the state half of Ribbon.cs.
// A twister spirals out of the tornado for 2 seconds, then splits into one
// twister per object of the arena (0x407774), each chasing its object (0x407974)
// for 5 seconds, bouncing off walls and hurting whatever it is inside by 2
// health per tick:
//
//   spiral (720°/s, 15 out and up at 1440°) ──► split ──► chase: v = 0.9 v + 20 towards the target
//
// Its ribbon keeps the last 5 positions (trail 0x4390ec/0x4392a4); the ribbon's
// triangles (Ribbon.Triangles, drawn by 0x439690 with WMIBTEX) are a renderer
// slice that is not ported, so only the ring the draw would read is kept here.
//
// The twisters live in ScriptRuntime (the C#'s Items._twisters), by value: a
// twister holds no pointer into its own list, only its runtime and its target,
// both of which outlive it (objects are deque slots that never move).
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

#include "render/render_types.h"

namespace mdk {
namespace scripts {

class ScriptRuntime;
struct MdkObject;

// Ribbon.cs's state: a ring of the last kLength positions, oldest first.
class Ribbon {
public:
    static const int32_t kLength = 5;
    // Ribbon.Push: a tick's position; the oldest goes once 5 are kept.
    void push(const Vec3& centre);
    int32_t count() const { return count_; }
    // The i-th kept position, oldest first (Ribbon.Centres).
    const Vec3& centre(int32_t i) const;

private:
    Vec3 centres_[kLength] = {};
    int32_t first_ = 0;
    int32_t count_ = 0;
};

class Twister {
public:
    Twister(ScriptRuntime* runtime, const std::string& arena, const Vec3& origin, float yaw);

    // Twister.Tick: one tick; `spawned` gets the twisters a split makes (the C# appends
    // them to the live list). Returns false when it is gone.
    bool tick(std::vector<Twister>* spawned);

    const std::string& arena() const { return arena_; }
    const Vec3& position() const { return position_; }
    const Ribbon& ribbon() const { return ribbon_; }

private:
    void spiral(std::vector<Twister>* spawned);
    void hurtObjects();
    void split(std::vector<Twister>* spawned);
    void moveTo(const Vec3& next);

    ScriptRuntime* runtime_;
    std::string arena_;
    Vec3 position_;
    Vec3 origin_;
    float yaw_;
    float angle_ = 0.0f;
    Vec3 velocity_{};
    MdkObject* target_ = nullptr;
    bool chasing_ = false;
    int32_t life_ = 150;  // Twister.Lifetime
    Ribbon ribbon_;
};

} // namespace scripts
} // namespace mdk

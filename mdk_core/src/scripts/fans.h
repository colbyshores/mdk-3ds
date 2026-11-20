#pragma once

// ============================================================================
// fans.h -- fans (updrafts) of the arenas, ported 1:1 from
// src/Mdk.Game/Scripts/Fans.cs (opcodes 142/143/190).
//
// Created on the arenas' type-7 hotspots by fan_create, they lift Kurt,
// objects and pieces inside their box. Each tick, with a chance of 1 in
// 8, a still fire spark appears just above the bottom of the box for the
// updraft to lift (through the spark callback; the effects slice owns
// what a spark looks like).
//
//        z1 + 5 ┌───────┐  the box reaches 5 units above the top
//           z1  │ ↑ ↑ ↑ │  type 6: full strength up to 5 below z1, then fading; wobble near z1
//               │ ↑ ↑ ↑ │
//     z0 - 0.5  └───────┘  sparks at z0 + 0.25
//
// The updraft query's consumers (Kurt's and the objects' motion) ask
// through query(); who asks with which mask is the caller's business.
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

#include "render/render_types.h"
#include "util/random.h"

namespace mdk {

namespace formats {
class Dti;
struct ArenaEntry;
} // namespace formats

namespace scripts {

class Fans {
public:
    // Who asks: Kurt, an object, or a spark or piece; a fan acts on
    // those whose bit is in its mask.
    static const int32_t kMaskKurt = 1;
    static const int32_t kMaskObjects = 2;
    static const int32_t kMaskEffects = 8;

    enum class Power {
        Off,
        On,
    };

    // A fan's still spark: (arena, point). Null: no sparks drawn.
    typedef void (*SparkFn)(void* user, const std::string& arena, const Vec3& point);
    // Whether an arena's objects run (only their fans let out sparks).
    typedef bool (*IsLiveFn)(void* user, const std::string& arena);

    Fans() = default;
    Fans(const formats::Dti* dti, Random* rng) : dti_(dti), rng_(rng) {}

    void setSpark(SparkFn fn, void* user) {
        spark_ = fn;
        sparkUser_ = user;
    }
    void setIsLive(IsLiveFn fn, void* user) {
        isLive_ = fn;
        isLiveUser_ = user;
    }

    // fan_create: a fan on the arena's type-7 hotspot `hotspot`. With
    // type 6, `strength` is the time to rise through the box.
    void create(const std::string& arena, int32_t hotspot, const std::string& name, int32_t param,
                int32_t type, float strength);

    // fan_remove.
    void remove(const std::string& arena, const std::string& name);

    // fan_enable: sets or clears bit 0 of the fan's mask.
    void enable(const std::string& arena, const std::string& name, Power power);

    // Each tick: the fans of the live arenas let out their sparks
    // (enabled or not).
    void update();

    // The vertical speed (u/s) for something at `point` going up or down
    // at `vz`, or NaN when no fan of the arena holds it (updraft_query).
    float query(const std::string& arena, const Vec3& point, float vz, int32_t mask, float dt);

    int32_t count() const { return (int32_t)fans_.size(); }
    // The shared wobble's current value (Fans._wobble), for the goldens.
    float wobbleNow() const { return wobble_; }

    // One fan's record, for tests and saves: false when there is none.
    bool fan(const std::string& arena, const std::string& name, int32_t* maskOut,
             float* strengthOut) const;

private:
    struct Fan {
        std::string name;
        std::string arena;
        int32_t param = 0;
        int32_t type = 0;
        float strength = 0.0f;
        int32_t mask = -1;
        Vec3 boxStart{};
        Vec3 boxEnd{};
    };

    static bool holds(const Fan& fan, const Vec3& point);
    float lift(Fan& fan, float z, float vz, float dt);
    float wobble();

    const formats::Dti* dti_ = nullptr;
    Random* rng_ = nullptr;
    SparkFn spark_ = nullptr;
    void* sparkUser_ = nullptr;
    IsLiveFn isLive_ = nullptr;
    void* isLiveUser_ = nullptr;
    std::vector<Fan> fans_;
    float wobble_ = 0.0f;
    bool wobbleUp_ = false;
};

} // namespace scripts
} // namespace mdk

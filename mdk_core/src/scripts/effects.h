#pragma once

// ============================================================================
// effects.h -- sprite effects of the arenas, ported 1:1 from
// src/Mdk.Game/Scripts/Effects.cs (opcodes 128/132/136): slime bleeding
// from wounds (attach_effect), slime drops (spawn_debris), bubbles
// (spawn_effect) and the smoke trails of sparks and pieces.
//
// Sprites are animated textures drawn as billboards width x scale / 256
// units wide; frame F - 1 - (life x speed mod F), so they play forwards
// as their life runs out. The billboard draw is the view's business
// (EffectsView); this class is the pool and its motion:
//
//   wound --(bursts)--> drops --> fall, bounce --> gone
//   bubble --> rise, grow --> pop (BUBB_POP) --> gone
//   trail --> fall --> gone (11 ticks)
// ============================================================================

#include <stdint.h>

#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "render/render_types.h"
#include "scripts/debris.h"
#include "util/random.h"

namespace mdk {
namespace scripts {

class MdkObject;

class Effects {
public:
    enum class Kind {
        Wound,
        Drop,
        Bubble,
        Pop,
        Trail,
    };

    // Most effects alive at once.
    static const int32_t kMaxEffects = 96;
    // Sprites are scale / 256 units per texel.
    static const float kTexelsPerUnit;

    struct Effect {
        Kind kind = Kind::Drop;
        // The arena whose pool holds it, and its sprite's texture there.
        std::string arena;
        std::string texture;
        int32_t frames = 1;
        Vec3 position{};
        // Units per tick.
        Vec3 velocity{};
        float scale = 1.0f;
        // Frames per tick.
        float speed = 0.5f;
        float life = 0.0f;
        // Wounds: the object, the point it bleeds at, the point the
        // drops fly towards, the burst chance and time, the jitter.
        MdkObject* owner = nullptr;
        int32_t slot = 0;
        int32_t towards = 0;
        int32_t intensity = 0x7fff;
        float burst = 0.0f;
        Vec3 jitter{};
        bool done = false;

        // The sprite's frame now.
        int32_t frame() const {
            const int32_t f = frames - 1 - (int32_t)(life * speed) % (frames ? frames : 1);
            return f < 0 ? 0 : (f > frames - 1 ? frames - 1 : f);
        }
    };

    // The frame count of an arena's texture, 0 when it has none.
    typedef int32_t (*FrameCountFn)(void* user, const std::string& arena,
                                    const std::string& texture);
    // The arena's nearest hit along a segment, false when it hits nothing.
    typedef bool (*RayFn)(void* user, const std::string& arena, const Vec3& from, const Vec3& to,
                          DebrisRayHit* out);

    Effects() = default;
    explicit Effects(Random* rng) : rng_(rng) {}

    void setFrameCount(FrameCountFn fn, void* user) {
        frameCount_ = fn;
        frameCountUser_ = user;
    }
    void setRay(RayFn fn, void* user) {
        ray_ = fn;
        rayUser_ = user;
    }

    // Starts the bleeding of a wound at the object's reference point.
    void attach(MdkObject* obj, int32_t slot, int32_t towards);

    // Stops the bleeding at a reference point (attach_effect "OFF").
    void detach(MdkObject* obj, int32_t slot);

    // A slime drop: SL_MED or SL_SMA, playing once over 4 seconds.
    void spawnDrop(const std::string& arena, const Vec3& point, const Vec3& velocity, float scale);

    // A puff of the smoke trail of sparks and pieces.
    void spawnTrail(const std::string& arena, const Vec3& point);

    // A bubble: rising, growing and wobbling, then popping.
    void spawnBubble(const std::string& arena, const Vec3& point);

    // Whether an arena has any effect.
    bool hasEffects(const std::string& arena) const;

    // Updates the effects by `ticks`.
    void update(float ticks);

    const std::deque<Effect>& all() const { return effects_; }
    int32_t count() const { return (int32_t)effects_.size(); }

private:
    Effect* create(const std::string& arena, const std::string& texture, Kind kind,
                   const Vec3& point, float scale, float speed);
    void remove(Effect* effect);
    void updateWound(Effect* effect, float ticks);
    void updateBubble(Effect* effect, float ticks);
    bool move(Effect* effect, float ticks);
    int32_t randomHalf();

    Random* rng_ = nullptr;
    FrameCountFn frameCount_ = nullptr;
    void* frameCountUser_ = nullptr;
    RayFn ray_ = nullptr;
    void* rayUser_ = nullptr;
    std::deque<Effect> effects_;
    std::map<std::pair<MdkObject*, int32_t>, Effect*> wounds_;
};

} // namespace scripts
} // namespace mdk

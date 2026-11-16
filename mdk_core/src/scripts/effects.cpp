// ============================================================================
// effects.cpp -- see effects.h.
// ============================================================================

#include "scripts/effects.h"

#include <cmath>

#include "scripts/object.h"

namespace mdk {
namespace scripts {

namespace {

const float kGravity = Debris::kGravity;
const float kBounce = 1.4f;
const int32_t kBounceTicks = 20;
// A wound spits drops for this many ticks at a time, at half a unit per tick.
const float kBurstTicks = 30.0f;
const float kBurstSpeed = 0.5f;
const float kWoundDropScale = 4.0f;
// A wound's chance of a burst (out of 32768), weaker by 128-255 every loop.
const int32_t kFullIntensity = 0x7fff;
const int32_t kIntensityLossMin = 0x80;
const int32_t kIntensityLossMask = 0x7f;
const int32_t kRandRange = 32768;
const int32_t kRandHalf = 0x4000;
const float kJitterScale = 1.0f / 65536.0f;
// Bubbles: scale 10 +- 3.3, 64 ticks, growing 4 a second, drifting
// sideways and rising at 0.5 u/s^2.
const float kBubbleScale = 10.0f;
const float kBubbleScaleSpread = 0.0002f;
const float kBubbleLife = 64.0f;
const float kBubbleGrowth = 4.0f;
const float kBubbleDrift = 1e-4f;
const float kBubbleRise = 0.5f;
// Trails: scale 4, one frame per tick for 11 ticks.
const float kTrailScale = 4.0f;
const float kTrailLife = 11.0f;
const float kTicksPerSecond = 30.0f;
const float kContactBackoff = 0.01f;

const char* const kWoundTexture = "SL_BIG";
const char* const kBigDrop = "SL_MED";
const char* const kSmallDrop = "SL_SMA";
const char* const kTrailTexture = "TRAIL";
const char* const kBubbleTexture = "BUBB";
const char* const kPopTexture = "BUBB_POP";

Vec3 add(const Vec3& a, const Vec3& b) { return Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 sub(const Vec3& a, const Vec3& b) { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 scaled(const Vec3& a, float s) { return Vec3{a.x * s, a.y * s, a.z * s}; }
float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float length(const Vec3& a) { return std::sqrt(dot(a, a)); }
Vec3 normalized(const Vec3& a) {
    const float len = length(a);
    return len > 0.0f ? scaled(a, 1.0f / len) : Vec3{0.0f, 0.0f, 0.0f};
}
bool isZero(const Vec3& a) { return a.x == 0.0f && a.y == 0.0f && a.z == 0.0f; }

} // namespace

const float Effects::kTexelsPerUnit = 256.0f;

void Effects::attach(MdkObject* obj, int32_t slot, int32_t towards) {
    const float kWoundScale = 10.0f;
    if (wounds_.find(std::pair<MdkObject*, int32_t>(obj, slot)) != wounds_.end()) {
        return;
    }
    Effect* effect = create(obj->arena, kWoundTexture, Kind::Wound, obj->reference_point(slot),
                            kWoundScale, 0.5f);
    if (!effect) {
        return;
    }
    effect->owner = obj;
    effect->slot = slot;
    effect->towards = towards;
    effect->life = (float)effect->frames * 2.0f - 1.0f;
    wounds_[std::pair<MdkObject*, int32_t>(obj, slot)] = effect;
}

void Effects::detach(MdkObject* obj, int32_t slot) {
    std::map<std::pair<MdkObject*, int32_t>, Effect*>::iterator found =
        wounds_.find(std::pair<MdkObject*, int32_t>(obj, slot));
    if (found != wounds_.end()) {
        remove(found->second);
    }
}

void Effects::spawnDrop(const std::string& arena, const Vec3& point, const Vec3& velocity,
                        float scale) {
    Effect* effect = create(arena, rng_->next(2) == 0 ? kBigDrop : kSmallDrop, Kind::Drop, point,
                            scale, 0.25f);
    if (!effect) {
        return;
    }
    effect->velocity = velocity;
    effect->life = (float)effect->frames * 4.0f - 1.0f;
}

void Effects::spawnTrail(const std::string& arena, const Vec3& point) {
    Effect* effect = create(arena, kTrailTexture, Kind::Trail, point, kTrailScale, 1.0f);
    if (effect) {
        effect->life = kTrailLife;
    }
}

void Effects::spawnBubble(const std::string& arena, const Vec3& point) {
    const float scale = (float)(rng_->next(kRandRange) - kRandHalf) * kBubbleScaleSpread +
                        kBubbleScale;
    Effect* effect = create(arena, kBubbleTexture, Kind::Bubble, point, scale, 0.5f);
    if (effect) {
        effect->life = kBubbleLife;
    }
}

bool Effects::hasEffects(const std::string& arena) const {
    for (size_t i = 0; i < effects_.size(); i++) {
        if (effects_[i].arena == arena) {
            return true;
        }
    }
    return false;
}

Effects::Effect* Effects::create(const std::string& arena, const std::string& texture, Kind kind,
                                 const Vec3& point, float scale, float speed) {
    if ((int32_t)effects_.size() >= kMaxEffects) {
        return nullptr;
    }
    const int32_t frames = frameCount_ ? frameCount_(frameCountUser_, arena, texture) : 0;
    if (frames == 0) {
        return nullptr;
    }
    Effect effect;
    effect.kind = kind;
    effect.arena = arena;
    effect.texture = texture;
    effect.frames = frames;
    effect.position = point;
    effect.scale = scale;
    effect.speed = speed;
    effects_.push_back(effect);
    return &effects_.back();
}

void Effects::remove(Effect* effect) {
    if (effect->owner) {
        std::map<std::pair<MdkObject*, int32_t>, Effect*>::iterator found =
            wounds_.find(std::pair<MdkObject*, int32_t>(effect->owner, effect->slot));
        if (found != wounds_.end() && found->second == effect) {
            wounds_.erase(found);
        }
    }
    for (size_t i = 0; i < effects_.size(); i++) {
        if (&effects_[i] == effect) {
            effects_.erase(effects_.begin() + (long)i);
            return;
        }
    }
}

void Effects::update(float ticks) {
    // A copy of the current effects (spawns during the update join next
    // tick, as the C#'s ListCopy gives it).
    std::vector<Effect*> copy;
    copy.reserve(effects_.size());
    for (size_t i = 0; i < effects_.size(); i++) {
        copy.push_back(&effects_[i]);
    }
    for (size_t i = 0; i < copy.size(); i++) {
        Effect* effect = copy[i];
        // The effect may have been removed by an earlier update in this
        // same pass (a wound's drop cannot remove it, but be exact: the
        // C# iterates its own copy of live references; a removed effect
        // is skipped there by removal from _effects only -- it still
        // updates this tick. Mirror that: update regardless.)
        switch (effect->kind) {
            case Kind::Wound:
                updateWound(effect, ticks);
                break;
            case Kind::Drop:
            case Kind::Trail:
                move(effect, ticks);
                effect->life -= ticks;
                break;
            case Kind::Bubble:
                updateBubble(effect, ticks);
                break;
            case Kind::Pop:
                effect->life -= ticks;
                break;
        }
        if (effect->done || (effect->life <= 0.0f && effect->kind != Kind::Wound)) {
            // Still in the pool? (It is, unless it removed itself.)
            for (size_t j = 0; j < effects_.size(); j++) {
                if (&effects_[j] == effect) {
                    remove(effect);
                    break;
                }
            }
        }
    }
}

void Effects::updateBubble(Effect* effect, float ticks) {
    const float dt = ticks / kTicksPerSecond;
    const Vec3 drift{(float)(rng_->next(kRandRange) - kRandHalf),
                     (float)(rng_->next(kRandRange) - kRandHalf), 0.0f};
    effect->velocity = add(effect->velocity,
                           scaled(add(scaled(drift, kBubbleDrift), Vec3{0.0f, 0.0f, kBubbleRise}),
                                  dt));
    effect->scale += kBubbleGrowth * dt;
    if (move(effect, ticks)) {
        effect->life = 0.0f;
    }
    effect->life -= ticks;
    if (effect->life >= 1.0f) {
        return;
    }
    const int32_t frames = frameCount_ ? frameCount_(frameCountUser_, effect->arena, kPopTexture)
                                       : 0;
    if (frames == 0) {
        return;
    }
    effect->kind = Kind::Pop;
    effect->texture = kPopTexture;
    effect->frames = frames;
    effect->velocity = Vec3{0.0f, 0.0f, 0.0f};
    effect->life = (float)frames * 2.0f - 1.0f;
}

void Effects::updateWound(Effect* effect, float ticks) {
    MdkObject* obj = effect->owner;
    if (!obj || obj->dead) {
        effect->done = true;
        return;
    }
    effect->life -= ticks;
    if (effect->life < 0.0f) {
        effect->life = (float)effect->frames * 2.0f - 1.0f;
        const int32_t loss = (rng_->next(kRandRange) & kIntensityLossMask) + kIntensityLossMin;
        effect->intensity = effect->intensity - loss > 0 ? effect->intensity - loss : 0;
    }
    effect->position = obj->reference_point(effect->slot);
    if (effect->burst < 1.0f && rng_->next(kRandRange) < effect->intensity) {
        effect->burst = kBurstTicks;
        effect->jitter = scaled(Vec3{(float)randomHalf(), (float)randomHalf(), (float)randomHalf()},
                                kJitterScale);
    }
    if (effect->burst <= 0.0f) {
        return;
    }
    effect->burst -= ticks;
    const Vec3 towards = sub(obj->reference_point(effect->towards), effect->position);
    const Vec3 direction = isZero(towards) ? Vec3{0.0f, 0.0f, 0.0f} : normalized(towards);
    spawnDrop(obj->arena, effect->position, scaled(add(direction, effect->jitter), kBurstSpeed),
              kWoundDropScale);
}

int32_t Effects::randomHalf() { return rng_->next(kRandRange) - kRandHalf; }

bool Effects::move(Effect* effect, float ticks) {
    const Vec3 motion = scaled(effect->velocity, ticks);
    if (isZero(motion)) {
        return false;
    }
    const bool falls = effect->kind == Kind::Drop || effect->kind == Kind::Trail;
    DebrisRayHit hit;
    const bool hitSomething =
        ray_ && ray_(rayUser_, effect->arena, effect->position, add(effect->position, motion),
                     &hit);
    if (!hitSomething) {
        effect->position = add(effect->position, motion);
        if (falls) {
            effect->velocity.z -= kGravity * ticks;
        }
        return false;
    }
    effect->position = sub(hit.point, scaled(normalized(motion), kContactBackoff));
    if (!falls) {
        effect->velocity = Vec3{0.0f, 0.0f, 0.0f};
        return true;
    }
    const float into = dot(effect->velocity, hit.normal);
    effect->velocity = sub(effect->velocity, scaled(hit.normal, into * kBounce));
    effect->life -= (float)kBounceTicks;
    return true;
}

} // namespace scripts
} // namespace mdk

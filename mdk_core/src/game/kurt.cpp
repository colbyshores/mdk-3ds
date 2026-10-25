// kurt.cpp — the Hurt/Damage half of Kurt.Combat.cs and the slot count of
// Inventory.cs. See kurt.h for the slice's scope.

#include "game/kurt.h"

#include <algorithm>

namespace mdk {

const float Kurt::kHurtFlashPerDamage = 25.0f;
const float Kurt::kHurtFlashMin = 75.0f;
const float Kurt::kHurtFlashMax = 180.0f;

void Kurt::hurt(int32_t damage) {
    if (health_ == 0) {
        return;
    }

    if (invulnerable_ > 0.0f || state_ == Dead || state_ == Knocked || state_ == GetUp ||
        state_ == HardLand) {
        knockDamage_ = 0.0f;
        return;
    }

    this->damage(damage);
}

void Kurt::damage(int32_t damage) {
    if (mortality_ == God) {
        return;
    }

    damage = scaled(damage);
    if (damage > 0) {
        hurtFlash_ = std::min(std::max(hurtFlash_ + (float)damage * kHurtFlashPerDamage, kHurtFlashMin),
                              kHurtFlashMax);
    }

    health_ = std::max(health_ - damage, (int32_t)0);
    knockDamage_ += (float)damage;
}

int32_t Kurt::scaled(int32_t damage) const {
    switch (difficulty_) {
        case Easy:
            return std::max(damage * kEasyNumerator / kEasyDenominator, (int32_t)1);
        case Hard:
            return damage * kHardFactor;
        default:
            return damage;
    }
}

int32_t Kurt::countItem(Item item) const {
    int32_t count = 0;
    for (int32_t i = 0; i < slotCount_; i++) {
        if (slots_[i].item == item) {
            count += slots_[i].count;
        }
    }
    return count;
}

} // namespace mdk

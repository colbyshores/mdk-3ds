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

void Kurt::setHealth(int32_t health) {
    // C# SetHealth: Math.Max(health, 0).
    health_ = health > 0 ? health : 0;
}

void Kurt::fadeFlashes(float delta) {
    // C# Kurt.FadeFlashes (Kurt.Combat.cs:204-205), both lines verbatim: the red flash
    // and the white one fall by the same rate and floor at zero. The floor is not
    // decoration -- HudView.DrawFlashes tests `> 0f` before it fills the screen, so a
    // field that went negative would still be a flash the HUD would draw (clamped, but
    // drawn) and opcode 215's MathF.Max would keep adding to a negative base.
    hurtFlash_ = std::max(hurtFlash_ - kFlashFade * delta, 0.0f);
    whiteFlash_ = std::max(whiteFlash_ - kFlashFade * delta, 0.0f);
}

bool Kurt::fadeSkullIn(float delta) {
    // C# Kurt.UpdateDeath's last two statements (Kurt.Combat.cs:250-253). No clamp on
    // the way up: the oracle lets HurtFlash pass 255 and stop there only because the
    // guard at :238 returns before the next rise, and the HUD clamps the alpha itself
    // (Math.Clamp(HurtFlash/FlashMax, 0, 1)). Keeping the overshoot is what makes the
    // guard's strict `>` the thing that ends the death beat.
    hurtFlash_ += kSkullFade * delta;
    return hurtFlash_ > kSkullEnd;
}

void Kurt::declineKnockDamage(float delta) {
    // C# Kurt.UpdateKnockDamage's decline tail (Kurt.Combat.cs:230-231), in the C#'s
    // order: Math.Clamp first, then the drain, so a counter a script set above the
    // threshold loses the excess on the same step it loses the drain. Math.Clamp(x, 0, 5)
    // is min(max(x, 0), 5) for every value but a NaN, and no writer here can produce one.
    knockDamage_ = std::min(std::max(knockDamage_, 0.0f), kKnockdownDamage);
    knockDamage_ = std::max(knockDamage_ - kKnockdownDrain * delta, 0.0f);
}

void Kurt::declineInvulnerability(float delta) {
    // C# Kurt.cs:139, verbatim: MathF.Max(Invulnerable - delta, 0f). The floor is not
    // decoration -- the field is read as `Invulnerable > 0f` by hurt() and by opcode
    // 240, so a timer that drains past zero and keeps going is invisible there, but it
    // is what opcode 244's accumulate arm would then add to, and a negative timer is
    // not the oracle's value for a golden to print. Branchless, like the knock drain.
    invulnerable_ = std::max(invulnerable_ - delta, 0.0f);
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

int32_t Kurt::ammo(int32_t type) const {
    if (type < 1 || type > kAmmoTypes) {
        return 0;
    }
    return ammo_[type - 1];
}

int32_t Kurt::superChainGun() const {
    return superChainGun_;
}

void Kurt::setAmmo(int32_t type, int32_t count) {
    if (type < 1 || type > kAmmoTypes) {
        return;
    }
    ammo_[type - 1] = count;
}

void Kurt::setSuperChainGun(int32_t ticks) {
    superChainGun_ = ticks;
}

} // namespace mdk

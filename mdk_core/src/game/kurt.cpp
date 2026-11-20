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

void Kurt::tickSuperChainGun(int32_t ticks) {
    // C# Inventory.TickSuperChainGun (Inventory.cs:290-308), in its order: nothing while
    // the gun has no time, the time runs, and the step that takes it to 0 or below sets it
    // to exactly 0 and drops the SuperChainGun slot (the first one, FindIndex) through
    // RemoveSlot, which keeps the selection inside the slots.
    if (superChainGun_ <= 0) {
        return;
    }
    superChainGun_ -= ticks;
    if (superChainGun_ > 0) {
        return;
    }
    superChainGun_ = 0;
    for (int32_t i = 0; i < slotCount_; i++) {
        if (slots_[i].item == SuperChainGun) {
            removeSlot(i);
            return;
        }
    }
}

namespace {

// Inventory.ItemPickups (Inventory.cs:24-35): the item pickups by model, in Item order
// (index = the Item value; "" for None).
const char* const kItemPickups[] = {"",        "SW_DUMMY", "SW_INTER", "SW_TWIST", "SW_THUMP",
                                    "SW_HBOMB", "SW_GATT",  "SW_KEY",   "SW_SEAL",  "SW_SBONE"};
const int32_t kItemPickupCount = 10;

// Inventory.InstantPickups (Inventory.cs:38, table 0x4920e0): sniper ammo (0-4), health
// (5-9), the easter eggs.
const char* const kInstantPickups[] = {"SW_HOME", "SW_SGREN", "SW_HGREN", "SW_LGREN",
                                       "SW_BONES", "SW_H25",  "SW_H50",   "SW_H100",
                                       "SW_H150", "SW_H01",   "SW_EWJ",   "BONEFLC"};
const int32_t kInstantPickupCount = 12;

const int32_t kLastAmmoPickup = 4;
const int32_t kBonesPickup = 4;
const int32_t kHealth25 = 5;
const int32_t kHealth50 = 6;
const int32_t kHealth100 = 7;
const int32_t kHealth150 = 8;
const int32_t kHealth01 = 9;
const int32_t kGroovy = 10;
const int32_t kBoneFlc = 11;

// Sniper ammo per pickup 0-4 (halved on hard when above 1); per difficulty, the grenades
// a pickup gives and the super chain gun's ticks (Inventory.cs:52-58).
const int32_t kAmmoPerPickup[5] = {8, 3, 3, 8, 1};
const int32_t kGrenades[3] = {5, 3, 1};
const int32_t kSuperChainGunTicks[3] = {400, 200, 100};
const int32_t kSmallHealth = 10;
const int32_t kBigHealth = 50;
const int32_t kSuperHealth = 150;

const char kCollectSound[] = "COLLECT";
const char kBombSound[] = "WMIB";
const char kBonesSound[] = "BONES";
const char kHealthSound[] = "APPLE";

int32_t indexOf(const char* const* table, int32_t count, const std::string& name) {
    for (int32_t i = 0; i < count; i++) {
        if (name == table[i]) {
            return i;
        }
    }
    return -1;
}

// Inventory.Heal (Inventory.cs:236): up to 100; above 100 (SW_H150) it stays.
int32_t heal(int32_t health, int32_t amount) {
    return health < Kurt::kMaxHealth ? std::min(health + amount, Kurt::kMaxHealth) : health;
}

} // namespace

bool Kurt::isPickup(const std::string& model) {
    // ItemPickups.ContainsKey skips the None entry: "" is no pickup.
    return indexOf(kInstantPickups, kInstantPickupCount, model) >= 0 ||
           (!model.empty() && indexOf(kItemPickups, kItemPickupCount, model) > 0);
}

const char* Kurt::collect(const std::string& pickup) {
    // C# Inventory.Collect (Inventory.cs:107-130); Kurt.Collect's health round trip is
    // the direct write of health_ inside useInstant.
    const int32_t index = indexOf(kInstantPickups, kInstantPickupCount, pickup);
    if (index >= 0) {
        // SW_H01 and BONEFLC show no message.
        if (index != kHealth01 && index != kBoneFlc) {
            pickedUp_.push_back(pickup);
        }
        return useInstant(index);
    }

    const int32_t item = pickup.empty() ? -1 : indexOf(kItemPickups, kItemPickupCount, pickup);
    if (item <= 0) {
        return "";
    }

    const char* sound = collectItem((Item)item);
    if (sound[0] != '\0') {
        pickedUp_.push_back(pickup);
    }
    return sound;
}

const char* Kurt::collectItem(Item item) {
    // C# Inventory.CollectItem (Inventory.cs:134-161). Grenades and the super chain gun
    // stack in their slot (FindIndex: the first one).
    if (item == Grenade || item == SuperChainGun) {
        for (int32_t i = 0; i < slotCount_; i++) {
            if (slots_[i].item != item) {
                continue;
            }
            slots_[i] = add(slots_[i], item);
            if (item != SuperChainGun) {
                selected_ = i;
            }
            return kCollectSound;
        }
    }

    if (slotCount_ >= kMaxSlots) {
        return "";
    }

    // The new item is selected, but the super chain gun only in an empty inventory.
    if (item != SuperChainGun || slotCount_ == 0) {
        selected_ = slotCount_;
    }

    slots_[slotCount_] = add(Slot{item, 0}, item);
    slotCount_++;
    return item == InterestingBomb ? kBombSound : kCollectSound;
}

Kurt::Slot Kurt::add(const Slot& slot, Item item) {
    // C# Inventory.Add (Inventory.cs:163-175).
    Slot result = slot;
    switch (item) {
        case Grenade:
            result.count = slot.count + kGrenades[(int)difficulty_];
            return result;
        case SuperChainGun:
            superChainGun_ += kSuperChainGunTicks[(int)difficulty_];
            result.count = 1;
            return result;
        default:
            result.count = 1;
            return result;
    }
}

const char* Kurt::useInstant(int32_t index) {
    // C# Inventory.UseInstant (Inventory.cs:177-233).
    if (index <= kLastAmmoPickup) {
        int32_t amount = kAmmoPerPickup[index];
        if (difficulty_ == Hard && amount > 1) {
            amount /= 2;
        }
        ammo_[index] += amount;
        selectedAmmo_ = index + 1;
        return index == kBonesPickup ? kBonesSound : kCollectSound;
    }

    switch (index) {
        case kHealth25:
            health_ = heal(health_, kSmallHealth);
            break;
        case kHealth50:
            health_ = heal(health_, kBigHealth);
            break;
        case kHealth100:
            health_ = std::max(health_, kMaxHealth);
            break;
        case kHealth150:
            health_ = std::max(health_, kSuperHealth);
            break;
        case kHealth01:
            health_ = heal(health_, 1);
            break;
        case kGroovy:
            return kCollectSound;
        case kBoneFlc:
            return kBonesSound;
        default:
            break;
    }
    return kHealthSound;
}

void Kurt::select(int32_t index) {
    // C# Inventory.Select (Inventory.cs:239).
    if (index >= 0 && index < slotCount_) {
        selected_ = index;
    }
}

void Kurt::selectNext(int32_t step) {
    // C# Inventory.SelectNext (Inventory.cs:248): ((Selected + step) % n + n) % n. The
    // two remainders are written as the exact wrap AGENTS.md R11 gives for a value
    // proven to lie in [-n, 2n): Selected is in [0, n) (every writer keeps it there:
    // Select's guard, CollectItem's appended index, RemoveSlot's clamp) and step is +1
    // or -1 (the two item keys), so Selected + step is in [-1, n], and adding or
    // subtracting n once is the C#'s result for every value of that range.
    if (slotCount_ == 0) {
        return;
    }
    int32_t next = selected_ + step;
    if (next < 0) {
        next += slotCount_;
    } else if (next >= slotCount_) {
        next -= slotCount_;
    }
    selected_ = next;
}

void Kurt::consume() {
    // C# Inventory.Consume (Inventory.cs:274-288).
    if (slotCount_ == 0) {
        return;
    }
    if (slots_[selected_].count > 1) {
        slots_[selected_].count--;
        return;
    }
    removeSlot(selected_);
}

void Kurt::removeSlot(int32_t index) {
    // C# Inventory.RemoveSlot (Inventory.cs:310-314): List.RemoveAt, then the selection
    // clamped into the slots left (0 when none).
    for (int32_t j = index + 1; j < slotCount_; j++) {
        slots_[j - 1] = slots_[j];
    }
    slotCount_--;
    slots_[slotCount_] = Slot{None, 0};
    const int32_t last = std::max(slotCount_ - 1, (int32_t)0);
    selected_ = std::min(std::max(selected_, (int32_t)0), last);
}

} // namespace mdk

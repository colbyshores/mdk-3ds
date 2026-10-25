#pragma once

// ============================================================================
// kurt.h — Kurt's combat and inventory state: the fields the scripts' hurt_kurt
// (opcode 109) writes and if_inventory (opcode 175) reads.
//
// A port of the Hurt/Damage half of src/Mdk.Game/Kurt/Kurt.Combat.cs and the
// slot list of src/Mdk.Game/Kurt/Inventory.cs. The movement state machine, the
// pickup collection, and the sniper/chain-gun combat are later slices: until
// they land, State stays Still and the slots stay empty — which is exactly what
// the C# reads in a world where Kurt never moves and never collects (the
// headless oracle ticks no Kurt.Update, so nothing else writes this state).
//
// The addresses are the original's: health 0x574324, the invulnerability
// 0x573bd4, the knock damage 0x573b20, the slots 0x57432c.
// ============================================================================

#include <stdint.h>

namespace mdk {

class Kurt {
public:
    // Kurt.State (Kurt.cs:20), in the C#'s order so a dumped value agrees.
    enum State {
        Still, Idle, Run, Side, Turn, Jump, RunJump, Fall, Chute, Land, Shot, RunFire, Throw,
        Knocked, GetUp, Dead, Slip, Slide, SlideFast, SlideBrake, Hang, HardLand,
        RollLeft, RollRight, HelmetOn, HelmetOff
    };

    // Inventory.Item (Inventory.cs), by pickup model (table 0x4921ac).
    enum Item { None, Dummy, InterestingBomb, Tornado, Mortar, Grenade, SuperChainGun, Key, Seal, SuperBone };
    struct Slot {
        Item item;
        int32_t count;
    };
    static constexpr int32_t kMaxSlots = 5;

    // Kurt.Combat.cs: MaxHealth (Inventory.MaxHealth), the red flash's per-damage
    // step and clamp, and the difficulty scaling (0x46a77c).
    static constexpr int32_t kMaxHealth = 100;
    static const float kHurtFlashPerDamage;
    static const float kHurtFlashMin;
    static const float kHurtFlashMax;
    static constexpr int32_t kEasyNumerator = 2;
    static constexpr int32_t kEasyDenominator = 3;
    static constexpr int32_t kHardFactor = 2;

    enum Mortality { Mortal, God };
    enum Difficulty { Easy, Normal, Hard };

    // Kurt.Hurt (Kurt.Combat.cs:118, 0x46a604): none while dead, invulnerable,
    // knocked down, getting up, or hard-landing.
    void hurt(int32_t damage);

    int32_t health() const { return health_; }
    float hurtFlash() const { return hurtFlash_; }
    float invulnerable() const { return invulnerable_; }
    float knockDamage() const { return knockDamage_; }
    State state() const { return state_; }

    // Inventory.Slots: filled by pickup collection (a later slice), read by
    // if_inventory (opcode 175).
    int32_t slotCount() const { return slotCount_; }
    const Slot* slots() const { return slots_; }
    // The C# if_inventory's count: the total Count of every slot of this Item.
    int32_t countItem(Item item) const;

private:
    // Damage (0x46a77c): the difficulty scaling, the red flash, health, knock damage.
    void damage(int32_t damage);
    // Scaled: easy takes 2/3 (at least 1), hard takes double, normal as-is.
    int32_t scaled(int32_t damage) const;

    int32_t health_ = kMaxHealth;
    float hurtFlash_ = 0.0f;
    float invulnerable_ = 0.0f;  // seconds (0x573bd4)
    float knockDamage_ = 0.0f;   // damage taken recently (0x573b20)
    State state_ = Still;
    Mortality mortality_ = Mortal;
    Difficulty difficulty_ = Normal;

    Slot slots_[kMaxSlots];
    int32_t slotCount_ = 0;
};

} // namespace mdk

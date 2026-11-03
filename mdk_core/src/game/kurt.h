#pragma once

// ============================================================================
// kurt.h — Kurt's combat and inventory state: the fields the scripts' hurt_kurt
// (opcode 109) writes and if_inventory (opcode 175) / if_ammo (opcode 174)
// read.
//
// A port of the Hurt/Damage half of src/Mdk.Game/Kurt/Kurt.Combat.cs and the
// slot, ammo and super-chain-gun state of src/Mdk.Game/Kurt/Inventory.cs. The
// movement state machine now lives in game/player.h (the C#'s Kurt is one
// class; this port splits the movement half from the combat half), and the
// pickup collection and the sniper/chain-gun combat are later slices: until
// they land, the slots, ammo and super-chain-gun timer stay empty — which is
// exactly what the C# reads in a world where Kurt never collects (the
// headless oracle ticks no Kurt.Update, so nothing else writes this state).
//
// The addresses are the original's: health 0x574324, the invulnerability
// 0x573bd4, the knock damage 0x573b20, the slots 0x57432c, the super-chain-
// gun ticks 0x5743ef and the sniper ammo 0x5743f3.
//
// ONE HEALTH. The C# has exactly one Kurt.Health (Kurt.Combat.cs:76) and every
// reader and writer goes through it: hurt_kurt (109) and the hard landing write
// it, if_player_health (241), the death transition and the HUD read it. This
// health_ is that field and there is no second one: Player owns a Kurt (it is the
// movement half of the same C# object) and reads its health through this object,
// and ScriptRuntime::kurt() hands out that same object rather than a copy of it,
// so a script that hurts Kurt to zero is the same event that kills him and empties
// the HUD's number. golden/L3_kurt_hurt.txt pins both columns of that.
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
    static constexpr int32_t kAmmoTypes = 5;

    // Kurt.Combat.cs: MaxHealth (Inventory.MaxHealth), the red flash's per-damage
    // step and clamp, and the difficulty scaling (0x46a77c).
    static constexpr int32_t kMaxHealth = 100;
    static const float kHurtFlashPerDamage;
    static const float kHurtFlashMin;
    static const float kHurtFlashMax;
    static constexpr int32_t kEasyNumerator = 2;
    static constexpr int32_t kEasyDenominator = 3;
    static constexpr int32_t kHardFactor = 2;

    // The knock-down counter's own bounds (Kurt.Combat.cs:32-33): recent damage
    // never counts above KnockdownDamage and drains by KnockdownDrain per second.
    // The rest of that constant block -- KnockdownInvulnerability, the 13-unit floor
    // probe and the -64 u/s slam -- belongs to the knock-down state machine, which
    // is a later slice (see declineKnockDamage and docs/OPEN_ITEMS.md).
    static constexpr float kKnockdownDamage = 5.0f;
    static constexpr float kKnockdownDrain = 2.0f;

    enum Mortality { Mortal, God };
    enum Difficulty { Easy, Normal, Hard };

    // Kurt.Hurt (Kurt.Combat.cs:118, 0x46a604): none while dead, invulnerable,
    // knocked down, getting up, or hard-landing (the guard is Kurt.Combat.cs:125).
    // The four states are read off state() below, which is the entity's own state,
    // not a copy of it.
    void hurt(int32_t damage);

    int32_t health() const { return health_; }
    // Kurt.SetHealth (Kurt.Combat.cs:169): "starts with another health than 100
    // (the fall's)". The only other writer besides damage().
    void setHealth(int32_t health);
    // The `Health = Math.Max(Health - damage, 0)` line of Kurt.Damage
    // (Kurt.Combat.cs:145) on its own: no difficulty scaling, no red flash, no
    // knock damage. That is the arithmetic Player needs for a hard landing, and
    // the two terms it leaves out are the ones Player does not model (see the
    // note at Player::damage).
    void damageHealth(int32_t damage);
    float hurtFlash() const { return hurtFlash_; }
    float invulnerable() const { return invulnerable_; }
    float knockDamage() const { return knockDamage_; }
    float whiteFlash() const { return whiteFlash_; }
    // The entity's state, and the ONLY copy of it. C# Kurt is one object whose
    // Hurt reads its own Current (Kurt.cs:104, read at Kurt.Combat.cs:125); this
    // port splits the class, so the field lives here and Player::state() is the
    // entity's view of it. Player::setState is the only writer. A second copy on
    // either side is the defect this comment exists to prevent: with the state in
    // Player alone, hurt()'s Dead/Knocked/GetUp/HardLand guard below reads a field
    // nothing writes and never fires, and a hard-landing Kurt takes damage the
    // oracle declines. Pinned by golden/L3_kurt_hurtland.txt.
    State state() const { return state_; }
    void setState(State state) { state_ = state; }

    // ---- The VM's Kurt-and-screen writes (ScriptVm.cs:1580-1600) ----
    // screen_flash (opcode 215) raises the white flash; the nuke and the
    // explosion flash write it too, but their slices are later (Kurt.Combat
    // .cs:80). The per-tick fade (FadeFlashes, Kurt.Combat.cs:203) is Kurt
    // movement — also later.
    void setWhiteFlash(float value) { whiteFlash_ = value; }
    // set_global_573bd4 (opcode 244): invulnerability time in seconds
    // (Kurt.Combat.cs:82); hurt() reads it.
    void setInvulnerable(float seconds) { invulnerable_ = seconds; }
    // set_hurt_flash (opcode 240): the knock-down counter (Kurt.Combat.cs:84).
    // hurt() clears it while invulnerable; the knock-down state machine that
    // reads it is Kurt movement, a later slice.
    void setKnockDamage(float value) { knockDamage_ = value; }

    // The decline half of Kurt.UpdateKnockDamage (Kurt.Combat.cs:230-231): clamp the
    // recent damage into [0, KnockdownDamage], then drain KnockdownDrain per second,
    // floored at zero. Player::update calls it at the C#'s call site (Kurt.cs:176 --
    // after the ride and noclip returns, before UpdateChuteSound and before the
    // Hang/HardLand branch), so a crashing Kurt's counter declines too.
    //
    // The TRIGGER half of that method (Kurt.Combat.cs:214-228: at KnockdownDamage on
    // a floor -- or in the air above one within KnockdownFloorDistance, slamming him
    // down at KnockdownSlamSpeed -- Kurt.KnockDown enters State.Knocked) is NOT
    // ported: entering Knocked needs the Knocked/GetUp arms of Kurt.UpdateCombatState
    // and the ArenaSpace.Floor probe, and neither exists here, so a port that entered
    // Knocked would strand Kurt in a state nothing animates and nothing exits.
    // Deferred with cause in docs/OPEN_ITEMS.md. The decline is ported on its own
    // because it is the counter's own rate and the trigger does not change it: the
    // C# runs this tail on every step the trigger does not knock him down on, and
    // Player already carries the same kind of half for the knock-down push
    // (Player::drainKnockdownPush).
    void declineKnockDamage(float delta);

    // Inventory.Slots: filled by pickup collection (a later slice), read by
    // if_inventory (opcode 175).
    int32_t slotCount() const { return slotCount_; }
    const Slot* slots() const { return slots_; }
    // The C# if_inventory's count: the total Count of every slot of this Item.
    int32_t countItem(Item item) const;

    // Inventory.Ammo: sniper rounds for types 1-5. if_ammo (opcode 174) reads
    // type 0 as the super chain gun's remaining ticks and types 1-5 as these
    // entries. An out-of-range type reads as zero, matching the oracle's
    // `kind <= Inventory.Ammo.Count ? Ammo[kind - 1] : 0` guard.
    int32_t ammo(int32_t type) const;
    int32_t superChainGun() const;

    // The pickup and combat writers for these values are deferred with their
    // subsystems. These setters are the state seam for the save/load and
    // pickup slices, and let the VM test supply otherwise unreachable values.
    void setAmmo(int32_t type, int32_t count);
    void setSuperChainGun(int32_t ticks);

private:
    // Damage (0x46a77c): the difficulty scaling, the red flash, health, knock damage.
    void damage(int32_t damage);
    // Scaled: easy takes 2/3 (at least 1), hard takes double, normal as-is.
    int32_t scaled(int32_t damage) const;

    int32_t health_ = kMaxHealth;
    float hurtFlash_ = 0.0f;
    float whiteFlash_ = 0.0f;    // the white screen flash (0x573b68)
    float invulnerable_ = 0.0f;  // seconds (0x573bd4)
    float knockDamage_ = 0.0f;   // damage taken recently (0x573b20)
    State state_ = Still;
    Mortality mortality_ = Mortal;
    Difficulty difficulty_ = Normal;

    // Value-initialised, not default-initialised: Player holds a Kurt and copies
    // it by value in the host harnesses, and an indeterminate Slot array is a read
    // of uninitialised memory even with slotCount() == 0 gating every reader.
    Slot slots_[kMaxSlots]{};
    int32_t slotCount_ = 0;
    int32_t ammo_[kAmmoTypes]{};
    int32_t superChainGun_ = 0;
};

} // namespace mdk

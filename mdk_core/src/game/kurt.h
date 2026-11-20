#pragma once

// ============================================================================
// kurt.h — Kurt's combat and inventory state: the fields the scripts' hurt_kurt
// (opcode 109) writes and if_inventory (opcode 175) / if_ammo (opcode 174)
// read.
//
// A port of the Hurt/Damage half of src/Mdk.Game/Kurt/Kurt.Combat.cs and the
// slot, ammo and super-chain-gun state of src/Mdk.Game/Kurt/Inventory.cs. The
// movement state machine now lives in game/player.h (the C#'s Kurt is one
// class; this port splits the movement half from the combat half). The chain
// gun's state (Firing, the muzzle flash) lives here too, written by the
// movement half's updateFiring/updateMuzzle and read by the scripts runtime.
// The inventory is Inventory.cs whole but for Restore (the saves) and the sniper's
// round selection (SelectAmmo/UseAmmo/NextAmmo, with the unported sniper mode):
// Collect fills the slots, the ammo and the super chain gun's time, the item keys
// select a slot, and a thrown item is consumed.
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

#include <string>
#include <vector>

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

    // KnockdownInvulnerability (Kurt.Combat.cs:34) is the value KnockDown sets this
    // timer to, and Player::knockDown is the game's only writer of it; opcode 244 is
    // the other one (see setInvulnerable below). The harness that opens the timer the
    // way a knock-down would (tests/scripts/tick1.cpp's invuln schedule) uses THIS
    // constant, so the value the golden prints is the oracle's and not a copy of it
    // in a test.
    static constexpr float kKnockdownInvulnerability = 3.0f;

    // The knock-down counter's own bounds (Kurt.Combat.cs:32-33): recent damage
    // never counts above KnockdownDamage and drains by KnockdownDrain per second.
    static constexpr float kKnockdownDamage = 5.0f;
    static constexpr float kKnockdownDrain = 2.0f;

    // The other two numbers of that constant block, the ones an AIRBORNE knock-down
    // needs (Kurt.Combat.cs:34-35): the counter reaches him only when the probe finds
    // a floor this far below his feet, and the slam is the vertical speed he is
    // dropped at. Player::updateKnockDamage is the only reader; the probe is
    // ArenaSpace::floor, the C#'s Floor and not its FloorCrossing.
    static constexpr float kKnockdownFloorDistance = 13.0f;
    static constexpr float kKnockdownSlamSpeed = -64.0f;

    // The two flashes' own numbers (Kurt.Combat.cs:25-27), written in the original's
    // per-tick units: Ticks is 30 (Kurt.cs:25), so a flash falls by 4 per tick = 120/s
    // and the skull rises by 2 per tick = 60/s up to 255. At the fixed 1/60 step that is
    // two units off a flash per step and one unit of skull per step, so the smallest
    // flash damage() can leave (75) is on screen for 38 steps and the skull takes 255
    // steps to fill. Player::update is what applies the first, Player::updateDeath the
    // second, and kurt.cpp says why the same field is both numbers.
    static constexpr float kTicks = 30.0f;
    static constexpr float kFlashFade = 4.0f * kTicks;
    static constexpr float kSkullFade = 2.0f * kTicks;
    static constexpr float kSkullEnd = 255.0f;

    enum Mortality { Mortal, God };
    enum Difficulty { Easy, Normal, Hard };

    // Kurt.Hurt (Kurt.Combat.cs:118, 0x46a604): none while dead, invulnerable,
    // knocked down, getting up, or hard-landing (the guard is Kurt.Combat.cs:125).
    // The four states are read off state() below, which is the entity's own state,
    // not a copy of it.
    void hurt(int32_t damage);

    // Damage (0x46a77c): the difficulty scaling, the red flash, health, knock damage.
    // It is public because the C#'s HardLanding calls Damage directly (Kurt.cs:580)
    // instead of going through Hurt, and the hard landing lives on Player in this port.
    // Its red flash used to be inert -- nothing faded it -- and now that something does
    // (fadeFlashes below), a slam that raised no flash leaves the HUD showing a
    // different screen from the oracle's for the next 90 steps.
    void damage(int32_t damage);

    int32_t health() const { return health_; }
    // Kurt.SetHealth (Kurt.Combat.cs:169): "starts with another health than 100
    // (the fall's)". The only other writer besides damage().
    void setHealth(int32_t health);
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
    // C# Kurt.Knocked (Kurt.Combat.cs:105): the two states the knock-down animation
    // owns. hurt() reads them through the guard above, UpdateKnockDamage reads this
    // one to refuse a second knock-down while he is already down, and Player's walk
    // suppression zeroes his input on it.
    bool knocked() const { return state_ == Knocked || state_ == GetUp; }

    // ---- The VM's Kurt-and-screen writes (ScriptVm.cs:1580-1600) ----
    // screen_flash (opcode 215) raises the white flash; the nuke and the
    // explosion flash write it too, but their slices are later (Kurt.Combat
    // .cs:80). The opcode itself MAXES (MathF.Max(WhiteFlash, Round(Value)), so a
    // weaker flash cannot lower a stronger one); the opcode does that at its own call
    // site and this setter is the plain write underneath it.
    void setWhiteFlash(float value) { whiteFlash_ = value; }
    // The other half of the death transition (Kurt.Combat.cs:375, `HurtFlash = 0f`
    // on the step health 0 on a floor becomes State.Dead). The C# sets it through a
    // private setter from inside Kurt; this port splits the class, so the movement
    // half that owns the transition needs the write. It is NOT cosmetic: without it a
    // Kurt hit just before he dies starts the skull from wherever the red flash had
    // got to, and the death beat is shorter than the oracle's.
    void setHurtFlash(float value) { hurtFlash_ = value; }

    // Kurt.FadeFlashes (Kurt.Combat.cs:203-206): both flashes fall by kFlashFade per
    // second and stop at zero. Player::update calls it where Kurt.Update does
    // (Kurt.cs:159) — after the Frozen and Dead early returns, so a corpse fades
    // nothing (its hurtFlash_ is the skull, rising below) and a cutscene freeze fades
    // through its own branch (Kurt.cs:144, unported: no Frozen yet). Both fields are
    // read by the HUD (HudView.DrawFlashes), so a flash that never fades is a red or
    // white screen for the rest of the level, not a cosmetic gap.
    void fadeFlashes(float delta);

    // The skull's rise, the other life of hurtFlash_ once Kurt is dead
    // (Kurt.Combat.cs:250-253): HurtFlash += SkullFade * delta, and the step that
    // takes it past SkullEnd is the C#'s `Died?.Invoke()`. Returns that step, because
    // the C#'s event has no port shape here (§10 forbids the delegate): the caller
    // latches it, and Player::died is what a frontend polls.
    bool fadeSkullIn(float delta);

    // Whether the skull has finished fading (Kurt.Combat.cs:238, the guard that stops
    // UpdateDeath). Strictly greater, as the oracle's is: at exactly 255 the corpse is
    // still live, and the same predicate is what makes Died fire once and only once.
    bool skullFinished() const { return hurtFlash_ > kSkullEnd; }
    // set_global_573bd4 (opcode 244): invulnerability time in seconds
    // (Kurt.Combat.cs:82); hurt() reads it, and declineInvulnerability below is what
    // ends it. Without that decay this field is a switch, not a timer: the first
    // opcode 244 would make Kurt immortal and knock-immune for the rest of the level.
    // Player::knockDown is the field's other writer, and the one that made the opcode
    // a timer rather than a switch in the first place.
    void setInvulnerable(float seconds) { invulnerable_ = seconds; }
    // The invulnerability timer's own decline: Kurt.Update's second statement
    // (Kurt.cs:139), Invulnerable = MathF.Max(Invulnerable - delta, 0f). Player::update
    // calls it at the C#'s own position, AHEAD of every early return Update has -- the
    // Dead one today, and the Frozen, noclip and Ride ones when those land -- so the
    // timer runs down on every step the entity takes, including a cutscene freeze and a
    // corpse's. A decay written after an early return leaves an invulnerability that
    // never ends whenever a script set one while Kurt was frozen or dead. Pinned by
    // golden/L3_kurt_invuln.txt, which prints the timer every step and hurts him on
    // both sides of the window it opens.
    void declineInvulnerability(float delta);
    // set_hurt_flash (opcode 240): the knock-down counter (Kurt.Combat.cs:84).
    // hurt() clears it while invulnerable, and Player::updateKnockDamage is what
    // reads it and decides he is knocked down.
    void setKnockDamage(float value) { knockDamage_ = value; }

    // The decline tail of Kurt.UpdateKnockDamage (Kurt.Combat.cs:230-231): clamp the
    // recent damage into [0, KnockdownDamage], then drain KnockdownDrain per second,
    // floored at zero. Player::updateKnockDamage calls it on every step the trigger
    // above it does not knock him down on -- which is the C#'s own shape, one method
    // with the trigger first and this tail second.
    void declineKnockDamage(float delta);

    // ---- The inventory (Inventory.cs, 0x57432c) ----
    // Inventory.Slots, filled by Collect (the pickups Kurt runs through,
    // ScriptRuntime::collectPickups, and the console's give), emptied by Consume
    // (an item thrown) and the super chain gun's time; read by if_inventory (opcode
    // 175) and the HUD.
    int32_t slotCount() const { return slotCount_; }
    const Slot* slots() const { return slots_; }
    // Inventory.Selected: the slot the item keys chose (a new item is selected when
    // it comes in). Inventory.SelectedItem is that slot's item, None when empty.
    int32_t selected() const { return selected_; }
    Item selectedItem() const { return slotCount_ == 0 ? None : slots_[selected_].item; }
    // Inventory.SelectedAmmo: the sniper round type the last ammo pickup chose (1-5,
    // 0 normal bullets). The sniper mode that reads it is not ported.
    int32_t selectedAmmo() const { return selectedAmmo_; }

    // Inventory.IsPickup: whether a model name is a pickup (SW_HBOMB, SW_H50...).
    static bool isPickup(const std::string& model);
    // Kurt.Collect + Inventory.Collect (Kurt.Combat.cs:160, Inventory.cs:107): takes a
    // pickup by model name. Returns the sound to play, or "" when Kurt cannot take it
    // (a full inventory, not a pickup). Health pickups change health_; every pickup
    // but SW_H01 and BONEFLC queues its name for the HUD's message (PickedUp).
    const char* collect(const std::string& pickup);
    // Inventory.Select (the item keys 1-5): slot `index`, if there is one.
    void select(int32_t index);
    // Inventory.SelectNext: the next (step 1) or previous (-1) slot, wrapping around.
    void selectNext(int32_t step);
    // Inventory.Consume: one of the selected item is used up; an empty slot goes.
    void consume();
    // Inventory.PickedUp, the C#'s event (§10 forbids the delegate): the names of the
    // pickups taken since the consumer last took them. The C#'s subscriber is the
    // viewer's (Viewer.cs:249: the HUD's message stream, 2 s, zoomed), which is not
    // ported; the harness reads it to pin which pickups raise a message.
    const std::vector<std::string>& pickedUp() const { return pickedUp_; }
    void clearPickedUp() { pickedUp_.clear(); }
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

    // Inventory.TickSuperChainGun (Inventory.cs:290): the super chain gun's time runs
    // while it fires (ScriptRuntime::fireChainGun, one tick per script tick); at 0 its
    // slot goes (RemoveSlot, which keeps the selection inside the slots).
    void tickSuperChainGun(int32_t ticks);

    // ---- The chain gun (Kurt.Combat.cs:86-88) ----
    // C# Kurt.Firing (0x573a38): the chain gun fires. Player::updateFiring is its only
    // writer that sets it, Player::stopFiring the one that clears it (the C#'s
    // UpdateFiring/StopFiring, which are Kurt's own methods; the port's movement half
    // owns the mixer they play on, so they live there). The readers are the scripts
    // runtime's tick (ScriptRuntime::fireChainGun runs while it is set), opcodes 181
    // (boss_bar) and 209 (if_player_on_me_low), and Kurt's own state machine (SHOT and
    // RUN_FIRE).
    bool firing() const { return firing_; }
    void setFiring(bool firing) { firing_ = firing; }

    // C# Kurt.MuzzleFlash (Kurt.Combat.cs:72): the K_MUZZF frame drawn behind Kurt with
    // its hotspot at his plus the offset (pixels, y down). C# Kurt.Muzzle is a nullable;
    // here it is the value plus hasMuzzle(). Player::updateMuzzle is the writer and
    // KurtSprite::draw the reader.
    struct MuzzleFlash {
        int32_t frame;
        int32_t x;
        int32_t y;
    };
    bool hasMuzzle() const { return hasMuzzle_; }
    const MuzzleFlash& muzzle() const { return muzzle_; }
    void setMuzzle(const MuzzleFlash& flash) {
        muzzle_ = flash;
        hasMuzzle_ = true;
    }
    void clearMuzzle() { hasMuzzle_ = false; }

private:
    // Scaled: easy takes 2/3 (at least 1), hard takes double, normal as-is.
    int32_t scaled(int32_t damage) const;
    // Inventory.CollectItem / Add / UseInstant / RemoveSlot (Inventory.cs:134-232, 310).
    const char* collectItem(Item item);
    Slot add(const Slot& slot, Item item);
    const char* useInstant(int32_t index);
    void removeSlot(int32_t index);

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
    int32_t selected_ = 0;
    int32_t ammo_[kAmmoTypes]{};
    int32_t selectedAmmo_ = 0;
    int32_t superChainGun_ = 0;
    std::vector<std::string> pickedUp_;
    bool firing_ = false;
    bool hasMuzzle_ = false;
    MuzzleFlash muzzle_{0, 0, 0};
};

} // namespace mdk

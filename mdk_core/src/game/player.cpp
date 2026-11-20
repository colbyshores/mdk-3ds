// ============================================================================
// player.cpp — the movement half of Kurt.cs. See player.h.
// ============================================================================

#include "game/player.h"

#include <algorithm>
#include <cmath>

#include "game/follow_camera.h"
#include "util/math_lut.h"

namespace mdk {

namespace {

// The C# Kurt.cs constants, converted to per-second (the original's per-tick
// numbers times Ticks for rates and times Ticks^2 for accelerations). kTicks is
// Kurt::kTicks and not a second copy of it: the constant behind the combat half's
// flash and skull rates must not be able to drift from the movement half's, which is
// the only way the two halves could ever run at different speeds.
const float kTicks = Kurt::kTicks;
const float kMaxSpeed = 0.6667f * kTicks;                 // 20.001 u/s (walk)
const float kMaxSpeedTurbo = 1.3333f * kTicks;            // 39.999 u/s (run)
const float kAcceleration = 0.04444f * kTicks * kTicks;   // 39.996 u/s^2
const float kAccelerationTurbo = 0.08889f * kTicks * kTicks;  // 80.001 u/s^2
const float kFrictionFast = 0.17778f * kTicks * kTicks;   // 160.0 u/s^2
const float kFrictionSlow = 0.08889f * kTicks * kTicks;   // 80.0 u/s^2
const float kAirControl = 0.75f;                          // air friction/strafe factor
const float kTurnSpeed = 4.0f * kTicks;                   // 120 deg/s
const float kTurnSpeedTurbo = 6.0f * kTicks;              // 180 deg/s
const float kTurnAcceleration = 0.9f * kTicks * kTicks;   // 810 deg/s^2
const float kTurnAccelerationTurbo = 1.3f * kTicks * kTicks;  // 1170 deg/s^2
const float kTurnFrictionFast = 1.6f * kTicks * kTicks;   // 1440 deg/s^2
const float kTurnFrictionSlow = 0.55f * kTicks * kTicks;  // 495 deg/s^2
const float kMouseDegrees = 0.15f;                        // degrees per mouse count

// The C# Kurt state machine's constants (Kurt.cs:81-88).
const float kIdleDelay = 6.0f;
const float kMovingSpeed = 1.0f;
const float kTurnFrameDegrees = 45.0f;
const int kCrashTicksPerFrame = 2;
const int kCrashHoldTick = 8;
// Kurt's footsteps (Kurt.cs:88-93): the run frames that stamp, and the same pair
// while running and firing (K_RUNFIR).
const int kLeftStep = 0;
const int kRightStep = 13;
const int kFireLeftStep = 4;
const int kFireRightStep = 17;
// CHUTEON flaps once the chute's opening frames are past (Kurt.cs:60-61).
const int kChuteOpenFrames = 4;
const char kChuteOnSound[] = "CHUTEON";

const float kGravity = 64.0f;
const float kMaxFallSpeed = 250.0f;
const float kJumpSpeed = 40.0f;
const int kJumpHoldTicks = 6;
const float kJumpReleasePenalty = 3.333f;
const float kFallStartSpeed = -16.0f;
const float kChuteGravity = 21.33f;
const float kChuteFallSpeed = -8.0f;
const float kChuteBrake = 256.0f;
// C# Kurt.Combat.cs: PushDrain. The horizontal knock-down push fades toward
// zero at this rate (u/s^2).
const float kPushDrain = 0.1f * kTicks * kTicks;

// The chain gun (Kurt.Combat.cs:47-70): its loop (0x46c3e4), MULTIFIRE, GATTFIRE with
// the super chain gun; K_MUZZF has 4 frames and a random 0-4 pixels is added to the
// state's offset.
const char kGunSound[] = "MULTIFIRE";
const char kSuperGunSound[] = "GATTFIRE";
const int32_t kMuzzleFrameMask = 3;
const int32_t kMuzzleJitter = 5;

// damp_collide_move's slide factors: walking stops within 30 degrees of
// head-on; falling slides off 45 degrees.
const float kWalkSlide = 0.75f;
const float kFallSlide = 0.5f;
// Landing slower than this keeps Kurt from creeping (0.35 u/tick).
const float kCreepSpeed = 0.35f * kTicks;
// Downhill glue: on floors flatter than this the feet follow the slope down.
const float kGlueNz = 0.25f;
// Landing faster than this is a hard landing (10 damage + K_CRASHL, which land
// with health). The C# gates the creep snap behind it, so the constant is
// needed for the position half now.
const float kHardLandingSpeed = -100.0f;
const int kHardLandingDamage = 10;   // C# HardLandingDamage
// Kurt dies this far below his arena's lowest point (C# FallOutDepth).
const float kFallOutDepth = -50.0f;

float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

float signf(float v) {
    return (v > 0.0f) - (v < 0.0f);
}

// The seam between the renderer's Vec3 and the collision's formats::Vec3f.
inline Vec toVec(const Vec3& v) { return Vec{v.x, v.y, v.z}; }
inline Vec3 fromVec(const Vec& v) { return Vec3{v.x, v.y, v.z}; }

// The C# Kurt.MoveToward.
float moveToward(float from, float to, float step) {
    return std::fabs(to - from) <= step ? to : from + signf(to - from) * step;
}

// The C# Kurt.Accelerate: towards input * max speed, or friction without input.
float accelerate(float speed, float input, bool turbo, float accelFactor, float frictionFactor,
                 float delta) {
    if (input == 0.0f) {
        const float friction = std::fabs(speed) > kMaxSpeed ? kFrictionFast : kFrictionSlow;
        return moveToward(speed, 0.0f, friction * frictionFactor * delta);
    }
    const float acceleration = (turbo ? kAccelerationTurbo : kAcceleration) * accelFactor;
    const float max = turbo ? kMaxSpeedTurbo : kMaxSpeed;
    if (speed * input < 0.0f) {
        speed = 0.0f;
    }
    return clampf(speed + input * acceleration * delta, -max, max);
}

} // namespace

Vec3 Player::facing() const {
    const float radians = follow_camera::degreesToRadians(yaw);
    return Vec3{fastCos(radians), fastSin(radians), 0.0f};
}

Vec3 Player::right() const {
    const Vec3 f = facing();
    return Vec3{f.y, -f.x, 0.0f};
}

Vec3 Player::velocity() const {
    // C# Kurt.Velocity (Kurt.cs:133):
    // Facing * ForwardSpeed + Right * StrafeSpeed + new Vector3(_push, VerticalSpeed).
    const Vec3 f = facing();
    const Vec3 r = right();
    return Vec3{f.x * forwardSpeed + r.x * strafeSpeed + _knockdownPush.x,
                f.y * forwardSpeed + r.y * strafeSpeed + _knockdownPush.y, verticalSpeed};
}

void Player::addKnockdownPush(const Vec2& push) {
    // C# Kurt.KnockDown (Kurt.Combat.cs:173-177): _push += push.
    _knockdownPush.x += push.x;
    _knockdownPush.y += push.y;
}

void Player::update(const Input& input, float delta) {
    // C# Kurt.Update decays the invulnerability timer as its second statement
    // (Kurt.cs:139), ahead of the ChuteClosing clear and of every early return below.
    // Opcode 244 (set_global_573bd4) is the only writer this port has, so without this
    // line that opcode is a permanent switch: hurt() would refuse forever and opcode
    // 240 could never raise the knock-down counter again. Kurt::declineInvulnerability
    // carries why the position ahead of the returns is the oracle's, and
    // golden/L3_kurt_invuln.txt pins it.
    combat_.declineInvulnerability(delta);
    // C# Kurt.Update clears ChuteClosing before every branch, including the
    // death branch (Kurt.cs:140, ahead of the `Current == State.Dead` return).
    // The clear is unconditional on purpose, and it is NOT pinned to be so:
    // `dead` and `_chuteClosing` can never be true at the same top-of-step, because
    // the only setter of `_chuteClosing` is the `!onFloor` Chute -> Fall transition
    // below and `dead` is set only with `onFloor`, so guarding this with `if (!dead)`
    // is unobservable dead code, not a behaviour change (review-9 R9-5; the scenario
    // that would have to distinguish them was built and matches byte for byte).
    _chuteClosing = false;
    // C# Kurt.Update's Frozen branch (Kurt.cs:141-150), ahead of the Dead one:
    // cutscenes and the level's end -- he stands still; the flashes still fade
    // (0x478704), the chute's flap stops, and his three speeds go. The end of a level
    // (scripts/end_level.cpp) moves his feet and yaw itself while he is frozen.
    if (frozen) {
        combat_.fadeFlashes(delta);
        stopChuteSound();
        forwardSpeed = 0.0f;
        strafeSpeed = 0.0f;
        verticalSpeed = 0.0f;
        return;
    }
    if (dead) {
        // The C# UpdateDeath advances the death beat. Fall keeps a corpse that
        // fell out of the arena at its death point (the post-move fall-out
        // assignment), and a corpse on a floor alternates OnFloor as the
        // oracle's Fall does.
        stopChuteSound();
        updateDeath(delta);
        return;
    }
    // C# Kurt.Update fades both flashes here (Kurt.cs:159): after the Frozen return
    // (Kurt.cs:144, which fades them too, on its own branch above the Dead one -- ported
    // above with the end of a level) and after the Dead return,
    // so a corpse fades nothing at all: its hurtFlash_ is the skull, which rises below and
    // is the only thing UpdateDeath does to it. A white flash that was up when Kurt died
    // therefore stays up for the whole death beat, which is the oracle's behaviour and is
    // pinned by golden/L3_kurt_flashes.txt, not an oversight of this port's.
    combat_.fadeFlashes(delta);
    // C# Kurt.Update levels the view's bank here (Kurt.cs:160), immediately after
    // FadeFlashes and ahead of every early return below -- so a knocked, crashing or
    // noclipping Kurt still settles his roll, and a corpse (the Dead return above)
    // still does not. It runs BEFORE the step's writers, which is why the roll can
    // hold one extra step after the keys come off (CameraRoll's held flag).
    _cameraRoll.settle(delta);
    // C# Kurt.Update runs UpdateKnockDamage here (Kurt.cs:176): after the ride and
    // noclip returns (neither is ported) and before UpdateChuteSound and the
    // Hang/HardLand branch, so a crashing Kurt's recent-damage counter declines too --
    // and so a Kurt who is knocked down mid-crash is knocked down before the crash
    // takes the step. The whole method is ported: the trigger and the decline tail.
    updateKnockDamage(delta);
    // C# Kurt.Update calls UpdateChuteSound before the Hang/HardLand branch, so a
    // crashing Kurt's chute still closes and stops flapping.
    updateChuteSound();
    if (state() == Kurt::HardLand) {
        // A hard landing owns the step until K_CRASHL is done (C#
        // UpdateSpecial). It also stops the ordinary walk state machine.
        updateSpecial(delta);
        return;
    }

    const bool turbo = input.isDown(kKeyTurbo);

    // The C# UpdateTurning (the mouse term is applied per frame in mouseLook).
    const float turn = input.axis(kKeyTurnLeft, kKeyTurnRight);
    if (turn == 0.0f) {
        const float friction = std::fabs(turnRate) > kTurnSpeed ? kTurnFrictionFast
                                                                : kTurnFrictionSlow;
        turnRate = moveToward(turnRate, 0.0f, friction * delta);
    } else {
        const float max = turbo ? kTurnSpeedTurbo : kTurnSpeed;
        const float acceleration = turbo ? kTurnAccelerationTurbo : kTurnAcceleration;
        turnRate = clampf(turnRate + turn * acceleration * delta, -max, max);
    }
    yaw += turnRate * delta;

    const float forwardIn = input.axis(kKeyForward, kKeyBack);
    const float strafeIn = input.axis(kKeyStrafeRight, kKeyStrafeLeft);
    // C# Kurt.cs:213-217: thrown, knocked down, getting up -- or kept by a platform,
    // which is not ported -- he does not walk. The INPUT is zeroed, not the speed: the
    // C# still runs Accelerate with a zero axis, so his speed decays by friction while
    // he is down instead of stopping dead, and a held key starts him walking the step
    // after he gets up. Zeroing the speed instead is a different animation.
    const bool walking = state() != Kurt::Throw && !combat_.knocked();
    const float forward = walking ? forwardIn : 0.0f;
    const float strafe = walking ? strafeIn : 0.0f;
    const float air = onFloor ? 1.0f : kAirControl;
    forwardSpeed = accelerate(forwardSpeed, forward, turbo, 1.0f, air, delta);
    strafeSpeed = accelerate(strafeSpeed, strafe, turbo, air, air, delta);
    // C# Kurt.cs:218, right after the two Accelerate calls: running while turning
    // with the keys rolls the view. The C# negates its own axis (TurnLeft is the
    // positive key but a left turn banks the view right), and it passes the ALREADY
    // zeroed `forward` from above, so a knocked Kurt with the keys down banks nothing
    // -- the same variable its Accelerate calls just used.
    _cameraRoll.walk(-turn, forward, delta);

    const Vec3 f = facing();
    const Vec3 r = right();
    // C# Kurt.cs:221-234: the move captures the current (pre-drain)
    // knock-down push, then that push drains before the walk move is applied
    // and before the vertical update consumes the move.
    const Vec3 move{(f.x * forwardSpeed + r.x * strafeSpeed + _knockdownPush.x) * delta,
                    (f.y * forwardSpeed + r.y * strafeSpeed + _knockdownPush.y) * delta, 0.0f};
    drainKnockdownPush(delta);

    if (_space && (move.x != 0.0f || move.y != 0.0f)) {
        feet = fromVec(_space->move(toVec(feet), toVec(move), ArenaSpace::kMotionWalk, kWalkSlide)
                           .feet);
    } else {
        feet.x += move.x;
        feet.y += move.y;
    }

    updateVertical(input, move, delta);
    // C# Kurt.Update returns after a hard landing, before UpdateState. The
    // landing set the state, and the next step runs UpdateSpecial instead.
    if (state() == Kurt::HardLand) {
        return;
    }
    // C# Kurt.cs:242-245, in that order: UpdateItems (which may start THROW, or use an
    // item at once in the air), then UpdateFiring (refused while throwing), the state
    // machine (which reads Firing for SHOT and RUN_FIRE, and runs the throw), and
    // UpdateMuzzle, which reads the state the machine just chose.
    updateItems(input);
    updateFiring(input);
    updateState(forward, strafe, delta);
    updateMuzzle(delta);
}

bool Player::pressed(const Input& input, Key key) {
    // C# Kurt.Pressed (Kurt.Combat.cs:257-267): a key that is up leaves the held set; a
    // key that is down is a press only when it was not there (HashSet.Add's answer).
    if (!input.isDown(key)) {
        _held[key] = false;
        return false;
    }
    const bool press = !_held[key];
    _held[key] = true;
    return press;
}

void Player::updateItems(const Input& input) {
    // C# Kurt.UpdateItems (Kurt.Combat.cs:271-311), in its order: next, previous, the slot
    // keys (Key.Item1-5, which input.h has no action for: never pressed here, and the C#
    // loop over them changes nothing when none is), then use. The use test's Pressed comes
    // first in its || chain, so the key's held flag is updated on every step this runs,
    // whatever the state -- a key held through a throw is not a second press after it.
    if (pressed(input, kKeyItemNext)) {
        combat_.selectNext(1);
    }
    if (pressed(input, kKeyItemPrevious)) {
        combat_.selectNext(-1);
    }

    const Kurt::Item item = combat_.selectedItem();
    if (!pressed(input, kKeyUseItem) || state() == Kurt::Throw || state() == Kurt::Dead ||
        combat_.knocked() || item == Kurt::None || item == Kurt::SuperChainGun) {
        return;
    }

    // The bomb is out (or another item is): "use" sets the bomb off.
    if (_itemHooks.canUseItem != nullptr &&
        !_itemHooks.canUseItem(_itemHooks.context, item)) {
        if (_itemHooks.bombTriggered != nullptr) {
            _itemHooks.bombTriggered(_itemHooks.context);
        }
        return;
    }

    if (onFloor) {
        setState(Kurt::Throw);
    } else if (state() == Kurt::Jump || state() == Kurt::RunJump || state() == Kurt::Fall ||
               state() == Kurt::Chute) {
        itemUsed();
    }
}

void Player::itemUsed() {
    // C# `ItemUsed?.Invoke()` (Kurt.Combat.cs:310, 422).
    if (_itemHooks.itemUsed != nullptr) {
        _itemHooks.itemUsed(_itemHooks.context);
    }
}

void Player::updateFiring(const Input& input) {
    // C# Kurt.UpdateFiring (Kurt.Combat.cs:345-358). The gun fires while the key is
    // held, Kurt is alive, not throwing and not knocked down. Nothing changes while the
    // same gun keeps firing (or keeps not firing); a start, a stop or a change of gun
    // stops the old loop first, so the super chain gun running out mid-burst swaps
    // GATTFIRE for MULTIFIRE on the step its time ends.
    const bool fire = input.isDown(kKeyFire) && health() > 0 && state() != Kurt::Throw &&
                      !combat_.knocked();
    const bool superGun = combat_.superChainGun() > 0;
    if (fire == combat_.firing() && (!combat_.firing() || superGun == _gunSuper)) {
        return;
    }

    stopFiring();
    combat_.setFiring(fire);
    _gunSuper = superGun;
    if (fire && _mixer != nullptr) {
        _gunVoice = _mixer->playLooped(superGun ? kSuperGunSound : kGunSound);
    }
}

void Player::stopFiring() {
    // C# Kurt.StopFiring (Kurt.Combat.cs:183-190). LeaveSniper() comes first there and
    // belongs to the unported sniper mode. StopVoice(0) is a no-op on both sides.
    combat_.setFiring(false);
    if (_mixer != nullptr) {
        _mixer->stopVoice(_gunVoice);
    }
    _gunVoice = 0;
    combat_.clearMuzzle();
}

void Player::updateMuzzle(float delta) {
    // C# Kurt.UpdateMuzzle (Kurt.Combat.cs:363-380). The clock counts original ticks
    // (30 a second) whether or not he fires; a flash shows on the odd ticks only, in the
    // states whose frames do not draw it themselves, and a new frame and jitter are
    // drawn once per such tick -- the second 1/60 step of a tick keeps the flash it has.
    // The truncation is the C#'s `(int)_muzzleTicks`; this is once a step, not a loop.
    _muzzleTicks += kTicks * delta;
    const int32_t tick = (int32_t)_muzzleTicks;
    int32_t offsetX = 0;
    int32_t offsetY = 0;
    if (!combat_.firing() || !muzzleOffset(&offsetX, &offsetY) || (tick & 1) == 0) {
        combat_.clearMuzzle();
        return;
    }

    if (tick == _muzzleTick) {
        return;
    }

    _muzzleTick = tick;
    // The next frame is (previous + 1-3) & 3, so the flash never repeats a frame; then
    // the X jitter and the Y jitter, in the C#'s argument order.
    _muzzleFrame = (_muzzleFrame + _muzzleRandom.next(kMuzzleFrameMask) + 1) & kMuzzleFrameMask;
    Kurt::MuzzleFlash flash;
    flash.frame = _muzzleFrame;
    flash.x = offsetX + _muzzleRandom.next(kMuzzleJitter);
    flash.y = offsetY + _muzzleRandom.next(kMuzzleJitter);
    combat_.setMuzzle(flash);
}

bool Player::muzzleOffset(int32_t* x, int32_t* y) const {
    // C# Kurt.MuzzleOffsets (Kurt.Combat.cs:57-65): the states that don't show the gun
    // firing by themselves. The board's K_SURF pose (-42, 12) comes first in the C# and
    // belongs to the unported ride.
    switch (state()) {
        case Kurt::Turn:
        case Kurt::Side:
        case Kurt::Jump:
            *x = 0;
            *y = 0;
            return true;
        case Kurt::Fall:
            *x = 40;
            *y = 6;
            return true;
        case Kurt::RunJump:
            *x = 0;
            *y = -10;
            return true;
        case Kurt::Chute:
            *x = 20;
            *y = 0;
            return true;
        default:
            return false;
    }
}

void Player::updateState(float forward, float strafe, float delta) {
    _stateTime += delta;

    // C# UpdateCombatState's first arm (Kurt.Combat.cs:371): health zero on a floor is
    // the death state. The knock-down, get-up and throw arms below are the C#'s next
    // three.
    if (health() == 0 && onFloor) {
        // C# UpdateCombatState's first arm (Kurt.Combat.cs:371-377): StopFiring, the
        // flash zeroed, State.Dead. updateFiring has already refused a corpse the gun
        // (Health > 0 is in its test), so the StopFiring is the oracle's belt and braces.
        // The zeroing is not: it is what makes the
        // death beat the oracle's 255 steps whatever the red flash was doing a step
        // earlier, and it is the reason the frontend's old stand-in (a skull fade of its
        // own, started from its own clock) could not be too far out -- it was starting
        // from the same zero, for the same 255/60 s.
        stopFiring();
        combat_.setHurtFlash(0.0f);
        setState(Kurt::Dead);
        return;
    }

    // C# UpdateCombatState's knock-down arms (Kurt.Combat.cs:379-399). K_BANG runs
    // frameCount(Knocked) frames at one frame per tick; then he flips, the push is
    // cleared at the flip, and K_BFLIP runs frameCount(GetUp). The flip carries the
    // leftover of the frame K_BANG ended on -- AnimationFrame - bang, not a reset to
    // zero -- so a step that crosses the boundary does not lose the fraction of the
    // frame it overshot by.
    if (state() == Kurt::Knocked) {
        _animationFrame += kTicks * delta;
        const float bang = (float)frameCount(Kurt::Knocked);
        if (_animationFrame < bang) {
            return;
        }
        const float flip = _animationFrame - bang;
        _knockdownPush.x = 0.0f;
        _knockdownPush.y = 0.0f;
        setState(Kurt::GetUp);
        _animationFrame = flip;
        return;
    }

    if (state() == Kurt::GetUp) {
        _animationFrame += kTicks * delta;
        if (_animationFrame < (float)frameCount(Kurt::GetUp)) {
            return;
        }
        // Back on his feet -- and the C# returns FALSE here, so the ordinary state
        // selection and Animate run on the very step he stands up: a Kurt who was
        // running when he went down has his run frame again at once, and one who was
        // standing goes through the Still/Idle test like anyone else.
        setState(Kurt::Still);
    } else if (state() == Kurt::Throw && onFloor &&
               _animationFrame < (float)(frameCount(Kurt::Throw) - 1)) {
        // C# UpdateCombatState's throw arm (Kurt.Combat.cs:410-423): Kurt stands still while
        // throwing (update() zeroed his walk) and the item leaves his hand on the step
        // K_SPWEP's frame clock crosses frame 8 (ThrowFrame, damp_animate). The arm owns the
        // step until the last frame; in the air, or on the last frame, the C# returns false
        // and the ordinary selection below takes him out of THROW. The GetUp arm above
        // returns false in the C# without reaching this test, which the else keeps.
        // Truncations, not floors: AnimationFrame only grows here from 0.
        const int32_t kThrowFrame = 8;
        const int32_t previous = (int32_t)_animationFrame;
        _animationFrame += kTicks * delta;
        if (previous < kThrowFrame && (int32_t)_animationFrame >= kThrowFrame) {
            itemUsed();
        }
        return;
    }

    // The C# has no missing-animation guard: a missing animation has count 0,
    // so `done` is `AnimationFrame >= -1`. A missing animation is a broken
    // archive, not a state; the comparison is kept exactly as the oracle's.
    const bool done = _animationFrame >= (float)(frameCount(state()) - 1);
    if (!onFloor) {
        if (chuteOpen) {
            setState(Kurt::Chute);
        } else if (state() == Kurt::Chute) {
            // The closing frame is shown once, then the fall starts.
            _chuteClosing = true;
            setState(Kurt::Fall);
            return;
        } else if (verticalSpeed > 0.0f &&
                   state() != Kurt::Jump && state() != Kurt::RunJump && state() != Kurt::Fall) {
            setState(std::fabs(forwardSpeed) > kMovingSpeed ? Kurt::RunJump : Kurt::Jump);
        } else if (verticalSpeed < kFallStartSpeed &&
                   ((state() != Kurt::Jump && state() != Kurt::RunJump) || done)) {
            setState(Kurt::Fall);
        }
    } else if (state() == Kurt::Jump || state() == Kurt::RunJump || state() == Kurt::Fall ||
               state() == Kurt::Chute) {
        playSound("LAND");
        setState(Kurt::Land);
    } else if (state() == Kurt::Land && !done && forward == 0.0f && strafe == 0.0f) {
        // The landing animation keeps running while he stands still.
    } else if (forward != 0.0f || std::fabs(forwardSpeed) > kMovingSpeed) {
        // Kurt.cs:663: running while the gun fires is K_RUNFIR.
        setState(combat_.firing() ? Kurt::RunFire : Kurt::Run);
    } else if (strafe != 0.0f || std::fabs(strafeSpeed) > kMovingSpeed) {
        setState(Kurt::Side);
    } else if (std::fabs(turnRate) > kMovingSpeed) {
        setState(Kurt::Turn);
    } else if (combat_.firing()) {
        // Kurt.cs:673: standing and firing is K_SHOT, ahead of the idle test, so a
        // Kurt who fires never goes idle.
        setState(Kurt::Shot);
    } else if (state() == Kurt::Idle && !done) {
        // The idle animation keeps running.
    } else if (state() == Kurt::Still && _stateTime > kIdleDelay) {
        setState(Kurt::Idle);
    } else if (state() != Kurt::Still) {
        setState(Kurt::Still);
    }

    animate(delta);
}

void Player::updateSpecial(float delta) {
    // C# Kurt.UpdateSpecial for K_CRASHL: gravity and the fall continue, and
    // the animation advances at two original ticks per frame. It is held at
    // tick eight while Kurt is still falling.
    _stateTime += delta;
    verticalSpeed = std::max(verticalSpeed - kGravity * delta, -kMaxFallSpeed);
    fall(delta);
    // The C#'s UpdateTouched has no port yet; the fall still runs.
    const float ticks = _animationFrame * (float)kCrashTicksPerFrame;
    float next = ticks + kTicks * delta;
    if (ticks <= (float)kCrashHoldTick && next >= (float)kCrashHoldTick && !onFloor &&
        verticalSpeed <= 0.0f && !chuteOpen) {
        next = (float)kCrashHoldTick;
    }
    _animationFrame = next / (float)kCrashTicksPerFrame;
    // The C# compares against the frame count with no guard: a missing
    // K_CRASHL (count 0) leaves the hard landing at once.
    if (_animationFrame >= (float)frameCount(Kurt::HardLand)) {
        setState(Kurt::Still);
    }
}

void Player::updateDeath(float delta) {
    // C# UpdateDeath's first statement (Kurt.Combat.cs:238): once the skull has faded
    // in, the corpse is finished -- no gravity, no fall, no state time, no frame. It is
    // not a cosmetic freeze: a corpse below the arena stops falling here, and the one on
    // a pad stops alternating OnFloor the way Fall makes it while the skull is still
    // rising. golden/L3_kurt_flashes.txt prints the step it starts on.
    if (combat_.skullFinished()) {
        return;
    }
    forwardSpeed = 0.0f;
    strafeSpeed = 0.0f;
    verticalSpeed = onFloor ? 0.0f : std::max(verticalSpeed - kGravity * delta, -kMaxFallSpeed);
    // The C# calls Fall() on either branch. It is the complete fall operation,
    // not only FallMove: a zero-length move hits nothing (Bsp.TestPlane bails
    // when the sweep is degenerate), but the fall-out test still runs after it.
    // On a real floor this clears OnFloor for the next step; the next gravity
    // move re-lands Kurt and the creep snap restores his position. Below the
    // arena the fall-out test sets OnFloor back to true, so the oracle holds
    // the corpse at the death point. Do not omit the post-move test.
    fall(delta);
    _stateTime += delta;
    _animationFrame = std::min(_animationFrame + kTicks * delta,
                               (float)(frameCount(Kurt::Dead) - 1));
    // The skull's rise and the one step it completes (Kurt.Combat.cs:250-253). The C#
    // raises its Died event there; see Player::died for the latch this port uses instead.
    if (combat_.fadeSkullIn(delta)) {
        _died = true;
    }
}

void Player::animate(float delta) {
    if (state() == Kurt::Run || state() == Kurt::RunFire) {
        const int32_t count = frameCount(state());
        if (count <= 0) {
            return;
        }
        // C# Kurt.Animate: the wrapped frames the step starts and ends on decide
        // which foot stamped. The floor is load-bearing when backing up: the frame
        // runs negative and Wrap maps it back into the animation, so truncating
        // here would drop a step every time the frame crosses zero backwards.
        const int32_t previous = wrapAnimationFrame((int32_t)std::floor(_animationFrame), count);
        _animationFrame += runRate() * kTicks * delta;
        const int32_t current = wrapAnimationFrame((int32_t)std::floor(_animationFrame), count);
        footstep(previous, current);
    } else if (state() == Kurt::Turn) {
        // No guard: a missing K_TRN45 leaves the frame at zero, as the C#'s
        // multiply by frameCount(Turn) == 0 does.
        _animationFrame = yaw / kTurnFrameDegrees * (float)frameCount(Kurt::Turn);
    } else {
        _animationFrame += kTicks * delta;
    }
}

void Player::footstep(int32_t previous, int32_t current) {
    // C# Kurt.Footstep: one stamp per frame the animation crossed, the left foot on
    // the left frame and the right foot alternating its pair. The pair flips only
    // on the right foot, so the pairs alternate FOOT3/FOOT4 then FOOT1/FOOT2.
    if (current == previous) {
        return;
    }

    const bool firing = state() == Kurt::RunFire;
    const int32_t left = firing ? kFireLeftStep : kLeftStep;
    const int32_t right = firing ? kFireRightStep : kRightStep;
    if (current == left) {
        playSound(_footstepPair ? "FOOT3" : "FOOT1");
    } else if (current == right) {
        playSound(_footstepPair ? "FOOT4" : "FOOT2");
        _footstepPair = !_footstepPair;
    }
}

void Player::updateChuteSound() {
    // C# Kurt.UpdateChuteSound: CHUTEON flaps after the opening frames, and closing
    // the chute stops it with CHUTEIN. CHUTEON is played Once, so the flap does not
    // stack voices; the headless device never ends a voice, so the Stop is what
    // takes it out of the mixer's list.
    if (chuteOpen && state() == Kurt::Chute && _animationFrame >= (float)kChuteOpenFrames) {
        playSound(kChuteOnSound, audio::SoundMixer::Once);
        return;
    }

    if (chuteOpen || !soundPlaying(kChuteOnSound)) {
        return;
    }

    stopSound(kChuteOnSound);
    playSound("CHUTEIN");
}

void Player::stopChuteSound() {
    // C# Kurt.StopChuteSound: CHUTEON stops without CHUTEIN when Kurt leaves the
    // chute otherwise (here: death). The ride, the cutscene and the ledge paths
    // that also call it are not ported.
    stopSound(kChuteOnSound);
}

void Player::playSound(const char* name, int32_t start) {
    if (_mixer) {
        _mixer->play(name, start);
    }
}

void Player::stopSound(const char* name) {
    if (_mixer) {
        _mixer->stop(name);
    }
}

bool Player::soundPlaying(const char* name) const {
    return _mixer != nullptr && _mixer->isPlaying(name);
}

void Player::setState(Kurt::State state) {
    // The one state, and it lives in the combat half: C# Kurt is one object whose
    // Hurt reads its own Current (Kurt.Combat.cs:125), so a state field here would
    // be a second copy for hurt() to miss. Same shape as the one health.
    if (state == combat_.state() && _stateTime > 0.0f) {
        return;
    }
    combat_.setState(state);
    _stateTime = 0.0f;
    _animationFrame = 0.0f;
    // The Died latch belongs to one death, and a death is over when Kurt leaves the
    // Dead state (the C#'s Revive, Kurt.Combat.cs:190-197, is the writer that would do
    // it -- SetState(Still) -- and it is not ported yet). Clearing here is what lets a
    // later death raise the event a second time instead of silently swallowing it.
    if (state != Kurt::Dead) {
        _died = false;
    }
}

int32_t Player::frameCount(Kurt::State state) const {
    const int index = (int)state;
    if (index < 0 || index >= kStateCount) {
        return 0;
    }
    return _frameCounts.values[index];
}

int32_t Player::wrapAnimationFrame(int32_t frame, int32_t count) {
    // The C#'s Wrap: ((frame % count) + count) % count. For a positive count
    // the first remainder is already in (-count, count), so adding the count
    // once when it is negative is the same value with ONE remainder instead of
    // two. This is straight-line frame selection, not a loop body.
    if (count <= 0) {
        return 0;
    }
    const int32_t remainder = frame % count;
    return remainder < 0 ? remainder + count : remainder;
}

float Player::runRate() const {
    const float u = std::fabs(forwardSpeed) / kTicks * 1.5f;
    const float rate = u <= 1.0f ? 0.75f * u + 0.25f : 0.25f * u + 0.75f;
    return forwardSpeed < 0.0f ? -rate : rate;
}

void Player::mouseLook(Input& input) {
    // Kurt's mouse look is the yaw only (C# Kurt.MouseLook); the camera's look
    // offset (its pitch) is the FollowCamera's, applied in FollowCamera::mouseLook.
    // A dead Kurt does not turn (C# MouseLook returns no turn on State.Dead), but
    // the accumulated mouse is still drained so it cannot build up.
    // A frozen Kurt does not turn either: the C#'s Frozen step returns before
    // UpdateTurning, so its _mouseTurn stays None and MouseLook turns nothing
    // (Kurt.cs:137-150, :340-352).
    if (dead || frozen) {
        input.takeMouseX();
        return;
    }
    yaw += -input.takeMouseX() * kMouseDegrees;
}

Vec3 Player::shove(Vec3 offset) {
    // C# Kurt.Shove: a horizontal Walk move (Nearby is empty until the objects
    // land, so the 4-arg move), returning how far the feet actually went. The
    // C#'s Contact() only records the touched triangle, which is not ported.
    const Vec3 start = feet;
    const Vec3 move{offset.x, offset.y, 0.0f};
    if (_space && (move.x != 0.0f || move.y != 0.0f)) {
        feet = fromVec(_space->move(toVec(feet), toVec(move), ArenaSpace::kMotionWalk, kWalkSlide).feet);
    }
    return Vec3{feet.x - start.x, feet.y - start.y, feet.z - start.z};
}

void Player::teleport(const Vec3& at, float newYaw) {
    // C# Kurt.Teleport (Kurt.cs:249-261), in the C#'s order. "The chute closes
    // silently": ChuteOpen = false and StopChuteSound(), which is CHUTEON stopped
    // WITHOUT CHUTEIN (the same call death makes, and for the same reason: Kurt left
    // the chute otherwise than by closing it). Then the three speeds go, which is the
    // "he stops" of the summary: a teleport does not carry the walk, the strafe or the
    // fall into the new place. `_keeper = null` is not here -- Kurt.Keeper.cs is not
    // ported, so there is no carrier to let go of.
    chuteOpen = false;
    stopChuteSound();
    feet = at;
    yaw = newYaw;
    forwardSpeed = 0.0f;
    strafeSpeed = 0.0f;
    verticalSpeed = 0.0f;
    teleports++;
}

void Player::setHealth(int health) {
    // C# SetHealth: max(health, 0) -- on the one health, which lives on the
    // combat half this Player owns.
    combat_.setHealth(health);
}

void Player::damage(int damage) {
    // C# Damage with Normal difficulty (Easy/Hard scaling lands with difficulty): the
    // scaling, the red flash, the health and the knock counter, all of it -- the C#'s
    // HardLanding calls Damage, not the Health line out of it.
    combat_.damage(damage);
}

void Player::knockDown(const Vec2& push) {
    // C# Kurt.KnockDown (Kurt.Combat.cs:173-180), in its order: the counter is spent,
    // the invulnerability opens, the push is added, the chute closes, K_BANG starts.
    // The order is not cosmetic -- the counter going to zero is what stops the next
    // step's trigger from knocking him down again while he is already down, and the
    // invulnerability is what makes hurt() refuse him for the whole animation.
    combat_.setKnockDamage(0.0f);
    combat_.setInvulnerable(Kurt::kKnockdownInvulnerability);
    addKnockdownPush(push);
    // Kurt.Combat.cs:178: a knocked-down Kurt stops firing (and updateFiring refuses
    // the gun while he is down, so it stays stopped until he is up).
    stopFiring();
    chuteOpen = false;
    setState(Kurt::Knocked);
}

void Player::updateKnockDamage(float delta) {
    // C# Kurt.UpdateKnockDamage (Kurt.Combat.cs:214) whole, in the C#'s order: the
    // trigger, and the decline as its tail on every step the trigger does not fire on.
    // The three guards are the C#'s three: a corpse is not knocked down, the counter
    // has to reach KnockdownDamage, and a Kurt already down does not get knocked down
    // again (which is what re-opens the 3 s of invulnerability every step otherwise).
    if (health() > 0 && combat_.knockDamage() >= Kurt::kKnockdownDamage &&
        !combat_.knocked()) {
        bool down = onFloor;
        if (!down) {
            // In the air he goes down only if a floor is within KnockdownFloorDistance
            // below his feet, and then he is slammed to it at KnockdownSlamSpeed. The
            // probe is the C#'s ArenaSpace.Floor, not its FloorCrossing: the arenas
            // around BOTH ends and the nearest crossing, which is the one that answers
            // across an arena boundary. A Player with no space has no floor to find --
            // the C# would have no space to ask, so this is the same answer an empty
            // arena list gives, not a substitute for the query.
            Vec hit;
            const Vec probe = vSub(toVec(feet), Vec{0.0f, 0.0f, Kurt::kKnockdownFloorDistance});
            if (_space != nullptr && _space->floor(toVec(feet), probe, &hit)) {
                down = true;
                verticalSpeed = std::min(verticalSpeed, Kurt::kKnockdownSlamSpeed);
            }
        }
        if (down) {
            // The C#'s own KnockDown(Vector2.Zero) (Kurt.Combat.cs:225): a knock-down the
            // counter triggered carries no shove. The only writer of a non-zero push is
            // ScriptRuntime::pushKurt (opcode 248), which calls knockDown with its own
            // vector, so this call site stays zero and is not a placeholder for it.
            knockDown(Vec2{0.0f, 0.0f});
            return;
        }
    }

    combat_.declineKnockDamage(delta);
}

void Player::drainKnockdownPush(float delta) {
    // C# DrainPush (Kurt.Combat.cs:208-209).
    const float step = kPushDrain * delta;
    _knockdownPush.x = moveToward(_knockdownPush.x, 0.0f, step);
    _knockdownPush.y = moveToward(_knockdownPush.y, 0.0f, step);
}

void Player::updateVertical(const Input& input, const Vec3& move, float delta) {
    const bool jumpHeld = input.isDown(kKeyJump);
    if (!jumpHeld) {
        _jumpReleased = true;
        if (_jumpTicksLeft > 0) {
            verticalSpeed -= _jumpTicksLeft * kJumpReleasePenalty;
            _jumpTicksLeft = 0;
        }
    }

    if (onFloor) {
        chuteOpen = false;
        if (jumpHeld && _jumpReleased) {
            verticalSpeed = kJumpSpeed;
            _jumpTicksLeft = kJumpHoldTicks;
            _jumpReleased = false;
        } else {
            glueDownhill(move, delta);
        }
    } else {
        updateAir(jumpHeld, delta);
    }

    // UpdateUpdraft is not ported (no fans/wind zones yet). When it lands it is
    // `if (Current != State.Knocked && VerticalSpeed > UpdraftExitSpeed) VerticalSpeed =
    // UpdraftExitSpeed;` (Kurt.Updraft.cs:29): it caps upward speed at 40 u/s on leaving
    // an updraft, and it does NOT cap it while Kurt is knocked down -- that exception is
    // reachable now that K_BANG is (batch 44), so a port that drops the state test would
    // cap a knocked-down Kurt's upward speed where the oracle leaves it alone. Today
    // upward speed never exceeds the 40 u/s jump, so the whole omission is inert -- add
    // the cap and its Knocked exception with the fan subsystem.
    fall(delta);
    // health 0 on the floor is death (C# "health 0 --on the floor--> DEAD"). A
    // hard landing lands the same tick and fall-out forces OnFloor, so both die
    // now; a future mid-air damage source would wait for the landing, as in the C#.
    if (health() == 0 && onFloor) {
        dead = true;
    }
}

void Player::updateAir(bool jumpHeld, float delta) {
    if (_jumpTicksLeft > 0) {
        _jumpTicksLeft = std::max(0, _jumpTicksLeft - (int)std::ceil(delta * kTicks));
    }

    if (!chuteOpen && jumpHeld && verticalSpeed < kFallStartSpeed) {
        chuteOpen = true;
        playSound("CHUTEOUT");
    } else if (chuteOpen && !jumpHeld) {
        // (no updraft to hold the chute open yet)
        chuteOpen = false;
    }

    if (!chuteOpen) {
        verticalSpeed = std::max(verticalSpeed - kGravity * delta, -kMaxFallSpeed);
        return;
    }

    verticalSpeed -= kChuteGravity * delta;
    if (verticalSpeed < kChuteFallSpeed) {
        verticalSpeed = std::min(verticalSpeed + kChuteBrake * delta, kChuteFallSpeed);
    }
}

void Player::glueDownhill(const Vec3& move, float delta) {
    const Vec3 n = _floorNormal.z < 0.0f
                       ? Vec3{-_floorNormal.x, -_floorNormal.y, -_floorNormal.z}
                       : _floorNormal;
    const float downhill = move.x * n.x + move.y * n.y;
    if (n.z > kGlueNz && downhill > 0.0f) {
        verticalSpeed = std::min(verticalSpeed, -downhill / delta);
    }
    verticalSpeed = std::max(verticalSpeed - kGravity * delta, -kMaxFallSpeed);
}

void Player::fall(float delta) {
    // C# Kurt.Fall: the fall move first, then the fall-out death (damp_gravity
    // 0x469efc). Fifty below his arena's lowest point Kurt dies and counts as
    // on a floor so the death plays. This post-move test is load-bearing in
    // UpdateDeath too: below the arena it sets OnFloor back to true after the
    // zero-length move, exactly as the oracle does. God mode is not ported.
    fallMove(delta);
    float bottom = 0.0f;
    if (_space && _space->bottom(toVec(feet), &bottom) && feet.z <= bottom + kFallOutDepth) {
        combat_.setHealth(0);
        verticalSpeed = 0.0f;
        onFloor = true;
    }
}

void Player::fallMove(float delta) {
    const Vec3 before = feet;
    const float dz = verticalSpeed * delta;

    if (!_space) {
        feet.z += dz;
        if (feet.z < before.z) {
            verticalSpeed = (feet.z - before.z) / delta;
        }
        onFloor = false;
        return;
    }

    const Vec3 down{0.0f, 0.0f, dz};
    const ArenaSpace::Result result =
        _space->move(toVec(feet), toVec(down), ArenaSpace::kMotionFall, kFallSlide);
    feet = fromVec(result.feet);

    if (!result.hit()) {
        // Sliding down a steep face: the real fall rate. (The crouch-slide
        // branch is false until the slide subsystem lands.)
        if (feet.z < before.z) {
            verticalSpeed = (feet.z - before.z) / delta;
        }
        onFloor = false;
        return;
    }

    if (verticalSpeed > 0.0f) {
        // Going up, bumped the head.
        verticalSpeed = 0.0f;
        onFloor = false;
        return;
    }

    land(before, fromVec(result.normal()), delta);
}

void Player::land(const Vec3& before, const Vec3& normal, float delta) {
    // The C# is `if (hard) HardLanding(); else if (creep) Feet = before;`. The
    // hard landing's damage and animation land with health, but the ELSE is
    // load-bearing for position: a hard landing must NOT snap to `before`, or
    // Kurt is left floating just above the floor with onFloor set.
    if (verticalSpeed < kHardLandingSpeed) {
        // C# HardLanding(): 10 damage, then K_CRASHL unless the damage killed
        // Kurt. The hard-landing state owns the next steps, so the ordinary
        // walk state machine cannot replace it on this tick.
        //
        // The C#'s own test is `Health == 0 || Current is Dead|Knocked|GetUp`
        // (Kurt.cs:584), and the `Health == 0` half is the observable one: a Kurt
        // already at 0 who lands at -194 u/s does not play K_CRASHL and does not
        // have his chute closed for him. golden/L3_kurt_lethal.txt pins it. The
        // guard around the damage call is the C#'s own `if (Health > 0)` and is not
        // observable here -- Player::damage floors at 0 exactly like Kurt.Damage's
        // Math.Max(Health - damage, 0) -- but the call is the C#'s Damage whole, so the
        // slam raises the red flash to its 180 clamp and adds to KnockDamage as the
        // oracle's does. Of those two the KnockDamage is provably inert for a hard
        // landing: Kurt.cs:583 zeroes that counter two lines after the damage call,
        // which is the line below. The flash is NOT inert and is no longer left out:
        // Kurt::fadeFlashes runs now, so a slam that raised no flash would leave the HUD
        // red for 90 steps fewer than the oracle leaves it -- golden/L3_kurt_flashes.txt
        // ends its fall in exactly this landing. What is still left out of the C#'s
        // Damage is Scaled(damage, Difficulty): on Easy the slam would be 6 and on Hard
        // 20, not the 10 the port deals. It is inert only because difficulty_ has no
        // writer yet -- the same deferral Player::damage records -- and it moves with
        // that, not with this.
        //
        // Kurt.cs:589 also calls StopFiring() (stop the chain-gun voice, drop the
        // muzzle flash) inside the same guard, below; the sniper half of it is not
        // ported.
        if (health() > 0) {
            damage(kHardLandingDamage);
        }
        // C# HardLanding zeroes the recent-damage counter HERE (Kurt.cs:583):
        // unconditionally, between the damage and the dead/knocked guard, so a hard
        // landing always starts the knock-down counter fresh. A counter a script
        // raised (set_hurt_flash, opcode 240) or a hurt taken in the air therefore
        // cannot survive the landing; without this line the port carries a nonzero
        // counter through a landing the oracle clears, which is exactly the shape that
        // would knock Kurt down on the step after every hard landing the moment the
        // knock-down machine lands. Pinned by golden/L3_kurt_knock.txt.
        combat_.setKnockDamage(0.0f);
        if (health() > 0 && state() != Kurt::Dead && state() != Kurt::Knocked &&
            state() != Kurt::GetUp) {
            stopFiring();
            chuteOpen = false;
            setState(Kurt::HardLand);
        }
    } else {
        const float creep = kCreepSpeed * delta;
        const float dx = feet.x - before.x;
        const float dy = feet.y - before.y;
        const float dz = feet.z - before.z;
        if (dx * dx + dy * dy + dz * dz < creep * creep) {
            feet = before;  // no creeping on a landing
        }
    }
    verticalSpeed = 0.0f;
    onFloor = true;
    _floorNormal = normal;
}

} // namespace mdk

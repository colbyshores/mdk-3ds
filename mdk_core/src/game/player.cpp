// ============================================================================
// player.cpp — the movement half of Kurt.cs. See player.h.
// ============================================================================

#include "game/player.h"

#include <string.h>

#include <algorithm>
#include <cmath>

#include "game/follow_camera.h"
#include "util/math_lut.h"
#include "util/math_minmax.h"

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
const float kMouseDegrees = Player::kMouseDegrees;        // degrees per mouse count

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
// Falling onto a platform first goes to 0.05 above its top (C# PlatformLift, damp_gravity).
const float kPlatformLift = 0.05f;
// Objects this far around a move (and Kurt's height above it) may stop it (C#
// NearbyMargin, KurtHeight).
const Vec kNearbyMargin = {1.0f, 1.0f, solids::kPlatformReach};
const float kKurtHeight = 5.0f;

// The updraft's looped sound (Kurt.Updraft.cs:10).
const char kFanSound[] = "FAN";

// The slide's constants (Kurt.Slide.cs:20-58), in the C#'s own spelling.
// The slope pushes by 10 x the smoothed floor normal; pushes within +/-0.1 brake by 2 u/s^2.
const float kSlideSlope = 10.0f;
const float kSlidePushDead = 0.1f;
const float kSlideFriction = 2.0f;
// The floor normal is smoothed by 0.8*old + 0.2*new horizontally, half and half in z.
const float kSlideNormalKeep = 0.8f;
const float kSlideNormalKeepZ = 0.5f;
// Speed cap: 50, raised by 10/s up to 80 going forward, lowered by 25/s down to 15
// braking, back towards 50 by 20/s.
const float kSlideCap = 50.0f;
const float kSlideCapMin = 15.0f;
const float kSlideCapMax = 80.0f;
const float kSlideCapRise = 10.0f;
const float kSlideCapFall = 25.0f;
const float kSlideCapRelax = 20.0f;
// Forward accelerates by 35 u/s^2 (above 15 u/s), back brakes by 15 u/s^2 (down to
// 15 u/s), turning turns by 45 deg/s; the mouse turns at most 4 x that.
const float kSlideAcceleration = 35.0f;
const float kSlideBrake = 15.0f;
const float kSlideMinSpeed = 15.0f;
const float kSlideTurn = 45.0f;
const float kSlideMouseTurn = 4.0f * kSlideTurn;
// Kurt faces the slide once |vx| + |vy| is above this.
const float kSlideHeading = 0.5f;
// A wall stops him when he moved less than half of a move over 0.5 units on an axis.
const float kSlideWallMove = 0.5f;
const float kSlideWallShare = 0.5f;
const float kSlideAirTicks = 20.0f;
// Ticks of sliding left after entering another arena.
const float kSlideArenaTicks = 15.0f;
// At rest: no speed and no push left (within this).
const float kSlideRest = 1e-5f;
// BUTSLIDE plays at 11025 Hz, at 15000 Hz going forward; BUTBRAKE while braking.
const float kSlideFastPitch = 15000.0f / 11025.0f;
const char kSlideSound[] = "BUTSLIDE";
const char kBrakeSound[] = "BUTBRAKE";

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

// C# Kurt.Accel (Kurt.Slide.cs:116-129, slide_accel 0x468be0): a push above 0.1
// accelerates by its square, below -0.1 it slows the same way, in between the
// velocity brakes towards 0.
float slideAxisAccel(float speed, float push, float dt) {
    if (push > kSlidePushDead) {
        return speed + push * push * dt;
    }
    if (push < -kSlidePushDead) {
        return speed - push * push * dt;
    }
    return moveToward(speed, 0.0f, kSlideFriction * dt);
}

// C# Kurt.Stopped (Kurt.Slide.cs:255-256): the move over 0.5 on an axis went less
// than half as far.
bool slideStopped(float planned, float moved) {
    return (planned > kSlideWallMove && moved < planned * kSlideWallShare) ||
           (planned < -kSlideWallMove && moved > planned * kSlideWallShare);
}

// System.Numerics Vector2.Length: sqrt(x*x + y*y).
float length2(const Vec2& v) {
    return sqrtf(v.x * v.x + v.y * v.y);
}

// System.Numerics Vector2.Normalize: each component divided by the length.
Vec2 normalize2(const Vec2& v) {
    const float length = length2(v);
    return Vec2{v.x / length, v.y / length};
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
    // C# Kurt.Update's first statement (Kurt.cs:137): no mouse turn until a step says
    // which (the scope's half of it, _mouseTurn = Scope, is the sniper step's).
    _mouseTurnScope = false;
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
    // C# Kurt.cs:161-164: the noclip step owns the whole step when Clipping is Off,
    // right after the roll settles and ahead of the ride (gap package #21).
    if (updateNoclip(input, delta)) {
        return;
    }
    // C# Kurt.Update runs UpdateKnockDamage here (Kurt.cs:176): after the ride and
    // noclip returns (the ride is not ported) and before UpdateChuteSound and the
    // Hang/HardLand branch, so a crashing Kurt's recent-damage counter declines too --
    // and so a Kurt who is knocked down mid-crash is knocked down before the crash
    // takes the step. The whole method is ported: the trigger and the decline tail.
    updateKnockDamage(delta);
    // C# Kurt.Update calls UpdateChuteSound before the Hang/HardLand branch, so a
    // crashing Kurt's chute still closes and stops flapping.
    updateChuteSound();
    // C# Kurt.cs:185: `if (UpdateSniper(input, turbo, delta) || UpdateSlide(input, delta))
    // return;` -- after UpdateChuteSound and the beta moves (not ported), BEFORE the
    // Hang/HardLand branch, so the sniper key can end a crash landing (EnterSniper's
    // CanSnipe does not refuse HardLand) and a sniper step owns the whole step
    // (game/player_sniper.cpp). The slide (Kurt.Slide.cs, package 8) owns the step the
    // same way while it runs: no turning, no walk, no items, no gun.
    if (updateSniper(input, input.isDown(kKeyTurbo), delta) || updateSlide(input, delta)) {
        return;
    }
    if (state() == Kurt::HardLand) {
        // A hard landing owns the step until K_CRASHL is done (C#
        // UpdateSpecial). It also stops the ordinary walk state machine.
        updateSpecial(delta);
        return;
    }

    const bool turbo = input.isDown(kKeyTurbo);

    // The C# UpdateTurning (the mouse term is applied per frame in mouseLook).
    updateTurning(input, turbo, delta);
    const float turn = input.axis(kKeyTurnLeft, kKeyTurnRight);
    // C# Kurt.cs:203: a platform that no longer keeps him lets go.
    checkKeeper();

    const float forwardIn = input.axis(kKeyForward, kKeyBack);
    const float strafeIn = input.axis(kKeyStrafeRight, kKeyStrafeLeft);
    // C# Kurt.cs:213-217: thrown, knocked down, getting up -- or kept by a platform,
    // which is not ported -- he does not walk. The INPUT is zeroed, not the speed: the
    // C# still runs Accelerate with a zero axis, so his speed decays by friction while
    // he is down instead of stopping dead, and a held key starts him walking the step
    // after he gets up. Zeroing the speed instead is a different animation.
    const bool walking = state() != Kurt::Throw && !combat_.knocked() && !kept();
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
        // C# Kurt.cs:225: the walk move against the arena and the objects nearby.
        feet = fromVec(contact(_space->move(toVec(feet), toVec(move), ArenaSpace::kMotionWalk,
                                            kWalkSlide,
                                            nearby(Vec3{feet.x + move.x, feet.y + move.y,
                                                        feet.z + move.z})))
                           .feet);
    } else {
        feet.x += move.x;
        feet.y += move.y;
    }

    updateVertical(input, move, delta);
    // C# Kurt.cs:235-236: on the keeping platform's XY, then the objects he touches.
    stayOnKeeper();
    updateTouched();
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

// C# Kurt.UpdateTurning (Kurt.cs:318-335), shared by the walk and noclip (Kurt.Cheats.cs
// calls it too). The C#'s `- input.MouseX * MouseDegrees` and `_mouseTurn = Walk` are
// mouseLook's here: `_mouseTurnScope` was cleared at the top of the step.
void Player::updateTurning(const Input& input, bool turbo, float delta) {
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
}

// C# Kurt.UpdateNoclip (Kurt.Cheats.cs:24-41), operation for operation:
//   Feet += (Facing * forward + Right * strafe + Vector3.UnitZ * rise) * speed * delta;
// each component summed left to right, then scaled by speed, then by delta, as
// System.Numerics evaluates it. No collision, no gravity, no state change: the
// speeds go to 0 and he is off the floor.
bool Player::updateNoclip(const Input& input, float delta) {
    // C# Kurt.NoclipSpeed and NoclipTurbo.
    static constexpr float kNoclipSpeed = 60.0f;
    static constexpr float kNoclipTurbo = 3.0f;
    if (_clipping == ClipOn) {
        return false;
    }
    const bool turbo = input.isDown(kKeyTurbo);
    updateTurning(input, turbo, delta);
    const float forward = input.axis(kKeyForward, kKeyBack);
    const float strafe = input.axis(kKeyStrafeRight, kKeyStrafeLeft);
    const float rise = input.axis(kKeyJump, kKeyDown);
    const float speed = kNoclipSpeed * (turbo ? kNoclipTurbo : 1.0f);
    const Vec3 f = facing();
    const Vec3 r = right();
    feet.x += (f.x * forward + r.x * strafe + 0.0f * rise) * speed * delta;
    feet.y += (f.y * forward + r.y * strafe + 0.0f * rise) * speed * delta;
    feet.z += (f.z * forward + r.z * strafe + 1.0f * rise) * speed * delta;
    forwardSpeed = 0.0f;
    strafeSpeed = 0.0f;
    verticalSpeed = 0.0f;
    onFloor = false;
    return true;
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
    // C# Kurt.StopFiring (Kurt.Combat.cs:183-190): sniper mode ends first, silently (the
    // knock-down, the hard landing; the cutscenes and rides when they land). StopVoice(0)
    // is a no-op on both sides.
    leaveSniper();
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
    // C# UpdateSpecial (Kurt.cs:607): the objects his box touches after the fall.
    updateTouched();
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
    // Kurt.Combat.cs:246: the updraft runs for a corpse too. Its Health > 0 test makes
    // it the NaN arm here: the FAN loop stops, the flag clears and an upward speed over
    // 40 u/s is capped -- a Kurt who dies in a fan's box falls out of it.
    updateUpdraft(delta);
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
    if (_mouseTurnScope) {
        // C# Kurt.MouseLook's scope arm (Kurt.cs:349-357): the last step looked through
        // the scope, so the frame's mouse turns and tilts it -- the mouse's share of
        // Scope.Look, with no keys and no time -- and both axes are taken (the follow
        // camera's MouseLook leaves the tilt alone in sniper mode).
        const float mouseX = input.takeMouseX() * kMouseDegrees;
        const float mouseY = input.takeMouseY() * kMouseDegrees;
        yaw += _scope.look(0.0f, 0.0f, mouseX, mouseY, Scope::TurboOff, 0.0f);
        return;
    }
    // Sliding, the C#'s MouseLook turns nothing (the slide step left _mouseTurn at
    // None) and the step's Steer reads the frame's motion instead; see _slideMouseX.
    if (_sliding) {
        _slideMouseX = input.takeMouseX();
        return;
    }
    _slideMouseX = 0.0f;
    yaw += -input.takeMouseX() * kMouseDegrees;
}

Vec3 Player::shove(Vec3 offset) {
    // C# Kurt.Shove (Kurt.cs:265): a horizontal Walk move against the arena and the
    // objects nearby, its contact recorded, returning how far the feet actually went.
    const Vec3 start = feet;
    const Vec3 move{offset.x, offset.y, 0.0f};
    if (_space && (move.x != 0.0f || move.y != 0.0f)) {
        feet = fromVec(contact(_space->move(toVec(feet), toVec(move), ArenaSpace::kMotionWalk,
                                            kWalkSlide,
                                            nearby(Vec3{feet.x + move.x, feet.y + move.y,
                                                        feet.z + move.z})))
                           .feet);
    }
    return Vec3{feet.x - start.x, feet.y - start.y, feet.z - start.z};
}

void Player::teleport(const Vec3& at, float newYaw) {
    // C# Kurt.Teleport (Kurt.cs:249-261), in the C#'s order. "The chute closes
    // silently": ChuteOpen = false and StopChuteSound(), which is CHUTEON stopped
    // WITHOUT CHUTEIN (the same call death makes, and for the same reason: Kurt left
    // the chute otherwise than by closing it). Then the three speeds go, which is the
    // "he stops" of the summary: a teleport does not carry the walk, the strafe or the
    // fall into the new place. A keeping platform lets him go (`_keeper = null`,
    // Kurt.cs:254, package #3).
    chuteOpen = false;
    stopChuteSound();
    _keeper = nullptr;
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

void Player::sniperMove(const Input& input, bool turbo, float delta) {
    // C# Kurt.SniperMove (Kurt.Sniper.cs:178-192): Kurt sidesteps at a quarter of the
    // running speed and still falls. It lives here rather than in player_sniper.cpp
    // because it is the walk's arithmetic: the same Accelerate, speeds, slide and
    // gravity. The move is the C#'s `(Right * StrafeSpeed + (push, 0)) * delta`, taken
    // before the push drains, as the walk's; the move passes Nearby (the solid objects).
    const float max = (turbo ? kMaxSpeedTurbo : kMaxSpeed) * 0.25f;  // SniperStrafe
    forwardSpeed = 0.0f;
    strafeSpeed = clampf(accelerate(strafeSpeed, input.axis(kKeyStrafeRight, kKeyStrafeLeft), turbo,
                                    1.0f, 1.0f, delta),
                         -max, max);
    const Vec3 r = right();
    const Vec3 move{(r.x * strafeSpeed + _knockdownPush.x) * delta,
                    (r.y * strafeSpeed + _knockdownPush.y) * delta, 0.0f};
    drainKnockdownPush(delta);
    if (move.x != 0.0f || move.y != 0.0f) {
        if (_space) {
            // Kurt.Sniper.cs:187: against the objects nearby too (package #3), and with
            // no Contact, as the C#'s sidestep records none.
            feet = fromVec(_space
                               ->move(toVec(feet), toVec(move), ArenaSpace::kMotionWalk, kWalkSlide,
                                      nearby(Vec3{feet.x + move.x, feet.y + move.y,
                                                  feet.z + move.z}))
                               .feet);
        } else {
            feet.x += move.x;
            feet.y += move.y;
        }
    }
    verticalSpeed = std::max(verticalSpeed - kGravity * delta, -kMaxFallSpeed);
    fall(delta);
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

    // C# Kurt.cs:417: the fans' updraft, after the floor/air branch and before the fall
    // (Kurt.Updraft.cs). Out of every fan it caps the upward speed at 40 u/s -- except
    // while Kurt is knocked down, which is the oracle's own exception.
    updateUpdraft(delta);
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
    } else if (chuteOpen && !jumpHeld && !_inUpdraft) {
        // Kurt.cs:435: letting go of jump closes the chute -- unless a fan holds him,
        // which keeps it open (UpdateUpdraft opened it).
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
    // zero-length move, exactly as the oracle does. God mode (Kurt.Mortality ==
    // God, the console's cheat, gap package #21) skips the death: `|| Mortality ==
    // Mortality.God` in the C#'s return test.
    fallMove(delta);
    float bottom = 0.0f;
    if (_space && _space->bottom(toVec(feet), &bottom) && feet.z <= bottom + kFallOutDepth &&
        combat_.mortality() != Kurt::God) {
        combat_.setHealth(0);
        verticalSpeed = 0.0f;
        onFloor = true;
    }
}

void Player::fallMove(float delta) {
    const Vec3 before = feet;
    float dz = verticalSpeed * delta;

    if (!_space) {
        feet.z += dz;
        if (feet.z < before.z) {
            verticalSpeed = (feet.z - before.z) / delta;
        }
        onFloor = false;
        return;
    }

    // C# Kurt.FallMove (Kurt.cs:510-526): a platform under the feet -- the keeping
    // one first, else one the fall reaches -- stops the fall 0.05 above its top, and
    // when the BSP lets him get there he lands on it (Platform) at its top.
    float platformTop = 0.0f;
    scripts::MdkObject* platformOwner = nullptr;
    const bool onPlatform = keptPlatform(dz, &platformTop, &platformOwner) ||
                            findPlatform(dz, &platformTop, &platformOwner);
    if (onPlatform) {
        dz = platformTop + kPlatformLift - feet.z;
    }

    const Vec3 down{0.0f, 0.0f, dz};
    const ArenaSpace::Result result =
        contact(_space->move(toVec(feet), toVec(down), ArenaSpace::kMotionFall, kFallSlide));
    feet = fromVec(result.feet);
    _platform = nullptr;
    if (!result.hit() && onPlatform) {
        feet.z = platformTop;
        _platform = platformOwner;
        keepOn(platformOwner);
        land(before, Vec3{0.0f, 0.0f, 1.0f}, delta);
        return;
    }

    if (!result.hit()) {
        // Sliding down a steep face: the real fall rate.
        if (feet.z < before.z) {
            verticalSpeed = (feet.z - before.z) / delta;
        }
        onFloor = false;
        return;
    }

    // C# FallMove's Sliding branch (Kurt.cs:540-546): on his back a hit is not a
    // landing -- no hard landing, no creep snap -- the vertical speed is what the move
    // made of it, and the floor it hit is the slide's floor normal.
    if (_sliding) {
        verticalSpeed = (feet.z - before.z) / delta;
        onFloor = true;
        _floorNormal = fromVec(result.normal());
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
        // Kurt.cs:589 also calls StopFiring() (leave sniper mode, stop the chain-gun
        // voice, drop the muzzle flash) inside the same guard, below.
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

// ---- Platforms, solid objects and touches (package #3) ----------------------

const std::vector<solids::Solid>& Player::nearby(const Vec3& to) {
    // C# Kurt.Nearby (Kurt.cs:290-300): the region around the feet and `to`, grown by
    // the margin, and Kurt's height above it. The C#'s evaluation order is kept:
    // (max + margin) + (0, 0, KurtHeight).
    static const std::vector<solids::Solid> kNone;
    if (_solidHooks.solidsWithin == nullptr) {
        return kNone;
    }
    formats::Box region;
    region.min = Vec{std::min(feet.x, to.x) - kNearbyMargin.x,
                     std::min(feet.y, to.y) - kNearbyMargin.y,
                     std::min(feet.z, to.z) - kNearbyMargin.z};
    region.max = Vec{std::max(feet.x, to.x) + kNearbyMargin.x,
                     std::max(feet.y, to.y) + kNearbyMargin.y,
                     std::max(feet.z, to.z) + kNearbyMargin.z + kKurtHeight};
    const std::vector<solids::Solid>* list = _solidHooks.solidsWithin(_solidHooks.context, region);
    return list != nullptr ? *list : kNone;
}

bool Player::findPlatform(float dz, float* top, scripts::MdkObject** owner) {
    // C# Kurt.FindPlatform (Kurt.cs:303-313): not while rising; the platform the ray
    // to the fall's bottom meets, if the fall gets down to its top.
    if (verticalSpeed > 0.0f) {
        return false;
    }
    const float bottom = feet.z + dz;
    float found = 0.0f;
    scripts::MdkObject* by = nullptr;
    if (!solids::platform(toVec(feet), bottom, nearby(Vec3{feet.x, feet.y, bottom}), &found,
                          &by) ||
        !(bottom <= found)) {
        return false;
    }
    *top = found;
    *owner = by;
    return true;
}

ArenaSpace::Result Player::contact(const ArenaSpace::Result& result) {
    // C# Kurt.Contact (Kurt.Ride.cs:123-131).
    if (result.hit() && result.bsp != nullptr) {
        _contacts.push_back(Contact{result.bsp->arena(), result.triangle});
    }
    return result;
}

const std::vector<Player::Contact>& Player::takeContacts() {
    // C# Kurt.TakeContacts (Kurt.Ride.cs:112-118).
    _taken.clear();
    _taken.insert(_taken.end(), _contacts.begin(), _contacts.end());
    _contacts.clear();
    return _taken;
}

void Player::updateTouched() {
    // C# Kurt.UpdateTouched (Kurt.Ride.cs:133-143): what his walking box touches, and
    // his platform if it is not among them.
    _touched.clear();
    ArenaSpace::touching(toVec(feet), nearby(feet), &_touched);
    if (_platform != nullptr &&
        std::find(_touched.begin(), _touched.end(), _platform) == _touched.end()) {
        _touched.push_back(_platform);
    }
}

void Player::carry(const Vec3& from, const Vec3& to, float turn) {
    // C# Kurt.Carry (Kurt.cs:282-287): Vector3.Transform(Feet - from,
    // Matrix4x4.CreateRotationZ(DegreesToRadians(turn))). Measured on .NET 10
    // (200000 random cases, 0 misses): the transform is x*M11 rounded, then ONE fused
    // multiply-add of y*M21 (likewise Y); the unfused sum misses a third of them. Z
    // passes through. CreateRotationZ's sin/cos are MathF.SinCos's, which equal
    // MathF.Sin/Cos on every one of those cases.
    /* @vfp-exempt R6 — Kurt's carried position on a turning platform is gameplay
     * state the oracle computes with exact sin/cos; the LUT's error would move his
     * feet. Once per tick, only while he stands on a platform. measured n/a. Verified
     * 2026-11-20. */
    const float pi = 3.14159265358979323846f;
    const float radians = turn * pi / 180.0f;
    const float c = cosf(radians);
    const float s = sinf(radians);
    const float x = feet.x - from.x;
    const float y = feet.y - from.y;
    const float z = feet.z - from.z;
    const float ox = fmaf(y, -s, x * c);
    const float oy = fmaf(y, c, x * s);
    feet = Vec3{to.x + ox, to.y + oy, to.z + z};
    yaw += turn;
}

void Player::releaseSolids() {
    _platform = nullptr;
    _keeper = nullptr;
    _touched.clear();
    _contacts.clear();
    _taken.clear();
    _solidHooks = SolidHooks();
}

bool Player::keeperAt(scripts::MdkObject* owner, Vec3* at) const {
    // The C#'s `Keeper?.Invoke(owner)`: null without the hook.
    return _solidHooks.keeperAt != nullptr &&
           _solidHooks.keeperAt(_solidHooks.context, owner, at);
}

void Player::checkKeeper() {
    // C# Kurt.CheckKeeper (Kurt.Keeper.cs:167-173).
    Vec3 at;
    if (_keeper != nullptr && !keeperAt(_keeper, &at)) {
        _keeper = nullptr;
    }
}

bool Player::keptPlatform(float dz, float* top, scripts::MdkObject** owner) const {
    // C# Kurt.KeptPlatform (Kurt.Keeper.cs:176-185): the keeping platform as the fall's
    // floor, no scan.
    Vec3 at;
    if (_keeper == nullptr || verticalSpeed > 0.0f || !keeperAt(_keeper, &at)) {
        return false;
    }
    const float height = at.z + _keptHeight;
    if (!(feet.z + dz <= height)) {
        return false;
    }
    *top = height;
    *owner = _keeper;
    return true;
}

void Player::keepOn(scripts::MdkObject* owner) {
    // C# Kurt.KeepOn (Kurt.Keeper.cs:188-197).
    Vec3 at;
    if (_keeper != nullptr || !keeperAt(owner, &at)) {
        return;
    }
    _keeper = owner;
    _keptHeight = feet.z - at.z;
}

void Player::stayOnKeeper() {
    // C# Kurt.StayOnKeeper (Kurt.Keeper.cs:200-217).
    Vec3 at;
    if (_keeper == nullptr || !keeperAt(_keeper, &at)) {
        return;
    }
    _platform = _keeper;
    const float top = at.z + _keptHeight;
    feet = Vec3{at.x, at.y, std::max(feet.z, top)};
    if (feet.z > top) {
        return;
    }
    onFloor = true;
    verticalSpeed = std::max(verticalSpeed, 0.0f);
}

// ---- end of platforms, solid objects and touches ---------------------------
// ---- The fans' updrafts: C# Kurt.Updraft.cs ----

void Player::updateUpdraft(float delta) {
    // C# Kurt.UpdateUpdraft (Kurt.Updraft.cs:21-49), in its order. The hook is the
    // scripts runtime's KurtUpdraft: his arena's fans, else the second arena's.
    const float vz = _updraftHook.updraft != nullptr && health() > 0
                         ? _updraftHook.updraft(_updraftHook.context, verticalSpeed, delta)
                         : NAN;
    if (std::isnan(vz)) {
        if (_mixer != nullptr) {
            _mixer->stopVoice(_fanVoice);
        }
        _fanVoice = 0;
        _inUpdraft = false;
        if (state() != Kurt::Knocked && verticalSpeed > kUpdraftExitSpeed) {
            verticalSpeed = kUpdraftExitSpeed;
        }
        return;
    }

    verticalSpeed = vz;
    if (!chuteOpen) {
        chuteOpen = true;
        playSound("CHUTEOUT");
    }
    if (!_inUpdraft) {
        _inUpdraft = true;
        _fanVoice = _mixer != nullptr ? _mixer->playLooped(kFanSound) : 0;
    }
}

// ---- The wind zones' slide: C# Kurt.Slide.cs ----

/* @vfp-exempt R6 — the slide's heading: cosf/sinf are the oracle's MathF.Cos/Sin
 * (Kurt.Facing) and the conversion is .NET's left-associative DegreesToRadians. The
 * slide feeds its own heading back every step (Steer: yaw from atan2 of the velocity,
 * then the velocity from the yaw), so the fastSin/fastCos LUT's error would compound
 * into the path golden/L6_wind_*.txt pins. Once a step while sliding, not a loop body.
 * measured n/a (corpus empty). Verified 2026-11-20. */
Vec2 Player::slideFacing() const {
    const float radians = yaw * 3.14159265358979323846f / 180.0f;
    return Vec2{cosf(radians), sinf(radians)};
}

void Player::startSlide() {
    // C# Kurt.StartSlide (Kurt.Slide.cs:77-93).
    if (_sliding || health() == 0) {
        return;
    }
    _sliding = true;
    _slideVelocity = Vec2{0.0f, 0.0f};
    _slideCap = kSlideCap;
    _slideNormal = Vec3{0.0f, 0.0f, 1.0f};
    _slidePush = Vec2{0.0f, 0.0f};
    _slideAir = 0.0f;
    _slideLeft = 0.0f;
    stopFiring();
    chuteOpen = false;
    setState(Kurt::Slip);
}

void Player::enterArena() {
    // C# Kurt.EnterArena (Kurt.Slide.cs:97-105): a slide ends 15 ticks after an arena
    // change (LEVEL6's slide drops him into OLYM_2 against a wall); a second crossing
    // inside those ticks does not restart them.
    if (!_sliding || _slideLeft > 0.0f) {
        return;
    }
    _slideLeft = kSlideArenaTicks;
}

void Player::stopSlide() {
    // C# Kurt.StopSlide (Kurt.Slide.cs:107-114).
    _sliding = false;
    _slideVelocity = Vec2{0.0f, 0.0f};
    if (_mixer != nullptr) {
        _mixer->stopVoice(_slideVoice);
    }
    _slideVoice = 0;
    _slideSound = "";
}

void Player::slideAccel(const Vec2& push, float dt) {
    // C# Kurt.SlideAccel (Kurt.Slide.cs:116-119).
    _slideVelocity = Vec2{slideAxisAccel(_slideVelocity.x, push.x, dt),
                          slideAxisAccel(_slideVelocity.y, push.y, dt)};
}

bool Player::updateSlide(const Input& input, float delta) {
    // C# Kurt.UpdateSlide (Kurt.Slide.cs:138-198), in its order. False: it ended (or
    // never ran) and the ordinary step goes on.
    if (!_sliding) {
        return false;
    }
    if (health() == 0) {
        stopSlide();
        return false;
    }

    // 15 ticks after an arena change he falls.
    if (_slideLeft > 0.0f) {
        _slideLeft -= kTicks * delta;
        if (_slideLeft <= 0.0f) {
            stopSlide();
            setState(Kurt::Fall);
            return false;
        }
    }

    if (onFloor) {
        _slideNormal = Vec3{
            kSlideNormalKeep * _slideNormal.x + (1.0f - kSlideNormalKeep) * _floorNormal.x,
            kSlideNormalKeep * _slideNormal.y + (1.0f - kSlideNormalKeep) * _floorNormal.y,
            kSlideNormalKeepZ * _slideNormal.z + (1.0f - kSlideNormalKeepZ) * _floorNormal.z};
        _slidePush = Vec2{_slideNormal.x * kSlideSlope, _slideNormal.y * kSlideSlope};
        _slideAir = 0.0f;
        if (_slidePush.x * _slidePush.x + _slidePush.y * _slidePush.y < kSlideRest &&
            _slideVelocity.x == 0.0f && _slideVelocity.y == 0.0f) {
            // At rest he gets up (state 901 from K_BFLIP): K_BANG at its last frame, so
            // the next step's knock-down arm flips him straight into K_BFLIP.
            stopSlide();
            setState(Kurt::Knocked);
            _animationFrame = (float)frameCount(Kurt::Knocked);
            return true;
        }
    } else {
        _slideAir += kTicks * delta;
        if (_slideAir > kSlideAirTicks) {
            stopSlide();
            setState(Kurt::Fall);
            return false;
        }
    }

    slideAccel(_slidePush, delta);
    _cameraRoll.slide(_slideNormal, slideFacing(), delta);
    const float forward = input.axis(kKeyForward, kKeyBack);
    const float speed = slideSpeed(forward, delta);
    steer(input, speed, delta);
    slideMove(delta);
    verticalSpeed -= kSlideGravity * delta;
    verticalSpeed = std::max(verticalSpeed - kGravity * delta, -kMaxFallSpeed);
    fall(delta);
    updateTouched();
    updateSlideState(forward, delta);
    updateMuzzle(delta);
    return true;
}

float Player::slideSpeed(float forward, float delta) {
    // C# Kurt.SlideSpeed (Kurt.Slide.cs:202-229): the speed within the cap; forward
    // speeds up and raises the cap, back brakes and lowers it, without input it goes
    // back to 50.
    float speed = mathMin(length2(_slideVelocity), _slideCap);
    if (forward > 0.0f) {
        if (speed >= kSlideMinSpeed) {
            speed += kSlideAcceleration * forward * delta;
        }
        _slideCap = mathMin(_slideCap + kSlideCapRise * delta, kSlideCapMax);
    } else if (forward < 0.0f) {
        if (speed > kSlideMinSpeed) {
            speed = mathMax(speed + kSlideBrake * forward * delta, kSlideMinSpeed);
        }
        _slideCap = mathMax(_slideCap - kSlideCapFall * delta, kSlideCapMin);
    } else {
        _slideCap = moveToward(_slideCap, kSlideCap, kSlideCapRelax * delta);
    }
    return mathMin(speed, _slideCap);
}

/* @vfp-exempt R6 — the slide's heading from its velocity: atan2f is the oracle's
 * MathF.Atan2 and the conversion .NET's left-associative RadiansToDegrees; see
 * slideFacing for why the LUT would move the pinned path. measured n/a (corpus empty).
 * Verified 2026-11-20. */
void Player::steer(const Input& input, float speed, float delta) {
    // C# Kurt.Steer (Kurt.Slide.cs:232-241): Kurt faces where the slide goes, the turn
    // keys (or the frame's mouse, clamped) turn it, and the velocity is the new facing
    // at the speed.
    if (std::fabs(_slideVelocity.x) + std::fabs(_slideVelocity.y) > kSlideHeading) {
        yaw = atan2f(_slideVelocity.y, _slideVelocity.x) * 180.0f / 3.14159265358979323846f;
    }
    const float mouse = kSlideMouseTurn * delta;
    yaw += input.axis(kKeyTurnLeft, kKeyTurnRight) * kSlideTurn * delta -
           clampf(_slideMouseX * kMouseDegrees, -mouse, mouse);
    const Vec2 f = slideFacing();
    _slideVelocity = Vec2{f.x * speed, f.y * speed};
}

void Player::slideMove(float delta) {
    // C# Kurt.SlideMove (Kurt.Slide.cs:244-262): the slide's walk move; a wall that
    // stops it on an axis turns it along what Kurt moved, at the mean of the two speeds.
    const Vec3 planned{_slideVelocity.x * delta, _slideVelocity.y * delta, 0.0f};
    if (planned.x == 0.0f && planned.y == 0.0f) {
        return;
    }

    // Feet = Contact(space.Move(Feet, planned, Walk, WalkSlide, Nearby(Feet + planned))).Feet:
    // the arena and the solid objects nearby, as the walk move (Kurt.cs:225).
    const Vec3 start = feet;
    if (_space) {
        feet = fromVec(contact(_space->move(toVec(feet), toVec(planned), ArenaSpace::kMotionWalk,
                                            kWalkSlide,
                                            nearby(Vec3{feet.x + planned.x, feet.y + planned.y,
                                                        feet.z + planned.z})))
                           .feet);
    } else {
        feet.x += planned.x;
        feet.y += planned.y;
    }
    const Vec3 moved{feet.x - start.x, feet.y - start.y, feet.z - start.z};
    if (!slideStopped(planned.x, moved.x) && !slideStopped(planned.y, moved.y)) {
        return;
    }

    const Vec2 actual{moved.x, moved.y};
    const bool still = actual.x == 0.0f && actual.y == 0.0f;
    const Vec2 direction = still ? normalize2(_slideVelocity) : normalize2(actual);
    const float speed = length2(_slideVelocity) * kSlideWallShare +
                        length2(actual) / delta * (1.0f - kSlideWallShare);
    _slideVelocity = Vec2{direction.x * speed, direction.y * speed};
}

void Player::updateSlideState(float forward, float delta) {
    // C# Kurt.UpdateSlideState (Kurt.Slide.cs:265-300): K_SLIP once, then K_SLIDE,
    // K_FSLIDE or K_BSLIDE by the forward key; BUTBRAKE while braking, BUTSLIDE
    // otherwise, pitched up going forward.
    _stateTime += delta;
    _animationFrame += kTicks * delta;
    if (state() != Kurt::Slip || _animationFrame >= (float)frameCount(Kurt::Slip)) {
        const Kurt::State wanted = forward > 0.0f   ? Kurt::SlideFast
                                   : forward < 0.0f ? Kurt::SlideBrake
                                                    : Kurt::Slide;
        if (wanted != state()) {
            setState(wanted);
        }
    }

    const char* sound = state() == Kurt::SlideBrake ? kBrakeSound : kSlideSound;
    if (strcmp(sound, _slideSound) != 0) {
        if (_mixer != nullptr) {
            _mixer->stopVoice(_slideVoice);
            _slideVoice = _mixer->playLooped(sound);
        } else {
            _slideVoice = 0;
        }
        _slideSound = sound;
    }
    if (_mixer != nullptr) {
        _mixer->setPitch(_slideVoice, state() == Kurt::SlideFast ? kSlideFastPitch : 1.0f);
    }
}

} // namespace mdk

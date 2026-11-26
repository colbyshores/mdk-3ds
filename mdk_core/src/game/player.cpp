// ============================================================================
// player.cpp — the movement half of Kurt.cs. See player.h.
//
// ONE TRANSLATION UNIT IN FIVE FILES (split 2026-11-25 so it reads in pieces):
// this file holds the tuning constants, the helpers and the per-frame update,
// and #includes player_state.inc, player_vertical.inc, player_platforms.inc and
// player_slide.inc at the bottom, in the original order. The .inc files are not
// headers -- no guards, no includes -- and share this file's constants and
// helpers; the split moved code and nothing else (the object code is unchanged).
// player_ledge.cpp and player_sniper.cpp are separate translation units.
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
// Upstream a91c0870 (C# 1.2.0): the jumps' frames (0x4646a4). The first ones play one a
// tick, then frame n waits for the vertical speed to drop to kJumpSpeeds[n] (0x492030;
// the running jump's 0x492074 is the same to frame 12); past the last comes the fall.
const int kJumpTimedFrames = 10;
const int kJumpFrames = 16;
const int kRunJumpTimedFrames = 7;
const int kRunJumpFrames = 12;
const float kJumpSpeeds[kJumpFrames] = {25.0f, 26.0f, 27.0f, 20.0f, 15.0f, 13.0f, 9.0f, 6.6f,
                                        4.0f,  0.2f,  -3.0f, -6.3f, -10.0f, -14.0f, -17.0f, -20.0f};
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
// Upstream 3f9315c5 (C# 1.2.0 Kurt.GlueMaxSpeed, FallGrace): the glue's fastest drop
// (u/s), under the hard landing's so the landing after it does not hurt; and a running
// or standing Kurt starts the fall animation only after this long (s) off the floor --
// down a steep slope his feet leave it for a step or two at a time.
const float kGlueMaxSpeed = 90.0f;
const float kFallGrace = 0.15f;
// Landing faster than this is a hard landing (10 damage + K_CRASHL, which land
// with health). The C# gates the creep snap behind it, so the constant is
// needed for the position half now.
const float kHardLandingSpeed = -100.0f;
const int kHardLandingDamage = 10;   // C# HardLandingDamage
// Kurt dies this far below his arena's lowest point (C# FallOutDepth).
const float kFallOutDepth = -50.0f;
// Upstream 3bb211b6 (C# 1.2.4 Kurt.SunkMin/SunkMax): sunk into a floor between these,
// Kurt climbs onto it -- from what the walking box steps over (its 0.5 lift) to his waist.
const float kSunkMin = 0.5f;
const float kSunkMax = 5.0f / 2.0f;
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
    // C# Kurt.cs:166-174 (package #9): a ride's step (the snowboard, the bomber) owns
    // the step -- no knock-down, chute or ledges while riding; the noclip step above
    // comes first. The snowboard (Rides::mountBoard, package #10) and the XE bomber
    // (scripts/bomber.cpp, package #11) set the hook.
    if (_rideHook.step != nullptr) {
        combat_.setKnockDamage(0.0f);
        stopChuteSound();
        _keeper = nullptr;
        _rideHook.step(_rideHook.context, input, delta);
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
    if (state() == Kurt::Hang || state() == Kurt::HardLand) {
        // C# Kurt.cs:190-195, states from 800 on: no walking (damp_control). A hard
        // landing owns the step until K_CRASHL is done, and a ledge's climb until
        // K_HANG is (C# UpdateSpecial's two arms). It also stops the ordinary walk
        // state machine.
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
    // C# Kurt.cs:207: no strafing in the XD2 (package #9).
    const float strafeIn =
        walkMode == WalkRiding ? 0.0f : input.axis(kKeyStrafeRight, kKeyStrafeLeft);
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

    // C# Kurt.cs:220: where the step started, for the ledge grab's hands (package #12).
    const Vec3 before = feet;
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

    // C# Kurt.cs:228-232 (package #9): in the XD2 the walk move above is the whole of
    // it -- gravity and the fall, the ride's keys, and he stands (hidden). No jump, no
    // keeper, no touches, no items, no gun.
    if (walkMode == WalkRiding) {
        rideWalker(input, forward, delta);
        return;
    }

    updateVertical(input, move, delta);
    // C# Kurt.cs:235-236: on the keeping platform's XY, then the objects he touches.
    stayOnKeeper();
    updateTouched();
    // C# Kurt.cs:237: `if (Current == State.HardLand || GrabLedge(before, forward))
    // return;` -- after a hard landing, or a ledge grabbed at the end of this step's
    // fall (game/player_ledge.cpp, package #12), no items, gun or state machine; the
    // landing or the grab set the state, and the next step runs UpdateSpecial instead.
    if (state() == Kurt::HardLand || grabLedge(before, forward)) {
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
    // C# Kurt.MuzzleOffset (Kurt.Combat.cs:358-363): on the board (state 201, the K_SURF
    // pose, package #10) the flash is drawn at (-42, 12) whatever the state.
    if (hasPose && poseName != nullptr && strcmp(poseName, "K_SURF") == 0) {
        *x = -42;
        *y = 12;
        return true;
    }
    // C# Kurt.MuzzleOffsets (Kurt.Combat.cs:57-65): the states that don't show the gun
    // firing by themselves.
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

#include "player_state.inc"

#include "player_vertical.inc"

#include "player_platforms.inc"

#include "player_slide.inc"

} // namespace mdk

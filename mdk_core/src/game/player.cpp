// ============================================================================
// player.cpp — the movement half of Kurt.cs. See player.h.
// ============================================================================

#include "game/player.h"

#include <algorithm>
#include <cmath>

#include "util/math_lut.h"

namespace mdk {

namespace {

// The C# Kurt.cs constants, converted to per-second (the original's per-tick
// numbers times Ticks for rates and times Ticks^2 for accelerations).
const float kTicks = 30.0f;
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
// while running and firing. RunFire is not reachable from this movement-only
// Player (nothing writes Firing yet), but its frames are ported with the rest of
// Footstep so the firing slice cannot quietly use the walking pair.
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
    const float radians = yaw * 3.14159265358979323846f / 180.0f;  // float.DegreesToRadians (left-assoc)
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
    // C# Kurt.Update clears ChuteClosing before every branch, including the
    // death branch (Kurt.cs:140, ahead of the `Current == State.Dead` return).
    // The clear is unconditional on purpose, and it is NOT pinned to be so:
    // `dead` and `_chuteClosing` can never be true at the same top-of-step, because
    // the only setter of `_chuteClosing` is the `!onFloor` Chute -> Fall transition
    // below and `dead` is set only with `onFloor`, so guarding this with `if (!dead)`
    // is unobservable dead code, not a behaviour change (review-9 R9-5; the scenario
    // that would have to distinguish them was built and matches byte for byte).
    _chuteClosing = false;
    if (dead) {
        // The C# UpdateDeath advances the death beat. Fall keeps a corpse that
        // fell out of the arena at its death point (the post-move fall-out
        // assignment), and a corpse on a floor alternates OnFloor as the
        // oracle's Fall does.
        stopChuteSound();
        updateDeath(delta);
        return;
    }
    // C# Kurt.Update calls UpdateChuteSound before the Hang/HardLand branch, so a
    // crashing Kurt's chute still closes and stops flapping.
    updateChuteSound();
    if (_state == Kurt::HardLand) {
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

    const float forward = input.axis(kKeyForward, kKeyBack);
    const float strafe = input.axis(kKeyStrafeRight, kKeyStrafeLeft);
    const float air = onFloor ? 1.0f : kAirControl;
    forwardSpeed = accelerate(forwardSpeed, forward, turbo, 1.0f, air, delta);
    strafeSpeed = accelerate(strafeSpeed, strafe, turbo, air, air, delta);

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
    if (_state == Kurt::HardLand) {
        return;
    }
    updateState(forward, strafe, delta);
}

void Player::updateState(float forward, float strafe, float delta) {
    _stateTime += delta;

    // C# UpdateCombatState: health zero on a floor is the death state. The
    // combat knock-down, get-up and throw states are separate deferred
    // subsystems and cannot be entered by this movement-only Player.
    if (health() == 0 && onFloor) {
        setState(Kurt::Dead);
        return;
    }

    // The C# has no missing-animation guard: a missing animation has count 0,
    // so `done` is `AnimationFrame >= -1`. A missing animation is a broken
    // archive, not a state; the comparison is kept exactly as the oracle's.
    const bool done = _animationFrame >= (float)(frameCount(_state) - 1);
    if (!onFloor) {
        if (chuteOpen) {
            setState(Kurt::Chute);
        } else if (_state == Kurt::Chute) {
            // The closing frame is shown once, then the fall starts.
            _chuteClosing = true;
            setState(Kurt::Fall);
            return;
        } else if (verticalSpeed > 0.0f &&
                   _state != Kurt::Jump && _state != Kurt::RunJump && _state != Kurt::Fall) {
            setState(std::fabs(forwardSpeed) > kMovingSpeed ? Kurt::RunJump : Kurt::Jump);
        } else if (verticalSpeed < kFallStartSpeed &&
                   ((_state != Kurt::Jump && _state != Kurt::RunJump) || done)) {
            setState(Kurt::Fall);
        }
    } else if (_state == Kurt::Jump || _state == Kurt::RunJump || _state == Kurt::Fall ||
               _state == Kurt::Chute) {
        playSound("LAND");
        setState(Kurt::Land);
    } else if (_state == Kurt::Land && !done && forward == 0.0f && strafe == 0.0f) {
        // The landing animation keeps running while he stands still.
    } else if (forward != 0.0f || std::fabs(forwardSpeed) > kMovingSpeed) {
        setState(Kurt::Run);
    } else if (strafe != 0.0f || std::fabs(strafeSpeed) > kMovingSpeed) {
        setState(Kurt::Side);
    } else if (std::fabs(turnRate) > kMovingSpeed) {
        setState(Kurt::Turn);
    } else if (_state == Kurt::Idle && !done) {
        // The idle animation keeps running.
    } else if (_state == Kurt::Still && _stateTime > kIdleDelay) {
        setState(Kurt::Idle);
    } else if (_state != Kurt::Still) {
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
}

void Player::animate(float delta) {
    if (_state == Kurt::Run || _state == Kurt::RunFire) {
        const int32_t count = frameCount(_state);
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
    } else if (_state == Kurt::Turn) {
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

    const bool firing = _state == Kurt::RunFire;
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
    if (chuteOpen && _state == Kurt::Chute && _animationFrame >= (float)kChuteOpenFrames) {
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
    if (state == _state && _stateTime > 0.0f) {
        return;
    }
    _state = state;
    _stateTime = 0.0f;
    _animationFrame = 0.0f;
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
    if (dead) {
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

void Player::setHealth(int health) {
    // C# SetHealth: max(health, 0) -- on the one health, which lives on the
    // combat half this Player owns.
    combat_.setHealth(health);
}

void Player::damage(int damage) {
    // C# Damage with Normal difficulty (Easy/Hard scaling lands with difficulty).
    // The death transition is the caller's (health 0 on the floor), not here.
    combat_.damageHealth(damage);
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

    // UpdateUpdraft is not ported (no fans/wind zones yet). When it lands it
    // also caps upward speed at 40 u/s on leaving an updraft (Kurt.Updraft.cs);
    // today upward speed never exceeds the 40 u/s jump, so the omission is
    // inert -- add the cap with the fan subsystem.
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
        // guard around the damage call is NOT observable here -- Player::damage
        // floors at 0 exactly like Kurt.Damage's Math.Max(Health - damage, 0) --
        // and it is kept because the C#'s Damage also raises HurtFlash and adds to
        // KnockDamage, which Player does not model yet (they live on mdk::Kurt).
        //
        // What else the C#'s HardLanding does that this does not: Kurt.cs:589 also
        // calls StopFiring() (leave the sniper, stop the chain-gun voice, drop the
        // muzzle flash) inside the same guard. Nothing in the port fires -- no
        // firing, sniper or muzzle state exists yet (grep finds no Firing member) --
        // so the omission is inert today and lands with the firing slice.
        if (health() > 0) {
            damage(kHardLandingDamage);
        }
        if (health() > 0 && _state != Kurt::Dead && _state != Kurt::Knocked &&
            _state != Kurt::GetUp) {
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

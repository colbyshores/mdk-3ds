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
    if (dead) {
        return; // C# Kurt.Update returns on State.Dead: a dead Kurt is frozen.
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
    // C# Kurt.cs:221-224: the move captures the current (pre-drain)
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
    // C# SetHealth: max(health, 0).
    this->health = health > 0 ? health : 0;
}

void Player::damage(int damage) {
    // C# Damage with Normal difficulty (Easy/Hard scaling lands with difficulty).
    // The death transition is the caller's (health 0 on the floor), not here.
    health = health - damage > 0 ? health - damage : 0;
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

    fallMove(delta);
    // UpdateUpdraft is not ported (no fans/wind zones yet). When it lands it
    // also caps upward speed at 40 u/s on leaving an updraft (Kurt.Updraft.cs);
    // today upward speed never exceeds the 40 u/s jump, so the omission is
    // inert -- add the cap with the fan subsystem.
    // The fall-out death (damp_gravity 0x469efc): 50 below his arena's lowest
    // point Kurt dies, and counts as on a floor so the death plays. God mode is
    // not ported.
    float bottom = 0.0f;
    if (_space && _space->bottom(toVec(feet), &bottom) && feet.z <= bottom + kFallOutDepth) {
        health = 0;
        verticalSpeed = 0.0f;
        onFloor = true;
    }
    // health 0 on the floor is death (C# "health 0 --on the floor--> DEAD"). A
    // hard landing lands the same tick and fall-out forces OnFloor, so both die
    // now; a future mid-air damage source would wait for the landing, as in the C#.
    if (health == 0 && onFloor) {
        dead = true;
    }
}

void Player::updateAir(bool jumpHeld, float delta) {
    if (_jumpTicksLeft > 0) {
        _jumpTicksLeft = std::max(0, _jumpTicksLeft - (int)std::ceil(delta * kTicks));
    }

    if (!chuteOpen && jumpHeld && verticalSpeed < kFallStartSpeed) {
        chuteOpen = true;
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
        // C# HardLanding(): 10 damage (no knock-down). The K_CRASHL animation and
        // the HardLand state are not ported; only the damage matters here. The
        // damage can bring health to 0, and this landing sets OnFloor, so the
        // death check in updateVertical fires the same tick.
        if (health > 0) {
            damage(kHardLandingDamage);
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

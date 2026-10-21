// ============================================================================
// player.cpp — the horizontal half of Kurt.cs. See player.h.
// ============================================================================

#include "game/player.h"

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
const float kTurnSpeed = 4.0f * kTicks;                   // 120 deg/s
const float kTurnSpeedTurbo = 6.0f * kTicks;              // 180 deg/s
const float kTurnAcceleration = 0.9f * kTicks * kTicks;   // 810 deg/s^2
const float kTurnAccelerationTurbo = 1.3f * kTicks * kTicks;  // 1170 deg/s^2
const float kTurnFrictionFast = 1.6f * kTicks * kTicks;   // 1440 deg/s^2
const float kTurnFrictionSlow = 0.55f * kTicks * kTicks;  // 495 deg/s^2
const float kMouseDegrees = 0.15f;                        // degrees per mouse count

const float kMinPitch = -60.0f;
const float kMaxPitch = 90.0f;

float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

float signf(float v) {
    return (v > 0.0f) - (v < 0.0f);
}

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
    const float radians = yaw * (3.14159265358979323846f / 180.0f);
    return Vec3{fastCos(radians), fastSin(radians), 0.0f};
}

Vec3 Player::right() const {
    const Vec3 f = facing();
    return Vec3{f.y, -f.x, 0.0f};
}

void Player::update(const Input& input, float delta) {
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
    forwardSpeed = accelerate(forwardSpeed, forward, turbo, 1.0f, 1.0f, delta);
    strafeSpeed = accelerate(strafeSpeed, strafe, turbo, 1.0f, 1.0f, delta);

    // Horizontal move on the flat plane (the BSP move lands with collision).
    const Vec3 f = facing();
    const Vec3 r = right();
    feet.x += (f.x * forwardSpeed + r.x * strafeSpeed) * delta;
    feet.y += (f.y * forwardSpeed + r.y * strafeSpeed) * delta;
}

void Player::mouseLook(Input& input) {
    yaw += -input.takeMouseX() * kMouseDegrees;
    lookPitch += input.takeMouseY() * kMouseDegrees;
    // Keep the total view pitch (arena + look) inside the C#'s clamp.
    lookPitch = clampf(lookPitch, kMinPitch - arenaPitch, kMaxPitch - arenaPitch);
}

} // namespace mdk

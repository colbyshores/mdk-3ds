// ============================================================================
// snowboard.cpp — C# src/Mdk.Game/Scripts/Snowboard.cs, operation for operation
// (package #10). See snowboard.h for the ride's shape; Rides (rides.cpp) mounts
// and dismounts it, Player (game/player.cpp) carries Kurt's half (glide,
// getOffBoard, the RideHook, the K_SURF muzzle, the camera roll's follow).
//
// THE TRIG. Every sin/cos/atan2 here is the libm one, not math_lut.h's: the board
// is gameplay (its heading decides where Kurt slides, its pitch whether he is on
// the ground and how fast he goes), it runs once per step for the one ride (cold
// by §2: not a per-object loop, not in a profile's top 15), and glibc's cosf/sinf/
// atan2f give the C# oracle's MathF results bit for bit on the goldens' machine, so
// golden/L4_board_*.txt pin the run exactly. The pins below say so per site.
// ============================================================================

#include "scripts/snowboard.h"

#include <math.h>

#include <string>
#include <vector>

#include "audio/sound_mixer.h"
#include "game/player.h"
#include "scripts/object.h"
#include "scripts/rides.h"
#include "scripts/script_runtime.h"
#include "util/math_minmax.h"

namespace mdk {
namespace scripts {

const char* const Snowboard::kSurfFrames = "K_SURF";
const char* const Snowboard::kJumpFrames = "K_SURFJ";

namespace {

// C# Snowboard's constants (Snowboard.cs:19-92). Kurt.Ticks is 30.
const float kTicks = 30.0f;
// Steering: +-30 deg at 4 deg/tick (reached over 15 ticks on the ground), back at 3 deg/tick.
const float kSteerMax = 30.0f;
const float kSteerRate = 4.0f;
// The mouse: 4 x clamp(dx / dt, +-4) deg/tick (input_read_axes 0x408334).
const float kMouseUnits = 0.05f;
const float kMouseLimit = 4.0f;
const float kSteerRampTicks = 15.0f;
const float kSteerReturn = 3.0f;
// Sideways speed L = -S*V/96, braked in the air by 0.0055556 per tick. The C# folds
// the constant in float, one rounding per operation, as this does.
const float kSideFactor = -0.8333f * 0.0125f;
const float kSideAirBrake = 0.0055556f;
// Speeds (units/tick): cruise 1.5, up to 2.5 with the key (or locked), down to 1.1667
// braking or uphill, up to 2.6667 downhill; 0.05 per tick^2, decay above cruise.
const float kCruise = 1.5f;
const float kFast = 2.5f;
const float kSlow = 1.16667f;
const float kDownhill = 2.66667f;
const float kAcceleration = 0.05f;
const float kDecay = 0.0027778f;
const float kSlopeAcceleration = 0.066667f;
// The thrown board falls by 64 u/s^2.
const float kGravity = 64.0f;
// The board's ends are 4 units from its middle; ground up to 3 units above them counts.
const float kBoardHalf = 4.0f;
const float kRayHeight = 3.0f;
const float kRayBelow = 0.01f;
// The board pitches towards the ground at 90 deg/s, between -45 and 45.
const float kPitchRate = 90.0f;
const float kPitchLimit = 45.0f;
const float kHalfTurn = 180.0f;
const float kFullTurn = 360.0f;
// Carving tilts the nose up by a quarter of |S|.
const float kCarveTilt = 0.25f;
// Jump: 28.5 u/s up, the key counts up to 12 ticks before landing; not at the end of CMEAT_1.
const float kJumpSpeed = 28.5f;
const float kJumpLift = 1.0f;
const int32_t kJumpBuffer = 12;
const float kJumpHeld = 50.0f;
const char* const kNoJumpArena = "CMEAT_1";
const float kNoJumpY = 2131.0f;
// Heading: 4/3 deg/tick +- S/30 on the ground, 0.4 in the air; Kurt's yaw follows at 360 deg/s.
const float kHeadingRate = 1.33333f;
const float kHeadingCarve = 1.33333f / 30.0f * 0.75f;
const float kHeadingAir = 0.4f;
const float kKurtTurn = 360.0f;
// The path search's steps (path frames), and the look ahead for its heading.
const float kPathSteps[] = {1.0f, 0.2f};
const float kPathAhead = 0.2f;
// Kurt nudged back towards the path when stuck (moving under 0.5 units/tick at speed).
const float kUnstickSpeed = 0.5f;
const float kUnstickStep = 2.0f;
// The board sits 0.25 below Kurt's feet; thrown at 1.1 x his speed.
const float kBoardDrop = 0.25f;
const float kThrowFactor = 1.1f;
// Sounds: SKITURN beyond 22.5 deg, SKILAND after 15 ticks in the air, SKI stops after 7.
const float kTurnSound = 22.5f;
const float kLandTicks = 15.0f;
const float kQuietTicks = 7.0f;
const char* const kSounds[] = {"SKI", "SKITURN", "SKILAND"};
// Ramming: objects without these flags and with 0 < health < 65000 die; Kurt takes 5.
const int32_t kRamSpared = 0x40304000;
const int32_t kRamDamage = 5;
// K_SURF loops; K_SURFJ holds on frame 5 in the air, lands from frame 6 to 10.
constexpr int32_t kSurfLoop = 8;
const int32_t kJumpHoldFrame = 5;
const int32_t kJumpLandFrame = 6;
const float kLateLanding = 3.0f;
const float kLandEnd = 11.0f;
// The camera pivot dips by 0.2 a frame while K_SURFJ takes off (Kurt.DefaultPivot 4.5).
const float kPivotDip = 0.2f;
const float kDefaultPivot = 4.5f;
const float kPi = 3.14159265358979323846f;

// .NET float.DegreesToRadians / RadiansToDegrees: (x * pi) / 180 and (x * 180) / pi,
// two roundings each, left-associative (follow_camera.h's spelling).
float degreesToRadians(float degrees) { return degrees * kPi / 180.0f; }
float radiansToDegrees(float radians) { return radians * 180.0f / kPi; }

// C# Math.Clamp(float): below the minimum, the minimum; above the maximum, the
// maximum; else the value (a NaN passes through).
float clamp(float value, float low, float high) {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

// C# MathF.Sign (a NaN throws in the C#; no input here is one).
float sign(float value) { return value > 0.0f ? 1.0f : (value < 0.0f ? -1.0f : 0.0f); }

// C# Snowboard.MoveToward (Snowboard.cs:501).
float moveToward(float from, float to, float step) {
    return fabsf(to - from) <= step ? to : from + sign(to - from) * step;
}

// C# Snowboard.Wrap360 / WrapHalf (Snowboard.cs:504-506): C#'s float % is fmod.
float wrap360(float degrees) { return fmodf(fmodf(degrees, kFullTurn) + kFullTurn, kFullTurn); }
float wrapHalf(float degrees) { return wrap360(degrees + kHalfTurn) - kHalfTurn; }

// C# Snowboard.TurnTowards (Snowboard.cs:418).
float turnTowards(float from, float to, float step) {
    return wrap360(from + clamp(wrapHalf(to - from), -step, step));
}

// .NET Vector2.Length: Sqrt(X*X + Y*Y).
float length2(float x, float y) { return sqrtf(x * x + y * y); }

}  // namespace

Snowboard::Snowboard(ScriptRuntime& runtime, Player& kurt, MdkObject* board)
    : runtime_(runtime), kurt_(kurt), board_(board) {
    // C# Snowboard(runtime, board) (Snowboard.cs:113-120).
    heading_ = board->yaw;
    pathHeading_ = board->yaw;
}

void Snowboard::step(void* context, const Input& input, float delta) {
    static_cast<Snowboard*>(context)->update(input, delta);
}

void Snowboard::update(const Input& input, float delta) {
    // C# Snowboard.Update (Snowboard.cs:126-170).
    const float dt = delta * kTicks;
    const bool controls = (board_->flags & Rides::kFlagLocked) == 0;
    pitch_ -= fabsf(steer_) * kCarveTilt;

    steer_ = steer(steer_, controls ? turnInput(input, dt) : 0.0f, grounded_, &turnTicks_, dt);
    if (grounded_) {
        updateSpeed(controls ? input.axis(kKeyForward, kKeyBack) : 0.0f, controls, dt);
    } else {
        side_ = moveToward(side_, 0.0f, kSideAirBrake * dt);
    }

    // Forward and sideways, sticking to downhill floors, falling.
    /* @vfp-exempt R6 — the board's heading: the C#'s MathF.Cos/Sin, which glibc
     * cosf/sinf match bit for bit on the goldens' machine; it moves Kurt (gameplay),
     * once per step for the one ride (cold). measured n/a (corpus empty). Verified 2026-11-20. */
    const float h = degreesToRadians(heading_);
    const float c = cosf(h);
    const float s = sinf(h);
    const Vec3 move{c * speed_ + s * side_, s * speed_ + -c * side_, 0.0f * speed_ + 0.0f * side_};
    const Vec3 previous = kurt_.feet;
    kurt_.glide(Vec3{move.x * kTicks, move.y * kTicks, move.z * kTicks}, delta);

    updatePitch(delta);
    if (grounded_) {
        speed_ = slopeSpeed(speed_, pitch_, dt);
    }

    jump(input, controls, dt);
    updateSounds(dt);
    if (pose_ == PoseJump) {
        kurt_.stopFiring();
    } else {
        kurt_.updateGun(input, delta);
    }

    followPath(previous, dt);
    turn(delta, dt);
    place(delta);
    ram();
    animate(dt);
}

void Snowboard::getOff() {
    // C# Snowboard.GetOff (Snowboard.cs:174-188).
    audio::SoundMixer& mixer = runtime_.mixer();
    for (size_t i = 0; i < sizeof(kSounds) / sizeof(kSounds[0]); i++) {
        mixer.stop(kSounds[i]);
    }

    /* @vfp-exempt R6 — the thrown board's velocity from Kurt's yaw: MathF.Cos/Sin in the
     * C#, glibc's bit for bit here; once, when he gets off. measured n/a (corpus empty).
     * Verified 2026-11-20. */
    const float y = degreesToRadians(kurt_.yaw);
    const float cy = cosf(y);
    const float sy = sinf(y);
    const Vec3 throwing{speed_ * cy + side_ * sy, speed_ * sy - side_ * cy, 0.0f};
    // (throwing * Ticks * ThrowFactor) with { Z = VerticalSpeed }: left-associative.
    board_->velocity = Vec3{throwing.x * kTicks * kThrowFactor, throwing.y * kTicks * kThrowFactor,
                            kurt_.verticalSpeed};
    board_->gravity = kGravity;
    board_->friction = 0.0f;
    board_->position = kurt_.feet;
    kurt_.getOffBoard(speed_ * kTicks, side_ * kTicks);
}

float Snowboard::steer(float steer, float right, bool grounded, float* turnTicks, float dt) {
    // C# Snowboard.Steer (Snowboard.cs:193-209): the carve angle S (positive left).
    if (right == 0.0f) {
        *turnTicks = 0.0f;
        return moveToward(steer, 0.0f, kSteerReturn * dt);
    }

    *turnTicks += dt;
    if ((right > 0.0f && steer > 0.0f) || (right < 0.0f && steer < 0.0f)) {
        return 0.0f;
    }

    const float rate =
        right * (grounded && *turnTicks < kSteerRampTicks ? *turnTicks * 2.0f / kTicks : 1.0f);
    return clamp(steer - rate * dt, -kSteerMax, kSteerMax);
}

void Snowboard::updateSpeed(float forward, bool controls, float dt) {
    // C# Snowboard.UpdateSpeed (Snowboard.cs:213-221): carving slides sideways.
    if (steer_ != 0.0f) {
        side_ = steer_ * kSideFactor * speed_;
    }

    speed_ = accelerate(speed_, controls ? forward : 1.0f, dt);
}

float Snowboard::accelerate(float speed, float forward, float dt) {
    // C# Snowboard.Accelerate (Snowboard.cs:224-237).
    if (forward > 0.0f) {
        return speed < kFast ? mathMin(speed + kAcceleration * dt, kFast) : speed;
    }

    if (forward < 0.0f) {
        return speed > kSlow ? mathMax(speed - kAcceleration * dt, kSlow) : speed;
    }

    return speed < kCruise ? mathMin(speed + kAcceleration * dt, kCruise)
                           : mathMax(speed - kDecay * dt, kCruise);
}

float Snowboard::slopeSpeed(float speed, float pitch, float dt) {
    // C# Snowboard.SlopeSpeed (Snowboard.cs:240-254): nose down (pitch above 180)
    // speeds up to 2.6667, nose up slows down to 1.1667.
    /* @vfp-exempt R6 — MathF.Sin of the board's pitch, glibc's sinf bit for bit; the
     * speed is gameplay, once per step for the one ride (cold). measured n/a (corpus
     * empty). Verified 2026-11-20. */
    const float k = sinf(degreesToRadians(pitch));
    if (pitch > kHalfTurn && speed < kDownhill) {
        return mathMin(speed - k * kSlopeAcceleration * dt, kDownhill);
    }

    if (pitch > 0.0f && pitch <= kHalfTurn && speed > kSlow) {
        return mathMax(speed - k * kSlopeAcceleration * dt, kSlow);
    }

    return speed;
}

void Snowboard::updatePitch(float delta) {
    // C# Snowboard.UpdatePitch (Snowboard.cs:258-289): the board's pitch from the ground
    // under its ends, turning at 90 deg/s; any ground under the board means grounded.
    const Vec3 p = kurt_.feet;
    /* @vfp-exempt R6 — the board's ends along H + S and its pitch: MathF.Cos/Sin/Atan2
     * in the C#, glibc's bit for bit; the pitch decides grounded and the slope speed
     * (gameplay), once per step (cold). measured n/a (corpus empty). Verified 2026-11-20. */
    const float yaw = degreesToRadians(heading_ + steer_);
    const Vec3 forward{cosf(yaw) * kBoardHalf, sinf(yaw) * kBoardHalf, 0.0f * kBoardHalf};
    Vec3 front{};
    Vec3 back{};
    const bool hasFront = ground(Vec3{p.x + forward.x, p.y + forward.y, p.z + forward.z}, &front);
    const bool hasBack = ground(Vec3{p.x - forward.x, p.y - forward.y, p.z - forward.z}, &back);
    grounded_ = hasFront || hasBack;
    if (!grounded_) {
        return;
    }

    Vec3 v{};
    if (hasFront && hasBack) {
        v = Vec3{front.x - back.x, front.y - back.y, front.z - back.z};
    } else if (hasFront) {
        v = Vec3{front.x - p.x, front.y - p.y, front.z - p.z};
    } else {
        v = Vec3{p.x - back.x, p.y - back.y, p.z - back.z};
    }
    float target = wrap360(radiansToDegrees(atan2f(v.z, length2(v.x, v.y))));
    if (pitch_ > kPitchLimit && pitch_ < kHalfTurn) {
        target = kPitchLimit;
    } else if (pitch_ >= kHalfTurn && pitch_ < kFullTurn - kPitchLimit) {
        target = kFullTurn - kPitchLimit;
    }

    const float step = kPitchRate * delta;
    pitch_ = wrap360(wrapHalf(pitch_) + clamp(wrapHalf(target - pitch_), -step, step));
}

bool Snowboard::ground(const Vec3& end, Vec3* point) {
    // C# Snowboard.Ground (Snowboard.cs:292): the ground under a board end, up to 3
    // units above it -- ScriptRuntime.Raycast's point, or null.
    return runtime_.raycastPoint(Vec3{end.x + 0.0f, end.y + 0.0f, end.z + kRayHeight},
                                 Vec3{end.x - 0.0f, end.y - 0.0f, end.z - kRayBelow}, point);
}

void Snowboard::jump(const Input& input, bool controls, float dt) {
    // C# Snowboard.Jump (Snowboard.cs:296-317): on the ground, within 12 ticks of
    // pressing the key; not at the end of CMEAT_1.
    const bool blocked = runtime_.currentArena() == kNoJumpArena && kurt_.feet.y >= kNoJumpY;
    if (!controls || !input.isDown(kKeyJump) || blocked) {
        jumpTicks_ = 0.0f;
        return;
    }

    if (grounded_ && jumpTicks_ <= (float)kJumpBuffer) {
        kurt_.verticalSpeed = kJumpSpeed;
        kurt_.feet = Vec3{kurt_.feet.x + 0.0f, kurt_.feet.y + 0.0f, kurt_.feet.z + kJumpLift};
        kurt_.onFloor = false;
        grounded_ = false;
        jumpTicks_ = kJumpHeld;
        pose_ = PoseJump;
        frame_ = 0.0f;
    }

    jumpTicks_ += dt;
}

void Snowboard::updateSounds(float dt) {
    // C# Snowboard.UpdateSounds (Snowboard.cs:319-349).
    audio::SoundMixer& mixer = runtime_.mixer();
    if (!grounded_) {
        airTicks_ += dt;
        if (airTicks_ >= kQuietTicks && mixer.isPlaying("SKI")) {
            mixer.stop("SKI");
            mixer.stop("SKITURN");
        }

        return;
    }

    if (airTicks_ >= kLandTicks) {
        mixer.play("SKILAND", audio::SoundMixer::Restart);
    }

    airTicks_ = 0.0f;
    mixer.play("SKI", audio::SoundMixer::Once);
    if (fabsf(steer_) > kTurnSound) {
        mixer.play("SKITURN", audio::SoundMixer::Once);
    } else {
        mixer.stop("SKITURN");
    }
}

void Snowboard::followPath(const Vec3& previous, float dt) {
    // C# Snowboard.FollowPath (Snowboard.cs:353-394): the path time moves on to the
    // point nearest to Kurt, the direction there is the path's heading; a stuck Kurt
    // is nudged towards the path.
    if (board_->path == 0) {
        return;
    }

    const float last =
        (float)runtime_.pathKeyFrame(board_->path, runtime_.pathKeyCount(board_->path) - 1);
    float t = board_->path_time;
    if (t >= last) {
        return;
    }

    const float kurtX = kurt_.feet.x;
    const float kurtY = kurt_.feet.y;
    const float start = t;
    for (size_t i = 0; i < sizeof(kPathSteps) / sizeof(kPathSteps[0]); i++) {
        const float step = kPathSteps[i];
        /* @vfp-exempt T1,T2 — the C#'s path search, ported as it is: it walks the board's
         * path forward while each step comes closer to Kurt, a few steps per tick from
         * the last tick's time, bounded by the path's last frame; a closed form would not
         * be the oracle's (golden/L4_board_*.txt pin the path time). measured n/a
         * (corpus empty). Verified 2026-11-20. */
        while (t + step < last && distance(t + step, kurtX, kurtY) < distance(t, kurtX, kurtY)) {
            t += step;
        }
    }

    if (t == start) {
        return;
    }

    const Vec3 here = pathPoint(start);
    const Vec3 ahead = pathPoint(t + kPathAhead);
    board_->path_time = t;
    // C# (int)MathF.Round(t): to nearest, ties to even (rintf in the default mode).
    board_->path_stop = (int32_t)rintf(t);
    /* @vfp-exempt R6 — the path's heading, MathF.Atan2 in the C#, glibc's atan2f bit for
     * bit; it steers the board (gameplay), once per step (cold). measured n/a (corpus
     * empty). Verified 2026-11-20. */
    pathHeading_ = radiansToDegrees(atan2f(ahead.y - here.y, ahead.x - here.x));
    const float moved = length2(kurt_.feet.x - previous.x, kurt_.feet.y - previous.y);
    if (speed_ >= kSlow && dt > 0.0f && moved / dt < kUnstickSpeed) {
        // Vector2.Normalize(here - kurt) * UnstickStep: v / v.Length(), then the step.
        const float nx = here.x - kurtX;
        const float ny = here.y - kurtY;
        const float n = length2(nx, ny);
        kurt_.feet = Vec3{kurt_.feet.x + nx / n * kUnstickStep, kurt_.feet.y + ny / n * kUnstickStep,
                          kurt_.feet.z + 0.0f};
    }
}

Vec3 Snowboard::pathPoint(float t) {
    // C# Snowboard.PathPoint (Snowboard.cs:396).
    const Vec3 p = runtime_.pathPosition(board_->path, t);
    return Vec3{p.x + board_->path_origin.x, p.y + board_->path_origin.y,
                p.z + board_->path_origin.z};
}

float Snowboard::distance(float t, float kurtX, float kurtY) {
    // C# Snowboard.Distance (Snowboard.cs:398-402): Vector2.DistanceSquared.
    const Vec3 p = pathPoint(t);
    const float dx = p.x - kurtX;
    const float dy = p.y - kurtY;
    return dx * dx + dy * dy;
}

void Snowboard::turn(float delta, float dt) {
    // C# Snowboard.Turn (Snowboard.cs:405-416): the heading turns towards the path's
    // (faster carving into the turn); Kurt faces the heading.
    float rate = kHeadingAir;
    if (grounded_) {
        const float d = wrapHalf(pathHeading_ - heading_);
        rate = kHeadingRate + (d >= 0.0f ? steer_ : -steer_) * kHeadingCarve;
    }

    heading_ = turnTowards(heading_, pathHeading_, mathMax(rate, 0.0f) * dt);
    kurt_.yaw = turnTowards(kurt_.yaw, heading_, kKurtTurn * delta);
}

void Snowboard::place(float delta) {
    // C# Snowboard.Place (Snowboard.cs:421-429): the board under Kurt's feet at the yaw
    // H + S, its nose up while carving; the view rolls with the board's bank.
    pitch_ = wrap360(pitch_ + fabsf(steer_) * kCarveTilt);
    board_->position = Vec3{kurt_.feet.x - 0.0f, kurt_.feet.y - 0.0f, kurt_.feet.z - kBoardDrop};
    board_->yaw = wrap360(heading_ + steer_);
    board_->pitch = wrapHalf(pitch_);
    kurt_.followCameraRoll(wrapHalf(board_->roll), delta);
}

void Snowboard::ram() {
    // C# Snowboard.Ram (Snowboard.cs:432-444): whatever Kurt runs into dies, and he
    // takes 5. Kurt.Touched is the list his last glide found; a kill does not change it.
    const std::vector<MdkObject*>& touched = kurt_.touched();
    for (size_t i = 0; i < touched.size(); i++) {
        MdkObject* obj = touched[i];
        if (obj == nullptr || obj == board_ || obj->dead || (obj->flags & kRamSpared) != 0 ||
            obj->health <= 0 || obj->health >= ScriptRuntime::kIndestructible) {
            continue;
        }

        runtime_.kill(obj);
        runtime_.hurtKurt(kRamDamage);
    }
}

void Snowboard::animate(float dt) {
    // C# Snowboard.Animate (Snowboard.cs:447-477): K_SURF loops; K_SURFJ holds on
    // frame 5 in the air and lands on frames 6-10.
    frame_ += dt;
    switch (pose_) {
        case PoseSurf:
            kurt_.cameraPivot = kDefaultPivot;
            kurt_.hasPose = true;
            kurt_.poseName = kSurfFrames;
            kurt_.poseFrame = (int32_t)frame_ % kSurfLoop;
            return;
        case PoseJump:
            frame_ = mathMin(frame_, (float)kJumpHoldFrame);
            if (grounded_ && kurt_.verticalSpeed <= 0.0f) {
                pose_ = frame_ > kLateLanding ? PoseLand : PoseSurf;
                frame_ = pose_ == PoseLand ? (float)kJumpLandFrame : 0.0f;
            }

            break;
        case PoseLand:
            if (frame_ >= kLandEnd) {
                pose_ = PoseSurf;
                frame_ = 0.0f;
            }

            break;
    }

    kurt_.cameraPivot = jumpPivot(frame_, kurt_.cameraPivot, dt / kTicks);
    kurt_.hasPose = true;
    kurt_.poseName = pose_ == PoseSurf ? kSurfFrames : kJumpFrames;
    kurt_.poseFrame = (int32_t)frame_;
}

float Snowboard::jumpPivot(float frame, float pivot, float seconds) {
    // C# Snowboard.JumpPivot (Snowboard.cs:481-482): the camera pivot at K_SURFJ frame
    // `frame`, coming from `pivot` after `seconds`.
    return frame < (float)kJumpHoldFrame ? kDefaultPivot - (float)(int32_t)frame * kPivotDip
                                         : mathMin(pivot + seconds, kDefaultPivot);
}

float Snowboard::turnInput(const Input& input, float dt) {
    // C# Snowboard.TurnInput (Snowboard.cs:486-496): the larger of the turn and strafe
    // keys x 4 (the strafe on a tie), or the mouse's 4 x clamp(dx / dt, +-4) when it moved.
    if (input.mouseX != 0.0f && dt > 0.0f) {
        return kSteerRate * clamp(input.mouseX * kMouseUnits / dt, -kMouseLimit, kMouseLimit);
    }

    const float turn = input.axis(kKeyTurnRight, kKeyTurnLeft);
    const float strafe = input.axis(kKeyStrafeRight, kKeyStrafeLeft);
    return kSteerRate * (fabsf(strafe) < fabsf(turn) ? turn : strafe);
}

}  // namespace scripts
}  // namespace mdk

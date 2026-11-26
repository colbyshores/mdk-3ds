// ============================================================================
// stream_flight.cpp — C# src/Mdk.Game/Stream/Flight.cs, operation for
// operation and in its tick order (see stream_flight.h). Each function names
// the C# member it ports.
// ============================================================================

#include "stream/stream_flight.h"

#include <cmath>

#include "game/level_flow.h"

namespace mdk {
namespace stream {

namespace {

constexpr int32_t kBlinkMask = 31;
constexpr float kMaxDelta = 0.1f;
// Kurt's speed (segments/s), its recovery (segments/s^2), its loss per hit and its minimum.
constexpr float kSpeed = 6.0f;
constexpr float kSpeedRecovery = 1.0f;
constexpr float kSpeedLoss = 0.9f;
constexpr float kSpeedMin = 4.5f;
// Kurt starts this far into the tube, and the tube moves on when he's this far past its tail + 1.
constexpr float kKurtStart = 0.75f;
// Steering (degrees/s, degrees around the neutral yaw 90 and pitch 0), and the sideways speed at full turn.
constexpr float kTurnRate = 180.0f;
constexpr float kTurnLimit = 45.0f;
constexpr float kSideSpeed = 25.0f;
// On bends Kurt keeps going straight (a lagging direction) and drifts outwards at this rate.
constexpr float kDrift = 100.0f;
constexpr float kDirectionLag = 0.75f;
// Without control he drifts back to the axis (units/s).
constexpr float kAxisReturn = 6.25f;
constexpr float kWallMargin = 1.5f;
// Health lost per wall hit: a base and a random extra (0 or 1 times the step) per difficulty.
constexpr int32_t kHitDamage[3] = {2, 2, 4};
constexpr int32_t kHitExtra[3] = {0, 1, 2};
// The rescue ends past this animation frame.
constexpr float kRescueEndFrame = 80.0f;
// The health bonus: where it starts, its speed, its reach and the health it gives.
constexpr float kBonusStart = 16.0f;
constexpr float kBonusSpeed = 5.48f;
constexpr float kBonusReach = 5.0f;
constexpr int32_t kBonusHealth = 150;
constexpr float kGuntaStart = 5.0f;
// The camera: from the axis behind Kurt and a point past him, looking two segments ahead.
constexpr float kCameraBack = 0.75f;
constexpr float kCameraAhead = 2.0f;
constexpr float kCameraBlend = 0.4f;
constexpr float kCameraPast = 1.375f;
// The background scrolls with the view's turns (0x438bfc) over its 600 x 360 pixels.
constexpr float kBackgroundWidth = 600.0f;
constexpr float kBackgroundHeight = 360.0f;
constexpr float kScrollX = 300.0f;
constexpr float kScrollY = 180.0f;
// The fade lasts a second; on death it goes on to 2 (red, then black).
constexpr float kFadeDeath = 2.0f;
const char* const kHurtSounds[7] = {"HURT1", "HURT2", "HURT3", "HURT4", "HURT5", "HURT6", "HURT7"};
constexpr int32_t kHurtSoundCount = 7;
// The last level index with statistics after the stream; the next one is Gunter's.
constexpr int32_t kLastStatisticsIndex = 3;

// MathF.Sign.
int32_t signOf(float v) { return v > 0.0f ? 1 : (v < 0.0f ? -1 : 0); }

// Flight.MoveToward.
float moveToward(float from, float to, float step)
{
    return std::fabs(to - from) <= step ? to : from + signOf(to - from) * step;
}

// Math.Clamp(float, float, float).
float clampFloat(float value, float low, float high)
{
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

// MathF.Min / MathF.Max for the finite values the flight has.
float minFloat(float a, float b) { return a < b ? a : b; }
float maxFloat(float a, float b) { return a > b ? a : b; }

// Flight.PositiveMod.
float positiveMod(float value, float modulus)
{
    return std::fmod(std::fmod(value, modulus) + modulus, modulus);
}

} // namespace

// Flight(tube, kind, difficulty, random, health, kurt, rescue, other).
Flight::Flight(StreamTube* tube, StreamTube::Kind kind, Kurt::Difficulty difficulty, WatcomRandom* random,
               int32_t health, Clip kurt, Clip rescue, Clip other)
    : tube_(tube),
      kind_(kind),
      difficulty_(difficulty),
      random_(random),
      kurtClip_(kurt),
      rescueClip_(rescue),
      otherClip_(other),
      health_(health)
{
    // The field initialisers: Kurt at the start, facing ahead at full speed.
    kurt_.t = kKurtStart;
    kurt_.yaw = kNeutralYaw;
    kurt_.speed = kSpeed;

    lightCount_ = addSpawns();
    hasOther_ = true;
    if (kind == StreamTube::Gunter) {
        other_.t = kGuntaStart;
        other_.speed = kSpeed;
        // Gunter's yaw (90 at 0x433b50) never turns him: 0x435a34 takes the tube's frame
        // (0x4366f4). Turned by it, he lay sideways across the tube (upstream 8fceff8c).
        other_.yaw = 0.0f;
    } else {
        other_.t = kBonusStart;
        other_.speed = kBonusSpeed;
    }
    updateCamera();
}

bool Flight::hasEvent(FlightEvent e) const
{
    for (int i = 0; i < eventCount_; i++) {
        if (events_[i] == e) {
            return true;
        }
    }
    return false;
}

void Flight::addEvent(FlightEvent e)
{
    if (eventCount_ < kMaxEvents) {
        events_[eventCount_++] = e;
    }
}

void Flight::addSound(const char* name)
{
    if (soundCount_ < kMaxSounds) {
        sounds_[soundCount_++] = name;
    }
}

// Flight.Update: one frame of `delta` seconds.
void Flight::update(float delta, Steering steering)
{
    eventCount_ = 0;
    soundCount_ = 0;
    if (ended_) {
        return;
    }

    const float dt = minFloat(delta, kMaxDelta);
    time_ += dt;
    tickFraction_ += dt * kTicksPerSecond;
    const int32_t ticks = (int32_t)tickFraction_;
    tickFraction_ -= ticks;
    blink_ = (blink_ + ticks) & kBlinkMask;

    checkEnd();
    if (!updateFade(dt)) {
        return;
    }

    updateKurt(dt, steering);
    updateOthers(dt);
    updateCamera();
}

// Flight.CheckEnd: the rescue after segment 177 or at 1 health, or the end of the Gunter tube.
void Flight::checkEnd()
{
    if (health_ <= 0 || (tube_->tail() <= kRescueSegment && health_ != kLowHealth)) {
        return;
    }

    if (kind_ == StreamTube::Gunter) {
        ending_ = ending_ || tube_->head() >= StreamTube::kLastSegment;
        return;
    }

    if (!hasBones_) {
        startRescue();
    }

    if (bones_.frame > kRescueEndFrame) {
        ending_ = true;
    }
}

// Flight.UpdateFade (0x520860): in from white (black after LEVEL8) over 1 s, out
// the same way once the stream ends; on death in the Gunter tube a red flash,
// then black. False once over.
bool Flight::updateFade(float dt)
{
    if (ending_) {
        fadeLevel_ -= dt;
        if (fadeLevel_ < 0.0f) {
            ended_ = true;
            addEvent(Ended);
            return false;
        }
    } else if (fadeLevel_ < 1.0f) {
        fadeLevel_ = minFloat(fadeLevel_ + dt, 1.0f);
    }

    const float f = clampFloat(fadeLevel_, 0.0f, kFadeDeath);
    if (health_ <= 0 && f > 1.0f) {
        fade_ = Fade{1.0f, 1.0f, kFadeDeath - f};
    } else if (health_ <= 0) {
        fade_ = Fade{1.0f, f, 1.0f};
    } else if (kind_ == StreamTube::Gunter) {
        fade_ = Fade{1.0f, minFloat(f, 1.0f), 0.0f};
    } else {
        fade_ = Fade{minFloat(f, 1.0f), 1.0f, 0.0f};
    }
    return true;
}

// Flight.AddSpawns: the lights and the planet the tube made fly along it. Returns how many.
int32_t Flight::addSpawns()
{
    taken_.clear();
    tube_->takeSpawns(&taken_);
    for (size_t i = 0; i < taken_.size(); i++) {
        Sprite sprite;
        sprite.spawn = taken_[i];
        sprite.thing.t = taken_[i].t;
        sprite.thing.x = taken_[i].x;
        sprite.thing.z = taken_[i].z;
        sprite.thing.speed = taken_[i].speed;
        sprites_.push_back(sprite);
    }
    spawnedTotal_ += (int32_t)taken_.size();
    return (int32_t)taken_.size();
}

// Flight.Move (0x436440): false once it leaves the rings still alive.
bool Flight::move(Thing* thing, float dt) const
{
    thing->t += thing->speed * dt;
    thing->frame += dt * kTicksPerSecond;
    return tube_->contains(thing->t);
}

// Flight.Transform.
Mat4 Flight::transform(const Thing& thing) const
{
    const TubeBasis b = basisMultiply(tube_->frame(thing.t, screenUp_), basisRotation(Vec3{0.0f, 0.0f, 1.0f}, thing.yaw));
    return basisAt(b, tube_->place(thing.t, thing.x, thing.z));
}

// Flight.KurtTransform.
Mat4 Flight::kurtTransform() const
{
    const TubeBasis turn = basisMultiply(basisRotation(Vec3{0.0f, 0.0f, 1.0f}, kurt_.yaw),
                                         basisRotation(nNeg(Vec3{0.0f, 1.0f, 0.0f}), pitch_));
    return basisAt(basisMultiply(tube_->frame(kurt_.t, screenUp_), turn), kurtPosition_);
}

// Flight.OtherFrame.
int32_t Flight::otherFrame(const Thing& thing) const
{
    // The C#'s (int)Frame % FrameCount, without the ARM11's software integer
    // divide (AGENTS.md R11; the screen calls this once per light): a frame is
    // never negative (it only grows by dt * 30 from 0), and for x >= 0 and a whole
    // n, fmod(x, n) is exact and trunc(fmod(x, n)) == trunc(x) mod n. The view
    // line of every stream golden pins it (of=, and the sprites' spr= hash).
    if (otherClip_.frameCount == 0) {
        return 0;
    }
    return (int32_t)std::fmod(thing.frame, (float)otherClip_.frameCount);
}

// Flight.KurtFrame.
int32_t Flight::kurtFrame() const
{
    if (!hasBones_) {
        return (int32_t)kurt_.frame;
    }
    const int32_t frame = (int32_t)kurt_.frame;
    const int32_t last = rescueClip_.frameCount - 1;
    return frame < last ? frame : last;
}

// Flight.BonesFrame.
int32_t Flight::bonesFrame() const
{
    const int32_t frame = (int32_t)(hasBones_ ? bones_.frame : 0.0f);
    const int32_t last = rescueClip_.frameCount - 1;
    return frame < last ? frame : last;
}

// Flight.UpdateOthers: the lights and the planet, the health bonus or Gunter, and Bones.
void Flight::updateOthers(float dt)
{
    // _sprites.RemoveAll(s => !Move(s.Thing, dt)): every sprite moved once, in
    // order, and the ones that left the tube dropped with the order kept.
    size_t kept = 0;
    for (size_t i = 0; i < sprites_.size(); i++) {
        if (move(&sprites_[i].thing, dt)) {
            sprites_[kept++] = sprites_[i];
        }
    }
    sprites_.resize(kept);

    if (hasOther_ && !move(&other_, dt)) {
        hasOther_ = false;
    }

    if (hasOther_ && kind_ == StreamTube::Normal) {
        takeBonus();
    }

    if (hasBones_) {
        bones_.frame += dt * kTicksPerSecond * rescueClip_.speed;
    }
}

// Flight.TakeBonus (0x435b18): caught within 5 units, it gives 150 health.
void Flight::takeBonus()
{
    if (nDistance(kurtPosition_, tube_->place(other_.t, other_.x, other_.z)) >= kBonusReach) {
        return;
    }

    health_ = kBonusHealth;
    addSound("APPLE");
    addEvent(BonusTaken);
    hasOther_ = false;
}

// Flight.UpdateKurt (0x435c4c): speed, the tube moving on, his animation, drift, steering and walls.
void Flight::updateKurt(float dt, Steering steering)
{
    kurt_.speed = minFloat(kurt_.speed + kSpeedRecovery * dt, kSpeed);
    kurt_.t += kurt_.speed * dt;

    // A new segment each time Kurt passes one.
    if (kurt_.t - kKurtStart > (float)(tube_->tail() + 1)) {
        tube_->advance();
        addSpawns();
    }

    const Clip clip = kurtClip();
    kurt_.frame += dt * kTicksPerSecond * clip.speed;
    if (!hasBones_ && clip.frameCount > 0) {
        kurt_.frame = std::fmod(kurt_.frame, (float)clip.frameCount);
    }

    const TubeBasis frame = tube_->frame(kurt_.t, screenUp_);
    driftKurt(frame, dt);
    steer(dt, controlled() ? steering : Steering{});

    const Vec3 centre = tube_->centre(kurt_.t);
    kurtPosition_ = nAdd(nAdd(centre, nScale(frame.x, kurt_.x)), nScale(frame.z, kurt_.z));
    if (controlled()) {
        collide(centre, frame);
    }
}

// Flight.DriftKurt: on bends Kurt keeps going straight and drifts to the outer
// wall; without control he goes back to the axis.
void Flight::driftKurt(const TubeBasis& frame, float dt)
{
    if (!controlled()) {
        kurt_.x = moveToward(kurt_.x, 0.0f, kAxisReturn * dt);
        kurt_.z = moveToward(kurt_.z, 0.0f, kAxisReturn * dt);
        return;
    }

    direction_ = nNormalize(nAdd(nScale(direction_, kDirectionLag), nScale(frame.y, 1.0f - kDirectionLag)));
    kurt_.x += kDrift * dt * nDot(direction_, frame.x);
    kurt_.z += kDrift * dt * nDot(direction_, frame.z);
}

// Flight.Steer: up to 45 degrees either way and sideways; he turns back when
// the keys are released. Left and up turn him left and up.
void Flight::steer(float dt, Steering steering)
{
    if (steering.right != 0) {
        kurt_.yaw = clampFloat(kurt_.yaw - steering.right * kTurnRate * dt, kNeutralYaw - kTurnLimit,
                               kNeutralYaw + kTurnLimit);
    } else {
        kurt_.yaw = moveToward(kurt_.yaw, kNeutralYaw, kTurnRate * dt);
    }

    if (steering.up != 0) {
        pitch_ = clampFloat(pitch_ + steering.up * kTurnRate * dt, -kTurnLimit, kTurnLimit);
    } else {
        pitch_ = moveToward(pitch_, 0.0f, kTurnRate * dt);
    }

    /* @vfp-exempt R6 — Kurt's sideways move in the stream, two calls a frame (not a
     * loop); MathF.Cos/Sin are glibc cosf/sinf bit for bit on this machine
     * (tube_basis.h), and the LUT would move Kurt off the oracle's path by a ULP a
     * frame, which the walls then turn into a different flight. measured n/a (corpus
     * empty). Verified 2026-11-20. */
    kurt_.x += std::cos(degreesToRadians(kurt_.yaw)) * kSideSpeed * dt;
    kurt_.z += std::sin(degreesToRadians(pitch_)) * kSideSpeed * dt;
}

// Flight.Collide: a wall hit pushes Kurt back towards the axis, turns him
// inwards, hurts him and slows him down.
void Flight::collide(Vec3 centre, const TubeBasis& frame)
{
    const float k = tube_->wallHit(kurtPosition_, kurt_.t, kWallMargin);
    if (k < 0.0f) {
        return;
    }

    // Facing the axis at full turn, e.g. at the right wall (x > 0) he turns left (yaw 135).
    const float r = nLength2(kurt_.x, kurt_.z);
    if (r > 0.0f) {
        kurt_.yaw = kNeutralYaw + kTurnLimit * kurt_.x / r;
        pitch_ = -kTurnLimit * kurt_.z / r;
    }

    kurt_.x *= 1.0f - k;
    kurt_.z *= 1.0f - k;
    kurtPosition_ = nAdd(nAdd(centre, nScale(frame.x, kurt_.x)), nScale(frame.z, kurt_.z));
    kurt_.speed = maxFloat(kurt_.speed * kSpeedLoss, kSpeedMin);

    addEvent(WallHit);
    addSound(kHurtSounds[random_->next() % kHurtSoundCount]);
    hurt();
}

// Flight.Hurt: wall damage by difficulty. At 0 health Kurt dies in the Gunter
// tube, else Bones comes for him.
void Flight::hurt()
{
    if (health_ <= 0) {
        return;
    }

    const int32_t d = (int32_t)difficulty_;
    health_ -= kHitDamage[d] + kHitExtra[d] * (random_->next() & 1);
    if (health_ >= kLowHealth) {
        return;
    }

    if (kind_ == StreamTube::Normal) {
        health_ = kLowHealth;
        return;
    }

    health_ = 0;
    ending_ = true;
    fadeLevel_ = kFadeDeath;
    addEvent(KurtDied);
}

// Flight.StartRescue: Bones picks Kurt up; both play BONESANIM once.
void Flight::startRescue()
{
    bones_ = Thing{};
    hasBones_ = true;
    addSound("RESCUE");
    addEvent(BonesCame);
    kurt_.frame = 0.0f;
}

// Flight.UpdateCamera (0x4352ac, 0x436828): behind Kurt and to his side,
// looking at the axis two segments ahead, its roll carried over from frame to frame.
void Flight::updateCamera()
{
    const Vec3 target = tube_->centre(kurt_.t + kCameraAhead);
    const Vec3 past = nAdd(target, nScale(nSub(kurtPosition_, target), kCameraPast));
    const Vec3 eye = nAdd(nScale(tube_->centre(kurt_.t - kCameraBack), kCameraBlend),
                          nScale(past, 1.0f - kCameraBlend));
    const Vec3 forward = nNormalize(nSub(target, eye));
    const Vec3 right = nNormalize(nCross(forward, screenUp_));
    screenUp_ = nCross(right, forward);
    eye_ = eye;
    forward_ = forward;
    right_ = right;

    // The background scrolls with the view's turns (0x438bfc).
    const Vec3 down = nNeg(screenUp_);
    if (hasViewRows_) {
        const Vec3 r0 = viewRows_[0];
        const Vec3 u0 = viewRows_[1];
        const Vec3 f0 = viewRows_[2];
        const float y = positiveMod(backgroundOffset_.y + kScrollY * (down.z * f0.z - forward.z * u0.z),
                                    kBackgroundHeight);
        const float x = positiveMod(backgroundOffset_.x + kScrollX * (right.x * u0.x - down.x * r0.x),
                                    kBackgroundWidth);
        backgroundOffset_ = Vec2{x, y};
    }

    viewRows_[0] = right;
    viewRows_[1] = down;
    viewRows_[2] = forward;
    hasViewRows_ = true;
}

// StreamScreen: Math.Clamp(GameState.IndexOf(level), 0, LastStatisticsIndex + 1).
int32_t streamIndexOf(int32_t level)
{
    const int32_t index = flow::indexOf(level);
    if (index < 0) {
        return 0;
    }
    return index > kLastStatisticsIndex + 1 ? kLastStatisticsIndex + 1 : index;
}

StreamTube::Kind streamKindOf(int32_t index)
{
    return index > kLastStatisticsIndex ? StreamTube::Gunter : StreamTube::Normal;
}

StreamRun::StreamRun(int32_t level, Kurt::Difficulty difficulty, uint32_t seed, int32_t health, Clip kurt,
                     Clip rescue, Clip other)
    : random(seed),
      tube(streamIndexOf(level), difficulty, streamKindOf(streamIndexOf(level)), &random),
      flight(&tube, streamKindOf(streamIndexOf(level)), difficulty, &random, health, kurt, rescue, other)
{
}

} // namespace stream
} // namespace mdk

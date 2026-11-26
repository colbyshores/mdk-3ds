#include "fall/fall_sim.h"

#include <math.h>

#include "formats/bni.h"
#include "formats/fall3d.h"
#include "formats/model_animation.h"
#include "game/level_flow.h"
#include "util/math_minmax.h"

// The four sections below are the C#'s four files, in the same order:
// FallSim.cs, FallSim.Missiles.cs, FallSim.Pickups.cs, FallSim.Radar.cs. Every
// function names the C# method it ports. Where the C# evaluates a call's
// arguments left to right and two of them draw from the Random, the draws are
// sequenced into locals first: C++ promises no argument order, and a swapped
// pair of draws is a different fall.

namespace mdk {
namespace fall {

using formats::Vec3f;

namespace {

// ---- FallSim.cs ----
constexpr float kMaxDelta = 0.1f;
constexpr float kFadeTime = 31.0f;
constexpr float kFadeLength = 2.0f;
constexpr float kCameraAbove = 10.0f;
// After 30 s the camera brakes to a stop in 2 s.
constexpr float kCameraBrake = FallSim::kKurtSpeed / 2.0f;
// The camera follows Kurt's x and y by 0.85.
constexpr float kCameraFollow = 0.85f;
// Kurt's collision box: ±4, ±4, ±5 (+0x198...+0x1ac).
constexpr float kKurtBoxX = 4.0f;
constexpr float kKurtBoxY = 4.0f;
constexpr float kKurtBoxZ = 5.0f;
constexpr float kBonesSpeed = 2000.0f / 27.0f;
constexpr float kBonesAbove = 20.0f;
constexpr float kWindSoundVolume = 0.625f;
// The wind follows the camera's braking: −vz × 0.015 × 3072.
constexpr float kWindPerSpeed = 0.015f * FallSim::kWindFull;
// The minecrawler's frames run at 15 per second.
constexpr float kCrawlerRate = 0.5f;
const char* const kFirstMessage = "FALL_T1";
constexpr int32_t kFirstMessageIndex = 0;
// Bones falls with Kurt above this index (LEVEL8).
constexpr int32_t kBonesIndex = 3;

// Intro timeline (ticks left): fade in from black until 90, Kurt flies in from
// 90 over 60, the wind rises from 120 to 60, fade to white from 60.
constexpr int32_t kIntroFadeIn = 90;
constexpr int32_t kIntroFadeOut = 60;
constexpr float kIntroFadeTicks = 60.0f;
constexpr int32_t kIntroWindStart = 120;
constexpr int32_t kIntroWindFull = 61;
constexpr int32_t kIntroWindSteps = 12;
const Vec3f kIntroKurtStart{-30.0f, -10.0f, 0.0f};
const Vec3f kIntroKurtMove{30.0f, 10.0f, -10.0f};

// Difficulty (0x410018) by Easy, Normal, Hard; i is the level index.
// Radar beam speed: 117.65 × (1 + factor × i).
const float kBeamFactor[3] = {0.1f, 0.2f, 1.0f / 3.0f};
// Missiles per detection: 2 + i / divisor (the divisor per difficulty is
// missilesFor's switch, so each division is by a compile-time constant).
constexpr int32_t kMissilesBase = 2;
// Missile aim spread: base − i.
const float kSpreadBase[3] = {7.5f, 6.5f, 5.5f};
// Ticks between missiles: 32 − step × i (+ 0-31).
const int32_t kMissileGapStep[3] = {1, 3, 5};
constexpr int32_t kMissileGapBase = 32;
// Ticks before the next radar: 63 − step × i (+ 0-63).
const int32_t kRadarGapStep[3] = {3, 7, 9};
constexpr int32_t kRadarGapBase = 63;

// Palette effects.
constexpr float kFlashRate = 3.0f;
constexpr float kSettleRate = 0.5f;
constexpr float kFlicker = 0.9f;
// A flicker starts on 1 tick in 32.
constexpr int32_t kFlickerChance = 32;
constexpr float kHitBrightness = 3.0f;
constexpr float kBrightnessEpsilon = 1e-5f;
// Dying: the red rises by 5/255 a tick while the screen is at full brightness.
constexpr float kRedPerTick = 5.0f / 255.0f;

// ---- FallSim.Missiles.cs ----
constexpr int32_t kMissileGapRandom = 32;
const char* const kExplodeSounds[] = {"EXPLODE1", "EXPLODE2"};
constexpr int32_t kExplodeSoundCount = 2;
const char* const kHitSounds[] = {"K_HIT1", "K_HIT2", "K_HIT3", "K_HIT4", "K_HIT5", "K_HIT6", "K_HIT7"};
constexpr int32_t kHitSoundCount = 7;
// Damage of a hit: 4 on easy, 4 + 0-7 on normal, twice that on hard.
constexpr int32_t kHitDamage = 4;
constexpr int32_t kHitDamageRandom = 8;
constexpr int32_t kHardFactor = 2;
// Screen flashes: a detection dims the target brightness by 0.5 (not below
// 0.75), a launch by 0.2 (not below 0.5).
constexpr float kDetectDim = 0.5f;
constexpr float kDetectFloor = 0.75f;
constexpr float kLaunchDim = 0.2f;
constexpr float kLaunchFloor = 0.5f;
// MathF.Tau.
constexpr float kTau = 6.28318548f;

// ---- FallSim.Pickups.cs ----
constexpr float kPickupFallSpeed = 2.0f * FallSim::kKurtSpeed;
constexpr float kPickupChuteSpeed = 50.0f;
// Degrees per second under the chute.
constexpr float kPickupSpin = 30.0f;
// Dropped 15 units above Kurt, within 95 % of his limits.
constexpr float kPickupAbove = 15.0f;
constexpr float kPickupSpread = 0.95f;
// The first pickup comes 31-62 ticks after the intro, the next ones 31-158
// ticks apart; each opens its chute after 30-93 ticks.
constexpr int32_t kPickupWait = 31;
constexpr int32_t kPickupFirstRandom = 32;
constexpr int32_t kPickupNextRandom = 128;
constexpr int32_t kChuteTicks = 30;
constexpr int32_t kChuteRandom = 64;
const char* const kCollectSounds[] = {"K_COLL1", "K_COLL2"};
constexpr int32_t kCollectSoundCount = 2;

// ---- FallSim.Radar.cs ----
constexpr float kRadarExtendSpeed = 3000.0f;
constexpr float kRadarRetractSpeed = 1500.0f;
constexpr float kRadarSee = 15.0f;
// The beam's spot stays 3 units below Kurt.
constexpr float kRadarBelow = 3.0f;
// Tracking: v = 0.75 v + 0.25 × beam speed × unit(target − spot).
constexpr float kRadarInertia = 0.75f;
// A new target after 30 ticks, or once within 2.94 units (on both axes).
constexpr int32_t kRadarTargetTicks = 30;
constexpr float kRadarNear = 2.94f;
// Choices of the next target: at most 9 of Kurt and the pickups, and 3 random
// points.
constexpr int32_t kRadarChoices = 9;
constexpr int32_t kRadarRandomChoices = 3;
// The first radar comes 7-22 ticks after the intro, later ones the gap + 0-63
// ticks after.
constexpr int32_t kRadarFirstWait = 7;
constexpr int32_t kRadarFirstRandom = 16;
constexpr int32_t kRadarGapRandom = 64;

// The falls are those of the level indices 0-4 (FallScreen.LastIndex).
constexpr int32_t kLastIndex = 4;

// MathF.Sign (its int result; the C# throws on NaN, which nothing here feeds it).
int32_t signOf(float value) {
    return value > 0.0f ? 1 : (value < 0.0f ? -1 : 0);
}

// Math.Clamp(float, float, float).
float clampOf(float value, float low, float high) {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

// 2 + i / divisor, the divisor a compile-time constant per difficulty
// (AGENTS.md R11: no runtime integer divisor).
int32_t missilesFor(int32_t difficulty, int32_t index) {
    switch (difficulty) {
        case Kurt::Easy:
            return kMissilesBase + index / 5;
        case Kurt::Hard:
            return kMissilesBase + index / 2;
        default:
            return kMissilesBase + index / 3;
    }
}

// The difficulty as an index into the tables, kept inside them.
int32_t difficultyIndex(Kurt::Difficulty difficulty) {
    const int32_t d = (int32_t)difficulty;
    return d < 0 ? 0 : (d > 2 ? 2 : d);
}

// Vector2.Normalize: divided by Vector2.Length (sqrt of the plain dot).
void normalize2(float* x, float* y) {
    const float length = sqrtf(*x * *x + *y * *y);
    *x = *x / length;
    *y = *y / length;
}

bool animationOf(const formats::Bni& bni, const char* name, FallSim::Animation* out) {
    // FallModels.Animation: ModelAnimation.Parse at the entry's offset.
    const formats::ArchiveIndex::Entry* entry = bni.index().find(name);
    if (entry == nullptr) {
        return false;
    }
    formats::ModelAnimation animation;
    const std::vector<uint8_t>& bytes = bni.bytes();
    if (!formats::ModelAnimation::parse(&animation, name, bytes.data(), (int32_t)bytes.size(), entry->offset)) {
        return false;
    }
    out->speed = animation.speed;
    out->frameCount = animation.frameCount;
    return true;
}

} // namespace

// ============================================================================
// FallSim.cs
// ============================================================================

FallSim::FallSim(const Setup& setup, int32_t seed) : setup_(setup), random_(seed) {
    // C# FallSim(Setup, Random).
    const int32_t i = setup.index;
    const int32_t d = difficultyIndex(setup.difficulty);
    beamSpeed_ = kSteerMax * (1.0f + kBeamFactor[d] * (float)i);
    missilesPerDetection_ = missilesFor(d, i);
    spread_ = kSpreadBase[d] - (float)i;
    missileGap_ = kMissileGapBase - kMissileGapStep[d] * i;
    radarGap_ = kRadarGapBase - kRadarGapStep[d] * i;
    pickupNames_ = setup.pickups;
    inventory_.setDifficulty(setup.difficulty);

    // Created as the fall goes, never more than a handful at once: reserved so
    // a frame does not grow them in the common case.
    missiles_.reserve(16);
    explosions_.reserve(16);
    pickups_.reserve(setup.pickups.size());
    collected_.reserve(setup.pickups.size());
}

float FallSim::windVolume() const {
    // C# WindVolume: (float)Wind / WindFull * WindSoundVolume.
    return (float)wind_ / (float)kWindFull * kWindSoundVolume;
}

float FallSim::grindVolume() const {
    // C# GrindVolume: (MathF.Min(Time / SteerTime, 1f) + 2f) / 3f.
    return (mathMin(time_ / kSteerTime, 1.0f) + 2.0f) / 3.0f;
}

void FallSim::sound(const char* name) {
    // C# Sound?.Invoke(name).
    if (hooks_.sound != nullptr) {
        hooks_.sound(hooks_.context, name);
    }
}

void FallSim::update(float delta, const Input& input) {
    // FallScreen.Frame: Esc first (_sim.Skip()), then _sim.Update.
    if (input.skip) {
        skip();
    }
    update(delta, input.steerX, input.steerY);
}

void FallSim::update(float delta, int32_t steerX, int32_t steerY) {
    // C# FallSim.Update.
    if (state_ != Running) {
        return;
    }

    const float dt = mathMin(delta, kMaxDelta);
    tickFraction_ += dt * kTicksPerSecond;
    const int32_t ticks = (int32_t)tickFraction_;
    tickFraction_ -= (float)ticks;
    ticks_ = ticks;
    if (introLeft_ > 0) {
        updateIntro(ticks);
        return;
    }

    updateFall(dt, ticks, steerX, steerY);
}

void FallSim::skip() {
    // C# FallSim.Skip.
    if (state_ == Running && !dying_) {
        state_ = Landed;
    }
}

// --- The intro in space (0x41106c) ---

void FallSim::updateIntro(int32_t ticks) {
    // C# FallSim.UpdateIntro.
    introLeft_ -= ticks;
    const int32_t left = introLeft_ > 0 ? introLeft_ : 0;

    // Fade in from black, then plain, then fade to white.
    if (left > kIntroFadeIn) {
        setScreen(1.0f, 1.0f - (float)(left - kIntroFadeIn) / kIntroFadeTicks);
    } else if (left < kIntroFadeOut) {
        setScreen(1.0f - (float)(kIntroFadeOut - left) / kIntroFadeTicks, 1.0f);
    } else {
        setScreen(1.0f, 1.0f);
    }

    if (left < kIntroWindStart) {
        // (120 - left) * 12 / 60: the divisor is the constant kIntroFadeOut.
        wind_ = left < kIntroWindFull ? kWindFull
                                       : ((kIntroWindStart - left) * kIntroWindSteps / kIntroFadeOut) << kWindShift;
        hazeFrame_ = (hazeFrame_ + ticks) % kHazeFrames;
    }

    cameraPosition_ = Vec3f{0.0f, 0.0f, 0.0f};

    // Kurt flies in from the camera to 10 units below it, easing out.
    if (left < kIntroFadeIn) {
        kurtVisible_ = true;
        animateKurt(ticks);
        const float f = 1.0f - powf(1.0f - mathMin((float)(kIntroFadeIn - left) / kIntroFadeTicks, 1.0f), 2.0f);
        kurtPosition_ = Vec3f{kIntroKurtStart.x + kIntroKurtMove.x * f, kIntroKurtStart.y + kIntroKurtMove.y * f,
                              kIntroKurtStart.z + kIntroKurtMove.z * f};
    }

    if (introLeft_ <= 0) {
        startFall();
    }
}

void FallSim::startFall() {
    // C# FallSim.StartFall.
    introLeft_ = 0;
    time_ = 0.0f;
    startRadars();
    startPickups();
    wind_ = kWindFull;
    kurtPosition_ = Vec3f{0.0f, 0.0f, kKurtStartZ};
    if (setup_.index > kBonesIndex) {
        hasBones_ = true;
        bonesPosition_ = Vec3f{0.0f, 0.0f, kKurtStartZ + kBonesAbove};
    }

    if (setup_.index == kFirstMessageIndex && hooks_.message != nullptr) {
        hooks_.message(hooks_.context, kFirstMessage);
    }
}

// --- The fall (0x4114a4) ---

void FallSim::updateFall(float dt, int32_t ticks, int32_t steerX, int32_t steerY) {
    // C# FallSim.UpdateFall, in its order.
    time_ += dt;
    updateKurt(dt, ticks, steerX, steerY);
    updateCamera(dt);
    updateRadar(dt, ticks);
    updateMissiles(dt, ticks);
    updateExplosions(ticks);
    updatePickups(dt, ticks);
    updateBones(dt, ticks);
    updatePalette(dt, ticks);
    hazeFrame_ = (hazeFrame_ + ticks) % kHazeFrames;
    crawlerFrame_ = fmodf(crawlerFrame_ + (float)ticks * kCrawlerRate, (float)kCrawlerFrames);

    if (dying_ || state_ != Running) {
        return;
    }

    if (time_ > kEndTime) {
        state_ = Landed;
    }
}

void FallSim::updateKurt(float dt, int32_t ticks, int32_t steerX, int32_t steerY) {
    // C# FallSim.UpdateKurt.
    animateKurt(ticks);
    float vx = kurtVelocityX_;
    float vy = kurtVelocityY_;
    float px = kurtPosition_.x;
    float py = kurtPosition_.y;
    if (time_ <= kSteerTime && !dying_) {
        vx = steer(vx, signOf((float)steerX), dt);
        vy = steer(vy, signOf((float)steerY), dt);
    } else if (time_ > kSteerTime) {
        if (!kurtFinished_) {
            kurtFinished_ = true;
            sound("K_FINISH");
        }

        // Pulled back to the centre: v = (v − 2 p) / 2 a tick.
        for (int32_t i = 0; i < ticks; i++) {
            vx = (vx - 2.0f * px) / 2.0f;
            vy = (vy - 2.0f * py) / 2.0f;
        }
    }

    // Clamp(p + velocity * dt, velocity): hitting a limit zeroes that speed.
    px = px + vx * dt;
    py = py + vy * dt;
    if (fabsf(px) > kLimitX) {
        px = (float)signOf(px) * kLimitX;
        vx = 0.0f;
    }

    if (fabsf(py) > kLimitY) {
        py = (float)signOf(py) * kLimitY;
        vy = 0.0f;
    }

    kurtPosition_ = Vec3f{px, py, kurtPosition_.z - kKurtSpeed * dt};
    kurtVelocityX_ = vx;
    kurtVelocityY_ = vy;
}

float FallSim::steer(float speed, int32_t direction, float dt) {
    // C# FallSim.Steer: a key accelerates by a step a tick, the opposite key
    // first resets the speed to one step, no key brakes by a step a tick.
    const float step = kSteerStep * kTicksPerSecond * dt;
    if (direction == 0) {
        return fabsf(speed) <= step ? 0.0f : speed - (float)signOf(speed) * step;
    }

    if (speed * (float)direction < 0.0f) {
        return (float)direction * kSteerStep;
    }

    return clampOf(speed + (float)direction * step, -kSteerMax, kSteerMax);
}

void FallSim::animateKurt(int32_t ticks) {
    // C# FallSim.AnimateKurt: KURTANIM loops; a hit plays KURT_HIT once
    // (looping KURTANIM forever once he's dying).
    const Animation& animation = kurtPose_ == AnimHit ? setup_.kurtHit : setup_.kurt;
    kurtFrame_ += (float)ticks * animation.speed;
    if (kurtFrame_ < (float)animation.frameCount) {
        return;
    }

    if (kurtPose_ == AnimHit && !dying_) {
        kurtPose_ = AnimFall;
        kurtFrame_ = 0.0f;
        return;
    }

    kurtFrame_ = fmodf(kurtFrame_, (float)animation.frameCount);
}

void FallSim::updateCamera(float dt) {
    // C# FallSim.UpdateCamera (0x413440): above Kurt until 30 s, then it
    // brakes to a stop in 2 s.
    float z = kurtPosition_.z + kCameraAbove;
    if (time_ > kSteerTime) {
        cameraSpeed_ = mathMin(cameraSpeed_ + kCameraBrake * dt, 0.0f);
        z = cameraPosition_.z + cameraSpeed_ * dt;
        wind_ = (int32_t)(-cameraSpeed_ * kWindPerSpeed);
    }

    cameraPosition_ = Vec3f{kCameraFollow * kurtPosition_.x, kCameraFollow * kurtPosition_.y, z};
}

bool FallSim::crossesBox(const Vec3f& centre, const Vec3f& from, const Vec3f& to) {
    // C# FallSim.CrossesBox (0x45ef80).
    const float minimum[3] = {centre.x - kKurtBoxX, centre.y - kKurtBoxY, centre.z - kKurtBoxZ};
    const float maximum[3] = {centre.x + kKurtBoxX, centre.y + kKurtBoxY, centre.z + kKurtBoxZ};
    const float end[3] = {to.x, to.y, to.z};
    const float start[3] = {from.x, from.y, from.z};

    // Vector3.Clamp(to, min, max) == to: the end is inside the box.
    if (minimum[0] <= end[0] && end[0] <= maximum[0] && minimum[1] <= end[1] && end[1] <= maximum[1] &&
        minimum[2] <= end[2] && end[2] <= maximum[2]) {
        return true;
    }

    // Slabs: the part of the segment inside each axis's range.
    float enter = 0.0f;
    float leave = 1.0f;
    for (int32_t axis = 0; axis < 3; axis++) {
        const float d = end[axis] - start[axis];
        // The C#'s `MathF.Abs(d) < float.Epsilon`: float.Epsilon is the smallest
        // denormal, so on the host that is exactly d == 0. Written as the
        // equality because under the 3DS's flush-to-zero (AGENTS.md R1) the
        // denormal constant would compare as zero and the test would never pass.
        if (d == 0.0f) {
            if (start[axis] < minimum[axis] || start[axis] > maximum[axis]) {
                return false;
            }

            continue;
        }

        const float a = (minimum[axis] - start[axis]) / d;
        const float b = (maximum[axis] - start[axis]) / d;
        enter = mathMax(enter, mathMin(a, b));
        leave = mathMin(leave, mathMax(a, b));
    }

    return enter <= leave;
}

// --- Bones (0x4133b8) ---

void FallSim::updateBones(float dt, int32_t ticks) {
    // C# FallSim.UpdateBones.
    if (!hasBones_) {
        return;
    }

    bonesTicks_ += (float)ticks;
    bonesFrame_ = fmodf(bonesTicks_ * setup_.bones.speed, (float)setup_.bones.frameCount);
    bonesPosition_.z = bonesPosition_.z - kBonesSpeed * dt;
    if (!bonesPassed_ && bonesPosition_.z < kurtPosition_.z) {
        bonesPassed_ = true;
        sound("BONES");
    }
}

// --- Palette effects (0x5209c8, 0x410cc0, 0x410c28) ---

void FallSim::updatePalette(float dt, int32_t ticks) {
    // C# FallSim.UpdatePalette.
    if (dying_) {
        // Red with the skull growing, then black; at 0 it's game over.
        if (brightness_ >= 1.0f) {
            red_ = mathMin(red_ + (float)ticks * kRedPerTick, 1.0f);
        }

        brightness_ -= dt;
        setScreen(1.0f, clampOf(brightness_, 0.0f, 1.0f));
        if (brightness_ <= 0.0f) {
            state_ = GameOver;
        }

        return;
    }

    flash(dt, ticks);
    const float dark = time_ > kFadeTime ? mathMax(1.0f - (time_ - kFadeTime) / kFadeLength, 0.0f) : 1.0f;
    setScreen(clampOf(brightness_, 0.0f, 1.0f), dark);
}

void FallSim::flash(float dt, int32_t ticks) {
    // C# FallSim.Flash: the brightness fades in over the first second, flashes
    // on detections, launches and hits, and flickers now and then.
    if (time_ < 1.0f) {
        brightness_ = time_;
        return;
    }

    if (fabsf(brightness_ - target_) > kBrightnessEpsilon) {
        const float step = rate_ * dt;
        brightness_ = fabsf(target_ - brightness_) <= step
                          ? target_
                          : brightness_ + (float)signOf(target_ - brightness_) * step;
        return;
    }

    if (target_ < 1.0f) {
        target_ = 1.0f;
        rate_ = kSettleRate;
        return;
    }

    for (int32_t i = 0; i < ticks; i++) {
        if (random_.next(kFlickerChance) != 0) {
            continue;
        }

        target_ = kFlicker;
        rate_ = kSettleRate;
        break;
    }
}

void FallSim::setScreen(float whiten, float dark) {
    whiten_ = whiten;
    dark_ = dark;
}

void FallSim::randomPoint(float* x, float* y) {
    // C# FallSim.RandomPoint: new(RandomRange(Limit.X), RandomRange(Limit.Y)).
    *x = randomRange(kLimitX);
    *y = randomRange(kLimitY);
}

float FallSim::randomRange(float range) {
    // C# FallSim.RandomRange: uniform in ±range.
    return (random_.nextSingle() * 2.0f - 1.0f) * range;
}

bool FallSim::carry(flow::FallCarry* out) const {
    // FallScreen.Next: `new FallCarry(_sim.Health, [.. _sim.Collected])` on Landed.
    if (state_ != Landed) {
        return false;
    }
    out->health = health_;
    out->pickups = collected_;
    return true;
}

// ============================================================================
// FallSim.Missiles.cs — the missiles (0x412568, 0x411ee8) launched when a
// radar sees Kurt, their hits and explosions.
// ============================================================================

void FallSim::updateMissiles(float dt, int32_t ticks) {
    // C# FallSim.UpdateMissiles.
    if (missileQueue_ > 0) {
        missileWait_ -= ticks;
        if (missileWait_ <= 0) {
            missileQueue_--;
            missileWait_ = missileGap_ + random_.next(kMissileGapRandom);
            launch();
        }
    }

    // `foreach (var missile in _missiles.ToList())`: the only removal inside
    // the loop is the current missile's (Gone, or Hit), so an index that stays
    // put on a removal walks the same snapshot.
    size_t i = 0;
    while (i < missiles_.size()) {
        FallMissile& missile = missiles_[i];
        const Vec3f previous = missile.position;
        const FallMissile::Event happened = missile.update(dt, ticks, kurtPosition_);
        if (happened == FallMissile::EventPassed) {
            sound("M_PASS");
        }

        if (happened == FallMissile::EventGone) {
            missiles_.erase(missiles_.begin() + (ptrdiff_t)i);
            continue;
        }

        if (time_ <= kSteerTime && !dying_ && crossesBox(kurtPosition_, previous, missile.position)) {
            hit(i);
            continue;
        }

        i++;
    }
}

void FallSim::launch() {
    // C# FallSim.Launch.
    sound("M_LNCH");
    rate_ = kFlashRate;
    target_ = mathMax(target_ - kLaunchDim, kLaunchFloor);
    missiles_.push_back(FallMissile(spread_, &random_));
}

void FallSim::hit(size_t index) {
    // C# FallSim.Hit: an explosion, damage and a sound; at 0 health he's dying.
    sound(kExplodeSounds[random_.next(kExplodeSoundCount)]);
    sound(kHitSounds[random_.next(kHitSoundCount)]);
    int32_t damage = kHitDamage;
    if (setup_.difficulty == Kurt::Normal) {
        damage = kHitDamage + random_.next(kHitDamageRandom);
    } else if (setup_.difficulty == Kurt::Hard) {
        damage = kHardFactor * (kHitDamage + random_.next(kHitDamageRandom));
    }

    health_ = health_ - damage > 0 ? health_ - damage : 0;
    brightness_ = kHitBrightness;
    kurtPose_ = AnimHit;
    kurtFrame_ = 0.0f;
    Explosion explosion;
    // On Kurt (0x411ee8 copies his position), not where the missile met his
    // box, up to 4 units to his side (upstream 8bc42528).
    explosion.position = kurtPosition_;
    explosion.yaw = random_.nextSingle() * kTau;
    explosions_.push_back(explosion);
    missiles_.erase(missiles_.begin() + (ptrdiff_t)index);
    if (health_ > 0) {
        return;
    }

    dying_ = true;
    brightness_ = kHitBrightness;
    kurtPose_ = AnimFall;
}

void FallSim::updateExplosions(int32_t ticks) {
    // C# FallSim.UpdateExplosions: explosions follow Kurt down while their
    // frames play.
    size_t i = 0;
    while (i < explosions_.size()) {
        Explosion& explosion = explosions_[i];
        explosion.ticks += (float)ticks;
        if (explosion.ticks >= (float)kExplosionTicks) {
            explosions_.erase(explosions_.begin() + (ptrdiff_t)i);
            continue;
        }

        explosion.position.z = kurtPosition_.z;
        i++;
    }
}

// ============================================================================
// FallSim.Pickups.cs — the pickups (0x4128fc, 0x41275c) of FALLPU_n, dropped
// one by one from above Kurt (the list's last first): they fall at twice his
// speed, open a chute after 1-3 seconds and brake to 50 u/s, spinning. Kurt
// takes the ones that cross his box until 30 s.
// ============================================================================

void FallSim::startPickups() {
    // C# FallSim.StartPickups.
    if (!pickupNames_.empty()) {
        pickupWait_ = random_.next(kPickupFirstRandom) + kPickupWait;
    }
}

void FallSim::updatePickups(float dt, int32_t ticks) {
    // C# FallSim.UpdatePickups.
    if (!pickupNames_.empty() && time_ <= kSteerTime) {
        pickupWait_ -= ticks;
        if (pickupWait_ <= 0) {
            pickupWait_ = kPickupWait + random_.next(kPickupNextRandom);
            spawn(pickupNames_.back());
            pickupNames_.pop_back();
        }
    }

    // `foreach (var pickup in _pickups.ToList())`: only the current pickup is
    // ever removed inside the loop.
    size_t i = 0;
    while (i < pickups_.size()) {
        Pickup& pickup = pickups_[i];
        const Vec3f previous = pickup.position;
        movePickup(&pickup, dt, ticks);

        // Only under its chute (0x41275c): dropped above the camera, it would go
        // at once.
        if (!pickup.chute) {
            i++;
            continue;
        }

        if (pickup.position.z > cameraPosition_.z) {
            pickups_.erase(pickups_.begin() + (ptrdiff_t)i);
            continue;
        }

        if (!dying_ && time_ <= kSteerTime && crossesBox(kurtPosition_, previous, pickup.position)) {
            take(i);
            continue;
        }

        i++;
    }
}

void FallSim::movePickup(Pickup* pickup, float dt, int32_t ticks) {
    // C# FallSim.MovePickup: falls, then brakes to 50 u/s and spins under its
    // chute.
    pickup->ticks += ticks;
    if (!pickup->chute && pickup->ticks >= pickup->fallTicks) {
        sound("P_CHUTE");
        pickup->chute = true;
    }

    if (pickup->chute) {
        pickup->velocity.z = mathMin(pickup->velocity.z + kKurtSpeed * dt, -kPickupChuteSpeed);
        pickup->yaw += kPickupSpin * dt;
    }

    pickup->position.x += pickup->velocity.x * dt;
    pickup->position.y += pickup->velocity.y * dt;
    pickup->position.z += pickup->velocity.z * dt;
}

void FallSim::take(size_t index) {
    // C# FallSim.Take: Inventory.Collect with the sim's health by ref, which is
    // the Kurt's health round trip here (Kurt.Collect's shape).
    sound("P_COLL");
    sound(kCollectSounds[random_.next(kCollectSoundCount)]);
    const std::string name = pickups_[index].name;
    inventory_.setHealth(health_);
    inventory_.collect(name);
    health_ = inventory_.health();
    collected_.push_back(name);
    pickups_.erase(pickups_.begin() + (ptrdiff_t)index);
}

void FallSim::spawn(const std::string& name) {
    // C# FallSim.Spawn. The Vector3's x and y draws, then the chute's: the C#'s
    // left-to-right argument order, sequenced here.
    sound("P_FALL");
    const float x = randomRange(kPickupSpread * kLimitX);
    const float y = randomRange(kPickupSpread * kLimitY);
    Pickup pickup;
    pickup.name = name;
    pickup.position = Vec3f{x, y, kurtPosition_.z + kPickupAbove};
    pickup.velocity = Vec3f{0.0f, 0.0f, -kPickupFallSpeed};
    pickup.fallTicks = kChuteTicks + random_.next(kChuteRandom);
    pickups_.push_back(pickup);
}

// ============================================================================
// FallSim.Radar.cs — the radars (0x4130ac create, 0x412af8 update): one at a
// time, its beam rises from the ground to just below Kurt, wanders between
// targets (Kurt, a pickup, random points) and, when its spot comes within 15
// units of him, he's seen: missiles are queued and it retracts.
//
//   wait ──► Extend (3000 u/s up) ──► Track (wanders) ──seen──► Retract (1500 u/s down) ──► wait
// ============================================================================

void FallSim::startRadars() {
    // C# FallSim.StartRadars.
    radarWait_ = random_.next(kRadarFirstRandom) + kRadarFirstWait;
}

void FallSim::updateRadar(float dt, int32_t ticks) {
    // C# FallSim.UpdateRadar.
    if (!radarActive_ && !startRadar(ticks)) {
        return;
    }

    switch (radar_) {
        case Extend:
            extend(dt);
            break;
        case Track:
            track(dt, ticks);
            break;
        default:
            retract(dt);
            break;
    }
}

bool FallSim::startRadar(int32_t ticks) {
    // C# FallSim.StartRadar: the next radar once its wait is over (none after 30 s).
    if (time_ > kSteerTime) {
        return false;
    }

    radarWait_ -= ticks;
    if (radarWait_ > 0) {
        return false;
    }

    radarActive_ = true;
    radar_ = Extend;
    randomPoint(&radarTargetX_, &radarTargetY_);
    radarHeight_ = kurtPosition_.z - kRadarBelow;
    radarSpot_ = Vec3f{0.0f, 0.0f, 0.0f};
    radarVelocityX_ = 0.0f;
    radarVelocityY_ = 0.0f;
    sound("R_START");
    return true;
}

void FallSim::extend(float dt) {
    // C# FallSim.Extend: the spot rises towards Kurt, its xy and radius growing
    // with it.
    radarHeight_ = kurtPosition_.z - kRadarBelow;
    const float z = mathMin(radarSpot_.z + kRadarExtendSpeed * dt, radarHeight_);
    const float f = z / radarHeight_;
    radarSpot_ = Vec3f{radarTargetX_ * f, radarTargetY_ * f, z};
    radarSpotRadius_ = kRadarRadius * f;
    if (z < radarHeight_) {
        return;
    }

    radar_ = Track;
    randomPoint(&radarTargetX_, &radarTargetY_);
    radarTicks_ = 0;
}

void FallSim::track(float dt, int32_t ticks) {
    // C# FallSim.Track.
    radarHeight_ = kurtPosition_.z - kRadarBelow;
    radarSpotRadius_ = kRadarRadius;
    float spotX = radarSpot_.x;
    float spotY = radarSpot_.y;
    float towardsX = radarTargetX_ - spotX;
    float towardsY = radarTargetY_ - spotY;
    if (towardsX != 0.0f || towardsY != 0.0f) {
        normalize2(&towardsX, &towardsY);
    }

    // RadarInertia * v + (1f - RadarInertia) * BeamSpeed * towards: the C#
    // multiplies (0.25 × beam speed) first, then the vector.
    const float pull = (1.0f - kRadarInertia) * beamSpeed_;
    for (int32_t i = 0; i < ticks; i++) {
        radarVelocityX_ = kRadarInertia * radarVelocityX_ + pull * towardsX;
        radarVelocityY_ = kRadarInertia * radarVelocityY_ + pull * towardsY;
    }

    spotX += radarVelocityX_ * dt;
    spotY += radarVelocityY_ * dt;
    radarSpot_ = Vec3f{spotX, spotY, radarHeight_};
    radarTicks_ += ticks;
    const float nearX = fabsf(radarTargetX_ - spotX);
    const float nearY = fabsf(radarTargetY_ - spotY);
    if (radarTicks_ >= kRadarTargetTicks || (nearX < kRadarNear && nearY < kRadarNear)) {
        sound("R_MOVE");
        chooseTarget(&radarTargetX_, &radarTargetY_);
        radarTicks_ = 0;
    }

    // Vector2.Distance(kurt, spot): sqrt of the plain dot of kurt − spot.
    const float dx = kurtPosition_.x - spotX;
    const float dy = kurtPosition_.y - spotY;
    if (sqrtf(dx * dx + dy * dy) < kRadarSee && !dying_) {
        detected();
    }
}

void FallSim::retract(float dt) {
    // C# FallSim.Retract.
    const float z = radarSpot_.z - kRadarRetractSpeed * dt;
    if (z <= 0.0f) {
        radarActive_ = false;
        radarWait_ = radarGap_ + random_.next(kRadarGapRandom);
        return;
    }

    radarSpotRadius_ = kRadarRadius * z / radarHeight_;
    radarSpot_.z = z;
}

void FallSim::detected() {
    // C# FallSim.Detected: Kurt was seen (K_SEEN): the radar retracts, missiles
    // are queued and the screen flashes.
    sound("K_SEEN");
    radar_ = Retract;
    missileQueue_ += missilesPerDetection_ + random_.next(2);
    missileWait_ = missileWait_ < 1 ? missileWait_ : 1;
    rate_ = kFlashRate;
    target_ = mathMax(target_ - kDetectDim, kDetectFloor);
}

void FallSim::chooseTarget(float* x, float* y) {
    // C# FallSim.ChooseTarget (0x412a38): Kurt, a falling pickup or a random
    // point. Kurt first, then at most 8 pickups, then 3 random points (always
    // drawn), then the pick.
    float choicesX[kRadarChoices + kRadarRandomChoices];
    float choicesY[kRadarChoices + kRadarRandomChoices];
    int32_t count = 0;
    choicesX[count] = kurtPosition_.x;
    choicesY[count] = kurtPosition_.y;
    count++;
    for (size_t i = 0; i < pickups_.size(); i++) {
        if (count < kRadarChoices) {
            choicesX[count] = pickups_[i].position.x;
            choicesY[count] = pickups_[i].position.y;
            count++;
        }
    }

    for (int32_t i = 0; i < kRadarRandomChoices; i++) {
        randomPoint(&choicesX[count], &choicesY[count]);
        count++;
    }

    const int32_t pick = random_.next(count);
    *x = choicesX[pick];
    *y = choicesY[pick];
}

// ============================================================================
// FallScreen's setup and Viewer's carry.
// ============================================================================

int32_t fallIndex(int32_t level) {
    // FallScreen: Math.Clamp(GameState.IndexOf(state.Level), 0, LastIndex).
    const int32_t index = flow::indexOf(level);
    return index < 0 ? 0 : (index > kLastIndex ? kLastIndex : index);
}

bool fallSetup(const formats::Bni& bni, int32_t level, Kurt::Difficulty difficulty, FallSim::Setup* out) {
    // FallScreen's constructor: `new FallSim.Setup(index, difficulty,
    // Fall3d.Pickups(bni, $"FALLPU_{n}"), Animation("KURTANIM"),
    // Animation("KURT_HIT"), Animation("BONESANM"))`, n = index + 1.
    FallSim::Setup setup;
    setup.index = fallIndex(level);
    setup.difficulty = difficulty;
    // n is 1-5 (fallIndex clamps the index to 0-4), one digit.
    const std::string name = std::string("FALLPU_") + (char)('1' + setup.index);
    setup.pickups = formats::Fall3d::pickups(bni, name);
    if (!animationOf(bni, "KURTANIM", &setup.kurt) || !animationOf(bni, "KURT_HIT", &setup.kurtHit) ||
        !animationOf(bni, "BONESANM", &setup.bones)) {
        return false;
    }
    *out = setup;
    return true;
}

void carryIntoLevel(const flow::FallCarry& carry, Kurt* kurt) {
    // Viewer.CarryFromFall: `foreach (pickup) _kurt.Collect(pickup)`, then
    // `_kurt.SetHealth(carry.Health)`, before the HUD subscribes PickedUp.
    const Kurt::PickupHook hook = kurt->pickupHook();
    kurt->setPickupHook(Kurt::PickupHook());
    for (size_t i = 0; i < carry.pickups.size(); i++) {
        kurt->collect(carry.pickups[i]);
    }
    kurt->setPickupHook(hook);
    kurt->setHealth(carry.health);
}

} // namespace fall
} // namespace mdk

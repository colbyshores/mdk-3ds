// ============================================================================
// bomber.cpp — C# src/Mdk.Game/Scripts/Bomber.cs, operation for operation
// (package #11). See bomber.h for the ride's shape.
// ============================================================================

#include "scripts/bomber.h"

#include <math.h>

#include <deque>
#include <vector>

#include "game/follow_camera.h"
#include "game/input.h"
#include "game/player.h"
#include "scripts/object.h"
#include "scripts/rides.h"
#include "scripts/script_runtime.h"

namespace mdk {
namespace scripts {

namespace {

// C# Bomber.cs:19-49.
// The view (0x4183f0): 50 units above Kurt at the mount, sinking 25 units/s.
const float kCameraHeight = 50.0f;
const float kCameraDescent = 25.0f;
// The cursor (0x573b0c): kept in a box; keys accelerate it by 1/3 px/tick² up to
// 10 px/tick, it brakes by 2/3 px/tick²; the mouse moves it by 1/3 of its motion.
const float kCursorMinX = 128.0f;
const float kCursorMinY = 64.0f;
const float kCursorMaxX = 472.0f;
const float kCursorMaxY = 296.0f;
const float kCursorAccel = 1.0f / 3.0f;
const float kCursorSpeed = 10.0f;
const float kCursorBrake = 2.0f / 3.0f;
const float kMouseScale = 1.0f / 3.0f;
// Bombs (0x573c64): 10, one back per second.
const int32_t kBombs = 10;
const float kRefillTime = 1.0f;
// The aim: a ray from Kurt through the cursor with the view's focal length (600 /
// zoom 2.4), as long as 1000 times that direction; the bomb starts 5 below Kurt and
// falls by 32 u/s² (the XE's gravity) onto the hit point, or for 2.5 s when nothing
// is hit.
const float kFocal = 250.0f;
const float kRayLength = 1000.0f;
const float kDropBelow = 5.0f;
const float kFallGravity = 32.0f;
const float kMissTime = 2.5f;
// The bomb: a thrown item of kind 0x81 (0x43deac) living 900 ticks, flags 0x818a6.
const char* const kBomb = "XBN_BOMB";
const int32_t kBombTicks = 900;
const int32_t kBombFlags = 0x818a6;
const char* const kDropSound = "DROP";
// Objects the aim passes through (not solid, not a target).
const int32_t kUnaimed = MdkObject::FlagNotSolid | MdkObject::FlagNotTarget;
// C# Kurt.Ticks (Kurt.cs): the original's 30 ticks a second.
const float kTicks = 30.0f;

// C# MathF.Sign as the float the C#'s int * float arithmetic promotes it to.
float sign(float v) { return v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f); }

// C# Math.Clamp(float, float, float).
float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// C# Bomber.Axis (Bomber.cs:272): 1 for the positive key, -1 for the negative one.
float keyAxis(const Input& input, Key positive, Key negative) {
    return (input.isDown(positive) ? 1.0f : 0.0f) - (input.isDown(negative) ? 1.0f : 0.0f);
}

float comp(const Vec3& v, int i) { return i == 0 ? v.x : (i == 1 ? v.y : v.z); }
float comp(const formats::Vec3f& v, int i) { return i == 0 ? v.x : (i == 1 ? v.y : v.z); }

// C# ScriptMath.SegmentEntry (ScriptMath.cs:89-119): where a segment enters a box (its
// start when inside); false for none. The same three slabs scripts/platforms.cpp ports
// (file-local there too).
bool segmentEntry(const formats::Box& box, const Vec3& from, const Vec3& to, Vec3* out) {
    const Vec3 d{to.x - from.x, to.y - from.y, to.z - from.z};
    float enter = 0.0f;
    float leave = 1.0f;
    for (int axis = 0; axis < 3; axis++) {
        const float start = comp(from, axis);
        const float step = comp(d, axis);
        if (step == 0.0f) {
            if (start < comp(box.min, axis) || start > comp(box.max, axis)) {
                return false;
            }
            continue;
        }
        const float t0 = (comp(box.min, axis) - start) / step;
        const float t1 = (comp(box.max, axis) - start) / step;
        const float near = t0 < t1 ? t0 : t1;
        const float far = t0 < t1 ? t1 : t0;
        enter = enter > near ? enter : near;
        leave = leave < far ? leave : far;
        if (enter > leave) {
            return false;
        }
    }
    *out = Vec3{from.x + d.x * enter, from.y + d.y * enter, from.z + d.z * enter};
    return true;
}

// C# Vector3.Distance.
float distance(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

}  // namespace

void Bomber::start(MdkObject* xe) {
    // C# Bomber's constructor and field initialisers (Bomber.cs:51-66).
    xe_ = xe;
    speedX_ = 0.0f;
    speedY_ = 0.0f;
    refill_ = kRefillTime;
    fireHeld_ = false;
    viewHeight_ = kCameraHeight;
    cursorX_ = kCursorCentreX;
    cursorY_ = kCursorCentreY;
    bombsLeft_ = kBombs;
}

bool Bomber::locked() const {
    // C# Bomber.Locked (Bomber.cs:69).
    return xe_ != nullptr && (xe_->flags & Rides::kFlagLocked) != 0;
}

bool Bomber::shown(Sight* out) const {
    // C# Bomber.Shown (Bomber.cs:75): `Locked ? null : new BomberSight(Cursor, BombsLeft)`.
    if (locked()) {
        return false;
    }
    out->cursorX = cursorX_;
    out->cursorY = cursorY_;
    out->bombs = bombsLeft_;
    return true;
}

void Bomber::stepTrampoline(void* context, const Input& input, float delta) {
    static_cast<Bomber*>(context)->update(input, delta);
}

void Bomber::update(const Input& input, float delta) {
    // C# Bomber.Update (Bomber.cs:78-109): Kurt goes with the XE, the view sinks, the
    // cursor moves and drops bombs.
    Player* kurt = runtime_.player_;
    if (kurt == nullptr || xe_ == nullptr) {
        return;
    }
    kurt->feet = xe_->position;
    kurt->yaw = xe_->yaw;
    kurt->verticalSpeed = 0.0f;
    const float sunk = viewHeight_ - kCameraDescent * delta;
    viewHeight_ = sunk > 0.0f ? sunk : 0.0f;
    kurt->hasTopDownHeight = true;
    kurt->topDownHeight = viewHeight_;
    if (hidesXe()) {
        xe_->flags |= MdkObject::FlagNotSolid;
    }

    const bool fire = input.isDown(kKeyFire);
    if (locked()) {
        speedY_ = 0.0f;
    } else {
        moveCursor(input, delta);
        if (fire && !fireHeld_ && bombsLeft_ > 0) {
            drop();
        }

        refillBombs(delta);
    }

    fireHeld_ = fire;
    passDamage();
}

void Bomber::moveCursor(const Input& input, float delta) {
    // C# Bomber.MoveCursor (Bomber.cs:111-128).
    const float ticks = delta * kTicks;
    const float turn = keyAxis(input, kKeyTurnRight, kKeyTurnLeft);
    const float strafe = keyAxis(input, kKeyStrafeRight, kKeyStrafeLeft);
    const float axisX = fabsf(turn) >= fabsf(strafe) ? turn : strafe;
    const float axisY = keyAxis(input, kKeyBack, kKeyForward);
    speedX_ = cursorStep(speedX_, axisX, ticks);
    speedY_ = cursorStep(speedY_, axisY, ticks);

    // The mouse moves it only while no key does.
    const float mouseX = input.mouseX * kMouseScale;
    const float mouseY = input.mouseY * kMouseScale;
    if (axisX == 0.0f && axisY == 0.0f && (mouseX != 0.0f || mouseY != 0.0f)) {
        cursorX_ += mouseX;
        cursorY_ += mouseY;
        speedX_ = 0.0f;
        speedY_ = 0.0f;
    }

    // Vector2.Clamp(Cursor + _speed * ticks, CursorMin, CursorMax): Min(Max(v, min), max).
    float x = cursorX_ + speedX_ * ticks;
    float y = cursorY_ + speedY_ * ticks;
    x = x > kCursorMinX ? x : kCursorMinX;
    y = y > kCursorMinY ? y : kCursorMinY;
    cursorX_ = x < kCursorMaxX ? x : kCursorMaxX;
    cursorY_ = y < kCursorMaxY ? y : kCursorMaxY;
}

float Bomber::cursorStep(float speed, float axis, float ticks) {
    // C# Bomber.CursorStep (Bomber.cs:132-143).
    if (axis == 0.0f) {
        return fabsf(speed) <= kCursorBrake * ticks ? 0.0f
                                                    : speed - sign(speed) * kCursorBrake * ticks;
    }

    const float step = axis * kCursorAccel * ticks;
    const float result = speed == 0.0f || sign(speed) == sign(step) ? speed + step : step;
    const float limit = fabsf(axis) * kCursorSpeed;
    return clampf(result, -limit, limit);
}

void Bomber::refillBombs(float delta) {
    // C# Bomber.RefillBombs (Bomber.cs:145-159).
    if (bombsLeft_ >= kBombs) {
        refill_ = kRefillTime;
        return;
    }

    refill_ -= delta;
    if (refill_ <= 0.0f) {
        bombsLeft_++;
        refill_ = kRefillTime;
    }
}

void Bomber::drop() {
    // C# Bomber.Drop (Bomber.cs:168-188): a bomb from under the XE, thrown so it falls
    // onto the point under the cursor.
    //
    //        Kurt ●───────► (vx, vy, 0)
    //             ╎  ╲ falls by gravity for t = √(2h / g)
    //             ╎     ╲
    //   ──────────┴───────✕ hit point (ray through the cursor)
    bombsLeft_--;
    const Vec3 kurt = runtime_.kurtPosition();
    const Vec3 start{kurt.x - 0.0f, kurt.y - 0.0f, kurt.z - kDropBelow};
    bool hit = false;
    const Vec3 target = aim(kurt, cursorX_, cursorY_, xe_->yaw, &hit);
    const float time = hit ? fallTime(start.z - target.z) : kMissTime;

    MdkObject* controller = &runtime_.getArenaState(runtime_.currentArena()).controller;
    MdkObject* bomb = runtime_.spawn(controller, kBomb, start, xe_->yaw, -1, 0, false);
    if (bomb == nullptr) {
        return;
    }

    bomb->flags |= kBombFlags;
    bomb->thrown_kind = ScriptRuntime::kItemKindBomb;
    bomb->item_ticks = kBombTicks;
    bomb->friction = 0.0f;
    bomb->velocity = Vec3{(target.x - start.x) / time, (target.y - start.y) / time, 0.0f};
    // C# `Mixer.PlayOn(DropSound, () => bomb.Position)`: a voice that follows the bomb.
    // The C++ mixer has no delegate placement; its Source placement at a zero offset is
    // the same point (MdkObject.SoundPosition(0) = Position), and the objects' deque
    // keeps the bomb's address for as long as the voice reads it.
    bomb->loop_sound =
        runtime_.mixer().playOn(kDropSound, &bomb->position, &bomb->yaw, Vec3{0.0f, 0.0f, 0.0f});
}

Vec3 Bomber::aim(const Vec3& kurt, float cursorX, float cursorY, float yaw, bool* hit) {
    // C# Bomber.Aim (Bomber.cs:191-203): where the ray through the cursor meets an object
    // of Kurt's arena or the arena.
    const Vec3 direction = aimDirection(cursorX, cursorY, yaw);
    const Vec3 end{kurt.x + direction.x * kRayLength, kurt.y + direction.y * kRayLength,
                   kurt.z + direction.z * kRayLength};
    Vec3 target = end;
    Vec3 nearest;
    if (firstObject(kurt, end, &nearest)) {
        target = nearest;
    }
    *hit = target.x != end.x || target.y != end.y || target.z != end.z;
    Vec3 point;
    if (runtime_.raycastPoint(kurt, target, &point)) {
        *hit = true;
        return point;
    }

    return target;
}

Vec3 Bomber::aimDirection(float cursorX, float cursorY, float yaw) {
    // C# Bomber.AimDirection (Bomber.cs:206-212).
    const float offsetX = cursorX - kCursorCentreX;
    const float offsetY = cursorY - kCursorCentreY;
    const float radians = follow_camera::degreesToRadians(yaw);
    /* @vfp-exempt R6 — the aim is gameplay (it sets where each bomb lands), and .NET takes
     * MathF.Sin/Cos of the XE's yaw; glibc sinf/cosf match it bit for bit on this host
     * (golden/L7_bomber_bombs, _target) while the fastSin/fastCos LUT moves the bombs'
     * velocities a ULP (both goldens then differ in the bombs' columns; the guard still dies).
     * Once per fire press, not per frame. measured n/a (corpus empty). Verified 2026-11-20. */
    const float s = sinf(radians);
    const float c = cosf(radians);
    return Vec3{s * offsetX - c * offsetY, -c * offsetX - s * offsetY, -kFocal};
}

float Bomber::fallTime(float height) {
    // C# Bomber.FallTime (Bomber.cs:215-219).
    const float time = sqrtf((height > 0.0f ? height : 0.0f) * 2.0f / kFallGravity);
    return time <= 0.0f ? kMissTime : time;
}

bool Bomber::firstObject(const Vec3& start, const Vec3& end, Vec3* nearest) {
    // C# Bomber.FirstObject (Bomber.cs:223-251): where the segment first meets a part of
    // an object of Kurt's arena (alive, not flagged 0x30: the hidden XE doesn't count).
    // The C#'s Objects hold no removed object; this deque does, so they are skipped.
    bool found = false;
    std::deque<MdkObject>& objects = runtime_.objects();
    for (size_t o = 0; o < objects.size(); o++) {
        MdkObject* obj = &objects[o];
        Vec3 entry;
        if (runtime_.isRemoved(obj) || obj->dead || obj->health == 0 ||
            obj->arena != runtime_.currentArena() || obj->model == nullptr ||
            (obj->flags & kUnaimed) != 0 ||
            !segmentEntry(runtime_.worldBounds(obj), start, end, &entry)) {
            continue;
        }

        const std::vector<ScriptRuntime::PartBox> parts = runtime_.partBounds(obj);
        for (size_t i = 0; i < parts.size(); i++) {
            // C# `1 << i` takes the count mod 32.
            Vec3 point;
            if ((obj->hidden_parts & (int32_t)(1u << (i & 31u))) != 0 || !parts[i].valid ||
                !segmentEntry(runtime_.worldBounds(obj, parts[i].box), start, end, &point)) {
                continue;
            }

            if (!found || distance(start, point) < distance(start, *nearest)) {
                *nearest = point;
                found = true;
            }
        }
    }

    return found;
}

void Bomber::passDamage() {
    // C# Bomber.PassDamage (Bomber.cs:255-270): hits on Kurt went to the XE
    // (Rides.TakesHits); Kurt takes them, and if he dies the XE does too (its death
    // script drops him).
    if (xe_->health == kHealth) {
        return;
    }

    Kurt& kurt = runtime_.kurt();
    kurt.hurt(kHealth - xe_->health);
    xe_->health = kHealth;
    if (kurt.health() < 1) {
        xe_->health = 0;
        runtime_.kill(xe_);
    }
}

}  // namespace scripts
}  // namespace mdk

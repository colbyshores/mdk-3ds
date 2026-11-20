// ============================================================================
// items.cpp — Kurt's pickups and thrown items: ScriptRuntime.CollectPickups and
// src/Mdk.Game/Scripts/Items.cs (0x46ce78 throws, 0x43deac updates, 0x463a94
// blasts), with Twister.cs and the state half of Ribbon.cs. See the Items
// section of script_runtime.h for the diagram.
//
// Operation for operation against the C#: the same order of tests, the same
// float expressions in the same association, the same object-list walks (the
// C#'s ListCopy is a snapshot of the live objects; a walk over the live list is
// a walk over the deque that skips the removed slots). The goldens that pin it
// are tests/scripts/golden/L*_items_*.txt, printed by the C# probe
// tests/scripts/items_probe and by `tick1 dump items`.
// ============================================================================

#include <math.h>

#include <algorithm>

#include "formats/ascii_case.h"
#include "game/player.h"
#include "scripts/script_runtime.h"
#include "util/math_minmax.h"

namespace mdk {
namespace scripts {

namespace {

// Items.cs's numbers (Items.cs:21-123).
const int32_t kThrownFlags = 0x818a6;
const int32_t kContactHitObject = 0x10;
const float kBombPitchRate = 45.0f;
const float kBombPitchMin = -90.0f;
const char* const kModels[] = {"",       "SW_DUMMY", "SW_INTER", "SW_TWIST", "SW_THUMP",
                               "SW_HBOMB", "SW_GATT", "SW_KEY",   "SW_SEAL",  "SW_SBONE"};
const float kThrowSpeed = 25.0f;
const float kThrowUpSpeed = 15.0f;
const float kGrenadeSpeedFactor = 3.0f;
const float kThrowHeight = 4.0f;
const float kThrowScale = 0.1f;
const float kGrowRate = 3.0f;
const int32_t kFlightTicks = 150;
const int32_t kLongFlightTicks = 750;
const float kSealRollRate = 30.0f;
const int32_t kHitMortar = -3;
const int32_t kHitGrenade = -7;
const int32_t kHitNuke = -8;
const int32_t kHitBomb = -9;
const int32_t kTargetWorld = ScriptRuntime::kBlastObjects | ScriptRuntime::kBlastGroups;
const int32_t kGrenadeDamage = 150;
const int32_t kGrenadeKurtDamage = 75;
const float kGrenadeRadius = 40.0f;
const float kGrenadeExplosion = 2.0f;
const int32_t kDecoyTicks = 450;
const float kDecoySpeed = 5.0f;
const int32_t kBombTicks = 600;
const float kBombSpin = 235.0f;
const int32_t kBombDamage = 450;
const int32_t kBombKurtDamage = 67;
const float kBombRadius = 80.0f;
const float kBigExplosion = 3.0f;
const int32_t kTornadoTicks = 60;
const int32_t kTornadoFirstTwister = 45;
const int32_t kTornadoTwisterEvery = 15;
const float kTornadoSpin = 360.0f;
const int32_t kNukeTicks = 900;
const int32_t kNukeWhiteFrame = 70;
const float kNukeShake = 3.0f;
const float kNukeDoorRangeSquared = 2500.0f;
const int32_t kNukeDoorOpen = 0x80;
const int32_t kDoorStateKeep = 0x1F;
const int32_t kNukeDamage = 200;
const int32_t kNukeKurtDamage = 19;
const float kNukeRadius = 60.0f;
const int32_t kMortarThumps[] = {28, 54, 64, 71, 77, 82, 87, 92, 97, 102, 107, 112, 117, 122, 127};
const int32_t kMortarThumpCount = 15;
const int32_t kMortarActiveTicks = 30;
const int32_t kMortarDamage = 4;
const float kMortarLift = 5.0f;
const float kMortarShake = 5.0f;
const int32_t kMortarKnockThump = 4;
const int32_t kMortarIgnored = 0x1030;
const int32_t kSealPickupTicks = 601;
const float kSealShrink = 0.9f;
const float kSealGone = 0.1f;
const int32_t kSealHeld = 1;
const int32_t kSealLanded = 2;
const float kBlastLift = 1.0f;
const int32_t kBlastKurtMax = 15;
const float kBlastKurtDistance = 2.0f;
const char kDecoyThrown[] = "SW_DUM_I";
const char kDecoyWalk[] = "SW_DUM_M";
const char kBombAnimation[] = "SW_INTER";
const char kMortarAnimation[] = "SW_THUMP";
const char kNuke[] = "SW_NUKE";
const char kSealLanding[] = "XMT_LAND";
const char kExplodeSound[] = "EXPLODE";
// ScriptRuntime.SpawnExplosion's default lift (ScriptRuntime.cs:106).
const float kDefaultLift = 3.0f;
// ScriptRuntime.HitBlast / HitOtherBlast (ScriptRuntime.cs:117-118).
const int32_t kHitBlast = 3;
const int32_t kHitOtherBlast = 4;
// ObjectMotion's hit event of a blast (Items.cs:637).
const int32_t kBlastEvent = -2;
// The fire sparks (ScriptRuntime.cs:92-96: FireSparks, SparkColours[Fire]).
const int32_t kFireSparks = 16;
const int32_t kFireSparkColour = 0x30;
const int32_t kFireSparkRange = 0x10;

// Twister.cs's numbers.
const float kSpiralRate = 720.0f;
const float kSpiralEnd = 1440.0f;
const float kSpiralSize = 15.0f;
const float kSpiralSpeed = 200.0f;
const int32_t kTwisterLifetime = 150;
const float kKeep = 0.9f;
const float kPull = 20.0f;
const int32_t kTwisterDamage = 2;
const float kWallGap = 0.1f;
const int32_t kTwisterHitEvent = -1;

const float kPi = 3.14159265358979323846f;

// ScriptMath.Wrap360 (ScriptMath.cs:14), the floor form.
float wrap360(float degrees) {
    return degrees - 360.0f * floorf(degrees / 360.0f);
}

// ScriptMath.FromAngle (ScriptMath.cs:23), as script_runtime.cpp's: .NET's
// left-associative DegreesToRadians, then the libm pair.
/* @vfp-exempt R6 — the thrown item's, the decoy's and the twister's heading, once
 * per throw or spiral step (cold: an item in flight, not a per-object loop); glibc
 * cosf/sinf match the C# oracle's MathF.Cos/Sin bit for bit on this machine (as for
 * script_runtime.cpp's fromAngle), and the LUT would move the flight the items
 * goldens pin. measured n/a (corpus empty). Verified 2026-11-20. */
Vec3 fromAngle(float degrees) {
    const float radians = degrees * kPi / 180.0f;
    return Vec3{cosf(radians), sinf(radians), 0.0f};
}

// ScriptMath.Heading (ScriptMath.cs:43): RadiansToDegrees(Atan2(dy, dx)), unwrapped.
/* @vfp-exempt R6 — a blast's, the mortar's and a twister's hit direction, once per
 * object hit (cold); glibc atan2f matches the oracle's MathF.Atan2 bit for bit on
 * this machine (script_runtime.cpp's heading), and the direction is what a kill's
 * explosion faces. measured n/a (corpus empty). Verified 2026-11-20. */
float heading(const Vec3& from, const Vec3& to) {
    return atan2f(to.y - from.y, to.x - from.x) * 180.0f / kPi;
}

// ScriptMath.WrapAngle (ScriptMath.cs:17): an angle in [-180, 180).
float wrapAngle(float degrees) {
    return degrees - 360.0f * floorf((degrees + 180.0f) / 360.0f);
}

// ScriptMath.MoveToward (ScriptMath.cs:19).
float moveTowardf(float from, float to, float step) {
    const float d = to - from;
    return fabsf(d) <= step ? to : from + (d > 0.0f ? 1.0f : (d < 0.0f ? -1.0f : 0.0f)) * step;
}

// ScriptMath.Intersects (ScriptMath.cs:78): strict on every axis.
bool boxesIntersect(const formats::Box& a, const formats::Box& b) {
    return a.min.x < b.max.x && a.max.x > b.min.x && a.min.y < b.max.y && a.max.y > b.min.y &&
           a.min.z < b.max.z && a.max.z > b.min.z;
}

// ScriptMath.Center (ScriptMath.cs:74).
Vec3 boxCenter(const formats::Box& box) {
    return Vec3{(box.min.x + box.max.x) * 0.5f, (box.min.y + box.max.y) * 0.5f,
                (box.min.z + box.max.z) * 0.5f};
}

// ScriptMath.Contains (ScriptMath.cs:81): inclusive.
bool boxContains(const formats::Box& box, const Vec3& p) {
    return p.x >= box.min.x && p.y >= box.min.y && p.z >= box.min.z && p.x <= box.max.x &&
           p.y <= box.max.y && p.z <= box.max.z;
}

// ScriptMath.SegmentEntry (ScriptMath.cs:89) as its null test: whether the segment
// enters the box. MathF.Max/Min (mathMax/mathMin), so a NaN keeps the C#'s answer.
bool segmentEnters(const formats::Box& box, const Vec3& from, const Vec3& to) {
    const float start[3] = {from.x, from.y, from.z};
    const float d[3] = {to.x - from.x, to.y - from.y, to.z - from.z};
    const float lo[3] = {box.min.x, box.min.y, box.min.z};
    const float hi[3] = {box.max.x, box.max.y, box.max.z};
    float enter = 0.0f;
    float leave = 1.0f;
    for (int axis = 0; axis < 3; axis++) {
        if (d[axis] == 0.0f) {
            if (start[axis] < lo[axis] || start[axis] > hi[axis]) {
                return false;
            }
            continue;
        }
        const float t0 = (lo[axis] - start[axis]) / d[axis];
        const float t1 = (hi[axis] - start[axis]) / d[axis];
        enter = mathMax(enter, mathMin(t0, t1));
        leave = mathMin(leave, mathMax(t0, t1));
        if (enter > leave) {
            return false;
        }
    }
    return true;
}

// Vector3.DistanceSquared / Distance, summed in the C#'s x, y, z order.
float distanceSquared(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

float dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 scaled(const Vec3& v, float s) {
    return Vec3{v.x * s, v.y * s, v.z * s};
}

Vec3 plus(const Vec3& a, const Vec3& b) {
    return Vec3{a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 minus(const Vec3& a, const Vec3& b) {
    return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}

// MdkObject.IsAnimationDone (MdkObject.cs:380).
bool animationDone(const MdkObject* obj) {
    return obj->animation == nullptr || obj->animation_end_frame == MdkObject::AnimationEnded;
}

// Items.IsDecoyKind (Items.cs:172): decoys, seals and bones may be out with another item.
bool isDecoyKind(int32_t item) {
    return item == Kurt::Dummy || item == Kurt::Seal || item == Kurt::SuperBone;
}

} // namespace

// ---------------------------------------------------------------------------
// The pickups (ScriptRuntime.cs:2571-2610)
// ---------------------------------------------------------------------------

void ScriptRuntime::collectPickups() {
    // C# CollectPickups: the segment Kurt moved along this tick (his previous position to
    // his position) crosses a pickup's world bounds, grown by 1 unit (and 5 downwards).
    const float kShrinkTicks = 30.0f;
    std::vector<MdkObject*> snapshot;
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        if (!it->dead) {
            snapshot.push_back(&(*it));
        }
    }
    for (size_t i = 0; i < snapshot.size(); i++) {
        MdkObject* obj = snapshot[i];
        if (obj->dead || obj->arena != currentArena_ || (obj->flags & MdkObject::FlagPickup) == 0 ||
            (obj->flags & (MdkObject::FlagCollected | MdkObject::FlagNotSolid)) != 0) {
            continue;
        }

        const formats::Box world = worldBounds(obj);
        formats::Box bounds;
        bounds.min.x = world.min.x - 1.0f;
        bounds.min.y = world.min.y - 1.0f;
        bounds.min.z = world.min.z - 5.0f;
        bounds.max.x = world.max.x + 1.0f;
        bounds.max.y = world.max.y + 1.0f;
        bounds.max.z = world.max.z + 1.0f;
        if (!boxContains(bounds, kurtPosition_) &&
            !segmentEnters(bounds, previousKurtPosition_, kurtPosition_)) {
            continue;
        }

        const char* sound = kurt().collect(obj->type_name);
        if (sound[0] == '\0') {
            continue;
        }

        mixer_.play(sound, audio::SoundMixer::Restart);
        if (obj->type_name == "SW_EWJ") {
            dropCow();
        }

        obj->flags |= MdkObject::FlagCollected | kItemFlagThrown;
        obj->parameter_timer = kShrinkTicks;
        obj->velocity = Vec3{0.0f, 0.0f, 0.0f};
        obj->flags &= ~MdkObject::FlagGravity;
        if (obj->attached != nullptr) {
            remove(obj->attached);
            obj->attached = nullptr;
        }
    }
}

// ---------------------------------------------------------------------------
// The holy cow of SW_EWJ (ScriptRuntime.cs:2505-2569, ObjectBehaviors.cs:326-397)
// ---------------------------------------------------------------------------

void ScriptRuntime::dropCow() {
    // C# DropCow (ScriptRuntime.cs:2507-2523): a cow falls from 100 units above the enemy
    // Kurt faces (CowTarget), or above Kurt himself.
    const float kCowHeight = 100.0f;
    const float kCowFuse = 0.5f;
    const int32_t kCowFlags = MdkObject::FlagGravity | MdkObject::FlagCollides |
                              MdkObject::FlagNotSolid2 | MdkObject::FlagCow;
    mixer_.play("COW", audio::SoundMixer::Restart);
    MdkObject* target = cowTarget();
    const Vec3 point = target != nullptr ? target->position : kurtPosition_;
    MdkObject* cow = spawn(&getArenaState(currentArena_).controller, "SW_HCOW",
                           Vec3{point.x + 0.0f, point.y + 0.0f, point.z + kCowHeight}, 0.0f, -1, 0,
                           false);
    if (cow == nullptr) {
        return;
    }

    cow->flags = kCowFlags;
    cow->health = kIndestructible;
    cow->velocity.z = -cow->gravity;
    cow->cow_target = target;
    cow->parameter_timer = kCowFuse;
}

MdkObject* ScriptRuntime::cowTarget() {
    // C# CowTarget (ScriptRuntime.cs:2528-2568): the object of Kurt's arena within 600
    // units with the lowest score (its distance, + 400 beyond 30 degrees of Kurt's yaw,
    // + 1000 beyond 50) that has open sky 100 units above it and no cow on it already.
    // The live list in order; a later tie wins (`score > bestScore` skips only worse).
    const float kCowRange = 600.0f;
    const float kEyeHeight = 5.0f;
    const float kCowHeight = 100.0f;
    MdkObject* best = nullptr;
    float bestScore = INFINITY;
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        MdkObject* obj = &(*it);
        if (obj->dead || obj->arena != currentArena_ ||
            (obj->flags & (MdkObject::FlagNotSolid | MdkObject::FlagNotTarget)) != 0 ||
            obj->health >= kIndestructible) {
            continue;
        }

        const float distance = sqrtf(distanceSquared(obj->position, kurtPosition_));
        if (distance > kCowRange) {
            continue;
        }

        const float angle = fabsf(wrapAngle(kurtYaw_ - heading(kurtPosition_, obj->position)));
        const float score = distance + (angle < 30.0f ? 0.0f : angle < 50.0f ? 400.0f : 1000.0f);
        if (score > bestScore) {
            continue;
        }

        const Vec3 above{obj->position.x + 0.0f, obj->position.y + 0.0f, obj->position.z + kEyeHeight};
        if (!raycastClear(above, Vec3{above.x + 0.0f, above.y + 0.0f, above.z + kCowHeight})) {
            continue;
        }

        bool covered = false;
        for (std::deque<MdkObject>::iterator other = objects_.begin();
             other != objects_.end() && !covered; ++other) {
            covered = !other->dead && (other->flags & MdkObject::FlagCow) != 0 &&
                      other->cow_target == obj;
        }
        if (covered) {
            continue;
        }

        best = obj;
        bestScore = score;
    }
    return best;
}

void ScriptRuntime::updateCow(MdkObject* obj) {
    // C# ObjectBehaviors.UpdateCow (ObjectBehaviors.cs:328-353): it falls on its target
    // (or Kurt), drifting over it at 50 u/s, hits what it overlaps until it lands, and
    // goes half a second after.
    const float kCowSpeed = 50.0f;
    const Vec3 target =
        obj->cow_target != nullptr && !obj->cow_target->dead ? obj->cow_target->position : kurtPosition_;
    const bool landed = (obj->contact_flags & MdkObject::ContactFloor) != 0;
    if ((obj->flags & MdkObject::FlagNotTarget) == 0) {
        cowHits(obj);
        if (!landed) {
            obj->position.x = moveTowardf(obj->position.x, target.x, kCowSpeed * kTick);
            obj->position.y = moveTowardf(obj->position.y, target.y, kCowSpeed * kTick);
        }
    }

    if (!landed) {
        return;
    }

    obj->flags |= MdkObject::FlagNotTarget;
    obj->parameter_timer -= kTick;
    if (obj->parameter_timer < 0.0f) {
        kill(obj);
    }
}

void ScriptRuntime::cowHits(MdkObject* obj) {
    // C# ObjectBehaviors.CowHits (ObjectBehaviors.cs:356-397): the objects of its arena it
    // overlaps lose 1000 health (a blast hit), Kurt 10.
    const int32_t kCowDamage = 1000;
    const int32_t kCowKurtDamage = 10;
    const int32_t kCowHitEvent = -3;
    const int32_t kCowHitType = -4;
    const formats::Box bounds = worldBounds(obj);
    bool hit = false;
    std::vector<MdkObject*> snapshot;
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        if (!it->dead) {
            snapshot.push_back(&(*it));
        }
    }
    for (size_t i = 0; i < snapshot.size(); i++) {
        MdkObject* other = snapshot[i];
        if (other == obj || other->dead || other->arena != obj->arena ||
            (other->flags & (MdkObject::FlagNotTarget | MdkObject::FlagNotSolid2)) != 0) {
            continue;
        }

        if (!boxesIntersect(bounds, worldBounds(other))) {
            continue;
        }

        hit = true;
        other->hit_event = kCowHitEvent;
        other->hit_type = kCowHitType;
        if (other->health >= kIndestructible) {
            continue;
        }

        other->health -= kCowDamage;
        if (other->health <= 0) {
            kill(other);
        }
    }

    if (boxesIntersect(bounds, kurtBox())) {
        hit = true;
        hurtKurt(kCowKurtDamage);
    }

    if (hit) {
        obj->flags |= MdkObject::FlagNotTarget;
    }
}

// ---------------------------------------------------------------------------
// Using an item (Items.cs:128-210)
// ---------------------------------------------------------------------------

void ScriptRuntime::itemUsedTrampoline(void* ctx) {
    static_cast<ScriptRuntime*>(ctx)->itemsUseItem();
}

void ScriptRuntime::bombTriggeredTrampoline(void* ctx) {
    static_cast<ScriptRuntime*>(ctx)->itemsTriggerBomb();
}

bool ScriptRuntime::canUseItemTrampoline(void* ctx, Kurt::Item item) {
    return static_cast<ScriptRuntime*>(ctx)->itemsCanUse(item);
}

void ScriptRuntime::itemsForget(MdkObject* obj) {
    // C# Items.Forget (Items.cs:128): an item that goes before it is done.
    if (obj == itemsBomb) {
        itemsBomb = nullptr;
    }
    if (obj == itemsDecoy) {
        itemsDecoy = nullptr;
    }
}

bool ScriptRuntime::itemsCanUse(Kurt::Item item) {
    // C# Items.CanUse (Items.cs:161): one thrown item at a time (decoys, seals and bones
    // excepted), and none while the bomb is out -- "use" then sets it off.
    if (itemsBomb != nullptr && !itemsBomb->dead) {
        return false;
    }
    bool thrown = false;
    for (std::deque<MdkObject>::const_iterator it = objects_.begin(); it != objects_.end() && !thrown;
         ++it) {
        thrown = !it->dead && it->thrown_kind > 0 && !isDecoyKind(it->thrown_kind);
    }
    return !thrown || isDecoyKind(item);
}

void ScriptRuntime::itemsTriggerBomb() {
    // C# Items.TriggerBomb (Items.cs:175, 0x43f258): it goes off at the end of its animation.
    if (itemsBomb != nullptr && !itemsBomb->dead && itemsBomb->animation_end_frame >= 0) {
        itemsBomb->animation_end_frame = MdkObject::NoHoldFrame;
    }
}

void ScriptRuntime::itemsUseItem() {
    // C# Items.UseItem (Items.cs:184-210, 0x46ce78): the selected item's pickup model leaves
    // Kurt 4 units up, along his yaw, under the current arena's controller.
    if (player_ == nullptr) {
        return;
    }
    Kurt& combat = kurt();
    const Kurt::Item item = combat.selectedItem();
    if (item == Kurt::None || currentArena_.empty()) {
        return;
    }

    const float yaw = wrap360(player_->yaw);
    MdkObject* controller = &getArenaState(currentArena_).controller;
    const Vec3 at{player_->feet.x + 0.0f, player_->feet.y + 0.0f, player_->feet.z + kThrowHeight};
    MdkObject* thrown = spawn(controller, kModels[item], at, yaw, -1, 0, false);
    if (thrown == nullptr) {
        return;
    }

    thrown->flags |= kThrownFlags;
    thrown->thrown_kind = item;
    thrown->item_ticks = item == Kurt::Seal || item == Kurt::SuperBone ? kLongFlightTicks : kFlightTicks;
    thrown->friction = 0.0f;
    thrown->scale = kThrowScale;
    const Vec3 direction = scaled(
        fromAngle(yaw), item == Kurt::Grenade ? kThrowSpeed * kGrenadeSpeedFactor : kThrowSpeed);
    thrown->velocity =
        Vec3{direction.x, direction.y, player_->state() == Kurt::Chute ? 0.0f : kThrowUpSpeed};
    if (item == Kurt::Dummy) {
        restartAnimation(thrown, itemsAnimation(kDecoyThrown), false);
        thrown->animation_end_frame = 0;
    }

    combat.consume();
}

// ---------------------------------------------------------------------------
// A thrown item in flight and its activation (Items.cs:213-342)
// ---------------------------------------------------------------------------

void ScriptRuntime::itemsUpdateThrown(MdkObject* obj) {
    // C# Items.UpdateThrown (Items.cs:213-257, 0x43deac), after the object moved.
    // A seal or bone Kurt took back shrinks away like a pickup.
    if ((obj->flags & MdkObject::FlagCollected) != 0) {
        motionPickup(obj);
        return;
    }

    if ((obj->flags & kItemFlagActive) != 0) {
        itemsUpdateActive(obj);
        return;
    }

    obj->scale = mathMin(obj->scale + kTick * kGrowRate, 1.0f);
    if (obj->thrown_kind == Kurt::Seal) {
        obj->roll = wrap360(obj->roll + kTick * kSealRollRate);
    }

    if (obj->thrown_kind == kItemKindBomb) {
        obj->pitch = mathMax(obj->pitch - kBombPitchRate * kTick, kBombPitchMin);
    }

    if (itemsHitsObject(obj)) {
        obj->contact_flags |= kContactHitObject;
    }

    obj->item_ticks--;
    const int32_t kStopped = MdkObject::ContactCollided | MdkObject::ContactFloor | kContactHitObject;
    if ((obj->contact_flags & kStopped) == 0 && obj->item_ticks > 0) {
        return;
    }

    // The mortar only works on a floor.
    if (obj->thrown_kind == Kurt::Mortar && (obj->contact_flags & MdkObject::ContactFloor) == 0) {
        return;
    }

    itemsActivate(obj);
}

bool ScriptRuntime::itemsHitsObject(MdkObject* obj) {
    // C# Items.HitsObject (Items.cs:260-284, 0x43eb48): grenades (and the XE's bombs) stop on
    // any alien, the others only on objects with flag 0x1000000. The live list, in order.
    const int32_t kExcluded = 0x810;
    const int32_t kBombExcluded = 0x830;
    const int32_t kCatches = 0x1000000;
    const int32_t excluded = obj->thrown_kind == kItemKindBomb ? kBombExcluded : kExcluded;
    const int32_t needed =
        obj->thrown_kind == Kurt::Grenade || obj->thrown_kind == kItemKindBomb ? 0 : kCatches;
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        MdkObject* other = &(*it);
        if (other == obj || other->dead || other->health == 0 || other->arena != obj->arena ||
            (other->flags & excluded) != 0 || (needed != 0 && (other->flags & needed) == 0)) {
            continue;
        }

        const formats::Box bounds = worldBounds(other);
        if (boxContains(bounds, obj->position) ||
            segmentEnters(bounds, obj->previous_position, obj->position)) {
            return true;
        }
    }
    return false;
}

void ScriptRuntime::itemsActivate(MdkObject* obj) {
    // C# Items.Activate (Items.cs:287-342): it stops and does its thing.
    const int32_t kMoving = MdkObject::FlagGravity | MdkObject::FlagCollides;
    obj->velocity = Vec3{0.0f, 0.0f, 0.0f};
    obj->flags = (obj->flags & ~kMoving) | kItemFlagActive;
    obj->contact_flags &= ~(MdkObject::ContactCollided | MdkObject::ContactFloor | kContactHitObject);
    switch (obj->thrown_kind) {
        case Kurt::Grenade:
        case kItemKindBomb:
            itemsExplode(obj, kGrenadeDamage, kGrenadeKurtDamage, kGrenadeRadius, kHitGrenade, obj,
                         kGrenadeExplosion);
            break;
        case Kurt::Dummy: {
            // The decoy walks forward for 15 seconds, and aliens aim at it.
            obj->item_ticks = kDecoyTicks;
            const Vec3 walk = scaled(fromAngle(obj->yaw), kDecoySpeed);
            obj->velocity = Vec3{walk.x, walk.y, 0.0f};
            obj->flags |= kMoving;
            restartAnimation(obj, itemsAnimation(kDecoyWalk), true);
            setLoopSound(obj, "DUMMY");
            itemsDecoy = obj;
            break;
        }
        case Kurt::InterestingBomb:
            // It spins on its first frame for 20 seconds (or until Kurt sets it off), then
            // plays its animation and blows up.
            restartAnimation(obj, itemsAnimation(kBombAnimation), false);
            obj->animation_end_frame = 0;
            obj->item_ticks = kBombTicks;
            itemsBomb = obj;
            break;
        case Kurt::Tornado:
            twisters_.push_back(Twister(this, obj->arena, obj->position, 0.0f));
            obj->item_ticks = kTornadoTicks;
            obj->parameter = (float)kTornadoFirstTwister;
            mixer_.play("TORNADO", audio::SoundMixer::Once);
            break;
        case Kurt::Mortar:
            restartAnimation(obj, itemsAnimation(kMortarAnimation), false);
            obj->item_ticks = kMortarActiveTicks;
            obj->parameter = 0.0f;
            itemsUpdateMortar(obj);
            break;
        case Kurt::Key:
            itemsStartNuke(obj);
            break;
        default:
            break;
    }
}

void ScriptRuntime::itemsExplode(MdkObject* obj, int32_t damage, int32_t kurtDamage, float radius,
                                 int32_t hitType, MdkObject* source, float scale) {
    // C# Items.Explode (Items.cs:345-354): on the world, then on Kurt, an explosion and its
    // sound, and the item goes.
    const Vec3 center = obj->position;
    blast(center, damage, radius, kTargetWorld, hitType, source);
    blast(center, kurtDamage, radius, kBlastKurt, hitType, source);
    spawnExplosion(obj->arena, center, scale, 0.0f, kDefaultLift);
    playSoundAt(kExplodeSound, center);
    remove(obj);
}

void ScriptRuntime::itemsStartNuke(MdkObject* obj) {
    // C# Items.StartNuke (Items.cs:357-372): the "key" becomes the nuke (SW_NUKE).
    const formats::Model* model = findModel(obj->arena, kNuke);
    if (model != nullptr) {
        obj->type_name = kNuke;
        obj->model = model;
    }

    obj->item_ticks = kNukeTicks;
    restartAnimation(obj, itemsAnimation(kNuke), false);
    obj->animation_time = 1.0f;
    Kurt& combat = kurt();
    combat.setWhiteFlash(mathMax(combat.whiteFlash(), 255.0f));

    // A loop without position, until the nuke goes.
    obj->loop_sound = mixer_.play("NUKE");
}

// ---------------------------------------------------------------------------
// Active items (Items.cs:375-612)
// ---------------------------------------------------------------------------

void ScriptRuntime::itemsUpdateActive(MdkObject* obj) {
    // C# Items.UpdateActive (Items.cs:377-406): 0x43e860 decoy, 0x43f18c bomb, 0x43e690
    // mortar, 0x43efcc nuke, 0x43e980 seal, 0x43ea84 bone, 0x43ee98 tornado.
    switch (obj->thrown_kind) {
        case Kurt::Tornado:
            itemsUpdateTornado(obj);
            break;
        case Kurt::Mortar:
            itemsUpdateMortar(obj);
            break;
        case Kurt::Key:
            itemsUpdateNuke(obj);
            break;
        case Kurt::Seal:
        case Kurt::SuperBone:
            itemsUpdateSeal(obj);
            break;
        case Kurt::InterestingBomb:
            itemsUpdateBomb(obj);
            break;
        default:
            // The decoy walks until its time is up.
            obj->item_ticks--;
            if (obj->item_ticks > 0) {
                return;
            }
            itemsForget(obj);
            remove(obj);
            break;
    }
}

void ScriptRuntime::itemsUpdateTornado(MdkObject* obj) {
    // C# Items.UpdateTornado (Items.cs:408-421): it spins, a twister every 15 ticks from 45
    // left, and it is killed when its time is up.
    obj->yaw = wrap360(obj->yaw + kTick * kTornadoSpin);
    obj->item_ticks--;
    if ((float)obj->item_ticks < obj->parameter) {
        obj->parameter -= (float)kTornadoTwisterEvery;
        twisters_.push_back(Twister(this, obj->arena, obj->position, obj->yaw));
    }

    if (obj->item_ticks <= 0) {
        kill(obj);
    }
}

void ScriptRuntime::itemsUpdateBomb(MdkObject* obj) {
    // C# Items.UpdateBomb (Items.cs:423-444): it spins on its first frame until its time is
    // up (or Kurt sets it off), then plays its animation and blows up.
    itemsBomb = obj;
    if (obj->animation_end_frame == 0) {
        obj->yaw = wrap360(obj->yaw + kTick * kBombSpin);
        obj->item_ticks--;
        if (obj->item_ticks < 1) {
            obj->animation_end_frame = MdkObject::NoHoldFrame;
        }
        return;
    }

    if (!animationDone(obj)) {
        return;
    }

    itemsExplode(obj, kBombDamage, kBombKurtDamage, kBombRadius, kHitBomb, nullptr, kBigExplosion);
    itemsBomb = nullptr;
}

void ScriptRuntime::updateTwisters() {
    // C# Items.UpdateTwisters (Items.cs:447-458): the twisters of Kurt's arena tick, over a
    // copy of the list; one that is gone leaves the list (List.Remove keeps the others'
    // order), and the ones a split makes are appended after every one that was there, in
    // the order they were made, and do not tick until the next tick -- which is the C#'s
    // list once the walk over its copy is done.
    if (twisters_.empty()) {
        return;
    }
    std::vector<Twister> spawned;
    twisterScratch_.clear();
    for (size_t i = 0; i < twisters_.size(); i++) {
        Twister& twister = twisters_[i];
        if (twister.arena() != currentArena_ || twister.tick(&spawned)) {
            twisterScratch_.push_back(twister);
        }
    }
    for (size_t i = 0; i < spawned.size(); i++) {
        twisterScratch_.push_back(spawned[i]);
    }
    twisters_.swap(twisterScratch_);
}

void ScriptRuntime::itemsUpdateMortar(MdkObject* obj) {
    // C# Items.UpdateMortar (Items.cs:463-513, 0x43e690): it pounds the ground on the frames
    // of MortarThumps; from the fourth thump Kurt is knocked down if he stands on a floor.
    if (obj->animation == nullptr || animationDone(obj)) {
        kill(obj);
        return;
    }

    int32_t thump = (int32_t)obj->parameter;
    if (thump >= kMortarThumpCount || obj->animation_frame < kMortarThumps[thump]) {
        return;
    }

    thump++;
    obj->parameter = (float)thump;
    if (thump >= kMortarKnockThump && player_ != nullptr && player_->onFloor) {
        kurt().setKnockDamage(Kurt::kKnockdownDamage);
    }

    raiseShake(kMortarShake);
    std::vector<MdkObject*> snapshot;
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        if (!it->dead) {
            snapshot.push_back(&(*it));
        }
    }
    for (size_t i = 0; i < snapshot.size(); i++) {
        MdkObject* other = snapshot[i];
        if (other == obj || other->dead || other->arena != obj->arena ||
            (other->flags & kMortarIgnored) != 0) {
            continue;
        }
        // MortarSpared.Contains(TypeName.ToUpperInvariant()): the flying aliens.
        const std::string upper = formats::toUpperAscii(other->type_name);
        if (upper == "XE" || upper == "XF") {
            continue;
        }

        if ((other->contact_flags & MdkObject::ContactFloor) != 0) {
            other->velocity.z += kMortarLift;
        }

        const float direction = heading(kurtPosition_, boxCenter(worldBounds(other)));
        if (other->health < kIndestructible) {
            other->health -= kurtDamage(other, kMortarDamage);
        }

        other->hit_event = -1;
        other->hit_type = kHitMortar;
        other->hit_direction = direction;
        if (other->health <= 0) {
            kill(other, direction + 180.0f);
        }
    }
}

void ScriptRuntime::itemsUpdateNuke(MdkObject* obj) {
    // C# Items.UpdateNuke (Items.cs:517-546, 0x43efcc): the screen shakes while its animation
    // plays (turning white towards the end), then it blows open the doors nearby and blasts
    // everything within 60 units.
    if (obj->animation != nullptr && !animationDone(obj)) {
        raiseShake(kNukeShake);
        if (obj->animation_frame > kNukeWhiteFrame) {
            // MathF.Round: the banker's round, rintf under the default rounding mode.
            const float flash = rintf(
                (float)(obj->animation_frame - kNukeWhiteFrame) * 255.0f /
                (float)std::max(obj->animation->frameCount - kNukeWhiteFrame, (int32_t)1));
            Kurt& combat = kurt();
            combat.setWhiteFlash(mathMax(combat.whiteFlash(), flash));
        }
        return;
    }

    const Vec3 center = obj->position;
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        MdkObject& other = *it;
        if (!other.dead && (other.flags & MdkObject::FlagDoor) != 0 &&
            distanceSquared(other.position, center) < kNukeDoorRangeSquared) {
            other.door_state = (other.door_state & kDoorStateKeep) | kNukeDoorOpen;
        }
    }

    blast(center, kNukeDamage, kNukeRadius, kTargetWorld, kHitNuke, nullptr);
    blast(center, kNukeKurtDamage, kNukeRadius, kBlastKurt, kHitNuke, nullptr);
    playSoundAt(kExplodeSound, center);
    sparkFire(obj->arena, center);
    spawnExplosion(obj->arena, center, kBigExplosion, 0.0f, kDefaultLift);
    remove(obj);
}

void ScriptRuntime::itemsUpdateSeal(MdkObject* obj) {
    // C# Items.UpdateSeal (Items.cs:550-594, 0x43e980 / 0x43ea84): XMT_LAND once it lands;
    // unless something holds it (script flag 1), Kurt can take it back after 5 seconds, and
    // a seal shrinks away after 25.
    if ((obj->script_flags & kSealHeld) == 0) {
        if (obj->item_ticks < kSealPickupTicks) {
            obj->flags |= MdkObject::FlagPickup;
        }

        if (obj->thrown_kind == Kurt::Seal) {
            obj->item_ticks--;
            if (obj->item_ticks < 1) {
                obj->scale *= kSealShrink;
                if (obj->scale < kSealGone) {
                    kill(obj);
                }
                return;
            }
        } else if (obj->item_ticks >= kSealPickupTicks) {
            obj->item_ticks--;
        }
    } else {
        obj->scale = 1.0f;
        obj->flags &= ~MdkObject::FlagPickup;
    }

    if (obj->animation != nullptr || (obj->script_flags & kSealLanded) != 0) {
        return;
    }

    obj->roll = 0.0f;
    obj->script_flags |= kSealLanded;
    obj->flags |= MdkObject::FlagGravity | MdkObject::FlagCollides;
    obj->flags &= ~MdkObject::FlagLoop;
    restartAnimation(obj, findArenaAnimation(obj->arena, kSealLanding), false);
}

// ---------------------------------------------------------------------------
// Blasts (Items.cs:596-801, 0x463a94)
// ---------------------------------------------------------------------------

void ScriptRuntime::blast(const Vec3& center, int32_t damage, float radius, int32_t targets,
                          int32_t hitType, MdkObject* source, bool counted) {
    // C# Items.Blast (Items.cs:614-630): objects, then Kurt, then the triangle groups.
    if ((targets & kBlastObjects) != 0) {
        blastObjects(center, damage, radius, hitType, source, counted);
    }
    if ((targets & kBlastKurt) != 0) {
        blastKurt(center, damage, radius);
    }
    if ((targets & kBlastGroups) != 0) {
        blastGroups(center, damage, radius, hitType, counted);
    }
}

void ScriptRuntime::blastObjects(const Vec3& center, int32_t damage, float radius, int32_t hitType,
                                 MdkObject* source, bool counted) {
    // C# Items.BlastObjects (Items.cs:632-677): every live, damageable object of Kurt's arena,
    // by its weak parts or its whole box, the better of the two; walls stop it.
    std::vector<MdkObject*> snapshot;
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        if (!it->dead) {
            snapshot.push_back(&(*it));
        }
    }
    for (size_t i = 0; i < snapshot.size(); i++) {
        MdkObject* obj = snapshot[i];
        if (obj->dead || obj->arena != currentArena_ || obj->health == 0 ||
            (obj->flags & (MdkObject::FlagNotSolid | MdkObject::FlagNotTarget)) != 0) {
            continue;
        }

        const BlastPartResult parts = blastWeakParts(obj, center, damage, radius, counted);
        int32_t best = parts.best;
        float distance = parts.distance;
        Vec3 point = parts.point;
        if (obj == source) {
            // The grenade itself takes it all.
            best = damage;
            distance = 0.0f;
            point = boxCenter(worldBounds(obj));
        } else {
            const formats::Box box = worldBounds(obj);
            const std::pair<int32_t, float> hit = blastDamage(box, center, damage, radius);
            if (hit.first > best) {
                best = hit.first;
                distance = hit.second;
                point = boxCenter(box);
            }
        }

        if (best <= 0 || distance > obj->blast_range) {
            continue;
        }

        const float direction = heading(center, point);
        if (obj->health < kIndestructible) {
            obj->health -= counted ? kurtDamage(obj, best) : best;
        }

        obj->hit_event = parts.event == 0 ? kBlastEvent : parts.event;
        obj->hit_type = hitType;
        obj->hit_direction = direction;
        if (obj->health >= 1) {
            continue;
        }

        if (counted) {
            stats.countEnemy(obj->type_name, GameStats::Killed);
        }
        kill(obj, direction + 180.0f);
    }
}

void ScriptRuntime::blastKurt(const Vec3& center, int32_t damage, float radius) {
    // C# Items.BlastKurt (Items.cs:711-727): Kurt is hurt at double the distance (walls stop
    // it), at most 15; blasts knock him down twice as easily. Kurt.Hurt, not HurtKurt.
    const Vec3 kurtPoint{kurtPosition_.x + 0.0f, kurtPosition_.y + 0.0f, kurtPosition_.z + kBlastLift};
    float distance = sqrtf(distanceSquared(center, kurtPoint));
    const Vec3 from{center.x + 0.0f, center.y + 0.0f, center.z + kBlastLift};
    if (!raycastClear(from, kurtPoint)) {
        distance = radius + 1.0f;
    }

    distance *= kBlastKurtDistance;
    if (distance >= radius) {
        return;
    }

    Kurt& combat = kurt();
    // ScriptMath.Round is AwayFromZero: roundf.
    combat.hurt(std::min((int32_t)roundf((float)damage * (1.0f - distance / radius)), kBlastKurtMax));
    combat.setKnockDamage(combat.knockDamage() * 2.0f);
}

void ScriptRuntime::blastGroups(const Vec3& center, int32_t damage, float radius, int32_t hitType,
                                bool counted) {
    // C# Items.BlastGroups (Items.cs:731-772): each triangle group of Kurt's arena that reacts
    // to hits gets one hit, on the first of its triangles within the radius that the blast
    // reaches; another group in the way stops it, the group's own triangle is the hit.
    const int32_t kind = counted ? kHitBlast : kHitOtherBlast;
    const std::string arena = currentArena_;
    ArenaState& state = getArenaState(arena);
    const Vec3 origin{center.x + 0.0f, center.y + 0.0f, center.z + kBlastLift};
    for (int32_t i = 0; i < kGroupCount; i++) {
        if (state.group_hit_flags[(size_t)i] == 0 && state.group_hit_scripts[(size_t)i] == 0) {
            continue;
        }

        groupCenters(arena, i + 1, &groupCentres_);
        for (size_t t = 0; t < groupCentres_.size(); t++) {
            const Vec3 triangle = groupCentres_[t];
            if (distanceSquared(origin, triangle) > radius * radius) {
                continue;
            }

            Vec3 point = triangle;
            DebrisRayHit hit;
            if (raycastHit(origin, triangle, &hit)) {
                if (hit.group != i + 1) {
                    continue;
                }
                point = hit.point;
            }

            const float distance = sqrtf(distanceSquared(origin, point));
            hitGroup(arena, i + 1, (int32_t)roundf((float)damage * (radius - distance) / radius), kind,
                     hitType);
            break;
        }
    }
}

void ScriptRuntime::groupCenters(const std::string& arena, int32_t group, std::vector<Vec3>* out) {
    // C# GroupCenters (ScriptRuntime.cs:1963-1976): none while the group is not solid; the
    // triangles in their order, each the mean of its corners ((a + b + c) / 3f, a true
    // division, as .NET's Vector3 / float is).
    out->clear();
    const formats::Arena* mesh = level_->arenaNamed(arena);
    if (mesh == nullptr || !mesh->hasGroup(group) ||
        (mesh->groupState(group) & formats::Arena::kGroupNotSolid) != 0) {
        return;
    }
    const std::vector<uint32_t>& flags = mesh->triangleFlags();
    const std::vector<int32_t>& indices = mesh->triangleIndices();
    const std::vector<formats::Vec3f>& vertices = mesh->vertices();
    for (size_t t = 0; t < flags.size(); t++) {
        if ((int32_t)(flags[t] >> 24) != group) {
            continue;
        }
        const formats::Vec3f a = vertices[(size_t)indices[t * 3]];
        const formats::Vec3f b = vertices[(size_t)indices[t * 3 + 1]];
        const formats::Vec3f c = vertices[(size_t)indices[t * 3 + 2]];
        out->push_back(Vec3{(a.x + b.x + c.x) / 3.0f, (a.y + b.y + c.y) / 3.0f,
                            (a.z + b.z + c.z) / 3.0f});
    }
}

void ScriptRuntime::sparkFire(const std::string& arena, const Vec3& point) {
    // C# SparkFire (ScriptRuntime.cs:2215-2219).
    debris_.spark(arena, point, kFireSparks, 1.0f, kFireSparkColour, kFireSparkRange);
}

const formats::ModelAnimation* ScriptRuntime::findArenaAnimation(const std::string& arena,
                                                                 const std::string& name) {
    // C# FindArenaAnimation (ScriptRuntime.cs:3208, arena_find_animation 0x440adc).
    if (!level_->mto().has(arena)) {
        return nullptr;
    }
    const formats::Arena* owner = level_->mto().getArena(arena);
    return owner != nullptr ? owner->animation(name) : nullptr;
}

// ---------------------------------------------------------------------------
// Twister.cs and Ribbon.cs
// ---------------------------------------------------------------------------

const char* const Ribbon::kTexture = "WMIBTEX";

namespace {

// The edge's half height (its profile points (0, 0, +-1)).
const float kRibbonHalfHeight = 1.0f;
// The texel columns across the edge and rows of each segment (0x491d7c...0x491dd8).
const float kRibbonLeft = 0.5f;
const float kRibbonRight = 15.5f;
const float kRibbonRows[3][2] = {{0.5f, 16.0f}, {16.0f, 32.0f}, {32.0f, 47.5f}};

} // namespace

float Ribbon::scaleOf(int32_t index, int32_t count) {
    // C# ScaleOf: integer division (by the constant 4), then the int to float.
    return index < count - 2 ? (float)((index + 1 + kLength - count) / (kLength - 1)) : 1.0f;
}

int32_t Ribbon::triangles(Vec3 positions[kMaxCorners], Vec2 uvs[kMaxCorners]) const {
    // C# Ribbon.Triangles(corners), operation for operation.
    const int32_t count = count_;
    if (count < 2) {
        return 0;
    }

    Vec3 bottoms[kLength];
    Vec3 tops[kLength];
    for (int32_t i = 0; i < count; i++) {
        // c -/+ UnitZ * HalfHeight * ScaleOf(i, count): x and y take a zero.
        const Vec3& c = centre(i);
        const float dz = kRibbonHalfHeight * scaleOf(i, count);
        bottoms[i] = Vec3{c.x - 0.0f, c.y - 0.0f, c.z - dz};
        tops[i] = Vec3{c.x + 0.0f, c.y + 0.0f, c.z + dz};
    }

    int32_t corners = 0;
    for (int32_t k = 1; k < count; k++) {
        const float* row = k == 1 ? kRibbonRows[0] : k == count - 1 ? kRibbonRows[2] : kRibbonRows[1];
        const Vec2 uv0{kRibbonLeft, row[0]};
        const Vec2 uv1{kRibbonRight, row[0]};
        const Vec2 uv2{kRibbonRight, row[1]};
        const Vec2 uv3{kRibbonLeft, row[1]};
        positions[corners] = bottoms[k - 1]; uvs[corners++] = uv0;
        positions[corners] = tops[k - 1];    uvs[corners++] = uv1;
        positions[corners] = tops[k];        uvs[corners++] = uv2;
        positions[corners] = bottoms[k - 1]; uvs[corners++] = uv0;
        positions[corners] = bottoms[k];     uvs[corners++] = uv3;
        positions[corners] = tops[k];        uvs[corners++] = uv2;
    }
    return corners;
}

void Ribbon::push(const Vec3& centre) {
    // C# Ribbon.Push: a queue of at most kLength; the oldest goes first.
    if (count_ == kLength) {
        first_ = first_ + 1 == kLength ? 0 : first_ + 1;
        count_--;
    }
    int32_t at = first_ + count_;
    if (at >= kLength) {
        at -= kLength;
    }
    centres_[at] = centre;
    count_++;
}

const Vec3& Ribbon::centre(int32_t i) const {
    int32_t at = first_ + i;
    if (at >= kLength) {
        at -= kLength;
    }
    return centres_[at];
}

Twister::Twister(ScriptRuntime* runtime, const std::string& arena, const Vec3& origin, float yaw)
    : runtime_(runtime), arena_(arena), position_(origin), origin_(origin), yaw_(yaw),
      life_(kTwisterLifetime) {}

namespace {

// Twister.Facing (Twister.cs:164): a wall's normal turned against the move.
Vec3 facing(const Vec3& normal, const Vec3& move) {
    return dot(normal, move) > 0.0f ? Vec3{-normal.x, -normal.y, -normal.z} : normal;
}

} // namespace

bool Twister::tick(std::vector<Twister>* spawned) {
    // C# Twister.Tick (Twister.cs:40-79).
    if (!chasing_) {
        spiral(spawned);
        return true;
    }

    // Chase, bouncing off the walls.
    Vec3 next = plus(position_, scaled(velocity_, ScriptRuntime::kTick));
    DebrisRayHit hit;
    if (runtime_->raycastHit(position_, next, &hit)) {
        const Vec3 normal = facing(hit.normal, minus(next, position_));
        velocity_ = minus(velocity_, scaled(scaled(normal, dot(velocity_, normal)), 2.0f));
        next = plus(hit.point, scaled(normal, kWallGap));
    }

    position_ = next;
    ribbon_.push(position_);
    hurtObjects();
    life_--;
    if (life_ <= 0) {
        return false;
    }

    if (target_ != nullptr && !target_->dead && target_->health > 0) {
        // Vector3.Normalize: the vector divided by its length.
        const Vec3 to = minus(boxCenter(runtime_->worldBounds(target_)), position_);
        const float length = sqrtf(dot(to, to));
        const Vec3 toward{to.x / length, to.y / length, to.z / length};
        velocity_ = plus(scaled(velocity_, kKeep), scaled(toward, kPull));
    } else {
        target_ = nullptr;
    }

    return true;
}

void Twister::spiral(std::vector<Twister>* spawned) {
    // C# Twister.Spiral (Twister.cs:81-97).
    angle_ += ScriptRuntime::kTick * kSpiralRate;
    const float r = angle_ / kSpiralEnd;
    const Vec3 direction = fromAngle(angle_ + yaw_);
    moveTo(plus(origin_, scaled(scaled(Vec3{direction.x, direction.y, 1.0f}, kSpiralSize), r)));
    ribbon_.push(position_);
    velocity_ = scaled(Vec3{direction.y, -direction.x, 0.0f}, kSpiralSpeed);
    if (angle_ <= kSpiralEnd) {
        return;
    }

    chasing_ = true;
    split(spawned);
}

void Twister::hurtObjects() {
    // C# Twister.HurtObjects (Twister.cs:100-130): the objects it is inside lose 2 health a
    // tick (and the twister a tick of life). Over a copy of the live objects.
    std::vector<MdkObject*> snapshot;
    for (std::deque<MdkObject>::iterator it = runtime_->objects_.begin(); it != runtime_->objects_.end();
         ++it) {
        if (!it->dead) {
            snapshot.push_back(&(*it));
        }
    }
    for (size_t i = 0; i < snapshot.size(); i++) {
        MdkObject* obj = snapshot[i];
        if (obj->dead || obj->arena != arena_ || obj->health == 0 ||
            (obj->flags & (MdkObject::FlagNotSolid | MdkObject::FlagNotTarget)) != 0) {
            continue;
        }

        const formats::Box bounds = runtime_->worldBounds(obj);
        if (!boxContains(bounds, position_)) {
            continue;
        }

        const float direction = heading(runtime_->kurtPosition(), boxCenter(bounds));
        if (obj->health < ScriptRuntime::kIndestructible) {
            obj->health -= runtime_->kurtDamage(obj, kTwisterDamage);
        }

        obj->hit_event = kTwisterHitEvent;
        obj->hit_direction = direction;
        life_--;
        if (obj->health < 1) {
            runtime_->kill(obj, direction + 180.0f);
        }
    }
}

void Twister::split(std::vector<Twister>* spawned) {
    // C# Twister.Split (Twister.cs:133-153, 0x407774): one twister for each object of the
    // arena -- this one takes the first, a new one each of the others. The live list.
    for (std::deque<MdkObject>::iterator it = runtime_->objects_.begin(); it != runtime_->objects_.end();
         ++it) {
        MdkObject* obj = &(*it);
        if (obj->dead || obj->arena != arena_ ||
            (obj->flags & (MdkObject::FlagNotSolid | MdkObject::FlagNotTarget)) != 0) {
            continue;
        }

        if (target_ == nullptr) {
            target_ = obj;
            continue;
        }

        Twister other(runtime_, arena_, origin_, yaw_);
        other.position_ = position_;
        other.velocity_ = velocity_;
        other.chasing_ = true;
        other.target_ = obj;
        spawned->push_back(other);
    }
}

void Twister::moveTo(const Vec3& next) {
    // C# Twister.MoveTo (Twister.cs:155-163).
    Vec3 to = next;
    DebrisRayHit hit;
    if (runtime_->raycastHit(position_, to, &hit)) {
        to = plus(hit.point, scaled(facing(hit.normal, minus(to, position_)), kWallGap));
    }
    position_ = to;
}

} // namespace scripts
} // namespace mdk

// ============================================================================
// platforms.cpp — the scripts runtime's half of Kurt against the solid objects
// (package #3): what he collides with and stands on (C# ScriptRuntime.SolidsWithin,
// MdkObject.Footing, GetKurtPlatform, KeeperAt), the triangle groups his moves run
// into (KurtTouchesGroups), the platform carrying him in the object loop (the Carry
// call site) and the camera's line of sight against the objects (ClipView).
// Kurt's half is in game/player.cpp, the box geometry in game/solids.cpp.
// ============================================================================

#include <algorithm>

#include "game/player.h"
#include "scripts/script_runtime.h"

namespace mdk {
namespace scripts {

namespace {

// C# ScriptRuntime.HitKurt (:120): what hit a group, Kurt's move.
const int32_t kHitKurt = 8;
// C# TriangleGroup's GroupShift: the group is the top byte of a triangle's flags.
const int32_t kGroupShift = 24;

// C# ScriptMath.Intersects (ScriptMath.cs:78): strict on every axis.
bool intersects(const formats::Box& a, const formats::Box& b) {
    return a.min.x < b.max.x && a.max.x > b.min.x && a.min.y < b.max.y && a.max.y > b.min.y &&
           a.min.z < b.max.z && a.max.z > b.min.z;
}

// C# ScriptMath.Contains (ScriptMath.cs:81): inclusive on every axis.
bool contains(const formats::Box& box, const Vec3& p) {
    return p.x >= box.min.x && p.y >= box.min.y && p.z >= box.min.z && p.x <= box.max.x &&
           p.y <= box.max.y && p.z <= box.max.z;
}

float comp(const Vec3& v, int i) { return i == 0 ? v.x : (i == 1 ? v.y : v.z); }
float comp(const formats::Vec3f& v, int i) { return i == 0 ? v.x : (i == 1 ? v.y : v.z); }

// C# ScriptMath.SegmentEntry (ScriptMath.cs:89-119): where a segment enters a box
// (its start when inside); false for none. Three slabs: a fixed count, not a search.
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
        enter = std::max(enter, std::min(t0, t1));
        leave = std::min(leave, std::max(t0, t1));
        if (enter > leave) {
            return false;
        }
    }
    *out = Vec3{from.x + d.x * enter, from.y + d.y * enter, from.z + d.z * enter};
    return true;
}

} // namespace

MdkObject* ScriptRuntime::getKurtPlatform() const {
    // C# GetKurtPlatform (ScriptRuntime.cs:2720): Kurt.OnFloor ? Kurt.Platform : null.
    if (player_ == nullptr || !player_->onFloor) {
        return nullptr;
    }
    return player_->platform();
}

bool ScriptRuntime::footingOf(const MdkObject* obj, solids::Footing* footing) {
    // C# MdkObject.Footing (MdkObject.cs:57-71): a wall without 0x10 and 0x800
    // (damp_collide_move 0x465e34); a floor with 0x100 and no 0x10
    // (damp_platform_floor 0x41d2c4). E.g. the ridden snowboard (0x800900): a floor only.
    const bool wall = (obj->flags & (MdkObject::FlagNotSolid | MdkObject::FlagNotSolid2)) == 0;
    const bool floor =
        (obj->flags & (MdkObject::FlagNotSolid | MdkObject::FlagPlatform)) == MdkObject::FlagPlatform;
    if (wall) {
        *footing = floor ? solids::kPlatform : solids::kWall;
        return true;
    }
    if (floor) {
        *footing = solids::kFloor;
        return true;
    }
    return false;
}

const std::vector<solids::Solid>& ScriptRuntime::solidsWithin(const formats::Box& region) {
    // C# SolidsWithin (ScriptRuntime.cs:2725-2751), in its order: the objects in list
    // order, each visible, unlocked part's world box.
    solidsWithin_.clear();
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        MdkObject* obj = &(*it);
        solids::Footing footing;
        if (obj->dead || obj->arena != currentArena_ || obj->health == 0 || obj->model == nullptr ||
            !footingOf(obj, &footing) || obj == rides_.ridden() ||
            !intersects(worldBounds(obj), region)) {
            continue;
        }

        const std::vector<PartBox> parts = partBounds(obj);
        for (size_t i = 0; i < parts.size(); i++) {
            if (((obj->hidden_parts | obj->lock_parts) & (1 << i)) != 0 || !parts[i].valid) {
                continue;
            }
            solids::Solid solid;
            solid.box = worldBounds(obj, parts[i].box);
            solid.owner = obj;
            solid.footing = footing;
            solidsWithin_.push_back(solid);
        }
    }
    return solidsWithin_;
}

bool ScriptRuntime::keeperAt(const MdkObject* obj, Vec3* at) {
    // C# KeeperAt (ScriptRuntime.cs:2229-2230): an alive object with 0x800000.
    if (obj == nullptr || obj->dead || (obj->flags & MdkObject::FlagStandable) == 0) {
        return false;
    }
    *at = obj->position;
    return true;
}

const std::vector<solids::Solid>* ScriptRuntime::solidsWithinTrampoline(
    void* ctx, const formats::Box& region) {
    return &static_cast<ScriptRuntime*>(ctx)->solidsWithin(region);
}

bool ScriptRuntime::keeperAtTrampoline(void* /*ctx*/, MdkObject* owner, Vec3* at) {
    return keeperAt(owner, at);
}

void ScriptRuntime::kurtTouchesGroups() {
    // C# KurtTouchesGroups (ScriptRuntime.cs:554-568): each group of his arena his
    // moves ran into since the last tick gets one hit (amount 0, HitKurt, weapon
    // HitTypeKurt), in the order first met; group 0 (the rest of the arena) none.
    if (player_ == nullptr) {
        return;
    }
    touchedGroups_.clear();
    const std::vector<Player::Contact>& contacts = player_->takeContacts();
    for (size_t i = 0; i < contacts.size(); i++) {
        const formats::Arena* arena = contacts[i].arena;
        const int32_t group =
            (int32_t)(arena->triangleFlags()[(size_t)contacts[i].triangle] >> kGroupShift);
        if (arena->name() != currentArena_ || group == 0 ||
            std::find(touchedGroups_.begin(), touchedGroups_.end(), group) !=
                touchedGroups_.end()) {
            continue;
        }
        touchedGroups_.push_back(group);
        hitGroup(currentArena_, group, 0, kHitKurt, kHitTypeKurt);
    }
}

void ScriptRuntime::carryKurt(MdkObject* obj, const Vec3& position, float yaw) {
    // C# ScriptRuntime.cs:982-986: Kurt moves and turns with the platform he stands on.
    if (player_ != nullptr && obj == player_->platform() && !obj->dead && player_->onFloor) {
        player_->carry(position, obj->position, obj->yaw - yaw);
    }
}

Vec3 ScriptRuntime::clipView(const Vec3& from, const Vec3& to) {
    // C# ClipView (ScriptRuntime.cs:1995-2024).
    const int32_t kBlocks = 0x1000000;
    const int32_t kIgnored = MdkObject::FlagNotSolid | MdkObject::FlagNotSolid2;
    Vec3 end = to;
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        const MdkObject* obj = &(*it);
        if (obj->dead || obj->health == 0 || obj->arena != currentArena_ || obj->model == nullptr ||
            (obj->flags & kBlocks) == 0 || (obj->flags & kIgnored) != 0) {
            continue;
        }
        const std::vector<PartBox> parts = partBounds(obj);
        for (size_t i = 0; i < parts.size(); i++) {
            if (!parts[i].valid) {
                continue;
            }
            // A part the head is in doesn't count (0x45f588 returns 2).
            const formats::Box box = worldBounds(obj, parts[i].box);
            if (contains(box, from)) {
                continue;
            }
            Vec3 entry;
            if (segmentEntry(box, from, end, &entry)) {
                end = entry;
            }
        }
    }
    return end;
}

Vec3 ScriptRuntime::clipViewTrampoline(void* ctx, const Vec3& from, const Vec3& to) {
    return static_cast<ScriptRuntime*>(ctx)->clipView(from, to);
}

} // namespace scripts
} // namespace mdk

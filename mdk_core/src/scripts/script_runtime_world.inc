// script_runtime_world.inc -- the world queries: raycasts, the debris callbacks, sight, the triangle groups.
// Part of script_runtime.cpp's single translation unit (see its header): pasted
// inside namespace mdk::scripts after its file-local constants and helpers,
// which it uses. No include guards, no includes of its own; the order is
// load-bearing.

// Raycast (0x421680): whether any triangle of the active arenas crosses [from,to].
// Only Kurt's arena and the second one stop rays; the rest are passed through.
// Script predicates may request the nearest hit point and its geometric normal.
// CrossesFloor (ScriptRuntime.cs:2914): an up-facing face of the arena crosses
// the segment. No BSP for the arena → false, as the C#'s TryGetValue miss.
bool ScriptRuntime::crossesFloor(const std::string& arena, const Vec3& from, const Vec3& to) {
    Bsp* bsp = bspOf(arena);
    if (bsp == nullptr) {
        return false;
    }
    Vec a{from.x, from.y, from.z};
    Vec b{to.x, to.y, to.z};
    Vec point;
    return bsp->segment(a, b, Bsp::kSegmentFloor, &point, Bsp::kClipPassesThrough) != Bsp::kNone;
}

bool ScriptRuntime::raycastPoint(const Vec3& from, const Vec3& to, Vec3* out, Vec3* normal) {
    Vec a{from.x, from.y, from.z};
    Vec b{to.x, to.y, to.z};
    // RayArenas: the current + second arena when a current is set, else every arena
    // (the C# _bsps is filled from level.Arenas at construction, so "all" is the full
    // set, not just the arenas touched so far). bspOf builds each on demand.
    // The C# Raycast keeps the NEAREST hit across the arenas by distance-squared.
    bool hit = false;
    float bestDistance = FLT_MAX;
    const auto tryArena = [&](const std::string& name) {
        Bsp* bsp = bspOf(name);
        if (bsp == nullptr) {
            return;
        }
        Vec point{};
        const int32_t triangle = bsp->segmentWithGroup(
            a, b, Bsp::kSegmentAny, &point, Bsp::kClipPassesThrough, nullptr);
        if (triangle == Bsp::kNone) {
            return;
        }
        const float dx = point.x - a.x;
        const float dy = point.y - a.y;
        const float dz = point.z - a.z;
        const float distance = dx * dx + dy * dy + dz * dz;
        // The C#'s exact skip: `distance >= bestDistance` (bestDistance starts at
        // float.MaxValue), so a NaN distance is ADOPTED, as in the C#, and a first
        // hit at exactly MaxValue is skipped. A `<`-with-hit-flag form would differ.
        if (distance >= bestDistance) {
            return;
        }
        bestDistance = distance;
        *out = Vec3{point.x, point.y, point.z};
        if (normal != nullptr) {
            *normal = triangleNormal(bsp->arena(), triangle);
        }
        hit = true;
    };
    if (!currentArena_.empty()) {
        tryArena(currentArena_);
        tryArena(secondArena_);
    } else {
        const std::vector<const formats::Arena*>& arenas = level_->arenas();
        for (size_t i = 0; i < arenas.size(); i++) {
            tryArena(arenas[i]->name());
        }
    }
    return hit;
}

// Raycast (ScriptRuntime.cs:2886) answering with the whole RayHit: the same arena set,
// mode and nearest-hit rule as raycastPoint, plus the hit's arena and triangle group.
bool ScriptRuntime::raycastHit(const Vec3& from, const Vec3& to, DebrisRayHit* out) {
    Vec a{from.x, from.y, from.z};
    Vec b{to.x, to.y, to.z};
    bool hit = false;
    float bestDistance = FLT_MAX;
    const auto tryArena = [&](const std::string& name) {
        Bsp* bsp = bspOf(name);
        if (bsp == nullptr) {
            return;
        }
        Vec point{};
        int32_t triangleGroup = 0;
        const int32_t triangle = bsp->segmentWithGroup(a, b, Bsp::kSegmentAny, &point,
                                                      Bsp::kClipPassesThrough, &triangleGroup);
        if (triangle == Bsp::kNone) {
            return;
        }
        const float dx = point.x - a.x;
        const float dy = point.y - a.y;
        const float dz = point.z - a.z;
        const float distance = dx * dx + dy * dy + dz * dz;
        // The C#'s exact skip, as in raycastPoint.
        if (distance >= bestDistance) {
            return;
        }
        bestDistance = distance;
        out->point = Vec3{point.x, point.y, point.z};
        out->normal = triangleNormal(bsp->arena(), triangle);
        out->arena = name;
        out->group = triangleGroup;
        hit = true;
    };
    if (!currentArena_.empty()) {
        tryArena(currentArena_);
        tryArena(secondArena_);
    } else {
        const std::vector<const formats::Arena*>& arenas = level_->arenas();
        for (size_t i = 0; i < arenas.size(); i++) {
            tryArena(arenas[i]->name());
        }
    }
    return hit;
}

bool ScriptRuntime::raycastClear(const Vec3& from, const Vec3& to) {
    Vec3 point;
    return !raycastPoint(from, to, &point);
}

// Raycast (ScriptRuntime.cs:2886) answering with the hit triangle's group. The
// SAME arena set and mode as raycastPoint: SegmentMode.Any with PassesThrough
// over the current arena and the second — the second is included REGARDLESS of
// whether it is active — or every arena when Kurt has none yet.
bool ScriptRuntime::raycastGroup(const Vec3& from, const Vec3& to, int32_t* group) {
    Vec a{from.x, from.y, from.z};
    Vec b{to.x, to.y, to.z};
    bool hit = false;
    float bestDistance = FLT_MAX;
    int32_t bestGroup = 0;
    const auto tryArena = [&](const std::string& name) {
        Bsp* bsp = bspOf(name);
        if (bsp == nullptr) {
            return;
        }
        Vec point{};
        int32_t triangleGroup = 0;
        const int32_t triangle = bsp->segmentWithGroup(a, b, Bsp::kSegmentAny, &point,
                                                      Bsp::kClipPassesThrough, &triangleGroup);
        if (triangle == Bsp::kNone) {
            return;
        }
        const float dx = point.x - a.x;
        const float dy = point.y - a.y;
        const float dz = point.z - a.z;
        const float distance = dx * dx + dy * dy + dz * dz;
        // The C#'s exact skip: `distance >= bestDistance`, so a NaN distance is
        // ADOPTED, as in the C#. Same form as raycastPoint.
        if (distance >= bestDistance) {
            return;
        }
        bestDistance = distance;
        bestGroup = triangleGroup;
        hit = true;
    };
    if (!currentArena_.empty()) {
        tryArena(currentArena_);
        tryArena(secondArena_);
    } else {
        const std::vector<const formats::Arena*>& arenas = level_->arenas();
        for (size_t i = 0; i < arenas.size(); i++) {
            tryArena(arenas[i]->name());
        }
    }
    if (hit) {
        *group = bestGroup;
    }
    return hit;
}

// RayIn (ScriptRuntime.cs:2249): the arena's nearest segment hit with its
// triangle normal and group, for Debris (and later Effects).
bool ScriptRuntime::rayIn(const std::string& arena, const Vec3& from, const Vec3& to,
                          DebrisRayHit* out) {
    Bsp* bsp = bspOf(arena);
    if (bsp == nullptr) {
        return false;
    }
    Vec a{from.x, from.y, from.z};
    Vec b{to.x, to.y, to.z};
    Vec point{};
    int32_t triangleGroup = 0;
    const int32_t triangle = bsp->segmentWithGroup(a, b, Bsp::kSegmentAny, &point,
                                                   Bsp::kClipPassesThrough, &triangleGroup);
    if (triangle == Bsp::kNone) {
        return false;
    }
    out->point = Vec3{point.x, point.y, point.z};
    out->normal = triangleNormal(level_->arenaNamed(arena), triangle);
    out->arena = arena;
    out->group = triangleGroup;
    return true;
}

// GroupFacets (ScriptRuntime.cs:2261): the triangles of an arena's group
// with their current material, for shattering. Null group 0, as the C#.
bool ScriptRuntime::groupFacets(const std::string& arena, int32_t group, DebrisGroup* out) {
    const formats::Arena* mesh = level_->arenaNamed(arena);
    if (mesh == nullptr || group == 0 || !mesh->hasGroup(group)) {
        return false;
    }
    int32_t groupMaterial = 0;
    bool hasGroupMaterial = mesh->groupMaterial(group, &groupMaterial);
    const std::vector<uint32_t>& flags = mesh->triangleFlags();
    const std::vector<int32_t>& idx = mesh->triangleIndices();
    const std::vector<formats::Vec3f>& verts = mesh->vertices();
    const std::vector<formats::Vec2f>& uvs = mesh->triangleUvs();
    const std::vector<int32_t>& mats = mesh->triangleMaterials();
    out->names = &mesh->materials();
    out->facets.clear();
    for (size_t t = 0; t < flags.size(); t++) {
        if ((int32_t)(flags[t] >> 24) != group) {
            continue;
        }
        DebrisFacet facet;
        formats::Vec3f va = verts[(size_t)idx[t * 3]];
        formats::Vec3f vb = verts[(size_t)idx[t * 3 + 1]];
        formats::Vec3f vc = verts[(size_t)idx[t * 3 + 2]];
        facet.a = Vec3{va.x, va.y, va.z};
        facet.b = Vec3{vb.x, vb.y, vb.z};
        facet.c = Vec3{vc.x, vc.y, vc.z};
        formats::Vec2f ua = uvs[t * 3], ub = uvs[t * 3 + 1], uc = uvs[t * 3 + 2];
        facet.uvA = Vec2{ua.x, ua.y};
        facet.uvB = Vec2{ub.x, ub.y};
        facet.uvC = Vec2{uc.x, uc.y};
        facet.material = hasGroupMaterial ? groupMaterial : mats[t];
        out->facets.push_back(facet);
    }
    return !out->facets.empty();
}

bool ScriptRuntime::debrisRayTrampoline(void* ctx, const std::string& arena, const Vec3& from,
                                        const Vec3& to, DebrisRayHit* out) {
    return static_cast<ScriptRuntime*>(ctx)->rayIn(arena, from, to, out);
}

bool ScriptRuntime::debrisFacetsTrampoline(void* ctx, const std::string& arena, int32_t group,
                                           DebrisGroup* out) {
    return static_cast<ScriptRuntime*>(ctx)->groupFacets(arena, group, out);
}

void ScriptRuntime::debrisTrailTrampoline(void* ctx, const std::string& arena, const Vec3& point) {
    static_cast<ScriptRuntime*>(ctx)->effects.spawnTrail(arena, point);
}

float ScriptRuntime::debrisUpdraftTrampoline(void* ctx, const std::string& arena, const Vec3& point,
                                             float vz, float dt) {
    return static_cast<ScriptRuntime*>(ctx)->fans.query(arena, point, vz, Fans::kMaskEffects, dt);
}

bool ScriptRuntime::fanIsLive(void* ctx, const std::string& arena) {
    ScriptRuntime* self = static_cast<ScriptRuntime*>(ctx);
    return arena == self->currentArena_ ||
           (self->secondActive_ && arena == self->secondArena_);
}

int32_t ScriptRuntime::animatedFrameCount(void* ctx, const std::string& arena,
                                          const std::string& texture) {
    ScriptRuntime* self = static_cast<ScriptRuntime*>(ctx);
    if (!self->level_) {
        return 0;
    }
    const formats::Arena* owner = self->level_->arenaNamed(arena);
    if (!owner) {
        return 0;
    }
    const std::vector<const formats::TextureArchive*> archives = self->level_->archivesOf(owner);
    for (size_t i = 0; i < archives.size(); i++) {
        const formats::Texture* found = archives[i]->findTexture(texture);
        if (found) {
            return found->frameCount();
        }
    }
    return 0;
}

// CanSeeKurt (ScriptRuntime.cs:3236): within range, inside the view cone (a cone
// under HalfTurn is clamped to MinCone), and a clear ray from the object's eye to
// Kurt's chest. The ray uses the same active-arena raycast as the move planner.
// MdkObject.YawTo: the compass yaw (degrees, [0, 360)) from the object to a
// point — the fmod wrap, NOT ScriptMath.Wrap360's floor form, and the two
// round differently at the range edges.
/* @vfp-exempt R6 — yaw-to-point in the movement-command path; glibc atan2f
 * matches the C# oracle's MathF.Atan2 bit-for-bit on this machine (probed:
 * .NET's MathF.Atan2 is the float libm call, and both picked the same value at
 * every ULP-divergent input tested), and the conversion is .NET's left-assoc
 * radiansToDegrees; the fastAtan2 LUT's error is far larger and would move the
 * turned yaw. measured n/a (corpus empty). Verified 2026-10-23. */
float ScriptRuntime::yawTo(const MdkObject* obj, const Vec3& point) const {
    float degrees = radiansToDegrees(atan2f(point.y - obj->position.y, point.x - obj->position.x));
    return fmodf(fmodf(degrees, 360.0f) + 360.0f, 360.0f);
}

// C# KurtLooksAt (ScriptRuntime.cs:2701): range, distance-scaled cone, and line of sight.
/* @vfp-exempt R6 — kurtLooksAt's heading is a cold script predicate; keep the
 * C# MathF.Atan2 + radians-to-degrees spelling to preserve cone-edge behavior.
 * measured n/a (corpus empty). Verified 2026-10-24. */
bool ScriptRuntime::kurtLooksAt(const MdkObject* obj, float minRange, float maxRange, float cone) {
    const float kEyeHeight = 5.0f;
    const float kTargetHeight = 2.0f;
    const float kFullTurn = 360.0f;
    float dx = obj->position.x - kurtPosition_.x;
    float dy = obj->position.y - kurtPosition_.y;
    float dz = obj->position.z - kurtPosition_.z;
    float distance = sqrtf(dx * dx + dy * dy + dz * dz);
    if (distance < minRange || distance > maxRange) {
        return false;
    }
    const float heading = radiansToDegrees(atan2f(dy, dx));
    const float raw = fabsf(kurtYaw_ - heading);
    const float angle = wrap360(raw);
    const float allowed = (cone - 90.0f) / maxRange * distance + 90.0f;
    if (angle > allowed && angle < kFullTurn - allowed) {
        return false;
    }
    Vec3 from{kurtPosition_.x, kurtPosition_.y, kurtPosition_.z + kEyeHeight};
    Vec3 to{obj->position.x, obj->position.y, obj->position.z + kTargetHeight};
    return raycastClear(from, to);
}

bool ScriptRuntime::canSeeKurt(MdkObject* obj, float range, float cone) {
    const float kMinCone = 10.0f;
    const float kEyeHeight = 5.0f;
    const float kKurtChest = 4.0f;
    const float kHalfTurn = 180.0f;
    float dx = obj->position.x - kurtPosition_.x;
    float dy = obj->position.y - kurtPosition_.y;
    float dz = obj->position.z - kurtPosition_.z;
    if (sqrtf(dx * dx + dy * dy + dz * dz) > range) {
        return false;
    }
    // MathF.Max(cone, MinCone) (ScriptRuntime.cs:3244). This one CANNOT be reached with a
    // NaN: the `cone < kHalfTurn` guard in front of it is false for a NaN, so a NaN cone
    // skips the cone test entirely — measured on .NET 10 (probe `nanclamp`, rows CONE
    // cone45=False / coneNaN=True / cone200=True, i.e. the NaN row is the no-test row).
    // mathMax is here because the oracle writes MathF.Max, not because a pin can reach it.
    if (cone < kHalfTurn &&
        fabsf(wrapAngle(yawTo(obj, kurtPosition_) - obj->yaw)) > mathMax(cone, kMinCone)) {
        return false;
    }
    Vec3 from{obj->position.x, obj->position.y, obj->position.z + kEyeHeight};
    Vec3 to{kurtPosition_.x, kurtPosition_.y, kurtPosition_.z + kKurtChest};
    return raycastClear(from, to);
}

void ScriptRuntime::setGroupHitScript(const std::string& arena, int32_t group, int32_t mask, int32_t script) {
    const int32_t index = (group - 1) & (kGroupCount - 1);
    ArenaState& state = getArenaState(arena);
    state.group_hit_masks[index] = mask;
    state.group_hit_scripts[index] = script;
}

void ScriptRuntime::setGroupHitFlags(const std::string& arena, int32_t group, int32_t flags) {
    const int32_t index = (group - 1) & (kGroupCount - 1);
    getArenaState(arena).group_hit_flags[index] = flags;
    if ((flags & kGroupDestructible) != 0) {
        setGroupState(arena, group, kGroupHide);
    }
}

void ScriptRuntime::setGroupCounter(const std::string& arena, int32_t group, int32_t value) {
    const int32_t index = (group - 1) & (kGroupCount - 1);
    getArenaState(arena).group_counters[index] = value;
}

int32_t ScriptRuntime::groupCounter(const std::string& arena, int32_t group) {
    const int32_t index = (group - 1) & (kGroupCount - 1);
    return getArenaState(arena).group_counters[index];
}

// HitGroup (ScriptRuntime.cs:1909, 0x40d560): a hit on a triangle of an arena
// group. The group's hit flags (opcode 168) decide what the hit does — a
// destructible group shows its damaged version, an always-hit group takes at
// least the amount, a stopping group answers | 2 — and the group's hit script
// (opcode 99) runs in the arena's scratch object when its mask admits the kind
// (or the group is always-hit). Answers | 1 when the script ran.
int32_t ScriptRuntime::hitGroup(const std::string& arena, int32_t group, int32_t amount,
                                int32_t kind, int32_t weapon) {
    const int32_t kScriptRan = 1;
    const int32_t kStopped = 2;
    if (group < 1 || group > kGroupCount) {
        return 0;
    }
    ArenaState& state = getArenaState(arena);
    const int32_t index = group - 1;
    const int32_t flags = state.group_hit_flags[index];
    bool always = false;
    int32_t result = 0;
    if ((flags & kind) != 0) {
        if ((flags & kGroupDestructible) != 0) {
            // A destructible group: its damaged version appears.
            setGroupState(arena, group, kGroupShow);
        }
        if ((flags & kGroupAlwaysHit) != 0) {
            always = true;
            amount = amount > 1 ? amount : 1;
        }
        if ((flags & kGroupStopsHit) != 0) {
            result = kStopped;
        }
    }
    if (state.group_hit_scripts[index] == 0 ||
        ((state.group_hit_masks[index] & kind) == 0 && !always)) {
        return result;
    }
    state.group_counters[index] += amount;
    // The script runs at once, from its start, in the arena's scratch object (0x45c9a0).
    MdkObject& scratch = state.hit_scripts;
    scratch.hit_type = weapon;
    scratch.hit_event = 0;
    scratch.wait_time = 0.0f;
    scratch.gosub_returns.clear();
    scratch.gosub_restarts.clear();
    scratch.restart = state.group_hit_scripts[index];
    vm_.run(&scratch);
    return result | kScriptRan;
}

// GetKurtFloorGroup (ScriptRuntime.cs:410, 0x573c10): the group of the floor
// triangle under Kurt — a raycast from one above him to ten below, the group of
// the nearest hit, or 0.
int32_t ScriptRuntime::getKurtFloorGroup() {
    Vec3 from{kurtPosition_.x, kurtPosition_.y, kurtPosition_.z + 1.0f};
    Vec3 to{kurtPosition_.x, kurtPosition_.y, kurtPosition_.z - 10.0f};
    int32_t group = 0;
    if (!raycastGroup(from, to, &group)) {
        return 0;
    }
    return group;
}

// C# TriangleGroups.SetState (TriangleGroups.cs:90): the group's state is a
// STORED per-group value, not derived from the triangle bits. The group must
// exist (a triangle with that top byte) and not be group 0; group numbers are
// the full top byte, not limited to the 16 the hit arrays use. The operation
// edits the stored state, and the result is written to every triangle of the
// group, where the collisions read it.
void ScriptRuntime::setGroupState(const std::string& arena, int32_t group, int32_t operation) {
    formats::Arena* mesh = level_->arenaNamedMutable(arena);
    if (mesh == nullptr || group == 0 || !mesh->hasGroup(group)) {
        return;
    }
    const uint32_t kHidden = formats::Arena::kGroupHidden;
    const uint32_t kNotSolid = formats::Arena::kGroupNotSolid;
    uint32_t state = mesh->groupState(group);
    switch (operation) {
        case 0:  // HideAndRelease
            state |= kHidden | kNotSolid;
            break;
        case 2:  // Hide
            state |= kHidden;
            break;
        case 3:  // Show
            state &= ~kHidden;
            break;
        case 4:  // Release
            state |= kNotSolid;
            break;
        case 5:  // Solidify
            state &= ~kNotSolid;
            break;
        default:  // any other operation shows and solidifies
            state = 0;
            break;
    }
    mesh->setGroupState(group, state);
    const uint32_t stateBits = kHidden | kNotSolid;
    std::vector<uint32_t>& triangleFlags = mesh->mutableTriangleFlags();
    for (size_t i = 0; i < triangleFlags.size(); i++) {
        if ((int32_t)(triangleFlags[i] >> 24) != group) {
            continue;
        }
        triangleFlags[i] = (triangleFlags[i] & ~stateBits) | state;
    }
}

// C# SetGroupTexture (ScriptRuntime.cs:1983) -> TriangleGroups.SetMaterial: the
// group must exist and not be group 0; it stores the group's material. The
// scene reads it when it is built, and the C#'s Changed event rebuilds a built
// one (LevelView.Rebuild): here the arena is recorded for the frontend, which
// rebuilds its view (retexturedArenas, package #15).
void ScriptRuntime::setGroupTexture(const std::string& arena, int32_t group, int32_t value) {
    formats::Arena* mesh = level_->arenaNamedMutable(arena);
    if (mesh == nullptr || group == 0 || !mesh->hasGroup(group)) {
        return;
    }
    mesh->setGroupMaterial(group, value);
    // SetMaterial's Changed?.Invoke(arena, number): each arena once until taken.
    for (size_t i = 0; i < retextured_.size(); i++) {
        if (retextured_[i] == arena) {
            return;
        }
    }
    retextured_.push_back(arena);
}

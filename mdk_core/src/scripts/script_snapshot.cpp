// script_snapshot.cpp -- full saves (package #19): the C#'s
//   src/Mdk.Game/Scripts/ScriptRuntime.Snapshot.cs (Capture, Load, Snapshot, Restore,
//                                                   PackObject, Recreate, FixedObjects,
//                                                   FindAnimationNamed),
//   src/Mdk.Game/Scripts/Snapshot.cs               (the packer: Pack/Unpack/Encode/Decode),
//   src/Mdk.Game/Kurt/Kurt.Snapshot.cs             (Kurt's half),
//   src/Mdk.Game/Scripts/Fans.cs:102-132           (Fans.Snapshot/Restore),
//   src/Mdk.Game/Level/TriangleGroups.cs:126-162   (TriangleGroups.Snapshot/Restore).
//
// THE PACKER. The C# packs an object's PUBLIC INSTANCE FIELDS by reflection, in
// declaration order, leaving out Model, LoopSound and TrackedVoice (rebuilt or
// restarted by their owners). C++ has no reflection, so the fields are listed by
// hand in visitObject, in the order `typeof(MdkObject).GetFields()` returns them
// (printed by tests/scripts/save_probe's development run and pinned by the goldens:
// a field out of order is a different byte stream). ONE list serves both
// directions -- SnapshotIo writes when it has a Writer and reads when it has a
// Document -- so the two cannot drift apart.
//
// The value encodings are Snapshot.Encode's:
//   MdkObject in the live list -> {"#obj": index}; an arena's script object ->
//   {"#fixed": "arena:NAME" | "hits:NAME"}; anything else -> null
//   ModelAnimation -> {"#anim": name}; Vector3 / Matrix4x4 / (int, int) -> arrays;
//   a non-finite float -> its text ("NaN", "Infinity", "-Infinity").
//
// What a value of the save does on the way back in is Snapshot.Unpack's: a member
// that is absent leaves the field alone; a member that is null sets the field's
// default (FieldInfo.SetValue(obj, null) on a value type); an array field takes
// the saved array (std::array: as far as both go).

#include "scripts/script_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

#include "game/player.h"
#include "util/json.h"
#include "util/sha256.h"

namespace mdk {
namespace scripts {

namespace {

const char kObjectKey[] = "#obj";
const char kFixedKey[] = "#fixed";
const char kAnimationKey[] = "#anim";
const char kControllerKey[] = "arena:";
const char kHitScriptsKey[] = "hits:";
const char kCmiAnimationPrefix[] = "CMI_";
const int kNone = json::Document::kNone;

// Snapshot.Float: a number, or a string's float (the non-finite ones).
float decodeFloat(const json::Document& doc, int node) {
    std::string text;
    if (doc.asString(node, &text)) {
        if (text == "NaN") {
            return strtof("nan", nullptr);
        }
        if (text == "Infinity") {
            return strtof("inf", nullptr);
        }
        if (text == "-Infinity") {
            return strtof("-inf", nullptr);
        }
        return strtof(text.c_str(), nullptr);
    }
    return doc.floatOr(node, 0.0f);
}

} // namespace

// The packer, both ways (Snapshot.cs). A friend of ScriptRuntime for
// findAnimationNamed.
class SnapshotIo {
public:
    // Writing: the live objects by index, the arenas' script objects by key.
    json::Writer* out = nullptr;
    std::map<const MdkObject*, int32_t> index;
    std::map<const MdkObject*, std::string> fixedKeys;

    // Reading: the member values of `node` in `doc`, objects by their index in
    // the created list (nulls kept) or by key.
    const json::Document* doc = nullptr;
    int node = kNone;
    const std::vector<MdkObject*>* created = nullptr;
    std::map<std::string, MdkObject*> fixedObjects;
    ScriptRuntime* runtime = nullptr;
    MdkObject* owner = nullptr;

    bool writing() const { return out != nullptr; }

    // The member to read, kNone when absent (the field is then left alone).
    int member(const char* name) const { return doc->member(node, name); }

    void field(const char* name, std::string& value) {
        if (writing()) {
            out->key(name);
            out->valueString(value);
            return;
        }
        const int m = member(name);
        if (m != kNone) {
            value = doc->stringOr(m, std::string());
        }
    }

    void field(const char* name, int32_t& value) {
        if (writing()) {
            out->key(name);
            out->valueInt(value);
            return;
        }
        const int m = member(name);
        if (m != kNone) {
            value = doc->intOr(m, 0);
        }
    }

    void field(const char* name, float& value) {
        if (writing()) {
            out->key(name);
            out->valueFloat(value);
            return;
        }
        const int m = member(name);
        if (m != kNone) {
            value = doc->isNull(m) ? 0.0f : decodeFloat(*doc, m);
        }
    }

    void field(const char* name, bool& value) {
        if (writing()) {
            out->key(name);
            out->valueBool(value);
            return;
        }
        const int m = member(name);
        if (m != kNone) {
            bool b = false;
            value = doc->asBool(m, &b) ? b : false;
        }
    }

    void field(const char* name, Vec3& value) {
        if (writing()) {
            out->key(name);
            writeVec3(value);
            return;
        }
        const int m = member(name);
        if (m != kNone) {
            value = readVec3(m);
        }
    }

    void field(const char* name, Mat4& value) {
        // Matrix4x4 as m[k / 4, k % 4], k = 0..15: row-major, Mat4's layout.
        if (writing()) {
            out->key(name);
            out->beginArray();
            for (int k = 0; k < 16; k++) {
                out->valueFloat(value.m[k]);
            }
            out->endArray();
            return;
        }
        const int m = member(name);
        if (m != kNone) {
            for (int k = 0; k < 16; k++) {
                value.m[k] = doc->isNull(m) ? 0.0f : decodeFloat(*doc, doc->at(m, k));
            }
        }
    }

    // A float[] / int[] / string[] / Vector3[] field held in a fixed array: written
    // whole, read as far as both go.
    void floats(const char* name, float* values, int count) {
        if (writing()) {
            out->key(name);
            out->beginArray();
            for (int i = 0; i < count; i++) {
                out->valueFloat(values[i]);
            }
            out->endArray();
            return;
        }
        const int m = member(name);
        for (int e = doc->first(m), i = 0; e != kNone && i < count; e = doc->next(e), i++) {
            values[i] = decodeFloat(*doc, e);
        }
    }

    void ints(const char* name, int32_t* values, int count) {
        if (writing()) {
            out->key(name);
            out->beginArray();
            for (int i = 0; i < count; i++) {
                out->valueInt(values[i]);
            }
            out->endArray();
            return;
        }
        const int m = member(name);
        for (int e = doc->first(m), i = 0; e != kNone && i < count; e = doc->next(e), i++) {
            values[i] = doc->intOr(e, 0);
        }
    }

    // An int[] or List<int> field of any length: replaced whole.
    void ints(const char* name, std::vector<int32_t>& values) {
        if (writing()) {
            out->key(name);
            out->beginArray();
            for (size_t i = 0; i < values.size(); i++) {
                out->valueInt(values[i]);
            }
            out->endArray();
            return;
        }
        const int m = member(name);
        if (m == kNone) {
            return;
        }
        values.clear();
        for (int e = doc->first(m); e != kNone; e = doc->next(e)) {
            values.push_back(doc->intOr(e, 0));
        }
    }

    void strings(const char* name, std::string* values, int count) {
        if (writing()) {
            out->key(name);
            out->beginArray();
            for (int i = 0; i < count; i++) {
                out->valueString(values[i]);
            }
            out->endArray();
            return;
        }
        const int m = member(name);
        for (int e = doc->first(m), i = 0; e != kNone && i < count; e = doc->next(e), i++) {
            values[i] = doc->stringOr(e, std::string());
        }
    }

    void vectors(const char* name, Vec3* values, int count) {
        if (writing()) {
            out->key(name);
            out->beginArray();
            for (int i = 0; i < count; i++) {
                writeVec3(values[i]);
            }
            out->endArray();
            return;
        }
        const int m = member(name);
        for (int e = doc->first(m), i = 0; e != kNone && i < count; e = doc->next(e), i++) {
            values[i] = readVec3(e);
        }
    }

    // A ValueTuple<int, int>: [a, b].
    void pair(const char* name, int32_t& a, int32_t& b) {
        if (writing()) {
            out->key(name);
            out->beginArray();
            out->valueInt(a);
            out->valueInt(b);
            out->endArray();
            return;
        }
        const int m = member(name);
        if (m != kNone) {
            a = doc->intOr(doc->at(m, 0), 0);
            b = doc->intOr(doc->at(m, 1), 0);
        }
    }

    void ref(const char* name, MdkObject*& value) {
        if (writing()) {
            out->key(name);
            writeRef(value);
            return;
        }
        const int m = member(name);
        if (m != kNone) {
            value = readRef(m);
        }
    }

    void animation(const char* name, const formats::ModelAnimation*& value) {
        if (writing()) {
            out->key(name);
            if (value == nullptr) {
                out->valueNull();
                return;
            }
            out->beginObject();
            out->key(kAnimationKey);
            out->valueString(value->name);
            out->endObject();
            return;
        }
        const int m = member(name);
        if (m == kNone) {
            return;
        }
        if (doc->isNull(m)) {
            value = nullptr;
            return;
        }
        value = runtime->findAnimationNamed(owner, doc->stringOr(doc->member(m, kAnimationKey), ""));
    }

    // Snapshot.Encode(MdkObject).
    void writeRef(const MdkObject* value) {
        if (value != nullptr) {
            std::map<const MdkObject*, int32_t>::const_iterator i = index.find(value);
            if (i != index.end()) {
                out->beginObject();
                out->key(kObjectKey);
                out->valueInt(i->second);
                out->endObject();
                return;
            }
            std::map<const MdkObject*, std::string>::const_iterator k = fixedKeys.find(value);
            if (k != fixedKeys.end()) {
                out->beginObject();
                out->key(kFixedKey);
                out->valueString(k->second);
                out->endObject();
                return;
            }
        }
        out->valueNull();
    }

    // Snapshot.Reference: by index into the created list (a null where the model was
    // gone), or by key; null otherwise.
    MdkObject* readRef(int value) const {
        if (doc->type(value) != json::Document::Object) {
            return nullptr;
        }
        const int i = doc->member(value, kObjectKey);
        if (i != kNone) {
            const int32_t at = doc->intOr(i, -1);
            return at >= 0 && at < (int32_t)created->size() ? (*created)[(size_t)at] : nullptr;
        }
        const int key = doc->member(value, kFixedKey);
        if (key != kNone) {
            std::map<std::string, MdkObject*>::const_iterator found =
                fixedObjects.find(doc->stringOr(key, ""));
            return found != fixedObjects.end() ? found->second : nullptr;
        }
        return nullptr;
    }

    void writeVec3(const Vec3& v) {
        out->beginArray();
        out->valueFloat(v.x);
        out->valueFloat(v.y);
        out->valueFloat(v.z);
        out->endArray();
    }

    Vec3 readVec3(int value) const {
        if (doc->isNull(value)) {
            return Vec3{0.0f, 0.0f, 0.0f};
        }
        return Vec3{decodeFloat(*doc, doc->at(value, 0)), decodeFloat(*doc, doc->at(value, 1)),
                    decodeFloat(*doc, doc->at(value, 2))};
    }
};

namespace {

// MdkObject's public fields, in the order the C#'s reflection lists them
// (Objects/MdkObject.cs:84-271), Model, TrackedVoice and LoopSound left out.
void visitObject(SnapshotIo& io, MdkObject& o) {
    io.field("TypeName", o.type_name);
    io.field("InstanceId", o.instance_id);
    io.field("Arena", o.arena);
    io.field("Position", o.position);
    io.field("Yaw", o.yaw);
    io.field("Pitch", o.pitch);
    io.field("Roll", o.roll);
    io.field("Scale", o.scale);
    io.field("SpawnPosition", o.spawn_position);
    io.field("PreviousPosition", o.previous_position);
    io.field("Health", o.health);
    io.field("MaxHealth", o.max_health);
    io.field("Indestructible", o.indestructible);
    io.field("Flags", o.flags);
    io.field("ScriptFlags", o.script_flags);
    io.field("ContactFlags", o.contact_flags);
    io.floats("Variables", o.variables.data(), (int)o.variables.size());
    io.ref("Linked", o.linked);
    io.ref("Leader", o.leader);
    io.field("Dead", o.dead);
    io.field("HiddenParts", o.hidden_parts);
    io.field("LockedParts", o.locked_parts);
    io.field("TextureFrame", o.texture_frame);
    io.field("Visible", o.visible);
    io.ref("CowTarget", o.cow_target);
    io.field("Restart", o.restart);
    io.field("WaitTime", o.wait_time);
    io.field("WaitResume", o.wait_resume);
    io.field("DeathScript", o.death_script);
    io.field("CommandTarget", o.command_target);
    io.ints("GosubReturns", o.gosub_returns);
    io.ints("GosubRestarts", o.gosub_restarts);
    io.floats("LevelTimers", o.level_timers.data(), (int)o.level_timers.size());
    io.field("HitEvent", o.hit_event);
    io.field("HitType", o.hit_type);
    io.field("HitDirection", o.hit_direction);
    io.field("WeakPrefix", o.weak_prefix);
    io.field("WeakPrefixLength", o.weak_prefix_length);
    io.ints("PartHealth", o.part_health);
    io.ints("PartMaxHealth", o.part_max_health);
    io.field("Priority", o.priority);
    io.field("ObeyLevel", o.obey_level);
    io.field("TargetMode", o.target_mode);
    io.field("Value2c0", o.value_2c0);
    io.field("Value104", o.value_104);
    io.field("MoveCommand", o.move_command);
    io.field("MoveParameter", o.move_parameter);
    io.field("MoveDestination", o.move_destination);
    io.field("StuckCount", o.stuck_count);
    io.field("StuckTicks", o.stuck_ticks);
    io.field("StuckMoved", o.stuck_moved);
    io.field("BankingYaw", o.banking_yaw);
    io.field("Waypoint", o.waypoint);
    io.pair("AttachPoints", o.attach_point_a, o.attach_point_b);
    io.field("HeightOffset", o.height_offset);
    io.field("Speed", o.speed);
    io.field("MaxSpeed", o.max_speed);
    io.field("Acceleration", o.acceleration);
    io.field("Deceleration", o.deceleration);
    io.field("Friction", o.friction);
    io.field("Gravity", o.gravity);
    io.field("Velocity", o.velocity);
    io.field("Push", o.push);
    io.field("Parameter", o.parameter);
    io.field("ParameterTimer", o.parameter_timer);
    io.field("TouchDamage", o.touch_damage);
    io.field("Path", o.path);
    io.field("PathTime", o.path_time);
    io.field("PathSpeed", o.path_speed);
    io.floats("PathSpeeds", o.path_speeds.data(), (int)o.path_speeds.size());
    io.field("PathStop", o.path_stop);
    io.field("PathOrigin", o.path_origin);
    io.field("PathYawOffset", o.path_yaw_offset);
    io.field("FrameSound", o.frame_sound);
    io.field("FrameSoundFrame", o.frame_sound_frame);
    io.field("EffectFrames", o.effect_frames);
    io.field("EffectTime", o.effect_time);
    io.field("ThrownKind", o.thrown_kind);
    io.field("ItemTicks", o.item_ticks);
    io.field("BlastRange", o.blast_range);
    io.field("ShotPart", o.shot_part);
    io.field("ShotPoint", o.shot_point);
    io.field("RollingBasis", o.rolling_basis);
    io.field("RollRadius", o.roll_radius);
    io.field("SwingPivot", o.swing_pivot);
    io.field("SwingSpeed", o.swing_speed);
    io.field("SwingGain", o.swing_gain);
    io.field("SwingLength", o.swing_length);
    io.field("SwingYaw", o.swing_yaw);
    io.field("RopeColor", o.rope_color);
    io.field("RopeMask", o.rope_mask);
    io.vectors("RopePoints", o.rope_points.data(), (int)o.rope_points.size());
    io.ref("Attached", o.attached);
    io.ints("DoorAnimations", o.door_animations.data(), (int)o.door_animations.size());
    io.field("DoorState", o.door_state);
    io.field("DoorDistance", o.door_distance);
    io.strings("DoorSounds", o.door_sounds.data(), (int)o.door_sounds.size());
    io.field("LockParts", o.lock_parts);
    io.field("HatchParts", o.hatch_parts);
    io.field("Connects", o.connects);
    io.field("TrackedSound", o.tracked_sound);
    io.field("LoopSoundName", o.loop_sound_name);
    io.strings("Labels", o.labels.data(), (int)o.labels.size());
    io.animation("Animation", o.animation);
    io.field("AnimationTime", o.animation_time);
    io.field("AnimationFrame", o.animation_frame);
    io.field("AnimationRate", o.animation_rate);
    io.field("AnimationEndFrame", o.animation_end_frame);
}

// GameStats' public fields (Scripts/GameStats.cs:19-28).
void visitStats(SnapshotIo& io, GameStats& s) {
    io.field("Shots", s.shots);
    io.field("ShotHits", s.shotHits);
    io.field("SniperShots", s.sniperShots);
    io.field("SniperHits", s.sniperHits);
    io.field("HeadShots", s.headShots);
    io.field("Enemies", s.enemies);
    io.field("Kills", s.kills);
    io.field("TownFlags", s.townFlags);
    io.field("StrikeUsed", s.strikeUsed);
}

// ArenaState's public fields (ScriptRuntime.cs:148-160). Name, Controller and
// HitScripts are readonly and not arrays, so Unpack skips them; the readonly arrays
// are copied into as far as both go; Flags and Started are set.
void visitArenaState(SnapshotIo& io, ArenaState& a) {
    if (io.writing()) {
        io.field("Name", a.name);
        MdkObject* controller = &a.controller;
        MdkObject* hitScripts = &a.hit_scripts;
        io.ref("Controller", controller);
        io.ref("HitScripts", hitScripts);
    }
    io.floats("Variables", a.variables.data(), (int)a.variables.size());
    io.field("Flags", a.flags);
    io.field("Started", a.started);
    io.ints("GroupHitFlags", a.group_hit_flags.data(), (int)a.group_hit_flags.size());
    io.ints("GroupHitMasks", a.group_hit_masks.data(), (int)a.group_hit_masks.size());
    io.ints("GroupHitScripts", a.group_hit_scripts.data(), (int)a.group_hit_scripts.size());
    io.ints("GroupCounters", a.group_counters.data(), (int)a.group_counters.size());
}

// The groups of an arena in the C#'s Dictionary order: by each group's first
// triangle (TriangleGroups.Add).
void groupOrder(const formats::Arena& arena, std::vector<int32_t>* out) {
    out->clear();
    bool seen[256] = {false};
    const std::vector<uint32_t>& flags = arena.triangleFlags();
    for (size_t t = 0; t < flags.size(); t++) {
        const int32_t number = (int32_t)(flags[t] >> 24);
        if (!seen[number]) {
            seen[number] = true;
            out->push_back(number);
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Fans (Fans.cs:102-132)

void Fans::snapshot(json::Writer* out) const {
    out->beginArray();
    for (size_t i = 0; i < fans_.size(); i++) {
        const Fan& f = fans_[i];
        out->beginObject();
        out->key("name");
        out->valueString(f.name);
        out->key("arena");
        out->valueString(f.arena);
        out->key("param");
        out->valueInt(f.param);
        out->key("type");
        out->valueInt(f.type);
        out->key("strength");
        out->valueFloat(f.strength);
        out->key("mask");
        out->valueInt(f.mask);
        out->key("box");
        out->beginArray();
        out->valueFloat(f.boxStart.x);
        out->valueFloat(f.boxStart.y);
        out->valueFloat(f.boxStart.z);
        out->valueFloat(f.boxEnd.x);
        out->valueFloat(f.boxEnd.y);
        out->valueFloat(f.boxEnd.z);
        out->endArray();
        out->endObject();
    }
    out->endArray();
}

void Fans::restore(const json::Document& doc, int node) {
    fans_.clear();
    for (int e = doc.first(node); e != kNone; e = doc.next(e)) {
        if (doc.type(e) != json::Document::Object) {
            continue;  // data.OfType<JsonObject>()
        }
        const int box = doc.member(e, "box");
        float b[6] = {0, 0, 0, 0, 0, 0};
        for (int k = 0; k < 6; k++) {
            b[k] = doc.floatOr(doc.at(box, k), 0.0f);
        }
        Fan fan;
        fan.name = doc.stringOr(doc.member(e, "name"), "");
        fan.arena = doc.stringOr(doc.member(e, "arena"), "");
        fan.param = doc.intOr(doc.member(e, "param"), 0);
        fan.type = doc.intOr(doc.member(e, "type"), 0);
        fan.strength = doc.floatOr(doc.member(e, "strength"), 0.0f);
        fan.mask = doc.intOr(doc.member(e, "mask"), 0);
        fan.boxStart = Vec3{b[0], b[1], b[2]};
        fan.boxEnd = Vec3{b[3], b[4], b[5]};
        fans_.push_back(fan);
    }
}

// ---------------------------------------------------------------------------
// ScriptRuntime

bool ScriptRuntime::canSnapshot() const {
    // C# CanSnapshot (ScriptRuntime.Snapshot.cs:24-26), term for term: Cutscene == 0 &&
    // !LevelOver && EndLevel == null && !AirStrike.IsActive() && Rides.Ridden == null &&
    // Kurt.Health > 0 && CurrentArena.Length != 0. A save mid-cutscene is refused
    // (golden/L5_cut_snapshot.txt).
    return cutscene == 0 && !levelOver && !endLevel_ && !airStrike_.isActive() &&
           rides_.ridden() == nullptr && player_ != nullptr && player_->health() > 0 &&
           !currentArena_.empty();
}

std::string ScriptRuntime::snapshotHash(const std::string& text) {
    // Snapshot.Hash: Convert.ToHexString(SHA256(UTF-8))[..16].ToLowerInvariant().
    Sha256 sha;
    sha.update((const uint8_t*)text.data(), text.size());
    uint8_t digest[32];
    sha.finish(digest);
    char hex[17];
    for (int i = 0; i < 8; i++) {
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    return std::string(hex, 16);
}

const formats::ModelAnimation* ScriptRuntime::findAnimationNamed(MdkObject* obj,
                                                                 const std::string& name) {
    // C# FindAnimationNamed (Snapshot.cs:200-208).
    const size_t prefix = sizeof(kCmiAnimationPrefix) - 1;
    if (obj != nullptr && name.compare(0, prefix, kCmiAnimationPrefix) == 0) {
        const int32_t offset = (int32_t)strtol(name.c_str() + prefix, nullptr, 16);
        return getAnimation(obj, offset);
    }
    const formats::ModelAnimation* found =
        obj != nullptr ? findArenaAnimation(obj->arena, name) : nullptr;
    return found != nullptr ? found : itemsAnimation(name.c_str());
}

void ScriptRuntime::applyGroupFlags(const std::string& arena, int32_t group, uint32_t state) {
    // TriangleGroups.SetFlags (:113-124): the group's state, kept in its triangles'
    // flags where the collisions read it -- as setGroupState writes them.
    formats::Arena* mesh = level_->arenaNamedMutable(arena);
    if (mesh == nullptr) {
        return;
    }
    mesh->setGroupState(group, state);
    const uint32_t stateBits = formats::Arena::kGroupHidden | formats::Arena::kGroupNotSolid;
    std::vector<uint32_t>& triangleFlags = mesh->mutableTriangleFlags();
    for (size_t i = 0; i < triangleFlags.size(); i++) {
        if ((int32_t)(triangleFlags[i] >> 24) == group) {
            triangleFlags[i] = (triangleFlags[i] & ~stateBits) | state;
        }
    }
}

std::vector<std::string> ScriptRuntime::enteredArenas() const {
    // Restore's last lines (Snapshot.cs:119-124).
    std::vector<std::string> arenas;
    arenas.push_back(currentArena_);
    if (!secondArena_.empty()) {
        arenas.push_back(secondArena_);
    }
    return arenas;
}

std::string ScriptRuntime::capture() const {
    if (player_ == nullptr) {
        return std::string();
    }
    ScriptRuntime& self = const_cast<ScriptRuntime&>(*this);
    std::string text;
    json::Writer out(&text);
    out.beginObject();

    // ---- "kurt": Kurt.Snapshot (Kurt.Snapshot.cs:10-26) ----
    const Player& kurt = *player_;
    const Kurt& combat = kurt.combat();
    out.key("kurt");
    out.beginObject();
    out.key("position");
    out.beginArray();
    out.valueFloat(kurt.feet.x);
    out.valueFloat(kurt.feet.y);
    out.valueFloat(kurt.feet.z);
    out.endArray();
    out.key("yaw");
    out.valueFloat(kurt.yaw);
    out.key("health");
    out.valueInt(combat.health());
    out.key("invulnerable");
    out.valueFloat(combat.invulnerable());
    // The sniper's clip (package #5 left "the clip fields in saves" to this one).
    out.key("clip_rounds");
    out.valueInt(kurt.scope().clipRounds);
    out.key("clip_time");
    out.valueFloat(kurt.scope().clipTime);
    out.key("inventory");
    out.beginObject();
    out.key("slots");
    out.beginArray();
    for (int32_t i = 0; i < combat.slotCount(); i++) {
        out.beginArray();
        out.valueInt((int32_t)combat.slots()[i].item);
        out.valueInt(combat.slots()[i].count);
        out.endArray();
    }
    out.endArray();
    out.key("selected");
    out.valueInt(combat.selected());
    out.key("ammo");
    out.beginArray();
    for (int32_t type = 1; type <= Kurt::kAmmoTypes; type++) {
        out.valueInt(combat.ammo(type));
    }
    out.endArray();
    out.key("selected_ammo");
    out.valueInt(combat.selectedAmmo());
    out.key("super_chain_gun");
    out.valueInt(combat.superChainGun());
    out.endObject();
    out.endObject();

    // ---- "level": Snapshot (ScriptRuntime.Snapshot.cs:43-84) ----
    SnapshotIo io;
    io.out = &out;
    io.runtime = &self;
    std::vector<MdkObject*> live;
    for (size_t i = 0; i < self.objects_.size(); i++) {
        if (!self.objects_[i].dead) {
            io.index[&self.objects_[i]] = (int32_t)live.size();
            live.push_back(&self.objects_[i]);
        }
    }
    for (size_t i = 0; i < arenaOrder_.size(); i++) {
        ArenaState& state = self.arenas_.find(arenaOrder_[i])->second;
        io.fixedKeys[&state.controller] = kControllerKey + state.name;
        io.fixedKeys[&state.hit_scripts] = kHitScriptsKey + state.name;
    }

    out.key("level");
    out.beginObject();
    std::array<float, 4> globals = globalVariables;
    io.floats("global_variables", globals.data(), (int)globals.size());
    out.key("global_flags");
    out.valueInt(globalFlags);
    out.key("alarm_ticks");
    out.valueInt(alarmTicks);
    out.key("sky_mode");
    out.valueInt(skyMode);
    out.key("option");
    out.valueInt(option);
    out.key("town_ticks");
    out.valueInt(townTicks);
    out.key("current_arena");
    out.valueString(currentArena_);
    out.key("second_arena");
    out.valueString(secondArena_);
    out.key("second_active");
    out.valueBool(secondActive_);
    out.key("next_instance");
    out.valueInt(nextInstance_);
    out.key("camera_track_pitch");
    out.valueFloat(cameraTrackPitch);
    out.key("camera_track_ticks");
    out.valueInt(cameraTrackTicks);
    out.key("shatter_point");
    io.writeVec3(shatterPoint);
    out.key("shatter_direction");
    io.writeVec3(shatterDirection);
    out.key("alien_target");
    io.writeRef(alienTarget_);
    out.key("strike_used");
    out.valueBool(airStrike_.usedUp);
    out.key("stats");
    out.beginObject();
    GameStats stats = this->stats;
    visitStats(io, stats);
    out.endObject();

    out.key("arenas");
    out.beginObject();
    for (size_t i = 0; i < arenaOrder_.size(); i++) {
        ArenaState& state = self.arenas_.find(arenaOrder_[i])->second;
        out.key(state.name);
        out.beginObject();
        visitArenaState(io, state);
        out.key("controller");
        out.beginObject();
        visitObject(io, state.controller);
        out.endObject();
        out.key("hit_scripts");
        out.beginObject();
        visitObject(io, state.hit_scripts);
        out.endObject();
        out.endObject();
    }
    out.endObject();

    // PackObject (:153-163): type, arena, the box's size for a spawn_box object, the
    // fields.
    out.key("objects");
    out.beginArray();
    for (size_t i = 0; i < live.size(); i++) {
        MdkObject& obj = *live[i];
        out.beginObject();
        out.key("type");
        out.valueString(obj.type_name);
        out.key("arena");
        out.valueString(obj.arena);
        bool isBox = false;
        for (size_t b = 0; b < boxes_.size(); b++) {
            isBox = isBox || boxes_[b] == &obj;
        }
        if (isBox && obj.model != nullptr) {
            out.key("box");
            const formats::Box& bounds = obj.model->bounds;
            io.writeVec3(Vec3{bounds.max.x - bounds.min.x, bounds.max.y - bounds.min.y,
                              bounds.max.z - bounds.min.z});
        }
        out.key("variables");
        out.beginObject();
        visitObject(io, obj);
        out.endObject();
        out.endObject();
    }
    out.endArray();

    out.key("fans");
    fans.snapshot(&out);
    out.key("items");
    out.beginObject();
    out.key("bomb");
    io.writeRef(itemsBomb);
    out.key("decoy");
    io.writeRef(itemsDecoy);
    out.endObject();

    // TriangleGroups.Snapshot (:126-144): per arena (every arena of the level, in its
    // order), the groups a script changed: number -> [state, material or null].
    out.key("groups");
    out.beginObject();
    const std::vector<const formats::Arena*>& arenas = level_->arenas();
    std::vector<int32_t> order;
    for (size_t a = 0; a < arenas.size(); a++) {
        const formats::Arena& arena = *arenas[a];
        groupOrder(arena, &order);
        bool opened = false;
        for (size_t g = 0; g < order.size(); g++) {
            const int32_t number = order[g];
            const uint32_t state = arena.groupState(number);
            int32_t material = 0;
            const bool hasMaterial = arena.groupMaterial(number, &material);
            if (state == 0 && !hasMaterial) {
                continue;
            }
            if (!opened) {
                out.key(arena.name());
                out.beginObject();
                opened = true;
            }
            char key[16];
            snprintf(key, sizeof(key), "%d", (int)number);
            out.key(key);
            out.beginArray();
            out.valueInt((int32_t)state);
            if (hasMaterial) {
                out.valueInt(material);
            } else {
                out.valueNull();
            }
            out.endArray();
        }
        if (opened) {
            out.endObject();
        }
    }
    out.endObject();

    out.endObject();  // level
    out.endObject();
    return text;
}

bool ScriptRuntime::load(const std::string& text, std::string* why) {
    // C# Load: Restore(data["level"]), Kurt.Restore(data["kurt"]), and the first
    // tick's move starts where Kurt is.
    json::Document doc;
    if (!doc.parse(text)) {
        *why = "the save's level state is not JSON";
        return false;
    }
    const int levelNode = doc.member(doc.root(), "level");
    const int kurtNode = doc.member(doc.root(), "kurt");
    if (doc.type(levelNode) != json::Document::Object ||
        doc.type(kurtNode) != json::Document::Object) {
        *why = "the save has no level or no Kurt";
        return false;
    }
    if (player_ == nullptr) {
        *why = "no Kurt to restore";
        return false;
    }
    // Restore runs on a fresh runtime, before the first tick (Viewer.RestoreSnapshot
    // does it in the Viewer's constructor). The C#'s Objects.Clear() would drop the
    // objects of a runtime that had run; here a runtime that has run is refused,
    // because its objects are pointed at from elsewhere and cannot be dropped.
    if (tickCount_ != 0 || !objects_.empty() || !stamped_.empty()) {
        *why = "a save loads into a fresh level only";
        return false;
    }
    // BULLET HOLES ARE NOT SAVED (package #16's stamps): the C#'s snapshot has no
    // texture in it, and its load is a new Viewer over a new LevelData, whose
    // textures are read from the archives again -- so a loaded game has no holes.
    // Here the same holds by construction: a load is a level-loop pass with its own
    // LevelData (fresh, unpinned textures) and a fresh runtime (stamped_ empty),
    // which the refusal above keeps true.

    // ---- Restore (ScriptRuntime.Snapshot.cs:87-125) ----
    // The arenas' script objects exist first, so references to them resolve.
    const int arenasNode = doc.member(levelNode, "arenas");
    for (int a = doc.first(arenasNode); a != kNone; a = doc.next(a)) {
        getArenaState(doc.name(a));
    }

    // The objects next (Recreate, :166-182), so that references between them resolve.
    const int objectsNode = doc.member(levelNode, "objects");
    std::vector<MdkObject*> created;
    for (int e = doc.first(objectsNode); e != kNone; e = doc.next(e)) {
        const std::string arena = doc.stringOr(doc.member(e, "arena"), "");
        const std::string type = doc.stringOr(doc.member(e, "type"), "");
        MdkObject* controller = &getArenaState(arena).controller;
        const int box = doc.member(e, "box");
        if (doc.type(box) == json::Document::Array) {
            const Vec3 size{doc.floatOr(doc.at(box, 0), 0.0f), doc.floatOr(doc.at(box, 1), 0.0f),
                            doc.floatOr(doc.at(box, 2), 0.0f)};
            // SpawnBox then Objects.Remove(box): the list is rebuilt from `created`
            // below (Objects.AddRange), so the box is appended in the order `created` has.
            created.push_back(spawnBox(controller, Vec3{0.0f, 0.0f, 0.0f}, size, type, 0, true));
            continue;
        }
        const formats::Model* model = findModel(arena, type);
        if (model == nullptr) {
            created.push_back(nullptr);
            continue;
        }
        MdkObject obj;
        obj.arena = arena;
        obj.type_name = type;
        obj.model = model;
        objects_.push_back(obj);
        created.push_back(&objects_.back());
    }

    SnapshotIo io;
    io.doc = &doc;
    io.created = &created;
    io.runtime = this;
    for (size_t i = 0; i < arenaOrder_.size(); i++) {
        ArenaState& state = arenas_.find(arenaOrder_[i])->second;
        io.fixedObjects[kControllerKey + state.name] = &state.controller;
        io.fixedObjects[kHitScriptsKey + state.name] = &state.hit_scripts;
    }

    // RestoreFields (:127-147).
    io.node = levelNode;
    io.floats("global_variables", globalVariables.data(), (int)globalVariables.size());
    globalFlags = doc.intOr(doc.member(levelNode, "global_flags"), 0);
    alarmTicks = doc.intOr(doc.member(levelNode, "alarm_ticks"), 0);
    skyMode = doc.intOr(doc.member(levelNode, "sky_mode"), 0);
    option = doc.intOr(doc.member(levelNode, "option"), 0);
    townTicks = doc.intOr(doc.member(levelNode, "town_ticks"), 0);
    currentArena_ = doc.stringOr(doc.member(levelNode, "current_arena"), "");
    secondArena_ = doc.stringOr(doc.member(levelNode, "second_arena"), "");
    bool flag = false;
    secondActive_ = doc.asBool(doc.member(levelNode, "second_active"), &flag) && flag;
    nextInstance_ = doc.intOr(doc.member(levelNode, "next_instance"), 0);
    cameraTrackPitch = decodeFloat(doc, doc.member(levelNode, "camera_track_pitch"));
    cameraTrackTicks = doc.intOr(doc.member(levelNode, "camera_track_ticks"), 0);
    io.field("shatter_point", shatterPoint);
    io.field("shatter_direction", shatterDirection);
    alienTarget_ = io.readRef(doc.member(levelNode, "alien_target"));
    airStrike_.usedUp = doc.asBool(doc.member(levelNode, "strike_used"), &flag) && flag;

    io.node = doc.member(levelNode, "stats");
    visitStats(io, stats);
    for (int a = doc.first(arenasNode); a != kNone; a = doc.next(a)) {
        ArenaState& state = getArenaState(doc.name(a));
        io.node = a;
        io.owner = nullptr;
        visitArenaState(io, state);
        io.node = doc.member(a, "controller");
        io.owner = &state.controller;
        visitObject(io, state.controller);
        io.node = doc.member(a, "hit_scripts");
        io.owner = &state.hit_scripts;
        visitObject(io, state.hit_scripts);
    }

    int e = doc.first(objectsNode);
    for (size_t i = 0; i < created.size() && e != kNone; i++, e = doc.next(e)) {
        if (created[i] != nullptr) {
            io.node = doc.member(e, "variables");
            io.owner = created[i];
            visitObject(io, *created[i]);
        }
    }

    fans.restore(doc, doc.member(levelNode, "fans"));
    const int items = doc.member(levelNode, "items");
    itemsBomb = io.readRef(doc.member(items, "bomb"));
    itemsDecoy = io.readRef(doc.member(items, "decoy"));

    // TriangleGroups.Restore (:147-162): Material = node[1]?.GetValue<int>(), then
    // SetFlags. A group the level does not have is skipped.
    const int groups = doc.member(levelNode, "groups");
    for (int a = doc.first(groups); a != kNone; a = doc.next(a)) {
        formats::Arena* mesh = level_->arenaNamedMutable(doc.name(a));
        for (int g = doc.first(a); g != kNone; g = doc.next(g)) {
            const int32_t number = (int32_t)strtol(doc.name(g).c_str(), nullptr, 10);
            if (mesh == nullptr || !mesh->hasGroup(number)) {
                continue;
            }
            const int material = doc.at(g, 1);
            if (doc.isNull(material)) {
                mesh->clearGroupMaterial(number);
            } else {
                mesh->setGroupMaterial(number, doc.intOr(material, 0));
                // SetFlags' Changed -> LevelView.Rebuild, which rebuilds a group that has
                // a material (package #15): the frontends rebuild the arena's view.
                if (std::find(retextured_.begin(), retextured_.end(), doc.name(a)) ==
                    retextured_.end()) {
                    retextured_.push_back(doc.name(a));
                }
            }
            applyGroupFlags(doc.name(a), number, (uint32_t)doc.intOr(doc.at(g, 0), 0));
        }
    }

    // An arena reached by a teleport is shown again (:119-124). The frontend adds
    // the same arenas to Kurt's space (enteredArenas()).
    enterArena(currentArena_);
    if (!secondArena_.empty()) {
        enterArena(secondArena_);
    }

    // ---- Kurt.Restore (Kurt.Snapshot.cs:28-40) ----
    Player& kurt = *player_;
    const int position = doc.member(kurtNode, "position");
    const Vec3 feet{doc.floatOr(doc.at(position, 0), 0.0f), doc.floatOr(doc.at(position, 1), 0.0f),
                    doc.floatOr(doc.at(position, 2), 0.0f)};
    kurt.teleport(feet, doc.floatOr(doc.member(kurtNode, "yaw"), 0.0f));
    kurt.combat().setHealth(doc.intOr(doc.member(kurtNode, "health"), 0));
    kurt.combat().setInvulnerable(doc.floatOr(doc.member(kurtNode, "invulnerable"), 0.0f));
    kurt.scope().clipRounds = doc.intOr(doc.member(kurtNode, "clip_rounds"), 0);
    kurt.scope().clipTime = doc.floatOr(doc.member(kurtNode, "clip_time"), 0.0f);
    const int inventory = doc.member(kurtNode, "inventory");
    Kurt::Slot slots[Kurt::kMaxSlots];
    int32_t slotCount = 0;
    const int slotsNode = doc.member(inventory, "slots");
    for (int s = doc.first(slotsNode); s != kNone && slotCount < Kurt::kMaxSlots; s = doc.next(s)) {
        slots[slotCount].item = (Kurt::Item)doc.intOr(doc.at(s, 0), 0);
        slots[slotCount].count = doc.intOr(doc.at(s, 1), 0);
        slotCount++;
    }
    int32_t ammo[Kurt::kAmmoTypes] = {0, 0, 0, 0, 0};
    int32_t ammoCount = 0;
    const int ammoNode = doc.member(inventory, "ammo");
    for (int a = doc.first(ammoNode); a != kNone && ammoCount < Kurt::kAmmoTypes; a = doc.next(a)) {
        ammo[ammoCount++] = doc.intOr(a, 0);
    }
    kurt.combat().restoreInventory(slots, slotCount, doc.intOr(doc.member(inventory, "selected"), 0),
                                   ammo, ammoCount,
                                   doc.intOr(doc.member(inventory, "selected_ammo"), 0),
                                   doc.intOr(doc.member(inventory, "super_chain_gun"), 0));

    // The first tick's move starts where Kurt is: it crosses no connection.
    previousKurtPosition_ = kurt.feet;
    return true;
}

} // namespace scripts
} // namespace mdk

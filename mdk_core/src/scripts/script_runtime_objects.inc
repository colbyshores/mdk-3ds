// script_runtime_objects.inc -- an object's floor, pose and animation, the doors, bounds and weak parts, Kurt's box and damage.
// Part of script_runtime.cpp's single translation unit (see its header): pasted
// inside namespace mdk::scripts after its file-local constants and helpers,
// which it uses. No include guards, no includes of its own; the order is
// load-bearing.

Bsp* ScriptRuntime::bspOf(const std::string& arena) {
    std::map<std::string, std::unique_ptr<Bsp>>::iterator it = bsps_.find(arena);
    if (it != bsps_.end()) {
        return it->second.get();
    }
    // The full arena set (MTO arenas AND SNI corridors), as the C# builds _bsps
    // from every level.Arenas entry -- not just the MTO.
    const formats::Arena* a = level_->arenaNamed(arena);
    if (a == nullptr) {
        return nullptr;
    }
    std::unique_ptr<Bsp> built(new Bsp(a));
    Bsp* raw = built.get();
    bsps_.emplace(arena, std::move(built));  // unique_ptr keeps the address stable
    return raw;
}

float ScriptRuntime::getArenaFloor(const std::string& arena) {
    std::map<std::string, float>::iterator it = floors_.find(arena);
    if (it != floors_.end()) {
        return it->second;
    }
    const float kNoArenaFloor = -1000.0f;  // C# NoArenaFloor
    float floor = kNoArenaFloor;
    const formats::Arena* a = level_->arenaNamed(arena);  // full set, as _floors
    if (a != nullptr && !a->vertices().empty()) {
        float minz = a->vertices()[0].z;
        for (size_t i = 1; i < a->vertices().size(); ++i) {
            if (a->vertices()[i].z < minz) {
                minz = a->vertices()[i].z;
            }
        }
        floor = minz;
    }
    floors_.emplace(arena, floor);
    return floor;
}

Box ScriptRuntime::poseBounds(const MdkObject* obj) {
    const Model* model = obj->model;
    if (model == nullptr) {
        Box d;  // C# DefaultBounds
        d.min = Vec3f{-1.0f, -1.0f, 0.0f};
        d.max = Vec3f{1.0f, 1.0f, 4.0f};
        return d;
    }
    // One pose: the rest vertices, or the baked animation frame -- from the SAME
    // per-(model, animation) cache the C# keeps (MdkObject.RestPoses / Baked,
    // scripts/pose_cache.cpp), read by reference. pose_parts() applies exactly
    // the selection this function used to do by hand: the rest pose without an
    // animation, else the frame clamped into [0, frames-1], else empty.
    //
    // This used to BAKE THE WHOLE ANIMATION ON EVERY CALL and copy one frame out.
    // Measured 2026-11-17 (callgrind, `tick1 dump transform 3 301`):
    // ModelAnimation::bake was 79% of all instructions, reached through
    // worldBounds from the VM, and level 3's script tick cost 20x level 7's. On
    // the Old 3DS that made one 60 Hz step take 35.5 ms, so levels 3, 5 and 6
    // ran in the fixed-step death spiral at 1.7-2.5 fps.
    const std::vector<std::vector<Vec3f> >& pose = obj->pose_parts();
    // PoseBoxes per part (min/max over its vertices), combined over the visible
    // parts (hidden_parts bit clear); fall back to the model's own bounds.
    bool have = false;
    Box result;
    for (size_t i = 0; i < pose.size(); ++i) {
        if ((obj->hidden_parts & (1 << i)) != 0 || pose[i].empty()) {
            continue;
        }
        Vec3f mn = pose[i][0];
        Vec3f mx = pose[i][0];
        for (size_t v = 1; v < pose[i].size(); ++v) {
            const Vec3f& p = pose[i][v];
            if (p.x < mn.x) mn.x = p.x;
            if (p.y < mn.y) mn.y = p.y;
            if (p.z < mn.z) mn.z = p.z;
            if (p.x > mx.x) mx.x = p.x;
            if (p.y > mx.y) mx.y = p.y;
            if (p.z > mx.z) mx.z = p.z;
        }
        if (!have) {
            result.min = mn;
            result.max = mx;
            have = true;
        } else {
            if (mn.x < result.min.x) result.min.x = mn.x;
            if (mn.y < result.min.y) result.min.y = mn.y;
            if (mn.z < result.min.z) result.min.z = mn.z;
            if (mx.x > result.max.x) result.max.x = mx.x;
            if (mx.y > result.max.y) result.max.y = mx.y;
            if (mx.z > result.max.z) result.max.z = mx.z;
        }
    }
    return have ? result : model->bounds;
}

// GetAnimation (0x442e50): a CMI-stored animation parsed once per offset and kept,
// or — when the record's first u32 is 0 — the arena animation named by its 8 chars.
// The 1996 demo's file animations (IsBeta) are not reachable: the runtime always
// builds the retail decoder.
const formats::ModelAnimation* ScriptRuntime::getAnimation(MdkObject* obj, int32_t offset) {
    if (offset == 0) {
        return nullptr;
    }
    const std::vector<uint8_t>& bytes = cmi_->bytes();
    if (offset >= 0 && offset + 4 <= (int32_t)bytes.size()) {
        uint32_t first = 0;
        memcpy(&first, &bytes[offset], sizeof(first));
        if (first == 0) {
            char name[9] = {0};
            formats::binAscii(bytes.data(), offset + 4, 8, name);
            const formats::Arena* arena = level_->mto().has(obj->arena) ? level_->mto().getArena(obj->arena)
                                                                        : nullptr;
            return arena != nullptr ? arena->animation(name) : nullptr;
        }
    }
    std::map<int32_t, formats::ModelAnimation>::iterator it = animations_.find(offset);
    if (it != animations_.end()) {
        return &it->second;
    }
    char name[24];
    snprintf(name, sizeof(name), "CMI_%x", (unsigned)offset);
    formats::ModelAnimation animation;
    if (!formats::ModelAnimation::parse(&animation, name, bytes.data(), (int32_t)bytes.size(), offset)) {
        return nullptr;
    }
    std::pair<std::map<int32_t, formats::ModelAnimation>::iterator, bool> inserted =
        animations_.emplace(offset, std::move(animation));
    return &inserted.first->second;
}

// MdkObject.PlayAnimation: a loop sets FlagLoop; replaying the same live animation
// only clears a hold frame, otherwise the animation restarts from frame 0.
void ScriptRuntime::playAnimation(MdkObject* obj, const formats::ModelAnimation* animation, bool loop) {
    if (loop) {
        obj->flags |= MdkObject::FlagLoop;
    } else {
        obj->flags &= ~MdkObject::FlagLoop;
    }
    if (animation == obj->animation && obj->animation_end_frame != MdkObject::AnimationEnded) {
        if (obj->animation_end_frame >= 0) {
            obj->animation_end_frame = MdkObject::NoHoldFrame;
        }
        return;
    }
    obj->animation = animation;
    obj->animation_time = 0.0f;
    obj->animation_frame = 0;
    obj->animation_end_frame = animation != nullptr ? MdkObject::NoHoldFrame : MdkObject::AnimationEnded;
}

// MdkObject.AdvanceAnimation (object_anim_update): the frame clock, the hold frame,
// the loop clamp, and the root motion of every passed frame joined into the push
// (divided by delta so ApplyVelocity's *Dt puts it back to per-tick units).
void ScriptRuntime::advanceAnimation(MdkObject* obj, float delta) {
    const formats::ModelAnimation* animation = obj->animation;
    if (animation == nullptr || obj->animation_end_frame == MdkObject::AnimationEnded) {
        return;
    }
    if (obj->animation_end_frame >= 0 && obj->animation_frame == obj->animation_end_frame) {
        return;
    }
    int32_t count = animation->frameCount;
    if (count <= 0) {
        return;
    }
    obj->animation_time += delta * obj->animation_rate * animation->speed;
    if (obj->animation_end_frame >= 0 && obj->animation_frame < obj->animation_end_frame &&
        roundToEven(obj->animation_time) >= obj->animation_end_frame) {
        obj->animation_time = (float)obj->animation_end_frame;
    }
    if (obj->animation_time >= (float)(count - 1)) {
        if ((obj->flags & MdkObject::FlagLoop) != 0) {
            if (obj->animation_time >= (float)count) {
                obj->animation_time -= (float)count;
            }
        } else {
            obj->animation_time = (float)(count - 1);
        }
    }
    /* @vfp-exempt R11 — the frame wrap's modulo by the animation's frame count, a
     * 1:1 port of the C#'s Round(AnimationTime) % count; the divisor is a small
     * positive per-animation constant, and a restructure would risk the pinned
     * frame sequence. measured n/a (corpus empty). Verified 2026-10-23. */
    int32_t frame = roundToEven(obj->animation_time) % count;
    if (frame != obj->animation_frame) {
        int32_t step = frame > obj->animation_frame ? frame - obj->animation_frame
                                                   : frame + count - obj->animation_frame;
        if ((obj->flags & MdkObject::FlagNoRootMotion) != 0) {
            step = 0;
        }
        for (int32_t i = 0; i < step; i++) {
            /* @vfp-exempt R11 — the passed-frames walk's modulo by the frame count,
             * a 1:1 port of the C#'s (AnimationFrame + 1 + i) % count; the divisor
             * is the same small positive constant as the wrap above.
             * measured n/a (corpus empty). Verified 2026-10-23. */
            size_t f = (size_t)((obj->animation_frame + 1 + i) % count);
            if (f < animation->rootMotion.size()) {
                const Vec3f& rm = animation->rootMotion[f];
                Vec3 turned = rotatedZ(Vec3{rm.x, rm.y, rm.z}, obj->yaw);
                /* @vfp-exempt R11 — root motion divided by the compile-time constant
                 * delta (1/30), matching the oracle's Push += motion/Dt bit-for-bit;
                 * not an integer divide. measured n/a. Verified 2026-10-23. */
                obj->push.x += turned.x / delta;
                obj->push.y += turned.y / delta;
                obj->push.z += turned.z / delta;
            }
        }
        obj->animation_frame = frame;
    }
    if (obj->animation_frame == count - 1 && (obj->flags & MdkObject::FlagLoop) == 0 &&
        obj->animation_frame != obj->animation_end_frame) {
        obj->animation_end_frame = MdkObject::AnimationEnded;
    }
}

// MdkObject.RestartAnimation: starts from frame 0 even when already playing.
void ScriptRuntime::restartAnimation(MdkObject* obj, const formats::ModelAnimation* animation,
                                     bool loop) {
    obj->animation = nullptr;
    playAnimation(obj, animation, loop);
}

// The door state bits (ObjectBehaviors): the low 4 are the engine's, the rest the
// scripts' (door_set_flags writes only them).
static const int32_t kDoorOpen = 0x1;
static const int32_t kDoorOpening = 0x2;
static const int32_t kDoorClosing = 0x4;
static const int32_t kDoorClosed = 0x8;
static const int32_t kDoorOpenNotSolid = 0x10;
static const int32_t kDoorStaysOpen = 0x20;
static const int32_t kDoorLocked = 0x40;
static const int32_t kDoorLockHidden = 0x100;
static const int32_t kDoorScriptBits = 0xF0;
static const int32_t kDoorOpenKept = 0x70;

// MdkObject.IsAnimationDone, as the runtime reads it (the VM has its own copy).
static bool animationDone(const MdkObject* obj) {
    return obj->animation == nullptr || obj->animation_end_frame == MdkObject::AnimationEnded;
}

// ObjectBehaviors.MoveDoor (0x43ca00): a door seen from its other side turns around,
// so it stays live, solid and drawn in Kurt's arena after the arena behind it goes.
static void moveDoor(MdkObject* obj) {
    std::string arena = obj->arena;
    obj->arena = obj->connects;
    obj->connects = arena;
    obj->yaw += 180.0f;
    if (obj->yaw >= 360.0f) {
        obj->yaw -= 360.0f;
    }
}

// ObjectBehaviors.ShowBehind: the arena the door connects to — or its own, when it
// connects to nothing — shows when the door opens.
void ScriptRuntime::showBehindDoor(MdkObject* obj) {
    for (int side = 0; side < 2; side++) {
        const std::string& arena = side == 0 ? obj->connects : obj->arena;
        if (arena.empty() || arena == currentArena_ ||
            (secondActive_ && arena == secondArena_)) {
            continue;
        }
        showArena(arena);
        return;
    }
}

// ObjectBehaviors.SetupDoor (spawn_connector): new doors are closed (0x45cdec), and
// the masks of their LOCK and HC... parts are found by name.
void ScriptRuntime::setupDoor(MdkObject* obj) {
    obj->door_state = kDoorClosed;
    obj->lock_parts = 0;
    obj->hatch_parts = 0;
    if (obj->model == nullptr) {
        return;
    }
    const std::vector<formats::Model::Part>& parts = obj->model->partList;
    for (size_t i = 0; i < parts.size(); i++) {
        const std::string name = parts[i].nameString();
        if (name == "LOCK") {
            obj->lock_parts |= (int32_t)(1 << i);
        } else if (name.size() >= 2 && name[0] == 'H' && name[1] == 'C') {
            obj->hatch_parts |= (int32_t)(1 << i);
        }
    }
}

// ObjectBehaviors.UpdateDoor: opens when Kurt comes closer than its distance, closes
// when he goes away, and plays the configured door sounds at the transitions.
void ScriptRuntime::updateDoor(MdkObject* obj) {
    if (obj->arena != currentArena_ && obj->connects == currentArena_) {
        moveDoor(obj);
    }

    int32_t state = obj->door_state;
    // ObjectBehaviors.Play: a door sound is played at the door unless it is
    // empty or the explicit "NONE" name.
    const auto play = [this, obj](const std::string& name) {
        if (!name.empty() && name != "NONE") {
            playSound(obj, name, 0, formats::ScriptValue());
        }
    };
    if ((state & kDoorOpening) != 0) {
        if (animationDone(obj)) {
            state = (state & kDoorOpenKept) | kDoorOpen;
            play(obj->door_sounds[2]);
        }
    } else if ((state & kDoorClosing) != 0 && animationDone(obj)) {
        state = (state & kDoorScriptBits) | kDoorClosed;
        play(obj->door_sounds[3]);
        showArena("");  // the arena behind it goes
    }

    const float dx = kurtPosition_.x - obj->position.x;
    const float dy = kurtPosition_.y - obj->position.y;
    const float dz = kurtPosition_.z - obj->position.z;
    if (sqrtf(dx * dx + dy * dy + dz * dz) >= obj->door_distance) {
        if ((state & (kDoorClosing | kDoorClosed | kDoorStaysOpen)) == 0) {
            restartAnimation(obj, getAnimation(obj, obj->door_animations[1]), false);
            state = (state & kDoorScriptBits) | kDoorClosing;
            play(obj->door_sounds[1]);
        }
    } else if ((state & (kDoorOpen | kDoorOpening | kDoorLocked)) == 0) {
        restartAnimation(obj, getAnimation(obj, obj->door_animations[0]), false);
        state = (state & kDoorScriptBits) | kDoorOpening;
        play(obj->door_sounds[0]);
        showBehindDoor(obj);
    }
    obj->door_state = state;

    // LOCK parts show a locked door; HC... parts hide while it's closed.
    int32_t hidden = obj->hidden_parts;
    if ((state & kDoorClosed) == 0) {
        hidden = (hidden | obj->lock_parts) & ~obj->hatch_parts;
    } else {
        hidden |= obj->hatch_parts;
        if ((state & kDoorLocked) != 0 && (state & kDoorLockHidden) == 0) {
            hidden &= ~obj->lock_parts;
        } else {
            hidden |= obj->lock_parts;
        }
    }
    obj->hidden_parts = hidden;
    if ((state & kDoorOpenNotSolid) == 0) {
        return;
    }
    obj->flags = (state & kDoorOpen) != 0 ? (obj->flags | MdkObject::FlagNotSolid)
                                         : (obj->flags & ~MdkObject::FlagNotSolid);
}

// GetWorldBounds: the pose box's eight corners, scaled, turned by yaw, lifted, and
// placed at the position. An object with no model is a point box at its position.
formats::Box ScriptRuntime::worldBounds(const MdkObject* obj) {
    if (obj->model == nullptr) {
        formats::Box box;
        box.min = Vec3f{obj->position.x, obj->position.y, obj->position.z};
        box.max = box.min;
        return box;
    }
    return worldBounds(obj, poseBounds(obj));
}

// GetWorldBounds(obj, bounds): the same corner transform on a caller-supplied box
// (a part's box from partBounds), not the object's pose bounds. The C# skips the
// no-model point-box branch when bounds is given (ScriptRuntime.cs:3120), and a
// part box only exists when there is a model, so this path always has one.
formats::Box ScriptRuntime::worldBounds(const MdkObject* obj, const formats::Box& box) {
    formats::Box world;
    bool have = false;
    for (int i = 0; i < 8; i++) {
        Vec3f corner{(i & 1) != 0 ? box.max.x : box.min.x, (i & 2) != 0 ? box.max.y : box.min.y,
                     (i & 4) != 0 ? box.max.z : box.min.z};
        Vec3 turned = rotatedZ(Vec3{corner.x * obj->scale, corner.y * obj->scale, corner.z * obj->scale},
                               obj->yaw);
        Vec3 point;
        point.x = obj->position.x + turned.x;
        point.y = obj->position.y + turned.y;
        point.z = obj->position.z + obj->lift() + turned.z;
        if (!have) {
            world.min = Vec3f{point.x, point.y, point.z};
            world.max = world.min;
            have = true;
        } else {
            if (point.x < world.min.x) world.min.x = point.x;
            if (point.y < world.min.y) world.min.y = point.y;
            if (point.z < world.min.z) world.min.z = point.z;
            if (point.x > world.max.x) world.max.x = point.x;
            if (point.y > world.max.y) world.max.y = point.y;
            if (point.z > world.max.z) world.max.z = point.z;
        }
    }
    return world;
}

// PartBounds: each part's box in the current pose, from the cached pose_parts().
// An empty part is invalid (the C# null). Same pose and per-part min/max as
// poseBounds, but every part is returned -- hidden parts included -- because the C#
// PartBounds has no HiddenParts filter; callers apply the hidden_parts bit.
std::vector<ScriptRuntime::PartBox> ScriptRuntime::partBounds(const MdkObject* obj) {
    std::vector<PartBox> result;
    partBoundsInto(obj, &result);
    return result;
}

void ScriptRuntime::partBoundsInto(const MdkObject* obj, std::vector<PartBox>* out) {
    std::vector<PartBox>& result = *out;
    result.clear();
    if (obj->model == nullptr) {
        return;  // C# PartBounds returns [] with no model
    }
    const std::vector<std::vector<Vec3f> >& pose = obj->pose_parts();
    result.resize(pose.size());
    for (size_t i = 0; i < pose.size(); ++i) {
        if (pose[i].empty()) {
            result[i].valid = false;
            continue;
        }
        Vec3f mn = pose[i][0];
        Vec3f mx = pose[i][0];
        for (size_t v = 1; v < pose[i].size(); ++v) {
            const Vec3f& p = pose[i][v];
            if (p.x < mn.x) mn.x = p.x;
            if (p.y < mn.y) mn.y = p.y;
            if (p.z < mn.z) mn.z = p.z;
            if (p.x > mx.x) mx.x = p.x;
            if (p.y > mx.y) mx.y = p.y;
            if (p.z > mx.z) mx.z = p.z;
        }
        result[i].valid = true;
        result[i].box.min = mn;
        result[i].box.max = mx;
    }
}

// IsWeakPart: the part name's character at weak_prefix_length is an ASCII digit and
// the name starts with the weak prefix (ordinal). The C# char.IsAsciiDigit is the
// '0'..'9' range only, not isdigit's locale set.
bool ScriptRuntime::isWeakPart(const MdkObject* obj, int32_t index) {
    // The C# indexes PartList[index] and name[length] with a managed bounds check
    // that throws; the port is -fno-exceptions, so guard the raw operands (index and
    // weak_prefix_length come from opcode 198's int operands). No valid caller is
    // out of range (the C# loops i < PartList.Count), so this changes no in-contract
    // behaviour -- it turns a would-be throw into "not a weak part" instead of UB.
    if (index < 0 || static_cast<size_t>(index) >= obj->model->partList.size()) {
        return false;
    }
    const std::string& name = obj->model->partList[static_cast<size_t>(index)].nameString();
    const int32_t length = obj->weak_prefix_length;
    if (length < 0 || static_cast<int32_t>(name.size()) <= length) {
        return false;
    }
    const char c = name[static_cast<size_t>(length)];
    if (c < '0' || c > '9') {
        return false;
    }
    const std::string& prefix = obj->weak_prefix;
    const size_t plen = std::min(static_cast<size_t>(length), prefix.size());
    return name.compare(0, plen, prefix, 0, plen) == 0;
}


// GetKurtBox: half width 0.6 in XY, height 5 from his feet.
formats::Box ScriptRuntime::kurtBox() const {
    const float kKurtHalfWidth = 0.6f;
    const float kKurtHeight = 5.0f;
    formats::Box box;
    box.min = Vec3f{kurtPosition_.x - kKurtHalfWidth, kurtPosition_.y - kKurtHalfWidth, kurtPosition_.z};
    box.max = Vec3f{kurtPosition_.x + kKurtHalfWidth, kurtPosition_.y + kKurtHalfWidth,
                    kurtPosition_.z + kKurtHeight};
    return box;
}

// The one Kurt health, seen from the scripts' side. With a Player bound this is
// the entity's own combat half — the C# hands its single Kurt to the runtime
// (ScriptRuntime.cs:168) and every writer (hurt_kurt, the hard landing) and every
// reader (if_player_health, the death transition, the HUD) goes through it. Out of
// line because the seam needs the complete Player.
const Kurt& ScriptRuntime::kurt() const { return player_ ? player_->combat() : unboundKurt_; }

Kurt& ScriptRuntime::kurt() { return player_ ? player_->combat() : unboundKurt_; }

// C# HurtKurt (ScriptRuntime.cs:2676, 0x46a498): riding the XD2 or the XE the
// ridden object takes the hits (and dies at 0 unless indestructible); on foot or
// on the snowboard Kurt does. Rides.TakesHits is Ridden != null && no board
// (scripts/rides.h, package #9).
void ScriptRuntime::hurtKurt(int32_t damage) {
    if (!rides_.takesHits()) {
        kurt().hurt(damage);
        return;
    }

    MdkObject* ridden = rides_.ridden();
    if (ridden->health >= kIndestructible) {
        return;
    }

    ridden->health -= damage;
    if (ridden->health < 1) {
        kill(ridden);
    }
}

// RaiseShake (ScriptRuntime.cs:1986, 0x467f7c): keep the strongest request
// made during this tick. MathF.Max propagates NaN; mathMax preserves that.
// Then the C#'s ShakeRaised event with the amount asked, which the follow camera
// takes (Viewer.cs:238: FollowCamera.RaiseShake; package #15).
void ScriptRuntime::raiseShake(float amount) {
    shake = mathMax(shake, amount);
    if (shakeRaisedHook_ != nullptr) {
        shakeRaisedHook_(shakeRaisedUser_, amount);
    }
}

// C# FlattenTown (ScriptRuntime.cs:2139-2154, package #15): the minecrawler reached the
// town -- the screen shakes and a message says which town is gone (OOT_L1, "There goes
// Laguna Beach!", or OOT_L1A for the second town), and the global flags record which one
// for the debriefing (GameStats.TownFlags, copied when the level ends). The C# raises no
// GameOver: the level goes on.
void ScriptRuntime::flattenTown() {
    raiseShake(kTownShake);
    std::string text = "OOT_L" + std::to_string(flow::indexOf(level_->number()) + 1);
    if ((globalFlags & kFlagSecondTown) != 0) {
        text += "A";
        globalFlags |= kFlagSecondTownFlattened;
    } else {
        globalFlags |= kFlagTownFlattened;
    }
    // Messages?.Push(text, FlagZoom | FlagFront, TownMessageSeconds).
    pushMessage(text, Messages::kFlagZoom | Messages::kFlagFront, kTownMessageSeconds);
}

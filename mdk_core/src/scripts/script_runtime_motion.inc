// script_runtime_motion.inc -- paths and the motions: path keys, velocity, updraft, bank, pickup, runner.
// Part of script_runtime.cpp's single translation unit (see its header): pasted
// inside namespace mdk::scripts after its file-local constants and helpers,
// which it uses. No include guards, no includes of its own; the order is
// load-bearing.

int32_t ScriptRuntime::pathKeyCount(int32_t path) {
    if (path < 0 || (size_t)path + 4 > cmi_->bytes().size()) {
        return 0;
    }
    return (int32_t)readU32(cmi_->bytes().data(), path);
}

int32_t ScriptRuntime::pathKeyFrame(int32_t path, int32_t key) {
    // A corrupt count could make key huge; compute the offset in 64-bit so the
    // multiply cannot overflow, and read nothing past the CMI image.
    if (key < 0) {
        return 0;
    }
    long long off = (long long)path + kPathCountSize + (long long)key * kPathKeySize;
    if (off < 0 || (size_t)off + 4 > cmi_->bytes().size()) {
        return 0;
    }
    return readS32(cmi_->bytes().data(), (int)off);
}

Vec3 ScriptRuntime::pathVector(int32_t path, int32_t key, int32_t field) {
    Vec3 v;
    long long off = (long long)path + kPathCountSize + (long long)key * kPathKeySize +
                    kPathFrameSize + field * kPathVectorSize;
    if (key < 0 || off < 0 || (size_t)off + 12 > cmi_->bytes().size()) {
        return v;
    }
    const uint8_t* b = cmi_->bytes().data();
    v.x = readF32(b, (int)off);
    v.y = readF32(b, (int)off + 4);
    v.z = readF32(b, (int)off + 8);
    return v;
}

Vec3 ScriptRuntime::pathPosition(int32_t path, float time) {
    int32_t key = pathKeyCount(path) - 2;
    while (key > 0 && (float)pathKeyFrame(path, key) >= time) {
        key--;
    }
    if (key < 0) {
        key = 0;
    }
    float start = (float)pathKeyFrame(path, key);
    float length = (float)pathKeyFrame(path, key + 1) - start;
    float u = length != 0.0f ? (time - start) / length : 0.0f;
    Vec3 p0 = pathVector(path, key, 0);
    Vec3 p1 = pathVector(path, key + 1, 0);
    Vec3 m0 = pathVector(path, key, 2);
    Vec3 m1 = pathVector(path, key + 1, 1);
    // Hermite: a = m0 + m1 - 2d, b = 3d - 2m0 - m1, result = ((a*u + b)*u + m0)*u + p0,
    // where d = p1 - p0. Component-wise (Vec3 has no operators).
    Vec3 out;
    out.x = hermite(p0.x, p1.x, m0.x, m1.x, u);
    out.y = hermite(p0.y, p1.y, m0.y, m1.y, u);
    out.z = hermite(p0.z, p1.z, m0.z, m1.z, u);
    return out;
}

void ScriptRuntime::startPath(MdkObject* obj, int32_t path, float time, const Vec3& origin) {
    obj->path = path;
    obj->path_time = time;
    obj->path_origin = origin;
    obj->path_stop = -1;
    Vec3 p = pathPosition(path, time);
    obj->position.x = p.x + origin.x;
    obj->position.y = p.y + origin.y;
    obj->position.z = p.z + origin.z;
}

void ScriptRuntime::motionPath(MdkObject* obj) {
    // An effect plays only its texture animation and never path-moves (the C#
    // Update returns on EffectFrames before any path work).
    if (obj->effect_frames > 0) {
        return;
    }
    if (obj->path == 0) {
        return;
    }
    if ((obj->flags & MdkObject::FlagPathSpeedByKurt) != 0) {
        // ObjectMotion.UpdatePathSpeed (ObjectMotion.cs:189, 0x43c258; opcode 164): the
        // path speed goes towards the speed for Kurt being ahead of, around, or behind
        // the set distance (along the object's heading), by (near - far) * 0.5 per
        // second. It runs before UpdatePath, so before the path-stop test below.
        const float kAround = 5.0f;
        const Vec3 heading = fromAngle(obj->yaw);
        const float ahead = (obj->position.x - kurtPosition_.x) * heading.x +
                            (obj->position.y - kurtPosition_.y) * heading.y;
        const std::array<float, 4>& speeds = obj->path_speeds;
        const float distance = speeds[0];
        float target = speeds[1];
        if (ahead <= distance + kAround) {
            target = ahead >= distance - kAround ? speeds[2] : speeds[3];
        }
        const float step = (speeds[3] - speeds[1]) * kTick * 0.5f;
        obj->path_speed = obj->path_speed <= target ? mathMin(obj->path_speed + step, target)
                                                    : mathMax(obj->path_speed - step, target);
    }
    int32_t path = obj->path;
    if (obj->path_stop >= 0) {
        // Round halves away from zero (ObjectMotion.Round == MathF.Round's
        // AwayFromZero); roundf matches it exactly, the ±0.5f idiom does not.
        int32_t r = (int32_t)roundf(obj->path_time);
        if (r == obj->path_stop) {
            return;
        }
    }
    const float step = obj->path_speed;  // Ticks is 1 frame per tick
    int32_t last = pathKeyFrame(path, pathKeyCount(path) - 1);
    float stop = (float)obj->path_stop;
    bool once = (obj->flags & MdkObject::FlagPathOnce) != 0;
    if (step < 0.0f) {
        if (obj->path_stop < 0 || obj->path_time < stop || stop < obj->path_time + step) {
            obj->path_time += step;
        } else {
            obj->path_time = stop;
        }
        if (obj->path_time < 0.0f) {
            if (once) {
                obj->path = 0;
                obj->path_time = 0.0f;
            } else {
                obj->path_time += (float)(last - 1);
            }
        }
    } else {
        if (obj->path_stop < 0 || stop < obj->path_time || obj->path_time + step < stop) {
            obj->path_time += step;
        } else {
            obj->path_time = stop;
        }
        if (once) {
            if (obj->path_time >= (float)(last - 2)) {
                obj->path = 0;
                obj->path_time = (float)(last - 2);
            }
        } else if (obj->path_time >= (float)(last - 1)) {
            obj->path_time -= (float)(last - 1);
        }
    }

    Vec3 old = obj->position;
    Vec3 pp = pathPosition(path, obj->path_time);
    Vec3 p;
    p.x = pp.x + obj->path_origin.x;
    p.y = pp.y + obj->path_origin.y;
    p.z = pp.z + obj->path_origin.z;
    bool pushes = (obj->flags & MdkObject::FlagPathPushes) != 0;
    if (pushes) {
        p.z = old.z;
    }
    if ((obj->flags & MdkObject::FlagNoTurning) == 0 &&
        fabsf(p.x - old.x) + fabsf(p.y - old.y) > kTick * 0.5f) {
        /* @vfp-exempt R6 — path-following turn (per path-riding object per tick);
         * glibc atan2f matches the C# oracle's MathF.Atan2 bit-for-bit on this
         * machine (probed), and the conversion is .NET's left-associative
         * radiansToDegrees; the fastAtan2 LUT's error is far larger and would move
         * those values. measured n/a (corpus empty). Verified 2026-10-23. */
        float deg = radiansToDegrees(atan2f(p.y - old.y, p.x - old.x));
        obj->yaw = wrap360(deg + obj->path_yaw_offset);
    }
    if (!pushes) {
        obj->position = p;
        return;
    }
    // The path drives the horizontal velocity instead of the position
    // (ObjectMotion.cs:309-310): Push, which ApplyVelocity spends this tick. The
    // snowboarders of LEVEL4 (XGSNOW) ride their paths this way, at the speed
    // path_speed_by_kurt sets above.
    const float pathDt = kTick;  // ObjectMotion.Dt: a float divide, as the C#'s
    obj->push.x += (p.x - old.x) / pathDt;
    obj->push.y += (p.y - old.y) / pathDt;
}

void ScriptRuntime::motionVelocity(MdkObject* obj) {
    // An effect plays only its texture animation (the C# Update returns before any
    // gravity/velocity work), exactly as motionPath skips it.
    if (obj->effect_frames > 0) {
        return;
    }
    // UpdateCommand (ObjectMotion.cs:80): a commanded object walks or flies toward
    // its waypoint, adding to push BEFORE gravity/ApplyVelocity below. The command
    // was set this tick by the VM (vm_.run ran before motionVelocity in runTick),
    // so the first step happens on the command tick exactly as the C#.
    updateCommand(obj);
    // The pendulum (ObjectMotion.cs:86-89): after the command and before gravity,
    // a FlagSwinging object (jump_to, opcode 226) swings under its pivot. The C#
    // returns from Update for an object its command killed, so a dead one does not
    // swing here either.
    if (!obj->dead && (obj->flags & MdkObject::FlagSwinging) != 0) {
        motionSwing(obj);
    }
    // A just-spawned object is not moved on its spawn tick by the tick loop's
    // snapshot (the C# ListCopy at ScriptRuntime.cs:962): objects spawned during
    // the loop or by the controllers are appended after it and skipped. There is
    // deliberately NO spawn-tick gate here — the C# has none, and one would wrongly
    // skip the first real fall tick for a DTI-spawned gravity object (spawned
    // before the snapshot) once arena crossings land; footing holds those instead.
    bool gravity = (obj->flags & MdkObject::FlagGravity) != 0;
    if (gravity) {
        // ObjectMotion.cs:91-96, in the C#'s order: gravity, the fans' updraft, then the
        // terminal clamp (MathF.Max, so a NaN stays NaN as there).
        obj->velocity.z -= obj->gravity * kTick;
        motionUpdraft(obj);
        obj->velocity.z = mathMax(obj->velocity.z, -220.0f);  // TerminalVelocity
    }

    // ApplyVelocity (0x45e810). Friction acts on the horizontal only for a gravity
    // object (its vertical is gravity's, not friction's).
    obj->contact_flags &= ~(MdkObject::ContactCollided | MdkObject::ContactFloor | 0x10);
    float mx = obj->velocity.x;
    float my = obj->velocity.y;
    float mz = gravity ? 0.0f : obj->velocity.z;
    float length = sqrtf(mx * mx + my * my + mz * mz);
    if (length > 0.0f) {
        // MathF.Max (ObjectMotion.cs:1066), so a NaN friction makes the FACTOR NaN and
        // the velocity NaN; fmaxf would return the 0 and silently stop the object dead.
        // Measured on .NET 10: friction NaN leaves Velocity = NaN,NaN,NaN (probe mode
        // `nanclamp`, row FRICTION nan), while a finite 60 leaves 1.8,2.4,0.
        float factor = mathMax(length - obj->friction * kTick, 0.0f) / length;
        obj->velocity.x *= factor;
        obj->velocity.y *= factor;
        if (!gravity) {
            obj->velocity.z *= factor;
        }
    }

    Vec3 motion;
    motion.x = (obj->velocity.x + obj->push.x) * kTick;
    motion.y = (obj->velocity.y + obj->push.y) * kTick;
    motion.z = (obj->velocity.z + obj->push.z) * kTick;
    obj->push.x = 0.0f;
    obj->push.y = 0.0f;
    obj->push.z = 0.0f;
    if (motion.x == 0.0f && motion.y == 0.0f && motion.z == 0.0f) {
        // C# ApplyVelocity returns here. Update still runs the animation, the
        // roll and the PreviousPosition write after it (ObjectMotion.cs:132-146),
        // which the caller does; nothing is recorded here.
        return;
    }

    // A FlagCollides object sweeps its box against the arena BSP and stops on a
    // floor (velocity.Z zeroed, ContactFloor set); one without it moves freely.
    if ((obj->flags & MdkObject::FlagCollides) == 0) {
        obj->position.x += motion.x;
        obj->position.y += motion.y;
        obj->position.z += motion.z;
    } else {
        Vec3 normal{0.0f, 0.0f, 1.0f};
        if (sweepBox(obj, motion, &normal)) {
            obj->contact_flags |= MdkObject::ContactCollided;
            if (motion.z <= 0.0f) {
                obj->contact_flags |= MdkObject::ContactFloor;
            }
            if ((obj->flags & MdkObject::FlagBounces) != 0) {
                // C# velocity += normal * dot(velocity, normal) * Bounce (-1.8).
                float d = obj->velocity.x * normal.x + obj->velocity.y * normal.y +
                          obj->velocity.z * normal.z;
                obj->velocity.x += normal.x * d * -1.8f;
                obj->velocity.y += normal.y * d * -1.8f;
                obj->velocity.z += normal.z * d * -1.8f;
            } else {
                obj->velocity.z = 0.0f;
            }
        }
    }

    // FallOut: below the arena floor by 200 (FallOutDepth).
    if (obj->position.z < getArenaFloor(obj->arena) - 200.0f) {
        fallOut(obj);
    }
}

// ObjectMotion.Swing (ObjectMotion.cs:537-569, 0x43cfe8): the pendulum that jump_to
// (opcode 226) starts. The pitch swings by w -= sin(theta) * k * t, theta += w * k * t
// (t = 1 tick), and the object hangs swing_length below the pivot in the vertical
// plane of its yaw:
//
//        pivot
//          | \  rope
//          |  \ theta = pitch
//          |   * object
//
// At each turning point (the speed changes sign, or is zero) that plane turns
// towards the scripts' target by at most 15 degrees, folded to +-90 (either side
// of the plane will do), and the yaw goes there at 10 degrees a second. The rope
// (RopeView's pair 0: the object, lifted 5, to the pivot) is the swing's to draw.
//
// Every operation is the C#'s, in its order: MathF.Sin/SinCos are libm sinf/cosf
// (bit-identical on this host, the probe-checked fromAngle's note), the conversion
// is .NET's left-associative degreesToRadians, and Math.Clamp/MoveToward are
// clampf/moveToward. A LEVEL6 OLYM_6 pendulum (XSWINGB) is the only retail user, so
// this runs for one object a tick: not a hot path (AGENTS.md section 2).
/* @vfp-exempt R6 — the pendulum's swing (ObjectMotion.Swing) is gameplay: it places a
 * damaging object, so it keeps the C# oracle's MathF.Sin/SinCos as glibc sinf/cosf
 * (bit-identical on this machine, golden L6_hazards_swing) and .NET's left-associative
 * DegreesToRadians; the fastSin/fastCos LUT would move the pendulum and where it hits Kurt.
 * measured n/a (corpus empty). Verified 2026-11-26. */
void ScriptRuntime::motionSwing(MdkObject* obj) {
    const float kMaxTurn = 15.0f;
    const float kQuarterTurn = 90.0f;
    const float kHalfTurn = 180.0f;
    const float kPlaneTurnSpeed = 10.0f;
    const float kTicks = 1.0f;  // ObjectMotion.Ticks
    const float oldSpeed = obj->swing_speed;
    obj->swing_speed -= sinf(degreesToRadians(obj->pitch)) * obj->swing_gain * kTicks;
    obj->pitch += obj->swing_speed * obj->swing_gain * kTicks;
    const float radians = degreesToRadians(obj->pitch);
    const float sine = sinf(radians);
    const float cosine = cosf(radians);
    const Vec3 heading = fromAngle(obj->yaw);
    // pivot + new Vector3(h.X * sin, h.Y * sin, -cos) * length, per component.
    obj->position.x = obj->swing_pivot.x + heading.x * sine * obj->swing_length;
    obj->position.y = obj->swing_pivot.y + heading.y * sine * obj->swing_length;
    obj->position.z = obj->swing_pivot.z + -cosine * obj->swing_length;
    if (obj->swing_speed * oldSpeed <= 0.0f) {
        obj->yaw = wrap360(obj->yaw);
        float diff = wrapAngle(yawTo(obj, targetPosition_) - obj->yaw);
        if (diff < -kQuarterTurn) {
            diff += kHalfTurn;
        } else if (diff > kQuarterTurn) {
            diff -= kHalfTurn;
        }
        obj->swing_yaw = obj->yaw + clampf(diff, -kMaxTurn, kMaxTurn);
    }
    obj->yaw = moveToward(obj->yaw, obj->swing_yaw, kPlaneTurnSpeed * kTick);
    obj->rope_color = 1;
    obj->rope_mask = 1;
    obj->rope_points[0] = obj->position;
    obj->rope_points[1] = obj->swing_pivot;
}

// UpdatePickup (0x43d8f0), run after ApplyVelocity for a FlagPickup object: a
// falling pickup lands on floor contact (Fall), opens its chute once it reaches
// terminal speed (a SW_CHUTE hung on it by attach command 74), shrinks that chute
// away after it lands, and — for the SW_H150, once the chute is gone — runs (the
// Runner, below). A landed or non-gravity pickup not in the Still list spins in
// place.
//
// What is NOT here is the Collected branch: an object the player has picked up
// shrinks over ShrinkTicks and is then removed (ObjectBehaviors.cs:206-212), and
// that waits on the inventory slice (ObjectBehaviors.cs:207-218). The guard below
// returns early for a FlagCollected object — nothing in this port sets that flag
// yet, and where the C# shrinks before returning, this returns without shrinking.

// ObjectMotion.Updraft (ObjectMotion.cs:162-181, 0x45e74c): the fans lift objects with
// gravity (not LEVEL6's rolling ones); Kurt's thrown items in flight -- flag 0x1000 without
// 0x4000 (active), 0x40000 (collected) -- but the mortar also lose most of their
// horizontal speed in them. Fans::query answers NaN where no fan of the arena holds it.
void ScriptRuntime::motionUpdraft(MdkObject* obj) {
    const int32_t kLevelSix = 6;
    const int32_t kThrownFlagsMask = 0x45000;
    const int32_t kMortarItem = 4;
    const float kUpdraftBrake = 0.1f;
    if (level_->number() == kLevelSix && (obj->flags & MdkObject::FlagRolling) != 0) {
        return;
    }
    const float vz = fans.query(obj->arena, obj->position, obj->velocity.z, Fans::kMaskObjects, kTick);
    if (vz != vz) {
        return;
    }
    obj->velocity.z = vz;
    if ((obj->flags & kThrownFlagsMask) == kItemFlagThrown && obj->thrown_kind != kMortarItem) {
        obj->velocity.x *= kUpdraftBrake;
        obj->velocity.y *= kUpdraftBrake;
    }
}

// ObjectMotion.Bank (0x43b65c): an object leans into its turn. The turn counted is
// the yaw change since the LAST call, at most 2 degrees of it per tick; the roll is
// the old roll minus that turn, damped by 0.95 and kept inside +/-10 degrees.
//
// Two details a rewrite loses:
//   * BankingYaw is written BEFORE the flag test, so an object that never banks
//     still tracks its own yaw. Writing it only for a banking object would make the
//     first tick after set_banking 1 count every degree turned while it was
//     flagged, and lean over by the whole of it.
//   * The roll is wrapped before the subtraction, so a roll that some other writer
//     left outside +/-180 comes back through the short way round.
void ScriptRuntime::motionBank(MdkObject* obj) {
    const float kMaxTurn = 2.0f;   // degrees per tick
    const float kMaxRoll = 10.0f;  // degrees
    const float kDamping = 0.95f;
    const int32_t kNoRoll = 1;     // the C#'s unnamed flag 1
    const float kTicks = 1.0f;     // ObjectMotion.Ticks: one tick, in ticks
    // The clamps are float predicates on a gameplay angle, not a loop bound and not
    // a lattice index: R2/R3/T2 have nothing to bite on here, and the same idiom
    // (clampf) is what the walk step and the turn step already use.
    const float turn = clampf(wrapAngle(obj->yaw - obj->banking_yaw) / kTicks, -kMaxTurn, kMaxTurn);
    obj->banking_yaw = obj->yaw;
    if ((obj->flags & (MdkObject::FlagNoBanking | kNoRoll | MdkObject::FlagRolling)) != 0 ||
        obj->thrown_kind > 0) {
        return;
    }
    obj->roll = clampf((wrapAngle(obj->roll) - turn) * kDamping, -kMaxRoll, kMaxRoll);
}

void ScriptRuntime::motionPickup(MdkObject* obj) {
    const float kPickupHover = 1.5f;
    const float kPickupTurnSpeed = 180.0f;
    if ((obj->flags & MdkObject::FlagCollected) != 0) {
        // A pickup Kurt took (ObjectBehaviors.cs:207-217): it shrinks over ShrinkTicks
        // (30), spinning 4 times as fast, and goes.
        const float kShrinkTicks = 30.0f;
        const float kCollectedSpin = 4.0f;
        obj->parameter_timer -= 1.0f;
        obj->scale = mathMax(obj->parameter_timer / kShrinkTicks, 0.0f);
        obj->yaw = wrap360(obj->yaw + kTick * kPickupTurnSpeed * kCollectedSpin);
        if (obj->parameter_timer <= 0.0f) {
            remove(obj);
        }
        return;
    }
    bool gravity = (obj->flags & MdkObject::FlagGravity) != 0;
    bool landed = (obj->flags & MdkObject::FlagLanded) != 0;
    if (gravity && !landed) {
        // Fall (0x43daf4): on floor contact the pickup lands. The Runner (SW_H150)
        // rests on the floor (hover 0); the others hover on their chute.
        if ((obj->contact_flags & MdkObject::ContactFloor) != 0) {
            float hover = obj->type_name == "SW_H150" ? 0.0f : kPickupHover;
            obj->height_offset = hover;
            obj->flags |= MdkObject::FlagLanded;
            obj->position.z += hover;
            return;
        }
        // Still falling: once it reaches terminal speed (PickupChuteSpeed, -15) it
        // clamps there and opens its chute — a SW_CHUTE hung on it by attach command
        // 74. Above that speed it is still accelerating, so nothing happens yet.
        const float kPickupChuteSpeed = -15.0f;
        if (obj->velocity.z >= kPickupChuteSpeed) {
            return;
        }
        obj->velocity.z = kPickupChuteSpeed;
        if (obj->attached != nullptr) {
            return;  // chute already open
        }
        MdkObject* chute = spawn(obj, "SW_CHUTE", obj->position, obj->yaw, -1, 0, false);
        if (chute == nullptr) {
            return;
        }
        chute->flags = 0x820;      // ChuteFlags
        chute->leader = obj;
        chute->attach_point_a = 0;
        chute->attach_point_b = 0;
        chute->move_command = 74;  // AttachCommand
        obj->attached = chute;
        return;
    }
    // The chute shrinks away once the pickup has landed (ObjectBehaviors.cs:226):
    // a notch per tick, and gone (removed) at ChuteGone.
    const float kChuteGone = 0.2f;
    MdkObject* chute = obj->attached;
    if (chute != nullptr && chute->scale > kChuteGone) {
        chute->scale -= kTick;
        if (chute->scale <= kChuteGone) {
            remove(chute);
            obj->attached = nullptr;
        }
    }
    // The Runner: SW_H150 runs away, and only once its chute is gone (the C#'s
    // `obj.Attached == null`, which is also the first tick it is on the floor).
    if (obj->type_name == "SW_H150" && obj->attached == nullptr) {
        motionRunner(obj);
    }
    // Landed or non-gravity: a pickup not in the Still list spins in place at
    // PickupTurnSpeed.
    if (obj->type_name != "SW_H150" && obj->type_name != "SW_SEAL" &&
        obj->type_name != "SW_SBONE") {
        obj->yaw = wrap360(obj->yaw + kTick * kPickupTurnSpeed);
    }
}

// The C#'s Items.GetAnimation, for the two animations the Runner plays. The
// archive is the level's TRAVSPRT.BNI; the C# parses each name once and keeps it,
// and ObjectBehaviors compares the RESULT BY IDENTITY (`obj.Animation != run`), so
// the cache must answer the same address for the same name — a std::map's nodes do.
//
// Null when there is no archive (a harness that did not load one) or no such entry.
// The C# has no such case: the entries are there in a real game, and the port does
// not add a guard the oracle does not have.
const ModelAnimation* ScriptRuntime::itemsAnimation(const char* name) {
    if (sprites_ == nullptr) {
        return nullptr;
    }
    std::map<std::string, ModelAnimation>::iterator found = itemsAnimations_.find(name);
    if (found != itemsAnimations_.end()) {
        return &found->second;
    }
    const formats::ArchiveIndex::Entry* entry = sprites_->index().find(name);
    if (entry == nullptr) {
        return nullptr;
    }
    ModelAnimation animation;
    const std::vector<uint8_t>& bytes = sprites_->bytes();
    if (!ModelAnimation::parse(&animation, name, bytes.data(), (int32_t)bytes.size(), entry->offset)) {
        return nullptr;
    }
    std::pair<std::map<std::string, ModelAnimation>::iterator, bool> inserted =
        itemsAnimations_.emplace(name, std::move(animation));
    return &inserted.first->second;
}

// MdkObject.IsAnimationDone, as the runtime reads it (the VM has its own copy).
static bool animationDone(const MdkObject* obj);

// ObjectBehaviors.UpdateRunner (0x43daf4's SW_H150 branch): the hamster.
//
//   IDLE ──(Kurt within 20)──► RUN ──(Kurt beyond sqrt(1000))──► IDLE
//
// Idle, it replays H150_I on a 72-in-32768 chance a tick; running, it pushes 40
// units/s along its yaw, turns away from Kurt at up to 270 degrees/s, and keeps
// FlagGravity set (it is on the floor, not hovering on a chute).
//
// THE RNG IS THE POINT. The chance is drawn ONLY when the object is idle or its
// animation has ended, from the same Random every scripted random decision takes.
// Drawing it unconditionally, or not at all, moves every later draw in the level —
// which is why this was not quietly approximated by a timer.
void ScriptRuntime::motionRunner(MdkObject* obj) {
    const float kRunnerStart = 20.0f;        // units: it starts when Kurt is this close
    const float kRunnerStopSquared = 1000.0f;  // units^2 (sqrt = 31.6): stops beyond this
    const float kRunnerSpeed = 40.0f;        // units/s
    const float kRunnerTurn = 270.0f;        // degrees/s
    const int32_t kRunnerIdleChance = 72;
    const int32_t kChanceRange = 32768;

    const ModelAnimation* run = itemsAnimation("H150_R");
    const Vec3 kurt = kurtPosition_;
    if (obj->animation != run) {
        if ((obj->animation == nullptr || animationDone(obj)) &&
            rng_.next(kChanceRange) < kRunnerIdleChance) {
            restartAnimation(obj, itemsAnimation("H150_I"), false);
            return;
        }
        // Vector3.DistanceSquared(Position, kurt) < (RunnerStart * RunnerStart).
        const float dx = obj->position.x - kurt.x;
        const float dy = obj->position.y - kurt.y;
        const float dz = obj->position.z - kurt.z;
        if (dx * dx + dy * dy + dz * dz < kRunnerStart * kRunnerStart) {
            restartAnimation(obj, run, true);
            mixer_.play("RUNNER", audio::SoundMixer::Once);
        }
        return;
    }

    // Running: ScriptMath.FromAngle(yaw) * RunnerSpeed joins the push, so the
    // ApplyVelocity step that follows moves it RunnerSpeed * Dt this tick.
    const Vec3 ahead = fromAngle(obj->yaw);
    obj->push.x += ahead.x * kRunnerSpeed;
    obj->push.y += ahead.y * kRunnerSpeed;
    // It faces away from Kurt: the heading to him, plus half a turn.
    const float away = heading(obj->position, kurt) + 180.0f;
    const float turn = clampf(wrapAngle(away - obj->yaw), -kRunnerTurn * kTick, kRunnerTurn * kTick);
    obj->yaw = wrap360(obj->yaw + turn);
    obj->flags |= MdkObject::FlagGravity;
    // The stop test is the plan-view distance only, and it is `>`, not `>=`.
    const float ex = kurt.x - obj->position.x;
    const float ey = kurt.y - obj->position.y;
    if (ex * ex + ey * ey > kRunnerStopSquared) {
        restartAnimation(obj, itemsAnimation("H150_I"), false);
    }
}

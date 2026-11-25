// script_runtime_squads.inc -- targets, finding and linking objects, chains, formations, the chase, commands.
// Part of script_runtime.cpp's single translation unit (see its header): pasted
// inside namespace mdk::scripts after its file-local constants and helpers,
// which it uses. No include guards, no includes of its own; the order is
// load-bearing.

// C# ScriptRuntime.SelectTarget: an object's script aims at Kurt, or at the decoy
// while it walks, or at the aliens' target (opcode 251), unless the object always
// targets Kurt (target mode 2). Items.Decoy is not ported yet, so the decoy branch
// stays null until the Items slice lands; the alien-target branch is live.
void ScriptRuntime::selectTarget(MdkObject* obj) {
    const int32_t alwaysKurt = 2;
    targetPosition_ = kurtPosition_;
    targetYaw_ = kurtYaw_;
    if (obj->target_mode == alwaysKurt) {
        return;
    }
    // The decoy while it walks (Items.Decoy), else the aliens' target (opcode 251).
    MdkObject* other = nullptr;
    if (itemsDecoy != nullptr && !itemsDecoy->dead) {
        other = itemsDecoy;
    } else if (alienTarget_ != nullptr && !alienTarget_->dead) {
        other = alienTarget_;
    }
    if (other != nullptr) {
        targetPosition_ = other->position;
        targetYaw_ = other->yaw;
    }
}

// C# FirstOther(obj, type, AnyInstance): the first live object of the same arena,
// of the named type, other than obj. The type compare is case-insensitive (C# Is).
MdkObject* ScriptRuntime::firstOther(MdkObject* obj, const std::string& type, int32_t instance) {
    const int32_t kAnyInstance = -1;
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        MdkObject& other = *it;
        if (&other == obj || other.dead || other.arena != obj->arena) {
            continue;
        }
        if (instance != kAnyInstance && other.instance_id != instance) {
            continue;
        }
        if (iequalsType(other.type_name, type)) {
            return &other;
        }
    }
    return nullptr;
}

// C# ScriptVm case 74: match the named TYPE_ID in this arena, retaining the last live match.
void ScriptRuntime::attachTo(MdkObject* obj, int32_t pointA, int32_t pointB,
                             const std::string& name) {
    MdkObject* found = nullptr;
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        MdkObject& other = *it;
        if (&other == obj || other.dead || isRemoved(&other) || other.arena != obj->arena) {
            continue;
        }
        const std::string objectName = other.type_name + "_" + std::to_string(other.instance_id);
        if (formats::toLowerAscii(objectName) == formats::toLowerAscii(name)) {
            found = &other;
        }
    }
    if (found != nullptr) {
        obj->leader = found;
        obj->attach_point_a = pointA;
        obj->attach_point_b = pointB;
        obj->move_command = 74;
    }
}

// C# ScriptVm.FindObject: mode 0 random, mode 1 nearest, mode 2 nearest thrown item
// within sight/range. Collection order is the stable object-list order.
MdkObject* ScriptRuntime::findObject(MdkObject* obj, const std::string& type, int32_t mode) {
    const float kItemRange = 200.0f;
    const float kItemAbove = 9.0f;
    const auto eligible = [&](MdkObject& other, float* outDistance) {
        if (&other == obj || other.dead || isRemoved(&other) || other.arena != obj->arena ||
            !iequalsType(other.type_name, type)) {
            return false;
        }
        float dx = other.position.x - obj->position.x;
        float dy = other.position.y - obj->position.y;
        float dz = other.position.z - obj->position.z;
        float distance = sqrtf(dx * dx + dy * dy + dz * dz);
        if (mode == 2) {
            if (distance > kItemRange || (other.flags & 0x1000) == 0 ||
                other.position.z > obj->position.z + kItemAbove) {
                return false;
            }
            Vec3 from{obj->position.x, obj->position.y, obj->position.z + 5.0f};
            Vec3 to{other.position.x, other.position.y, other.position.z + 2.0f};
            if (!raycastClear(from, to)) {
                return false;
            }
        }
        *outDistance = distance;
        return true;
    };
    MdkObject* best = nullptr;
    float bestDistance = FLT_MAX;
    int32_t count = 0;
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        float distance = 0.0f;
        if (!eligible(*it, &distance)) {
            continue;
        }
        if (distance < bestDistance) {
            best = &*it;
            bestDistance = distance;
        }
        count++;
    }
    if (count == 0) {
        return nullptr;
    }
    if (mode != 0) {
        return best;
    }
    int32_t selected = rng_.next(count);
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        float distance = 0.0f;
        if (!eligible(*it, &distance)) {
            continue;
        }
        if (selected-- == 0) {
            return &*it;
        }
    }
    return nullptr;
}

// C# ScriptVm.LinkPartners: pair the arena's live, matching-type objects when
// they are closer than 100 units. Collection order is the stable object-list
// order, and a pair takes the first later eligible object, not the nearest.
void ScriptRuntime::linkPartners(MdkObject* obj, const std::string& type) {
    const float kRangeSquared = 10000.0f;
    std::vector<MdkObject*> loose;
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        MdkObject& other = *it;
        if (other.dead || other.arena != obj->arena ||
            !iequalsType(other.type_name, type)) {
            continue;
        }
        // The C# filter admits a null link or a link to a DEAD object, then
        // skips every non-null link in the pairing loop. Admitting the dead
        // link is unobservable for the pairing itself — a widow is skipped as
        // i and as j — but the clause is the oracle's filter and is kept
        // verbatim; the retained link it admits is what variable source 3 and
        // getFlags/setFlags read, and what keeps the widow from being paired
        // again here. The other `linked` consumers (180, 213/214, 246) read the
        // field too, but they reject a dead partner exactly as they reject a
        // null one.
        if (other.linked != nullptr && !other.linked->dead) {
            continue;
        }
        loose.push_back(&other);
    }
    for (size_t i = 0; i < loose.size(); i++) {
        if (loose[i]->linked != nullptr) {
            continue;
        }
        for (size_t j = i + 1; j < loose.size(); j++) {
            if (loose[j]->linked != nullptr) {
                continue;
            }
            const float dx = loose[i]->position.x - loose[j]->position.x;
            const float dy = loose[i]->position.y - loose[j]->position.y;
            const float dz = loose[i]->position.z - loose[j]->position.z;
            if (dx * dx + dy * dy + dz * dz >= kRangeSquared) {
                continue;
            }
            loose[i]->linked = loose[j];
            loose[j]->linked = loose[i];
            break;
        }
    }
}

// LastFollowerId (ScriptVm.cs:273): the highest instance id of the live objects
// following the leader, or -1 when it has none. The C# takes the max over the
// followers' ids (so a follower with a negative id is the answer) and returns
// -1 only for the "no follower" case.
int32_t ScriptRuntime::lastFollowerId(const MdkObject* leader) const {
    bool found = false;
    int32_t maxId = 0;
    for (std::deque<MdkObject>::const_iterator it = objects_.begin(); it != objects_.end(); ++it) {
        const MdkObject& other = *it;
        if (other.dead || other.leader != leader) {
            continue;
        }
        if (!found || other.instance_id > maxId) {
            maxId = other.instance_id;
            found = true;
        }
    }
    return found ? maxId : -1;
}

// LastChainLink (ScriptVm.cs:288): the live same-arena follower with movement
// command 30 and the highest instance id, or null. The leader itself and the
// objects of another arena are skipped, as the C#'s IsArenaOther does.
MdkObject* ScriptRuntime::lastChainLink(const MdkObject* leader) {
    const int32_t kChainCommand = 30;
    MdkObject* best = nullptr;
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        MdkObject& other = *it;
        if (&other == leader || other.dead || other.arena != leader->arena) {
            continue;
        }
        if (other.leader != leader || other.move_command != kChainCommand) {
            continue;
        }
        if (best == nullptr || other.instance_id > best->instance_id) {
            best = &other;
        }
    }
    return best;
}

// C# MayCommand (0x440384): the receiver has no other leader of a higher priority,
// and obeys the sender's priority.
bool ScriptRuntime::mayCommand(const MdkObject* sender, const MdkObject* receiver) {
    const MdkObject* leader = receiver->leader;
    if (leader != nullptr && leader != sender && !leader->dead &&
        sender->priority <= leader->priority) {
        return false;
    }
    return receiver->obey_level >= sender->priority;
}

// ObjectMotion.FormationPosition: the leader's frame; note the off-diagonal signs
// are NOT the standard rotation — X uses (ox*s - oy*c) and Y uses (-oy*s - ox*c).
static Vec3 formationPosition(const MdkObject* leader, const Vec3& offset) {
    /* @vfp-exempt R6 — cold formation placement (command 1); libm sinf/cosf track
     * the C# oracle's MathF.SinCos bit-for-bit on this machine (probed).
     * measured n/a (cold). Verified 2026-10-23. */
    const float s = sinf(degreesToRadians(leader->yaw));
    const float c = cosf(degreesToRadians(leader->yaw));
    Vec3 out;
    out.x = leader->position.x + offset.x * s - offset.y * c;
    out.y = leader->position.y - offset.y * s - offset.x * c;
    out.z = leader->position.z + offset.z;
    return out;
}

// ObjectMotion.TurnTowards (ObjectMotion.cs:766): turn the yaw toward a point by at
// most TurnSpeed × Dt, returning the angle still to turn measured BEFORE the turn.
// The point-equals-position test is the C#'s, and it is not redundant: with it a
// zero-length offset keeps the object's own yaw instead of atan2's arbitrary answer.
float ScriptRuntime::turnTowards(MdkObject* obj, const Vec3& point) {
    float targetYaw = obj->yaw;
    if (point.x != obj->position.x || point.y != obj->position.y) {
        targetYaw = yawTo(obj, point);
    }
    const float diff = wrapAngle(targetYaw - obj->yaw);
    const float maxStep = kTurnSpeed * kTick;
    obj->yaw = wrap360(obj->yaw + clampf(diff, -maxStep, maxStep));
    return diff;
}

// ObjectMotion.FollowFormation (ObjectMotion.cs:929, movement command 1). The slot
// is the leader's frame, so a turning leader swings the formation around with him.
// The step test is LengthSquared against (MaxSpeed × Dt)², and inside it the object
// is PLACED on the slot: the push branch below can never land exactly.
void ScriptRuntime::followFormation(MdkObject* obj) {
    const float kTicks = 1.0f;  // ObjectMotion.Ticks: one tick, in ticks
    MdkObject* leader = obj->leader;
    if (leader == nullptr || leader->dead) {
        obj->move_command = 0;
        return;
    }
    const Vec3 destination = formationPosition(leader, obj->waypoint);
    obj->move_destination = destination;
    const float dx = destination.x - obj->position.x;
    const float dy = destination.y - obj->position.y;
    const float dz = destination.z - obj->position.z;
    const float maxStep = obj->max_speed * kTick;
    if (dx * dx + dy * dy + dz * dz < maxStep * maxStep) {
        obj->position = destination;
        obj->yaw = moveToward(obj->yaw, leader->yaw, kTurnSpeed * kTick);
        return;
    }
    turnTowards(obj, destination);
    // The push is a per-tick velocity, and ApplyVelocity below multiplies it by Dt;
    // the C# adds the unscaled heading × MaxSpeed here, so the step is one frame of
    // MaxSpeed, not MaxSpeed × Dt.
    const Vec3 direction = fromAngle(obj->yaw);
    obj->push.x += direction.x * obj->max_speed;
    obj->push.y += direction.y * obj->max_speed;
    // The vertical is three-way: a unit per tick while a whole unit away, else the
    // exact remaining gap so the follower lands on its slot's Z.
    if (obj->position.z <= destination.z - kTicks) {
        obj->push.z += kTicks;
    } else if (obj->position.z >= destination.z + kTicks) {
        obj->push.z -= kTicks;
    } else {
        obj->push.z = destination.z - obj->position.z;
    }
}

// ObjectMotion.Chase (ObjectMotion.cs:894, movement command 6, 0x45e448): the speed
// is in units per TICK, unlike every other movement command, and it is the object's
// own `Speed` field carried from tick to tick — the gain and the brake are what make
// it accelerate. Its clamp pair is the C#'s OWN Chase formula (ObjectMotion.cs:916-917),
// not Accelerate — different constants, no MaxSpeed/TurnSpeedFactor target and no Dt — so
// it is a second oracle function, not a fifth copy of the first, and must not be folded
// into accelerate(). The step is applied to the POSITION (no push, no friction), and the
// object only turns when the point is ahead of it or far away: a point close and
// behind is ignored until it comes round.
void ScriptRuntime::chase(MdkObject* obj, const Vec3& point) {
    const float kBehind = 90.0f;      // degrees: at or past this, do not turn
    const float kFar = 30.0f;         // units: this far to a side overrides kBehind
    const float kAhead = 22.5f;       // degrees: within this, gain instead of brake
    const float kGain = 0.0444444f / 6.0f;
    const float kMaxChase = 2.3333333f;
    const float kBrake = 0.05f;
    const float kMinChase = 0.3333333f;
    const float kClimb = 0.25f;       // fraction of the step spent closing Z
    const float kTicks = 1.0f;        // ObjectMotion.Ticks: one tick, in ticks
    float targetYaw = obj->yaw;
    if (point.x != obj->position.x || point.y != obj->position.y) {
        targetYaw = yawTo(obj, point);
    }
    const float offAngle = fabsf(wrapAngle(obj->yaw - targetYaw));
    float chaseSpeed = obj->speed;
    if (offAngle < kBehind || fabsf(obj->position.x - point.x) > kFar ||
        fabsf(obj->position.y - point.y) > kFar) {
        turnTowards(obj, point);
        // The gain's multiply order is the C#'s: (Ahead - offAngle) * Ticks * Gain.
        chaseSpeed = offAngle <= kAhead
            ? mathMin(chaseSpeed + (kAhead - offAngle) * kTicks * kGain, kMaxChase)
            : mathMax(chaseSpeed - kTicks * kBrake, kMinChase);
        obj->speed = chaseSpeed;
    }
    const float step = chaseSpeed * kTicks;
    const Vec3 direction = fromAngle(obj->yaw);
    obj->position.x += direction.x * step;
    obj->position.y += direction.y * step;
    obj->position.z = moveToward(obj->position.z, point.z, step * kClimb);
}

// C# NearTargetDestination (command 43's operand): forward/side off the target's
// heading, scaled by 1% of the 3D distance to it, landing on the target's Z with
// the object's height offset.
Vec3 ScriptRuntime::nearTargetDestination(const MdkObject* obj, float forward, float side) {
    /* @vfp-exempt R6 — cold command path; libm atan2f/sinf/cosf track the C#
     * oracle's MathF calls bit-for-bit on this machine (probed), with .NET's
     * left-associative conversions. measured n/a (cold). Verified 2026-10-23. */
    const float direction =
        radiansToDegrees(atan2f(targetPosition_.y - obj->position.y, targetPosition_.x - obj->position.x));
    const float s = sinf(degreesToRadians(direction));
    const float c = cosf(degreesToRadians(direction));
    const float dx = obj->position.x - targetPosition_.x;
    const float dy = obj->position.y - targetPosition_.y;
    const float dz = obj->position.z - targetPosition_.z;
    // The C# multiplies left-to-right: (rotated * 0.01f) * distance. Keeping
    // that order matters — the destination feeds PlanMove and the goldens.
    const float distance = sqrtf(dx * dx + dy * dy + dz * dz);
    Vec3 out;
    out.x = targetPosition_.x + (forward * c - side * s) * 0.01f * distance;
    out.y = targetPosition_.y + (forward * s + side * c) * 0.01f * distance;
    out.z = targetPosition_.z + obj->height_offset;
    return out;
}

// One receiver of command_objects (0x4405d0). True when it joined a formation.
bool ScriptRuntime::commandObject(MdkObject* sender, MdkObject* receiver, int32_t command,
                                 int32_t target, const Vec3& destination, int32_t formationIndex) {
    switch (command) {
        case 1: {  // join a formation around the sender: places alternate left/right
            const bool wide = iequalsType(receiver->type_name, "XE");
            const float spread = wide ? 4.0f : 1.0f;
            const float back = wide ? 2.5f : 1.0f;
            const float side = (formationIndex & 1) != 0 ? 1.0f : -1.0f;
            receiver->waypoint.x = side * (float)(formationIndex / 2 + 1) * 5.0f * spread;
            receiver->waypoint.y = back * -4.0f;
            receiver->waypoint.z = 8.0f;
            receiver->move_destination = formationPosition(sender, receiver->waypoint);
            receiver->leader = sender;
            receiver->move_command = 1;
            receiver->path = 0;
            return true;
        }
        case 7:  // goto: the receiver's script restarts at the target
            receiver->wait_time = 0.0f;
            receiver->restart = target;
            receiver->wait_resume = target;
            receiver->leader = sender;
            receiver->gosub_returns.clear();
            receiver->gosub_restarts.clear();
            receiver->level_timers[0] = 0.0f;
            receiver->command_target = target;
            break;
        case 0xFC: {  // gosub: the receiver's script gosubs the target from where it is
            if ((int32_t)receiver->gosub_returns.size() < ScriptVm::kGosubDepth) {
                receiver->gosub_returns.push_back(receiver->restart);
                receiver->gosub_restarts.push_back(receiver->restart);
                receiver->wait_time = 0.0f;
                receiver->restart = target;
                receiver->wait_resume = target;
                receiver->leader = sender;
                receiver->level_timers[receiver->gosub_returns.size()] = 0.0f;
            }
            break;
        }
        case 43:  // move to a destination
            if (receiver->arena == currentArena_) {
                receiver->move_destination = destination;
                receiver->leader = sender;
                receiver->move_command = 43;
                receiver->path = 0;
                planMove(receiver);
            }
            break;
        default:
            break;
    }
    return false;
}

// command_objects (0x440384): every matching live object of the sender's arena gets
// the command. Selectors: 2 all of a type, 3 everything, 4 the first of a type,
// 5 a type by instance id, 6 of a type in sight, 7 the sender's followers,
// 8 everything following the sender, 9 the linked object, 10 of a type beyond a Y.
void ScriptRuntime::commandObjects(MdkObject* sender, int32_t command, int32_t target,
                                  const Vec3& destination, int32_t selector,
                                  const std::string& type, float param, int32_t id) {
    const float kSightHeight = 8.0f;
    if (selector == 9) {
        if (sender->linked != nullptr && !sender->linked->dead) {
            commandObject(sender, sender->linked, command, target, destination, 0);
        }
        return;
    }
    int32_t formationIndex = 0;
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        MdkObject* receiver = &(*it);
        if (receiver == sender || receiver->dead || receiver->arena != sender->arena) {
            continue;
        }
        if (selector != 3 && selector != 8 && !iequalsType(receiver->type_name, type)) {
            continue;
        }
        if (!mayCommand(sender, receiver)) {
            continue;
        }
        if ((selector == 7 || selector == 8) && receiver->leader != sender) {
            continue;
        }
        if (selector == 5 && receiver->instance_id != id) {
            continue;
        }
        if (selector == 6) {
            const float dx = sender->position.x - receiver->position.x;
            const float dy = sender->position.y - receiver->position.y;
            const float dz = sender->position.z - receiver->position.z;
            Vec3 from{sender->position.x, sender->position.y, sender->position.z + kSightHeight};
            Vec3 to{receiver->position.x, receiver->position.y, receiver->position.z + kSightHeight};
            if (sqrtf(dx * dx + dy * dy + dz * dz) > param || !raycastClear(from, to)) {
                continue;
            }
        }
        if (selector == 10 && receiver->position.y < param) {
            continue;
        }
        if ((command == 7 && receiver->command_target == target) || receiver->health == 0) {
            continue;
        }
        if (commandObject(sender, receiver, command, target, destination, formationIndex)) {
            formationIndex++;
        }
        if (selector == 4) {
            return;
        }
    }
}

// script_runtime_ai.inc -- the movement AI: spots, waypoints, cover, plans, chains, getting stuck, projectiles, updateCommand.
// Part of script_runtime.cpp's single translation unit (see its header): pasted
// inside namespace mdk::scripts after its file-local constants and helpers,
// which it uses. No include guards, no includes of its own; the order is
// load-bearing.

// FindAdvanceSpot (ScriptRuntime.cs:2413, opcode 197): advance toward Kurt
// (command 197) via a type-5 spot of the object's arena, 5-400 units from the
// object, no farther from the object than Kurt is, and nearer to Kurt than the
// object is. The qualifying spots are collected in arena-record order and one is
// picked uniformly with a single RNG draw. Sets MoveCommand 197 on success and
// clears (zeroes) it otherwise, matching the C# one-for-one.
void ScriptRuntime::findAdvanceSpot(MdkObject* obj) {
    const int32_t kAdvanceCommand = 197;
    const uint32_t kDtiCover = 5;
    auto distance2d = [](const Vec3& a, const Vec3& b) {
        float dx = a.x - b.x;
        float dy = a.y - b.y;
        return sqrtf(dx * dx + dy * dy);
    };
    obj->move_command = 0;
    float toKurt = distance2d(obj->position, kurtPosition_);
    std::vector<Vec3> spots;
    const formats::Dti::ArenaEntry* entry = level_->entryNamed(obj->arena);
    if (entry != nullptr) {
        for (const formats::Dti::Record& record : entry->records) {
            if (record.type != kDtiCover) {
                continue;
            }
            Vec3 spot{record.position[0], record.position[1], record.position[2]};
            float dist = distance2d(obj->position, spot);
            if (dist < 5.0f || dist > 400.0f || dist > toKurt ||
                distance2d(spot, kurtPosition_) >= toKurt) {
                continue;
            }
            spots.push_back(spot);
        }
    }
    if (spots.empty()) {
        return;
    }
    const Vec3& destination = spots[rng_.next((int)spots.size())];
    obj->move_command = kAdvanceCommand;
    obj->move_destination = destination;
    obj->waypoint = destination;
    obj->contact_flags &= ~MdkObject::ContactStuck;
}

// PickWaypoint8 (ScriptRuntime.cs:2338, opcode 221): one of the arena's DTI
// type-8 waypoints, chosen by weighted random, becomes the destination (command
// 221). Every waypoint weighs at least 1 inside the mode's window and 0 outside
// it: 500 - distance (2D), plus 200 - distance-to-Kurt when Kurt is closer than
// 200, plus 250 when the object is nearer to Kurt than the waypoint is and the
// waypoint is farther from Kurt than from the object, less half the height
// difference. Mode 1 keeps the waypoints whose id is within 3 of the nearest one
// (nearest by 3D distance); any other mode keeps those 50-500 units away (2D).
// The single RNG draw (when any waypoint qualifies) matches the C# one-for-one.
// Command 221 is not a case in the oracle's UpdateCommand, so setting the
// destination is the whole effect -- there is no per-tick consumer to port.
void ScriptRuntime::pickWaypoint8(MdkObject* obj, int32_t mode) {
    const int32_t kPickCommand = 221;
    const uint32_t kDtiWaypoint = 8;
    auto distance2d = [](const Vec3& a, const Vec3& b) {
        float dx = a.x - b.x;
        float dy = a.y - b.y;
        return sqrtf(dx * dx + dy * dy);
    };
    auto distance3d = [](const Vec3& a, const Vec3& b) {
        float dx = a.x - b.x;
        float dy = a.y - b.y;
        float dz = a.z - b.z;
        return sqrtf(dx * dx + dy * dy + dz * dz);
    };
    obj->move_command = 0;

    // OfType(ArenaRecords(arena), DtiWaypoint): the arena's type-8 records in DTI
    // order. arenaRecords caches the list; a null list is the C#'s empty list.
    std::vector<const formats::Dti::Record*> records;
    const std::vector<formats::Dti::Record>* all = arenaRecords(obj->arena);
    if (all != nullptr) {
        for (size_t i = 0; i < all->size(); i++) {
            if ((*all)[i].type == kDtiWaypoint) {
                records.push_back(&(*all)[i]);
            }
        }
    }

    // Mode 1's nearest waypoint id, by 3D distance from the object.
    int32_t nearestId = 0;
    if (mode == 1) {
        float nearest = 999999.0f;
        for (size_t i = 0; i < records.size(); i++) {
            const formats::Dti::Record& r = *records[i];
            Vec3 spot{r.position[0], r.position[1], r.position[2]};
            float distance = distance3d(obj->position, spot);
            if (distance < nearest) {
                nearest = distance;
                nearestId = r.id;
            }
        }
    }

    // One weight per waypoint and their total. The distance from the object to
    // Kurt is loop-invariant (the C# recomputes it every iteration; same value).
    std::vector<int32_t> weights;
    weights.reserve(records.size());
    int32_t total = 0;
    const float objToKurt = distance2d(obj->position, kurtPosition_);
    for (size_t i = 0; i < records.size(); i++) {
        const formats::Dti::Record& r = *records[i];
        Vec3 spot{r.position[0], r.position[1], r.position[2]};
        float distance = distance2d(spot, obj->position);
        int32_t weight = 0;
        int32_t idDelta = r.id - nearestId;
        if (idDelta < 0) {
            idDelta = -idDelta;
        }
        const bool nearNearest = mode == 1 && idDelta <= 3;
        const bool inWindow = mode != 1 && distance >= 50.0f && distance <= 500.0f;
        if (nearNearest || inWindow) {
            float toKurt = distance2d(spot, kurtPosition_);
            // Round is ScriptMath.Round (MidpointRounding.AwayFromZero), which the
            // C# reaches unqualified via `using static ScriptMath`; C roundf matches
            // it exactly, MathF.Round's banker's form (roundToEven above) does not.
            weight = (int32_t)roundf(500.0f - distance);
            if (toKurt < 200.0f) {
                weight = (int32_t)roundf((float)weight + 200.0f - toKurt);
            }
            if (objToKurt < distance && toKurt > distance) {
                weight += 250;
            }
            weight = (int32_t)roundf((float)weight - fabsf(spot.z - obj->position.z) * 0.5f);
            weight = weight > 1 ? weight : 1;
        }
        weights.push_back(weight);
        total += weight;
    }

    if (total == 0) {
        return;
    }

    int32_t pick = rng_.next(total);
    for (size_t i = 0; i < records.size(); i++) {
        pick -= weights[i];
        if (pick >= 0) {
            continue;
        }
        const formats::Dti::Record& r = *records[i];
        obj->move_command = kPickCommand;
        obj->move_destination = Vec3{r.position[0], r.position[1], r.position[2]};
        obj->waypoint = obj->move_destination;
        obj->contact_flags &= ~MdkObject::ContactStuck;
        return;
    }
}

// FindCoverSpot (ScriptRuntime.cs:2447, opcode 46): a DTI cover spot (type 5) in
// the object's arena that Kurt cannot see (a blocked ray Kurt->spot) but the
// object can reach (a clear ray obj->spot), within [9,400] of the object, nearer
// to the object than Kurt is, and nearer to the object than Kurt. The nearest is
// chosen, biased nearer-first when three or more qualify. Sets MoveCommand 43 on
// success and clears it otherwise. The single RNG draw (when any spot qualifies)
// matches the C# one-for-one.
void ScriptRuntime::findCoverSpot(MdkObject* obj) {
    const int kCoverCommand = 43;
    const uint32_t kDtiCover = 5;
    const float kEyeHeight = 5.0f;
    static const int kNearerFirst[] = {0, 0, 0, 1, 1, 2};
    const int kNearerFirstCount = (int)(sizeof(kNearerFirst) / sizeof(kNearerFirst[0]));
    auto distance = [](const Vec3& a, const Vec3& b) {
        float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return sqrtf(dx * dx + dy * dy + dz * dz);
    };
    float toKurt = distance(obj->position, kurtPosition_);
    Vec3 kurtEye{kurtPosition_.x, kurtPosition_.y, kurtPosition_.z + kEyeHeight};
    Vec3 objEye{obj->position.x, obj->position.y, obj->position.z + kEyeHeight};
    std::vector<Vec3> spots;
    const formats::Dti::ArenaEntry* entry = level_->entryNamed(obj->arena);
    if (entry != nullptr) {
        for (const formats::Dti::Record& record : entry->records) {
            if (record.type != kDtiCover) {
                continue;
            }
            Vec3 spot{record.position[0], record.position[1], record.position[2]};
            float dist = distance(obj->position, spot);
            if (dist < 9.0f || dist > 400.0f || dist > toKurt ||
                distance(spot, kurtPosition_) >= toKurt) {
                continue;
            }
            Vec3 spotEye{spot.x, spot.y, spot.z + kEyeHeight};
            if (raycastClear(kurtEye, spotEye) || !raycastClear(objEye, spotEye)) {
                continue;
            }
            spots.push_back(spot);
        }
    }
    obj->move_command = 0;
    if (spots.empty()) {
        return;
    }
    std::sort(spots.begin(), spots.end(), [&](const Vec3& a, const Vec3& b) {
        return distance(obj->position, a) < distance(obj->position, b);
    });
    int choice = (int)spots.size() >= 3 ? kNearerFirst[rng_.next(kNearerFirstCount)]
                                        : rng_.next((int)spots.size());
    if (choice > (int)spots.size() - 1) {
        choice = (int)spots.size() - 1;
    }
    const Vec3& destination = spots[choice];
    obj->move_command = kCoverCommand;
    obj->move_destination = destination;
    obj->waypoint = destination;
    obj->path = 0;
}

// PlanMove (0x45a434): head straight for the destination when a ray clears it, else
// the nearest detour point around the block (both sides, widening from the midpoint).
void ScriptRuntime::planMove(MdkObject* obj) {
    const int kDetours = 11;
    obj->waypoint = obj->move_destination;
    obj->stuck_count = 0;
    obj->stuck_ticks = 0.0f;
    obj->stuck_moved = Vec3{0.0f, 0.0f, 0.0f};
    obj->contact_flags &= ~MdkObject::ContactStuck;
    Vec3 up{0.0f, 0.0f, kPlanHeight};
    Vec3 a{obj->position.x + up.x, obj->position.y + up.y, obj->position.z + up.z};
    Vec3 b{obj->move_destination.x + up.x, obj->move_destination.y + up.y,
           obj->move_destination.z + up.z};
    if (raycastClear(a, b)) {
        return;
    }
    Vec3 middle{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f};
    const float sides[2] = {-1.0f, 1.0f};
    for (int i = 0; i < kDetours; i++) {
        for (int s = 0; s < 2; s++) {
            // Detour(a,b,middle, 0.1*(i+1)*side): a point offset perpendicular to
            // the a->b line through the midpoint, by a fraction of the a->b distance:
            // raw radians, no degree conversion anywhere, as the C#'s
            // MathF.SinCos(MathF.Atan2(...)).
            /* @vfp-exempt R6 — the plan-move detour probe's perpendicular
             * direction; glibc atan2f/sinf/cosf match the C# oracle's MathF
             * calls bit-for-bit on this machine (probed); the LUT would move the
             * chosen waypoint. measured n/a (corpus empty). Verified 2026-10-26. */
            float rad = atan2f(b.y - a.y, b.x - a.x);
            float amount = 0.1f * (i + 1) * sides[s];
            float dist = sqrtf((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y) +
                               (b.z - a.z) * (b.z - a.z));
            Vec3 point{middle.x + sinf(rad) * amount * dist, middle.y + cosf(rad) * amount * dist,
                       middle.z};
            if (raycastClear(a, point) && raycastClear(point, b)) {
                obj->waypoint = point;
                return;
            }
        }
    }
}

// The replan on a stuck object (0x45a434): head for the destination again when the
// waypoint was a detour, otherwise probe detours from 3/4 of the way there, then
// from itself (0.1 to 1.9 of the distance, both sides).
static Vec3 detour(const Vec3& a, const Vec3& b, const Vec3& origin, float amount) {
    /* @vfp-exempt R6 — the stuck-replan detour's perpendicular direction; glibc
     * atan2f/sinf/cosf match the C# oracle's MathF.SinCos/Atan2 bit-for-bit on
     * this machine (probed), and the conversion is absent exactly as the C#'s;
     * the LUT would move the chosen waypoint. measured n/a (corpus empty).
     * Verified 2026-10-26. */
    const float rad = atan2f(b.y - a.y, b.x - a.x);
    const float distance = sqrtf((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y) +
                                 (b.z - a.z) * (b.z - a.z));
    Vec3 point;
    point.x = origin.x + sinf(rad) * amount * distance;
    point.y = origin.y + cosf(rad) * amount * distance;
    point.z = origin.z;
    return point;
}

// The replan on a stuck object (0x45a434): head for the destination again when the
// waypoint was a detour; otherwise probe detours from 3/4 of the way there, then
// from itself (0.1 to 1.9 of the distance, both sides).
void ScriptRuntime::replan(MdkObject* obj) {
    const int kDetours = 7;
    if (obj->waypoint.x != obj->move_destination.x || obj->waypoint.y != obj->move_destination.y ||
        obj->waypoint.z != obj->move_destination.z) {
        obj->waypoint = obj->move_destination;
        obj->stuck_ticks = 0.0f;
        obj->stuck_moved = Vec3{0.0f, 0.0f, 0.0f};
        obj->contact_flags &= ~MdkObject::ContactStuck;
        return;
    }
    static const float kPlanHeight = 8.0f;
    Vec3 up{0.0f, 0.0f, kPlanHeight};
    Vec3 a{obj->position.x + up.x, obj->position.y + up.y, obj->position.z + up.z};
    Vec3 b{obj->move_destination.x + up.x, obj->move_destination.y + up.y,
           obj->move_destination.z + up.z};
    // The C#'s two origins in order: 3/4 there, then itself.
    for (int originIndex = 0; originIndex < 2; originIndex++) {
        Vec3 origin;
        if (originIndex == 0) {
            origin.x = b.x * 0.75f + a.x * 0.25f;
            origin.y = b.y * 0.75f + a.y * 0.25f;
            origin.z = b.z * 0.75f + a.z * 0.25f;
        } else {
            origin = a;
        }
        const float sides[2] = {-1.0f, 1.0f};
        for (int i = 0; i < kDetours; i++) {
            for (int s = 0; s < 2; s++) {
                Vec3 point = detour(a, b, origin, (0.1f + 0.3f * i) * sides[s]);
                if (raycastClear(a, point) && raycastClear(point, b)) {
                    obj->waypoint = point;
                    return;
                }
            }
        }
    }
}

void ScriptRuntime::startMove(MdkObject* obj, int32_t command, const Vec3& destination) {
    obj->move_command = command;
    obj->move_destination = destination;
    obj->path = 0;
    planMove(obj);
}

// TurnAndJump (ObjectMotion.cs:426): turn towards the destination by at most
// rate degrees per second. Once facing it, enter movement command 229 and set
// the ballistic velocity. The arc clears the higher end by at least ten units
// and the lower end by at least thirty; its duration accounts for friction.
void ScriptRuntime::turnAndJump(MdkObject* obj, float rate) {
    const int32_t kJumpCommand = 229;
    const float kAboveHigher = 10.0f;
    const float kAboveLower = 30.0f;
    const float heading = yawTo(obj, obj->move_destination);
    const float diff = wrapAngle(heading - obj->yaw);
    if (fabsf(diff) > rate * kTick) {
        obj->yaw = wrap360(obj->yaw + signf(diff) * rate * kTick);
        return;
    }

    obj->yaw = heading;
    obj->move_command = kJumpCommand;
    const float z = obj->position.z;
    const float goal = obj->move_destination.z;
    const float top = goal < z ? mathMax(z + kAboveHigher, goal + kAboveLower)
                               : mathMax(goal + kAboveHigher, z + kAboveLower);
    const float gravity = obj->gravity;
    const float up = sqrtf((top - z) * 2.0f * gravity);
    const float flight = mathMax(up * up + (z - goal) * gravity * 2.0f, 0.0f);
    const float time = (up + sqrtf(flight)) / gravity;
    obj->parameter = time;
    obj->parameter_timer = 0.0f;

    const float dx = obj->move_destination.x - obj->position.x;
    const float dy = obj->move_destination.y - obj->position.y;
    const float distance = sqrtf(dx * dx + dy * dy);
    const float speed =
        (obj->friction * 0.5f * time * time + distance) / time;
    const float directionX = distance > 0.0f ? dx / distance : 0.0f;
    const float directionY = distance > 0.0f ? dy / distance : 0.0f;
    obj->velocity.x = directionX * speed;
    obj->velocity.y = directionY * speed;
    obj->velocity.z = up;
}

// Links (ObjectMotion.cs:503): the live followers of the leader, ordered by
// instance id. The C#'s insertion sort is stable, so equal ids keep the
// object-list order; this one does the same.
void ScriptRuntime::chainLinks(const MdkObject* leader) {
    chainLinks_.clear();
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        MdkObject& other = *it;
        if (other.dead || other.leader != leader) {
            continue;
        }
        size_t at = chainLinks_.size();
        while (at > 0 && chainLinks_[at - 1]->instance_id > other.instance_id) {
            at--;
        }
        chainLinks_.insert(chainLinks_.begin() + at, &other);
    }
}

// UpdateChain (ObjectMotion.cs:464, movement command 30): the chain links hang
// off their leader — each link's reference point 0 sits at the previous link's
// reference point 1, and the links turn 90 degrees apart. The link with instance
// id 0 drives the chain; a link whose chain is broken (a link before it is gone)
// lets go of the leader, exactly as the C#.
void ScriptRuntime::updateChain(MdkObject* link) {
    const float kLinkTurn = 90.0f;
    MdkObject* leader = link->leader;
    if (leader == nullptr || leader->dead) {
        link->leader = nullptr;
        link->move_command = 0;
        return;
    }
    chainLinks(leader);
    // Every link before this one must be there, with the instance id its place
    // in the chain says; a gap breaks the chain.
    for (int32_t i = 0; i < link->instance_id; i++) {
        if (i >= (int32_t)chainLinks_.size() ||
            chainLinks_[(size_t)i]->instance_id != i) {
            link->leader = nullptr;
            link->move_command = 0;
            return;
        }
    }
    // Only the first link moves the chain; the others keep their place.
    if (link->instance_id != 0) {
        return;
    }
    MdkObject* previous = leader;
    for (size_t i = 0; i < chainLinks_.size() &&
                        chainLinks_[i]->instance_id == (int32_t)i; i++) {
        MdkObject* current = chainLinks_[i];
        current->yaw = wrap360(leader->yaw + kLinkTurn * (float)i);
        // The C# sets the yaw first, so the link's own reference point 0 is the
        // one with the NEW yaw and the old position.
        Vec3 from = current->reference_point(0);
        Vec3 to = previous->reference_point(1);
        current->position.x += to.x - from.x;
        current->position.y += to.y - from.y;
        current->position.z += to.z - from.z;
        previous = current;
    }
}

// FollowAttachment (0x45b6c8, movement command 74): an attached object (a pickup's
// chute) hangs on its leader's reference point. With AttachPoints (0,0) the delta is
// the leader's reference point minus its own, so a chute with no reference points
// snaps to the leader's position and copies its yaw/pitch. The command clears when
// the leader is dead or gone (the C# also clears on Health==0).
void ScriptRuntime::followAttachment(MdkObject* obj) {
    MdkObject* other = obj->leader;
    if (other == nullptr || other->dead || other->health == 0) {
        obj->move_command = 0;
        return;
    }
    Vec3 a = obj->reference_point(obj->attach_point_a);
    Vec3 b = other->reference_point(obj->attach_point_b);
    obj->position.x += b.x - a.x;
    obj->position.y += b.y - a.y;
    obj->position.z += b.z - a.z;
    obj->yaw = other->yaw;
    obj->pitch = other->pitch;
}

// ObjectMotion.Accelerate (ObjectMotion.cs:780-790): the oracle's ONE speed step, and it
// has exactly three callers — MoveForward (:979), Walk (:819) and Fly (:840). The port had
// inlined it at all three, which is the shape review-20 F2 came from: a fidelity fix to the
// clamp lands in one copy and not the others and no gate sees it (batch 49 changed 2 of the
// 8 clamp sites that way, and review-20 caught it). One implementation per behaviour, as an
// ordinary file-scope free function — no template, no virtual, and the same shape as
// detectStuck below (§10 permits exactly this). NOTHING here is new arithmetic: the two
// comparisons, the two clamps and the multiply order are the lines that were already in all
// three arms, moved, so no hot-path rule changes state (R3's float comparison is the
// oracle's own, relocated rather than introduced). Pinned by
// AccelerateIsOneImplementationAcrossTheArms, which drives all three arms from identical
// state and requires each to land on the value .NET's own Accelerate prints (probe mode
// "accel"), and by the three per-arm NaN pins of batch 50.
static void accelerate(MdkObject* obj, float targetSpeed) {
    if (obj->speed > targetSpeed) {
        obj->speed = mathMax(obj->speed - obj->deceleration * ScriptRuntime::kTick,
                             targetSpeed);
    } else if (obj->speed < targetSpeed) {
        obj->speed = mathMin(obj->speed + obj->acceleration * ScriptRuntime::kTick,
                             targetSpeed);
    }
}

// ObjectMotion.MoveForward (command 88, set_move_x): accelerate to the max speed
// (or to a stop, parameter 0) and push along the yaw — and the pitch, when the
// object is not held on the ground by gravity.
void ScriptRuntime::moveForward(MdkObject* obj) {
    const float targetSpeed = obj->move_parameter != 0 ? obj->max_speed : 0.0f;
    accelerate(obj, targetSpeed);
    const Vec3 direction = fromAngle(obj->yaw);
    if ((obj->flags & MdkObject::FlagGravity) != 0) {
        obj->push.x += obj->speed * direction.x;
        obj->push.y += obj->speed * direction.y;
    } else {
        /* @vfp-exempt R6 — MoveForward's pitch push (per moving object per tick);
         * glibc sinf/cosf match the C# oracle's MathF.SinCos bit-for-bit on this
         * machine (probed), and the conversion is .NET's left-associative
         * DegreesToRadians; the fastSin/fastCos LUT's error is far larger and
         * would move those values. measured n/a (corpus empty).
         * Verified 2026-10-23. */
        const float s = sinf(degreesToRadians(obj->pitch));
        const float c = cosf(degreesToRadians(obj->pitch));
        obj->push.x += direction.x * c * obj->speed;
        obj->push.y += direction.y * c * obj->speed;
        obj->push.z += s * obj->speed;
    }
    if (obj->move_parameter == 0 && obj->speed == targetSpeed) {
        obj->move_command = 0;
    }
}

// ObjectMotion.DetectStuck (0x45a7d4, 0x45ae74): after Walk/Fly, collisions with
// enough speed and little motion over a 16-tick window (9 for a flying 197) mark
// the object stuck. A walking 43 counts ANY collision as stuck (the cover case).
static void detectStuck(MdkObject* obj, bool walking) {
    const float kStuckSpeed = 2.0f;
    const float kWindow = 16.0f;
    const float kAdvanceWindow = 9.0f;
    const float kCoverStuckTicks = 30.0f;
    const bool collided = (obj->contact_flags & MdkObject::ContactCollided) != 0;
    if (walking && obj->move_command == 43) {
        if (collided) {
            obj->contact_flags |= MdkObject::ContactStuck;
            obj->stuck_ticks = kCoverStuckTicks;
            return;
        }
        obj->stuck_count = 0;
        obj->stuck_ticks = 0.0f;
        obj->stuck_moved = Vec3{0.0f, 0.0f, 0.0f};
        return;
    }
    if (!collided || obj->speed <= kStuckSpeed) {
        obj->stuck_count = 0;
        obj->stuck_ticks = 0.0f;
        obj->stuck_moved = Vec3{0.0f, 0.0f, 0.0f};
        return;
    }
    const float window = walking && obj->move_command == 197 ? kAdvanceWindow : kWindow;
    if (obj->stuck_ticks < window) {
        // Ticks, not Dt: the window counts updates (ObjectMotion.Ticks = 1f).
        obj->stuck_moved.x += obj->position.x - obj->previous_position.x;
        obj->stuck_moved.y += obj->position.y - obj->previous_position.y;
        obj->stuck_moved.z += obj->position.z - obj->previous_position.z;
        obj->stuck_ticks += 1.0f;
        return;
    }
    if (fabsf(obj->stuck_moved.x) + fabsf(obj->stuck_moved.y) + fabsf(obj->stuck_moved.z) >=
        obj->speed * 0.5f) {
        obj->stuck_ticks = 0.0f;
        obj->stuck_moved = Vec3{0.0f, 0.0f, 0.0f};
        return;
    }
    obj->contact_flags |= MdkObject::ContactStuck;
}

// ObjectMotion.HandleStuck (0x45b6c8): a stuck object that isn't turning (under 3°
// this update) replans once, then move_to slides straight to the waypoint through
// everything and the others give up (if_move_idle_flag sees StuckCount). A 197
// gives up at once.
void ScriptRuntime::handleStuck(MdkObject* obj, float turned) {
    const float kTurning = 3.0f;
    const int32_t kReplans = 3;

    if ((obj->contact_flags & MdkObject::ContactStuck) == 0 || turned >= kTurning) {
        return;
    }
    obj->stuck_count++;
    if (obj->move_command == 197) {
        // ObjectMotion.Stop: an idled command keeps no stale state.
        obj->move_command = 0;
        obj->speed = 0.0f;
        obj->contact_flags &= ~MdkObject::ContactStuck;
    } else if (obj->stuck_count < kReplans) {
        replan(obj);
        obj->stuck_count++;
    } else if (obj->move_command == 78) {
        Vec3 toWaypoint;
        toWaypoint.x = obj->waypoint.x - obj->position.x;
        toWaypoint.y = obj->waypoint.y - obj->position.y;
        toWaypoint.z = obj->waypoint.z - obj->position.z;
        const float step = obj->speed * kTick;
        obj->position.x += clampf(toWaypoint.x, -step, step);
        obj->position.y += clampf(toWaypoint.y, -step, step);
        obj->position.z += clampf(toWaypoint.z, -step, step);
    } else {
        obj->move_command = 0;
        obj->speed = 0.0f;
    }
}

// FlyProjectile (ObjectMotion.cs:1016): straight along the yaw and pitch until the
// lifetime (parameter, set by the projectile's own script — opcode 106) runs out,
// the raycast hits the arena, or the grown Kurt box is entered. Touching Kurt (in
// his box grown by 1) records the contact; with damage, the C# hurts Kurt and
// kills the projectile — HurtKurt is Kurt's slice, so only the projectile's own
// death is observable here (the zero-damage touch records without removing).
void ScriptRuntime::flyProjectile(MdkObject* obj) {
    // ObjectMotion.FlyProjectile, in .NET evaluation order: from the old position.
    const Vec3 old = obj->position;
    // float.DegreesToRadians is left-associated (x * pi / 180f).
    /* @vfp-exempt R6 — the projectile's straight-line step (per projectile per
     * tick); glibc sinf/cosf match the C# oracle's MathF.SinCos bit-for-bit on
     * this machine (probed), and the conversions are .NET's left-associative
     * spellings; the LUT would move the step. measured n/a (corpus empty).
     * Verified 2026-10-24. */
    const float yawSin = sinf(obj->yaw * 3.14159265358979323846f / 180.0f);
    const float yawCos = cosf(obj->yaw * 3.14159265358979323846f / 180.0f);
    const float pitchSin = sinf(obj->pitch * 3.14159265358979323846f / 180.0f);
    const float pitchCos = cosf(obj->pitch * 3.14159265358979323846f / 180.0f);
    obj->position.x += yawCos * pitchCos * obj->speed * kTick;
    obj->position.y += yawSin * pitchCos * obj->speed * kTick;
    obj->position.z += pitchSin * obj->speed * kTick;
    obj->parameter -= kTick;
    if (obj->parameter <= 0.0f) {
        kill(obj);
        return;
    }
    Vec3 point;
    if (raycastPoint(old, obj->position, &point)) {
        obj->position = point;
        kill(obj);
        return;
    }
    const float kKurtMargin = 1.0f;
    formats::Box grown = kurtBox();
    grown.min.x -= kKurtMargin;
    grown.min.y -= kKurtMargin;
    grown.min.z -= kKurtMargin;
    grown.max.x += kKurtMargin;
    grown.max.y += kKurtMargin;
    grown.max.z += kKurtMargin;
    // SegmentEntry (ScriptMath.cs:89): where the segment enters the box (its start
    // when inside), or no entry. Like the C#, the hit writes the contact flag and
    // the entry point.
    float enter = 0.0f;
    float leave = 1.0f;
    bool miss = false;
    const float from[3] = {old.x, old.y, old.z};
    const float delta[3] = {obj->position.x - old.x, obj->position.y - old.y,
                            obj->position.z - old.z};
    const float boxMin[3] = {grown.min.x, grown.min.y, grown.min.z};
    const float boxMax[3] = {grown.max.x, grown.max.y, grown.max.z};
    for (int axis = 0; axis < 3 && !miss; axis++) {
        if (delta[axis] == 0.0f) {
            if (from[axis] < boxMin[axis] || from[axis] > boxMax[axis]) {
                miss = true;
            }
            continue;
        }
        const float t0 = (boxMin[axis] - from[axis]) / delta[axis];
        const float t1 = (boxMax[axis] - from[axis]) / delta[axis];
        // MathF.Max/MathF.Min (ScriptMath.cs:110-111), so a NaN box or a NaN step makes
        // BOTH enter and leave NaN — and NaN > NaN is false, so there is still no miss and
        // the entry point is from + d*NaN = NaN. fmaxf/fminf would return 0 and 1 and
        // report an entry at the segment's own start. Measured on .NET 10 (probe
        // `nanclamp`, rows SEGMENT nan_box and nan_delta: hit=True point=NaN,NaN,NaN).
        enter = mathMax(enter, mathMin(t0, t1));
        leave = mathMin(leave, mathMax(t0, t1));
        if (enter > leave) {
            miss = true;
        }
    }
    if (!miss) {
        obj->contact_flags |= MdkObject::ContactTouchedKurt;
        obj->position.x = old.x + delta[0] * enter;
        obj->position.y = old.y + delta[1] * enter;
        obj->position.z = old.z + delta[2] * enter;
        if (obj->touch_damage > 0) {
            // C#: runtime.HurtKurt(obj.TouchDamage) — Kurt's slice — then Kill.
            kill(obj);
        }
    } else {
        obj->contact_flags &= ~MdkObject::ContactTouchedKurt;
    }
}

// A numeric operand as an int (the C#'s ScriptMath.I), file-local because the
// runtime's only ScriptValue inputs are decoded lists (fire's origin).
static int32_t operandInt(const formats::ScriptValue& v) {
    if (v.kind == formats::ScriptValueKind::Int) {
        return v.intValue;
    }
    if (v.kind == formats::ScriptValueKind::Float) {
        return (int32_t)v.floatValue;
    }
    return 0;
}

// A string operand (the C#'s `origin[1] as string ?? ""`): "" for anything else.
static std::string operandString(const formats::ScriptValue& v) {
    if (v.kind == formats::ScriptValueKind::String) {
        return v.stringValue;
    }
    return std::string();
}

// Fire (ScriptRuntime.cs:2659): a projectile from a reference point of the object
// (origin [0, index]) or at the centre of one of its parts ([1, name]), flying
// along the object's yaw and pitch, under the projectile's script.
MdkObject* ScriptRuntime::fire(MdkObject* obj, const std::vector<formats::ScriptValue>& origin,
                               const std::string& bullet, int32_t script) {
    const int32_t kProjectileCommand = 61;
    // ProjectileFlags (ScriptRuntime.cs:25): FlagNotTarget | FlagNotSolid2 |
    // FlagChangesArena — the shot is not the chain gun's target, walks through,
    // and may change arena.
    static const int32_t kProjectileFlags = 0x80820;
    Vec3 start;
    if (!origin.empty() && operandInt(origin[0]) == 0) {
        const int32_t index = origin.size() > 1 ? operandInt(origin[1]) : 0;
        start = obj->reference_point(index);
    } else {
        const int32_t part =
            origin.size() > 1 ? obj->findPart(operandString(origin[1])) : -1;
        start = obj->partCenter(part);
    }
    MdkObject* shot = spawn(obj, bullet, start, obj->yaw, -1, 0, false);
    if (shot == nullptr) {
        return nullptr;
    }
    shot->move_command = kProjectileCommand;
    shot->flags |= kProjectileFlags;
    shot->pitch = obj->pitch;
    shot->restart = script;
    return shot;
}

// UpdateCommand (0x45a7d4): the movement-command dispatch for one object, run from
// motionVelocity before gravity and ApplyVelocity. The C#'s switch (ObjectMotion.cs:
// 331) has one arm per command and no default; every one of them is here now.
void ScriptRuntime::updateCommand(MdkObject* obj) {
    switch (obj->move_command) {
        case 1:  // FollowFormation: hold the slot command_objects gave us
            followFormation(obj);
            break;
        case 6: {  // Chase (move_to_target): fly at the runtime's target, 8 above it
            const float kChaseAbove = 8.0f;  // ObjectMotion.cs:328
            Vec3 point = targetPosition_;
            point.z += kChaseAbove;
            chase(obj, point);
            break;
        }
        case 74:
            followAttachment(obj);
            break;
        case 88:  // MoveForward (command of set_move_x): straight along the yaw
            moveForward(obj);
            break;
        case 30:  // UpdateChain: the chain links hang off their leader
            updateChain(obj);
            break;
        case 15: {  // alarm: ALERT every 32 ticks; if_alarm holds for ten ticks
            const int32_t kAlarmEvery = 31;
            const int32_t kAlarmTicks = 10;
            if (obj->arena != currentArena_) {
                break;
            }
            if ((tickCount_ & kAlarmEvery) == 0) {
                mixer_.playAt("ALERT", obj->position);
            }
            alarmTicks = kAlarmTicks;
            break;
        }
        case 61:  // FlyProjectile (fire): straight along the yaw and pitch
            flyProjectile(obj);
            break;
        case 229: {  // TurnAndJump's flight: stop after half the recorded flight time
            obj->parameter_timer += kTick;
            if (obj->parameter_timer >= obj->parameter * 0.5f &&
                (obj->contact_flags & MdkObject::ContactFloor) != 0) {
                obj->move_command = 0;
                obj->velocity = Vec3{};
            }
            break;
        }
        case 43:   // move to a destination (command_objects 43):
        case 197:  // the same walk/fly handling as move_to (ObjectMotion.cs:338)
        case 78: {
            const float yaw = obj->yaw;
            bool walking = (obj->flags & MdkObject::FlagGravity) != 0;
            if (walking) {
                // Walk (0x45a7d4): step to the waypoint on the ground, then the
                // destination. Arrival within 5 (Manhattan in X/Y) advances or stops.
                const float kArrived = 5.0f;
                bool final = vecEq(obj->waypoint, obj->move_destination);
                if (fabsf(obj->waypoint.x - obj->position.x) +
                        fabsf(obj->waypoint.y - obj->position.y) <
                    kArrived) {
                    if (final) {
                        obj->move_command = 0;
                        obj->speed = 0.0f;
                        obj->contact_flags &= ~MdkObject::ContactStuck;
                        return;
                    }
                    obj->waypoint = obj->move_destination;
                }
                float targetYaw = obj->yaw;
                if (obj->waypoint.x != obj->position.x || obj->waypoint.y != obj->position.y) {
                    targetYaw = yawTo(obj, obj->waypoint);
                }
                float diff = wrapAngle(targetYaw - obj->yaw);
                float maxStep = kTurnSpeed * kTick;
                obj->yaw = wrap360(obj->yaw + clampf(diff, -maxStep, maxStep));
                float targetSpeed = obj->max_speed * (fabsf(diff) > kSlowTurnAngle ? 0.5f : 1.0f);
                accelerate(obj, targetSpeed);
                Vec3 direction = fromAngle(obj->yaw);
                obj->push.x += obj->speed * direction.x;
                obj->push.y += obj->speed * direction.y;
            } else {
                // Fly (0x45ae74): turn toward the waypoint (unless NoTurning), then
                // step along the heading, climbing by the vertical fraction of the
                // remaining distance.
                const float kTurnFrom = 3.0f;
                const float kArrivedAcross = 4.0f;
                const float kArrivedUp = 3.0f;
                bool final = vecEq(obj->waypoint, obj->move_destination);
                Vec3 toW{obj->waypoint.x - obj->position.x, obj->waypoint.y - obj->position.y,
                         obj->waypoint.z - obj->position.z};
                float diff = 0.0f;
                bool noTurning = (obj->flags & MdkObject::FlagNoTurning) != 0;
                if (!noTurning && fabsf(toW.x) + fabsf(toW.y) > kTurnFrom) {
                    float targetYaw = obj->yaw;
                    if (obj->waypoint.x != obj->position.x || obj->waypoint.y != obj->position.y) {
                        targetYaw = yawTo(obj, obj->waypoint);
                    }
                    diff = wrapAngle(targetYaw - obj->yaw);
                    float maxStep = kTurnSpeed * kTick;
                    obj->yaw = wrap360(obj->yaw + clampf(diff, -maxStep, maxStep));
                }
                float targetSpeed = obj->max_speed * (fabsf(diff) > kSlowTurnAngle ? 0.5f : 1.0f);
                accelerate(obj, targetSpeed);
                if (!noTurning) {
                    float horizontal = sqrtf(toW.x * toW.x + toW.y * toW.y);
                    float vertical = fabsf(toW.z);
                    float climb = horizontal + vertical > 0.0f ? vertical / (horizontal + vertical)
                                                               : 0.0f;
                    Vec3 direction = fromAngle(obj->yaw);
                    obj->push.x += direction.x * obj->speed * (1.0f - climb);
                    obj->push.y += direction.y * obj->speed * (1.0f - climb);
                    obj->push.z += obj->speed * climb * signf(toW.z);
                    if (horizontal < kArrivedAcross && vertical < kArrivedUp) {
                        if (final) {
                            obj->move_command = 0;
                            obj->speed = 0.0f;
                            obj->contact_flags &= ~MdkObject::ContactStuck;
                        } else {
                            obj->waypoint = obj->move_destination;
                        }
                    }
                } else {
                    float distance = fabsf(toW.x) + fabsf(toW.y) + fabsf(toW.z);
                    if (distance < 1.0f) {
                        if (final) {
                            obj->position = obj->waypoint;
                            obj->move_command = 0;
                            obj->speed = 0.0f;
                            obj->contact_flags &= ~MdkObject::ContactStuck;
                            return;
                        }
                        obj->waypoint = obj->move_destination;
                    }
                    if (distance > 0.0f) {
                        float scale = obj->speed * kTick / distance;
                        float stepX = toW.x * scale;
                        float stepY = toW.y * scale;
                        float stepZ = toW.z * scale;
                        if (fabsf(stepX) > fabsf(toW.x)) {
                            stepX = toW.x;
                        }
                        if (fabsf(stepY) > fabsf(toW.y)) {
                            stepY = toW.y;
                        }
                        if (fabsf(stepZ) > fabsf(toW.z)) {
                            stepZ = toW.z;
                        }
                        obj->push.x += stepX / kTick;
                        obj->push.y += stepY / kTick;
                        obj->push.z += stepZ / kTick;
                    }
                }
            }
            // DetectStuck then HandleStuck(|yaw - yawAtEntry|) (ObjectMotion.cs:354-358).
            // The guard is the oracle's, not a port choice. Stop() sets MoveCommand = 0,
            // Speed = 0 and clears ContactStuck — StuckCount is NOT touched
            // (ObjectMotion.cs:792-797) — and on the tick it runs the C# skips BOTH calls.
            // A Walk arrival gets that skip from Walk's own early return (:811-812), which
            // is why the walk case cannot see this guard; it is load-bearing on the FLY
            // arrival, where Fly returns to the switch at :860 with MoveCommand already 0.
            // Without the guard DetectStuck's !collided || speed <= 2 branch would run
            // ResetStuck and zero a stuck_count an earlier replan left, and opcode 231
            // (if_move_idle_flag) reads move_command == 0 && stuck_count != 0, so it would
            // branch the other way on an object that arrived after a replan. Pinned by
            // ArrivalKeepsTheStuckCountTheOracleKeeps (both directions).
            if (obj->move_command != 0) {
                detectStuck(obj, walking);
                handleStuck(obj, fabsf(wrapAngle(obj->yaw - yaw)));
            }
            break;
        }
        default:
            break;
    }
}

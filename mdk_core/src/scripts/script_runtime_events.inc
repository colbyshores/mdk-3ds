// script_runtime_events.inc -- special events, cutscenes, the level's end, the camera track, gun aim, Kurt's pushes, sounds, the sweep.
// Part of script_runtime.cpp's single translation unit (see its header): pasted
// inside namespace mdk::scripts after its file-local constants and helpers,
// which it uses. No include guards, no includes of its own; the order is
// load-bearing.

// C# SpecialEvent (ScriptRuntime.cs:1007-1076, opcode 131 special_event, 0x4456d2): 50 and
// below end the level; above 50 are the cutscenes (0x477cf4). Every retail call above 50 is
// an ARENA CONTROLLER's (L5 MUSE_5: 51-55, 61, 81; L7 DANT_10: 92, 93; L8 GUNT_10: 91, 92),
// so `obj` -- the target of 51, 81, 91 and 93 -- is that controller. A value the C#'s switch
// has no case for (54, the L5 script's own) is the C#'s no-op, and handled.
bool ScriptRuntime::specialEvent(MdkObject* obj, int32_t value) {
    if (value <= kLastEndEvent) {
        endTheLevel();
        return true;
    }
    switch (value) {
        case 51: {
            // Kurt strikes (0x4779e0): X_STRIKD full screen (0x4398f0), then he's put at the
            // object, which moves 4 units along y.
            // The scene is its own object: the frontend waits on it (package 14).
            playStrikeScene(StrikeScene::Kurt, StrikeScene::WithPilot);
            if (player_ != nullptr) {
                player_->teleport(obj->position, obj->yaw);
            }
            obj->position.y += 4.0f;
            startCutscene(kCutsceneStrike, obj);
            cameraDistance = 10.0f;
            // obj.Position - new Vector3(10f, 4f, -8f), component by component.
            cameraPosition = Vec3{obj->position.x - 10.0f, obj->position.y - 4.0f,
                                  obj->position.z - -8.0f};
            cameraPitch = 0.0f;
            cameraYaw = 90.0f - obj->yaw;
            return true;
        }
        case 52: {
            MdkObject* dog = findObjectNamed("XBN");
            if (dog != nullptr) {
                startCutscene(kCutsceneDog, dog);
                cameraMode = 0;
            }
            return true;
        }
        case 53:
        case 92:
            endCutscene();
            return true;
        case 55:
            cameraMode = 2;
            return true;
        case 61: {
            MdkObject* gunter = findObjectNamed("XGUNTAM");
            if (gunter != nullptr) {
                startCutscene(kCutsceneAll, gunter);
                cameraMode = 11;
                // FromAngle(gunter.Yaw) * 45f, then Position + (heading.X, heading.Y, 3f).
                const Vec3 facing = fromAngle(gunter->yaw);
                const float hx = facing.x * 45.0f;
                const float hy = facing.y * 45.0f;
                cameraPosition = Vec3{gunter->position.x + hx, gunter->position.y + hy,
                                      gunter->position.z + 3.0f};
                cameraPitch = -20.0f;
                cameraYaw = 270.0f - gunter->yaw;
                cameraDistance = 45.0f;
            }
            return true;
        }
        case 81:
            // The end of the game: MISC/FLIC/MDKEND.FLC and MDKBZK.MVE (game state 8). The
            // movies and the credits are the frontends' (GameFinished, Flow/Game.cs:458).
            startCutscene(kCutsceneEnd, obj);
            gameFinished_ = true;
            return true;
        case 91:
            startCutscene(kCutsceneAllFlagged, obj);
            cameraMode = 12;
            cameraPosition = Vec3{-121.0f, 3347.0f, -350.0f};
            return true;
        case 93:
            startCutscene(kCutsceneBoss, obj);
            cameraMode = 12;
            cameraPosition = Vec3{1158.0f, 5006.0f, 315.0f};
            return true;
        default:
            return true;
    }
}

// C# FindObjectNamed (ScriptRuntime.cs:1338): Objects.FirstOrDefault(o => !o.Dead &&
// o.TypeName == type). A removed object is dead (removeObject sets it first), so the deque's
// order with the dead skipped is the C# list's.
MdkObject* ScriptRuntime::findObjectNamed(const std::string& type) {
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        if (!it->dead && it->type_name == type) {
            return &(*it);
        }
    }
    return nullptr;
}

// C# StartCutscene (ScriptRuntime.cs:1341-1347): Kurt stops firing (0x4779b0) and stands
// still; the camera looks at the target from the next tick (cutsceneTick).
void ScriptRuntime::startCutscene(int32_t state, MdkObject* target) {
    cutscene = state;
    cutsceneTarget = target;
    // Kurt.StopFiring, as endTheLevel spells it: with no Player bound there is no gun, voice
    // or muzzle to stop, only the combat half's flag.
    if (player_ != nullptr) {
        player_->stopFiring();
        player_->frozen = true;
    } else {
        kurt().setFiring(false);
        kurt().clearMuzzle();
    }
}

// C# EndCutscene (ScriptRuntime.cs:1349-1359): every object is drawn again, and Kurt moves
// and shows. The shot (cameraMode ..) is left as it was, as in the C#.
void ScriptRuntime::endCutscene() {
    cutscene = 0;
    cutsceneTarget = nullptr;
    if (player_ != nullptr) {
        player_->frozen = false;
        player_->visible = true;
    }
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        it->visible = true;
    }
}

// C# CutsceneTick (ScriptRuntime.cs:1363-1410, 0x478704): a tick during a cutscene. Only
// Kurt's arena's objects are considered; below kCutsceneAll only the cutscene types run
// (BOLT and BIGBOLT unseen); from it on the objects that are not pickups or Kurt's items run
// and show (kCutsceneAllFlagged runs those too, unseen); the end of the game runs and shows
// nothing. Then the camera takes its shot of the target, unless it died.
void ScriptRuntime::cutsceneTick() {
    // C# CutsceneTypes (0x47868c).
    static const char* const kCutsceneTypes[] = {"XM5_FLAP", "XBN",      "BOLT",
                                                 "BIGBOLT",  "SW_SBONE", "SW_SEAL"};
    if (player_ != nullptr) {
        player_->visible = cutscene == kCutsceneStrike || cutscene == kCutsceneEnd;
    }
    // ListCopy<MdkObject>.Of(Objects): what this tick spawns waits for the next.
    std::vector<MdkObject*> snapshot;
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        snapshot.push_back(&(*it));
    }
    for (size_t i = 0; i < snapshot.size(); i++) {
        MdkObject* obj = snapshot[i];
        if (obj->dead || obj->arena != currentArena_) {
            continue;
        }
        const bool flagged = (obj->flags & kCutsceneHiddenFlags) != 0 || obj->thrown_kind > 0;
        bool runs;
        bool shown;
        if (cutscene == kCutsceneEnd) {
            runs = false;
            shown = false;
        } else if (cutscene < kCutsceneAll) {
            runs = false;
            for (size_t t = 0; t < sizeof(kCutsceneTypes) / sizeof(kCutsceneTypes[0]); t++) {
                if (obj->type_name == kCutsceneTypes[t]) {
                    runs = true;
                    break;
                }
            }
            shown = runs && obj->type_name != "BOLT" && obj->type_name != "BIGBOLT";
        } else {
            runs = cutscene == kCutsceneAllFlagged || !flagged;
            shown = !flagged;
        }
        obj->visible = shown;
        if (!runs) {
            continue;
        }
        vm_.run(obj);
        if (!obj->dead) {
            motionUpdate(obj);
        }
    }
    // A script that ran above may have ended the cutscene (53, 92): no shot then.
    if (cutscene != 0 && cutsceneTarget != nullptr && !cutsceneTarget->dead) {
        updateCutsceneCamera();
    }
}

// C# UpdateCutsceneCamera (ScriptRuntime.cs:1415-1492, 0x477d94). Every shot but 11 looks at
// the target, 3 units above its origin. Shot 0 starts from (423, 85) and switches to 1 once
// the target is 12 units away; 1 and 2 orbit behind the target (12 or 25 units), easing in
// for kCameraBlendTicks; 12 looks from the stored position; 11 holds it all.
//
// Gameplay-exact arithmetic, operation for operation (the golden pins every bit): the C#'s
// Vector3 operators are per component and unfused, Vector3.Distance is Sqrt(Dot(d, d)) with
// Dot summing x, y, then z, and heading()/fromAngle()/atan2f are the libm calls that match
// .NET's MathF bit for bit here (see heading's note). It runs once per tick, not per object,
// so it is not a hot path (AGENTS.md section 2).
void ScriptRuntime::updateCutsceneCamera() {
    const float kHalfTurn = 180.0f;
    const float kFullTurn = 360.0f;
    const float kMotionTicks = 1.0f;  // C# ObjectMotion.Ticks
    const Vec3 target = cutsceneTarget->position;
    Vec3 point = cameraPosition;
    float yaw = cameraYaw;
    float distance = cameraDistance;
    switch (cameraMode) {
        case 0: {
            point = Vec3{423.0f, 85.0f, mathMax(target.z - 25.0f, -2260.0f)};
            yaw = 90.0f - heading(point, target);
            const float dx = point.x - target.x;
            const float dy = point.y - target.y;
            const float dz = point.z - target.z;
            distance = sqrtf((dx * dx + dy * dy) + dz * dz);
            if (distance >= 12.0f) {
                cameraMode = 1;
            }
            cameraTimer = kCameraBlendTicks;
            break;
        }
        case 1:
        case 2: {
            distance = (cameraMode == 1 ? 12.0f : 25.0f) * 0.1f + cameraDistance * 0.9f;
            const Vec3 facing = fromAngle(cutsceneTarget->yaw + 150.0f);
            const float aroundX = facing.x * distance;
            const float aroundY = facing.y * distance;
            point = Vec3{target.x + aroundX, target.y + aroundY, -2258.0f};
            if (cameraMode == 2) {
                point.z = mathMin(cameraPosition.z + kMotionTicks, -2246.0f);
            }
            if (cameraTimer > 0) {
                point = Vec3{point.x * 0.2f + cameraPosition.x * 0.8f,
                             point.y * 0.2f + cameraPosition.y * 0.8f,
                             point.z * 0.2f + cameraPosition.z * 0.8f};
            }
            yaw = 120.0f - cutsceneTarget->yaw;
            break;
        }
        case 12: {
            yaw = 90.0f - heading(point, target);
            const float dx = point.x - target.x;
            const float dy = point.y - target.y;
            const float dz = point.z - target.z;
            distance = sqrtf((dx * dx + dy * dy) + dz * dz);
            cameraTimer = kCameraBlendTicks;
            break;
        }
        default:
            break;
    }

    float pitch = cameraPitch;
    if (cameraMode != 11) {
        /* @vfp-exempt R6 — the cutscene shot's pitch, once a tick; glibc atan2f matches the
         * C# oracle's MathF.Atan2 bit-for-bit on this machine (as heading's pin records),
         * and golden/L5_cut_*.txt compare the pitch's bits, which the fastAtan2 LUT would
         * move. measured n/a (corpus empty). Verified 2026-11-20. */
        pitch = -radiansToDegrees(atan2f(target.z + 3.0f - point.z, distance));
        if (pitch < -kHalfTurn) {
            pitch += kFullTurn;
        }
    }

    if (cameraMode == 1 || cameraMode == 2) {
        if (cameraTimer > 0) {
            pitch = pitch * 0.2f + cameraPitch * 0.8f;
            yaw = yaw * 0.2f + cameraYaw * 0.8f;
            cameraTimer--;
        } else {
            pitch = pitch * 0.7f + cameraPitch * 0.3f;
            yaw = yaw * 0.3f + cameraYaw * 0.7f;
        }
    }

    cameraDistance = distance;
    cameraPitch = pitch;
    cameraYaw = yaw;
    if (cutscene != kCutsceneBoss) {
        cameraPosition = point;
    }
    cameraPoint = point;
}

// C# EndTheLevel (ScriptRuntime.cs:1496-1511): once (LevelOver, 0x573b60), the town flags
// are kept for the debriefing, Kurt stops firing, NUKE and TORNADO play, and the arena
// breaks up around him as he rises (EndLevel.Start, then its StartShake).
void ScriptRuntime::endTheLevel() {
    if (levelOver) {
        return;
    }
    levelOver = true;
    stats.townFlags = globalFlags;
    // Kurt.StopFiring. With no Player bound there is no gun, no voice and no muzzle to
    // stop, so the combat half's flag is all there is.
    if (player_ != nullptr) {
        player_->stopFiring();
    } else {
        kurt().setFiring(false);
        kurt().clearMuzzle();
    }
    mixer_.play("NUKE");
    mixer_.play("TORNADO");
    // _sprites.GetAnimation(EndLevel.Takeoff).FrameCount: K_TAKEOF's frames (the C#
    // throws on an archive without it; here the pose is K_FLOATC from the first tick).
    int32_t takeoffFrames = 0;
    formats::SpriteAnimation takeoff;
    if (sprites_ != nullptr && sprites_->has(EndLevel::kTakeoff) &&
        sprites_->animation(EndLevel::kTakeoff, &takeoff)) {
        takeoffFrames = takeoff.frameCount();
    }
    endLevel_.reset(new EndLevel(takeoffFrames));
    endLevel_->start(level_, currentArena_, player_, &rng_);
    raiseShake(EndLevel::kStartShake);
}

// CameraTrack (ScriptRuntime.cs:1188-1216, opcode 203): calculate the pitch
// the follow camera should ease towards while Kurt faces a tall object. The
// camera is presentation, so its two atan2 calls use the shared fastAtan2 LUT
// on both targets (R6); the table's angular error is far below one pixel of
// camera pitch.
void ScriptRuntime::cameraTrack(MdkObject* obj, int32_t mode, float height) {
    const float maxUp = -30.0f;
    const float halfTurn = 180.0f;
    const float fullTurn = 360.0f;

    const float heading = radiansToDegrees(
        fastAtan2(obj->position.y - kurtPosition_.y, obj->position.x - kurtPosition_.x));
    float off = wrap360(kurtYaw_ - heading);
    if (off > halfTurn) {
        off = fullTurn - off;
    }

    float top = obj->position.z + height * obj->scale;
    if (mode == 0) {
        float boundsTop = obj->position.z;
        if (obj->model != nullptr) {
            boundsTop += obj->model->bounds.max.z * obj->scale;
        }
        top = 0.3f * obj->position.z + 0.7f * boundsTop;
    }

    const float rest = level_->arenaPitch(currentArena_);
    float target = rest;
    if (off <= 90.0f && top >= kurtPosition_.z) {
        const float dx = obj->position.x - kurtPosition_.x;
        const float dy = obj->position.y - kurtPosition_.y;
        const float distance = sqrtf(dx * dx + dy * dy);
        const float angle = radiansToDegrees(fastAtan2(top - kurtPosition_.z, distance));
        target = clampf(-angle * (120.0f - off) / 120.0f, maxUp, rest);
    }

    cameraTrackPitch = target;
    cameraTrackTicks = 2;
}

void ScriptRuntime::placeNearKurt(MdkObject* obj, float xMin, float xMax, float yLimit) {
    const float kLead = 2.67333f;
    const int32_t kTries = 100;
    const float kSpacing = 12.0f;

    // PlaceNearKurt (ScriptRuntime.cs:1139-1186). The first draw chooses the
    // branch; the random branch alone consumes a second draw. The operation
    // order and float literals are kept as in the oracle.
    const int32_t choice = rng_.next(100);
    float x;
    if (choice < 25 || kurtPosition_.x < xMin || kurtPosition_.x > xMax ||
        kurtPosition_.y > yLimit) {
        x = xMin + (xMax - xMin) * rng_.next(10000) * 0.0001f;
    } else if (choice < 50) {
        x = kurtPosition_.x;
    } else {
        x = kurtPosition_.x + kurtVelocity.x * kLead;
    }

    if (x < xMin) {
        x += xMax - xMin;
    }

    for (int32_t attempt = 0; attempt < kTries; attempt++) {
        bool moved = false;
        for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
            MdkObject& other = *it;
            // The C# iterates its live Objects list. The C++ deque keeps
            // removed objects in stable slots, but remove() sets dead on
            // them, so the dead test IS the list membership.
            if (&other == obj || other.dead || other.arena != obj->arena ||
                other.position.y != obj->position.y ||
                fabsf(x - other.position.x) >= kSpacing - 0.5f) {
                continue;
            }

            x = other.position.x - kSpacing;
            moved = true;
        }

        if (!moved) {
            break;
        }
    }

    obj->position.x = x;
}

// GunAim (ScriptRuntime.cs:1084-1134, opcode 219): the shooting galleries'
// guns (level 6) only fire along -y (270 degrees +/- 3) and lead Kurt by the
// 1.67 s their shot takes (sideways only). Faster sideways movement makes
// them likelier to fire at him; otherwise they aim at a raised target (the
// objects within 2 of y and 3 of z in their cone), then at Kurt anyway, and
// else face 270 degrees and fire 29% of the time. Sets the yaw; returns
// whether to fire.
bool ScriptRuntime::gunAim(MdkObject* obj, float y, float z) {
    const float kLead = 1.67333f;
    const float kDown = 270.0f;
    const float kCone = 3.0f;
    const int32_t kMaxTargets = 4;

    // The C#'s lead point: Kurt's X advanced by the observed sideways
    // velocity over the shot's flight time, at Kurt's Y.
    Vec3 lead{kurtPosition_.x + kurtVelocity.x * kLead, kurtPosition_.y, 0.0f};
    // The C#'s `obj.YawTo(lead) + Rng.Next(500) * 0.001f - 0.25f`: the aim
    // jitter of up to a quarter of a degree. The draw order matters, so the
    // multiplication stays where the oracle has it.
    float angle = yawTo(obj, lead) + rng_.next(500) * 0.001f - 0.25f;
    const bool aimed = fabsf(angle - kDown) <= kCone;

    // ScriptMath.Round is MathF.Round(AwayFromZero), which C's roundf is
    // (unlike the banker's round the animation clock uses).
    if (aimed && rng_.next(70) < (int32_t)roundf(fabsf(kurtVelocity.x)) + 10) {
        obj->yaw = angle;
        return true;
    }

    // The raised targets: the live objects of the gun's arena within 2 in Y
    // and 3 in Z of the target row whose bearing from the gun is inside the
    // cone. The C# collects at most four and picks one of them at random.
    float angles[kMaxTargets];
    int32_t count = 0;
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        MdkObject& other = *it;
        // The C# iterates its live Objects list. The C++ deque keeps removed
        // objects in stable slots, but remove() sets dead on them, so the
        // dead test IS the list membership.
        if (other.dead || other.arena != obj->arena ||
            fabsf(other.position.y - y) > 2.0f || fabsf(other.position.z - z) > 3.0f) {
            continue;
        }

        const float otherAngle = yawTo(obj, other.position);
        if (fabsf(otherAngle - kDown) > kCone) {
            continue;
        }

        angles[count] = otherAngle;
        count++;
        if (count == kMaxTargets) {
            break;
        }
    }

    if (count != 0) {
        obj->yaw = angles[rng_.next(count)];
        return true;
    }

    if (aimed) {
        obj->yaw = angle;
        return true;
    }

    obj->yaw = kDown;
    return rng_.next(100) > 70;
}

// A numeric operand as a float (the C#'s ScriptMath.F).
static float operandFloat(const formats::ScriptValue& v) {
    if (v.kind == formats::ScriptValueKind::Float) {
        return v.floatValue;
    }
    if (v.kind == formats::ScriptValueKind::Int) {
        return (float)v.intValue;
    }
    return 0.0f;
}

// An operand by index, or the null one past the end. The C# would throw
// IndexOutOfRangeException on a short list; the port reads a missing operand as the
// null one, which is what ScriptMath.F and ScriptMath.I already answer 0 for, so a
// malformed script stops the opcode instead of the level.
static const formats::ScriptValue& operandAt(const std::vector<formats::ScriptValue>& args,
                                             size_t index) {
    static const formats::ScriptValue kNull;
    return index < args.size() ? args[index] : kNull;
}

// push_kurt (opcode 248, ScriptRuntime.cs:2826-2852, dispatched at ScriptVm.cs:1600):
// knock Kurt down and shove him. It is the ONLY writer of a non-zero knock-down push in
// the whole oracle. The operand list comes in whole because its arity depends on args[0]:
//   [0, a, b, up]  the push is (a, b) turned into the world by the OBJECT's yaw, and
//                  the lift comes from args[3];
//   [n, a, up]     the push points straight away from the object in XY at a u/s, and
//                  the lift is args[2].
void ScriptRuntime::pushKurt(MdkObject* obj, const std::vector<formats::ScriptValue>& args) {
    // The guard, in the C#'s order: a Kurt already on his back is not knocked down
    // again, and a corpse is not shoved. Kurt.Knocked is Knocked or GetUp
    // (Kurt.Combat.cs:102), so the whole animation is the refused window.
    if (kurt().knocked() || kurt().health() == 0) {
        return;
    }
    // The C# cannot reach here without a Kurt: its constructor requires one
    // (ScriptRuntime.cs:168). With no Player bound there is no entity to shove, and
    // writing only the combat half would invent a Kurt who is knocked but cannot move.
    if (player_ == nullptr) {
        return;
    }

    const Vec3 origin = obj != nullptr ? obj->position : Vec3{};
    Vec2 push;
    float up = 0.0f;
    if (operandInt(operandAt(args, 0)) == 0) {
        // fromAngle supplies (cos, sin); preserve the C#'s asymmetric expression.
        const Vec3 heading = fromAngle(obj != nullptr ? obj->yaw : 0.0f);
        const float c = heading.x;
        const float s = heading.y;
        const float a = operandFloat(operandAt(args, 1));
        const float b = operandFloat(operandAt(args, 2));
        push.x = -a * c - b * s;
        push.y = -b * c - a * s;
        up = operandFloat(operandAt(args, 3));
    } else {
        const float a = operandFloat(operandAt(args, 1));
        // C# reads its per-tick KurtPosition snapshot, not live Kurt.Feet.
        const float awayX = kurtPosition_.x - origin.x;
        const float awayY = kurtPosition_.y - origin.y;
        if (awayX != 0.0f || awayY != 0.0f) {
            const float length = sqrtf(awayX * awayX + awayY * awayY);
            // System.Numerics.Vector2.Normalize divides each component before the
            // following multiplication by a; keep that rounding order.
            push.x = awayX / length * a;
            push.y = awayY / length * a;
        } else {
            // C# fallback when the object and Kurt share an XY position.
            push.x = a;
            push.y = 0.0f;
        }
        up = operandFloat(operandAt(args, 2));
    }

    // C# scales by Tick * TicksPerSecond (1/30 * 30), retained in the same order.
    push.x *= kTick * kTicksPerSecond;
    push.y *= kTick * kTicksPerSecond;
    player_->knockDown(push);
    // Kurt.AddVerticalSpeed is a plain += on the movement field.
    player_->verticalSpeed += up;
}

// play_sound (opcode 89, 0x442402). flags & 3 is the start mode — play, restart,
// play unless it is already playing, stop — and the place comes from the rest:
// 0x80 without position, 0x10 following the object at an offset, 0x20 at a
// reference point, 0x40 at a point, none of them where the object is. Flag 4
// makes the sound the object's TRACKED sound, the one if_own_sound (249) asks
// about, and plays it at the object, following it.
void ScriptRuntime::playSound(MdkObject* obj, const std::string& name, int32_t flags,
                              const formats::ScriptValue& position) {
    const int32_t kStopMode = 3;
    const int32_t kTracked = 4;
    const int32_t kFollowing = 0x10;
    const int32_t kAtReference = 0x20;
    const int32_t kAtPoint = 0x40;
    const int32_t kFlat = 0x80;
    // SoundStarts (ScriptRuntime.cs:3257): the mode is the index. Mode 3 never
    // indexes it — the C# stops (3D) or does nothing (2D) before reading it.
    static const int32_t kSoundStarts[3] = {audio::SoundMixer::New, audio::SoundMixer::Restart,
                                            audio::SoundMixer::Once};

    const int32_t mode = flags & 3;
    if ((flags & kFlat) != 0) {
        if (mode != kStopMode) {
            mixer_.play(name, kSoundStarts[mode]);
        }
        return;
    }
    if (mode == kStopMode) {
        mixer_.stop(name);
        return;
    }
    const int32_t start = kSoundStarts[mode];

    // The point operand: three floats of a list (the C#'s V(list)), or the zero
    // vector when the operand is not a list.
    Vec3 point;
    if (position.kind == formats::ScriptValueKind::List) {
        const std::vector<formats::ScriptValue>& items = position.items;
        point.x = items.size() > 0 ? operandFloat(items[0]) : 0.0f;
        point.y = items.size() > 1 ? operandFloat(items[1]) : 0.0f;
        point.z = items.size() > 2 ? operandFloat(items[2]) : 0.0f;
    }

    int32_t voice = 0;
    if ((flags & kFollowing) != 0) {
        voice = playOn(obj, name, start, point);
    } else if ((flags & kAtReference) != 0) {
        voice = mixer_.playAt(name, obj->reference_point(operandInt(position)), start);
    } else if ((flags & kAtPoint) != 0) {
        voice = mixer_.playAt(name, point, start);
    } else if ((flags & kTracked) != 0) {
        voice = playOn(obj, name, start, Vec3());
    } else {
        voice = mixer_.playAt(name, obj->position, start);
    }

    if ((flags & kTracked) != 0) {
        obj->tracked_sound = name;
        obj->tracked_voice = voice;
    }
}

// set_loop_sound (opcode 107): start the object's looping sound, stopping the
// previous one; an empty name just stops it. It only loops if the sound itself
// does (its SNI flag) — the mixer plays the entry as stored.
void ScriptRuntime::setLoopSound(MdkObject* obj, const std::string& name) {
    mixer_.stopVoice(obj->loop_sound);
    obj->loop_sound = 0;
    obj->loop_sound_name = name;
    if (name.empty()) {
        return;
    }
    obj->loop_sound = playOn(obj, name, audio::SoundMixer::New, Vec3());
}

// if_sound_playing (opcode 233): the name is upper-cased before the search, as
// the C# does — the archives' keys are upper case, the scripts' are not always.
bool ScriptRuntime::isSoundPlaying(const std::string& name) const {
    return mixer_.isPlaying(formats::toUpperAscii(name));
}

// C# PlayOn (ScriptRuntime.cs:3317): a sound following an object at an offset in
// its frame, kept per object so Remove can stop it with the object. Ended voices
// are forgotten as the list is walked, and a voice of 0 (every voice busy) is
// kept too, exactly as the C# adds it.
int32_t ScriptRuntime::playOn(MdkObject* obj, const std::string& name, int32_t start,
                              const Vec3& offset) {
    const int32_t voice = mixer_.playOn(name, &obj->position, &obj->yaw, offset, start);
    std::vector<int32_t>& voices = following_[obj];
    for (size_t i = voices.size(); i-- > 0;) {
        if (!mixer_.isVoicePlaying(voices[i])) {
            voices.erase(voices.begin() + (long)i);
        }
    }
    voices.push_back(voice);
    return voice;
}

bool ScriptRuntime::sweepBox(MdkObject* obj, const Vec3& motion, Vec3* normal) {
    const float kWidthFactor = 0.25f;
    const float kMinHeight = 0.5f;
    const float kVerticalLift = 0.05f;
    const float kLift = 0.5f;
    Bsp* bsp = bspOf(obj->arena);
    if (bsp == nullptr) {
        obj->position.x += motion.x;
        obj->position.y += motion.y;
        obj->position.z += motion.z;
        return false;
    }
    Box bounds = poseBounds(obj);
    /* @vfp-exempt R6 — the footing sweep's yaw rotation of the collision box; glibc
     * sinf/cosf match the C# oracle's MathF.SinCos bit-for-bit on this machine
     * (probed), and the conversion is .NET's left-associative DegreesToRadians;
     * the fastSin/fastCos LUT's error is far larger and would move the box and its
     * landing. measured n/a (corpus empty). Verified 2026-10-23. */
    float s = fabsf(sinf(degreesToRadians(obj->yaw)));
    float c = fabsf(cosf(degreesToRadians(obj->yaw)));
    float sx = (bounds.max.x - bounds.min.x) * obj->scale;
    float sy = (bounds.max.y - bounds.min.y) * obj->scale;
    Vec half{(c * sx + s * sy) * kWidthFactor, (s * sx + c * sy) * kWidthFactor,
             // MathF.Max (ObjectMotion.cs:1138): a NaN lift makes the half-height NaN and
             // the sweep's centre NaN, which the oracle keeps; fmaxf would return the 0.5
             // floor and sweep an ordinary box instead. Measured on .NET 10, probe
             // `nanclamp`, row SWEEP nan_lift: level 7 (arena DANT_1) ends at 20,4,NaN and
             // level 3 (arena HMO_1) at 16,0,NaN — the Z is NaN in both, and the X/Y are
             // each level's own slide of the same step.
             mathMax(bounds.max.z * obj->scale + obj->lift(), kMinHeight) * 0.5f};
    Vec3 centre = rotatedZ(Vec3{(bounds.min.x + bounds.max.x) * 0.5f, (bounds.min.y + bounds.max.y) * 0.5f,
                                (bounds.min.z + bounds.max.z) * 0.5f},
                           obj->yaw);
    Vec centrep{centre.x * obj->scale, centre.y * obj->scale, centre.z * obj->scale};
    centrep.z = half.z + (motion.z != 0.0f ? kVerticalLift : kLift);
    if (motion.z != 0.0f) {
        float drop = obj->height_offset - obj->lift();
        half.z += drop * 0.5f;
        centrep.z -= drop * 0.5f;
    }
    Vec a{obj->position.x + centrep.x, obj->position.y + centrep.y, obj->position.z + centrep.z};
    Vec b{a.x + motion.x, a.y + motion.y, a.z + motion.z};
    Vec end{b.x, b.y, b.z};
    int node = -1;
    int firstNode = -1;
    int firstTri = Bsp::kNone;
    bsp->sweepBox(a, b, half, 2, 0.75f, &end, &node, &firstNode, &firstTri);
    obj->position.x += end.x - a.x;
    obj->position.y += end.y - a.y;
    obj->position.z += end.z - a.z;
    // Contact is the FIRST contact across the sweep passes (the C# Sweep returns its
    // first-contact normal), not the stopping pass: a slide that ends free still hit
    // a face, and an empty arena reports no contact at all (firstTri stays kNone).
    bool contacted = firstTri != Bsp::kNone;
    if (contacted && firstNode >= 0) {
        const formats::BspNode& n = bsp->arena()->nodes()[firstNode];
        *normal = Vec3{n.normal[0], n.normal[1], n.normal[2]};
    }
    return contacted;
}

void ScriptRuntime::fallOut(MdkObject* obj) {
    // The C# FallOut kills a death-scripted object (Kill switches it to its death
    // script) and fully removes an unscripted one (Remove takes a live Attached with
    // it). kill()'s death-script branch and remove() both match that now; only
    // kill()'s Explode branch stays the documented gap. The threshold and restore
    // height match.
    const float kRestore = 150.0f;
    if (obj->death_script == 0) {
        remove(obj);
        return;
    }
    kill(obj);
    obj->velocity.z = 0.0f;
    obj->position.z = getArenaFloor(obj->arena) - kRestore;
    obj->flags &= ~MdkObject::FlagGravity;
}

const Model* ScriptRuntime::findModel(const std::string& arena, const std::string& type) {
    std::map<std::string, Model>::iterator cached = modelCache_.find(type);
    if (cached != modelCache_.end()) {
        return &cached->second;
    }
    Model model;
    if (cmi_->model(type, &model)) {
        std::pair<std::map<std::string, Model>::iterator, bool> inserted =
            modelCache_.emplace(type, model);
        return &inserted.first->second;
    }
    if (level_->mto().has(arena)) {
        const formats::Arena* arenaModel = level_->mto().getArena(arena);
        if (arenaModel != nullptr) {
            const Model* found = arenaModel->model(type);
            if (found != nullptr) {
                return found;
            }
        }
    }
    return nullptr;
}

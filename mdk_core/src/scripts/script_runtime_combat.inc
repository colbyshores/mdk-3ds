// script_runtime_combat.inc -- kills, explosions, the break-up, sparks, the health bar, the chain gun, blasts, removal.
// Part of script_runtime.cpp's single translation unit (see its header): pasted
// inside namespace mdk::scripts after its file-local constants and helpers,
// which it uses. No include guards, no includes of its own; the order is
// load-bearing.

void ScriptRuntime::kill(MdkObject* obj, float yaw) {
    // C# Kill (ScriptRuntime.cs:1678): Health=0, then either Explode (no death
    // script) or switch to the death script. NoteAlarmEnded is the 1996 demo's alarm
    // (ScriptRuntime.Beta.cs), which this runtime does not reach. The death-script
    // branch is pure VM state and is exercised by the L3 BOLT (its death script ends in
    // opcode 110 delete_self, which removes it the next tick); the explosion branch is
    // the chain gun's kill of an object with no death script (golden/L8_fire.txt).
    obj->health = 0;
    if (obj->death_script == 0) {
        explode(obj, yaw);
        return;
    }
    obj->move_command = 0;
    obj->flags |= MdkObject::FlagNotTarget;
    obj->restart = obj->death_script;
    obj->wait_time = 0.0f;
    obj->wait_resume = obj->death_script;
    obj->death_script = 0;
}

namespace {

// The explosion's numbers (ScriptRuntime.cs:96-111).
const int32_t kFireSparks = 16;
const float kExplosionFlash = 1000.0f;
const float kExplosionFlashMax = 150.0f;
const int32_t kGoreDrops = 32;
const float kGoreScale = 5.0f;
const float kExplosionLift = 5.0f;
const float kExplosionNear = 5.0f;
const float kExplosionNearHeight = 8.0f;
const int32_t kExplosionFrames = 26;
const float kExplosionSizeFactor = 1.5f;
const char kExplodeSound[] = "EXPLODE";

// The chain gun's numbers (ScriptRuntime.cs:30-36, 116, 140).
const int32_t kChainGunDamage = 1;
const int32_t kSuperChainGunFactor = 6;
const float kChainGunReach = 140.0f;
const float kChainGunWallReach = 150.0f;
const float kSuperChainGunPush = 20.0f;
const int32_t kMaxBarHealth = 900;
const float kGunEyeHeight = 5.0f;
const int32_t kHitChainGun = 2;

// The sparks' colours (palette base, range) by ScriptRuntime::Spark, and the flesh
// spark without gore (ScriptRuntime.cs:92-95).
const int32_t kSparkColours[4][2] = {{3, 3}, {0x25, -16}, {10, 3}, {0x30, 0x10}};
const int32_t kSparkFleshNoGore[2] = {13, 3};
const float kSparkSize = 0.5f;
const float kSparkSlow = 0.5f;
const char* const kRicochets[3] = {"RICO1", "RICO2", "RICO3"};

// ScriptMath.Center / Size (ScriptMath.cs:74-76).
Vec3 boxCenter(const formats::Box& box) {
    return Vec3{(box.min.x + box.max.x) * 0.5f, (box.min.y + box.max.y) * 0.5f,
                (box.min.z + box.max.z) * 0.5f};
}

Vec3 boxSize(const formats::Box& box) {
    return Vec3{box.max.x - box.min.x, box.max.y - box.min.y, box.max.z - box.min.z};
}

float length3(const Vec3& v) {
    return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
}

} // namespace

void ScriptRuntime::explode(MdkObject* obj, float yaw) {
    // C# Explode (ScriptRuntime.cs:1710-1723), in its order: the sound and the flash at
    // the centre, the pieces, the removal, then the explosion effect scaled to the
    // object's height (at least 0.1 of the effect's own).
    const float kMinHeight = 0.1f;
    const formats::Box bounds = worldBounds(obj);
    const Vec3 center = obj->model != nullptr ? boxCenter(bounds) : obj->position;
    playSoundAt(!obj->labels[0].empty() ? obj->labels[0] : std::string(kExplodeSound), center);
    flashAt(center);
    breakUp(obj);
    remove(obj);
    MdkObject* effect = spawnExplosion(obj->arena, center, 1.0f, yaw, kExplosionLift);
    if (effect != nullptr && effect->model != nullptr) {
        const float height = boxSize(effect->model->bounds).z;
        effect->scale = boxSize(bounds).z / mathMax(height, kMinHeight) * kExplosionSizeFactor;
    }
}

void ScriptRuntime::flashAt(const Vec3& point) {
    // C# FlashAt (ScriptRuntime.cs:1725-1735). MathF.Round is the banker's round, which
    // rintf is under the default rounding mode.
    const float distance = length3(Vec3{kurtPosition_.x - point.x, kurtPosition_.y - point.y,
                                        kurtPosition_.z - point.z});
    Kurt& combat = kurt();
    if (distance <= 0.0f || combat.whiteFlash() >= kExplosionFlashMax) {
        return;
    }
    combat.setWhiteFlash(mathMin(combat.whiteFlash() + rintf(kExplosionFlash / distance),
                                 kExplosionFlashMax));
}

void ScriptRuntime::breakUp(MdkObject* obj) {
    // C# BreakUp (ScriptRuntime.cs:1738-1765).
    const float kDropSpread = 1.0f / 16384.0f;
    const formats::Model* model =
        obj->model != nullptr ? findModel(obj->arena, obj->model->name + "D") : nullptr;
    if (model == nullptr) {
        debris_.spark(obj->arena, obj->position, kFireSparks, 1.0f,
                      kSparkColours[(int)Spark::Fire][0], kSparkColours[(int)Spark::Fire][1]);
        return;
    }

    debris_.breakUp(*obj, *model, materialNamesOf(model));

    // Slime drops thrown like the pieces, with gore (0x43d55e).
    if (option == 0) {
        return;
    }

    const Vec3 velocity{obj->position.x - obj->previous_position.x,
                        obj->position.y - obj->previous_position.y,
                        obj->position.z - obj->previous_position.z};
    for (int32_t i = 0; i < kGoreDrops; i++) {
        // The C#'s argument order: x, y and z of the throw, then the scale.
        const int32_t tx = rng_.next(32768) - 0x4000;
        const int32_t ty = rng_.next(32768) - 0x4000;
        const int32_t tz = rng_.next(32768) - 0x800;
        const Vec3 thrown{(float)tx * kDropSpread, (float)ty * kDropSpread,
                          (float)tz * kDropSpread};
        const Vec3 launch{velocity.x + thrown.x, velocity.y + thrown.y, velocity.z + thrown.z};
        const float scale = kGoreScale + (float)rng_.next(32768) * 5e-5f;
        effects.spawnDrop(obj->arena, obj->position, launch, scale);
    }
}

const std::vector<std::string>* ScriptRuntime::materialNamesOf(const formats::Model* model) {
    std::map<const formats::Model*, std::vector<std::string>>::iterator found =
        materialNames_.find(model);
    if (found != materialNames_.end()) {
        return &found->second;
    }
    std::vector<std::string> names;
    for (size_t i = 0; i < model->materials.size(); i++) {
        names.push_back(model->materials[i].nameString());
    }
    return &materialNames_.emplace(model, names).first->second;
}

bool ScriptRuntime::findTextureFrames(const std::string& arena, const std::string& name,
                                      int32_t* frames) {
    // C# FindTexture (ScriptRuntime.cs:3224): the arena's own archive when the MTO has
    // the arena, then the level's. False is the C#'s null.
    if (level_->mto().has(arena)) {
        const formats::Arena* owner = level_->mto().getArena(arena);
        if (owner != nullptr) {
            const formats::Texture* found = owner->textures().findTexture(name);
            if (found != nullptr) {
                *frames = found->frameCount();
                return true;
            }
        }
    }
    const formats::Texture* level = level_->levelTextures().findTexture(name);
    if (level == nullptr) {
        return false;
    }
    *frames = level->frameCount();
    return true;
}

MdkObject* ScriptRuntime::spawnExplosion(const std::string& arena, const Vec3& center, float scale,
                                         float yaw, float lift) {
    // C# SpawnExplosion (ScriptRuntime.cs:1770-1796): the CMI's FIRST model by the file's
    // order (Cmi.ModelOffsets is an insertion-ordered dictionary, and modelOffsets()
    // keeps the file's order), spawned plain under the arena's controller.
    const std::vector<std::pair<std::string, int32_t>>& models = cmi_->modelOffsets();
    const std::string modelName = models.empty() ? std::string() : models[0].first;
    MdkObject* effect =
        spawn(&getArenaState(arena).controller, modelName, center, yaw, -1, 0, false);
    if (effect == nullptr || effect->model == nullptr) {
        return nullptr;
    }

    effect->flags |= MdkObject::FlagNotTarget | MdkObject::FlagNotSolid2;
    effect->effect_frames = kExplosionFrames;
    int32_t frames = 0;
    if (!effect->model->materials.empty() &&
        findTextureFrames(arena, effect->model->materials[0].nameString(), &frames)) {
        effect->effect_frames = frames;
    }

    effect->scale = scale;
    const float hx = eye.x - center.x;
    const float hy = eye.y - center.y;
    const float horizontal = sqrtf(hx * hx + hy * hy);
    effect->yaw = yaw == 0.0f ? wrap360(heading(center, eye)) : yaw;
    const float rise = eye.z + lift - center.z;
    if (horizontal > kExplosionNear || fabsf(rise) > kExplosionNearHeight) {
        /* @vfp-exempt R6 — the explosion turns to the camera once, when it spawns (cold);
         * the C#'s MathF.Atan2 is glibc's atan2f bit for bit on this machine (see yawTo's
         * pin) and the LUT would tilt it differently. measured n/a (corpus empty).
         * Verified 2026-11-19. */
        effect->pitch = radiansToDegrees(atan2f(rise, horizontal));
    }

    effect->texture_frame = 0;
    return effect;
}

void ScriptRuntime::motionEffect(MdkObject* obj) {
    // ObjectMotion.Update's first branch (ObjectMotion.cs:54-68), where Ticks is 1: an
    // effect plays its texture animation one frame a tick and goes after the last.
    obj->effect_time += 1.0f;
    if (obj->effect_time >= (float)obj->effect_frames) {
        remove(obj);
    } else {
        obj->texture_frame = (int32_t)obj->effect_time;
    }
}

void ScriptRuntime::playSoundAt(const std::string& name, const Vec3& point) {
    mixer_.playAt(name, point);
}

void ScriptRuntime::sparkAt(const Vec3& point, int32_t count, const std::string& sound,
                            Spark kind) {
    // C# SparkAt (ScriptRuntime.cs:2201-2212). The ricochet's pick is drawn from the
    // game's random stream ONLY when it plays and the caller named no sound, ahead of the
    // sparks' own draws, exactly as the C# orders them.
    const int32_t kRicochetEvery = 3;
    const bool quiet = !effects.hasEffects(currentArena_) && !debris_.hasPieces(currentArena_);
    if (quiet || count > 1 || (tickCount_ & kRicochetEvery) == 0) {
        playSoundAt(!sound.empty() ? sound : std::string(kRicochets[rng_.next(3)]), point);
    }
    const bool gore = kind != Spark::Flesh || option != 0;
    const int32_t colour = gore ? kSparkColours[(int)kind][0] : kSparkFleshNoGore[0];
    const int32_t range = gore ? kSparkColours[(int)kind][1] : kSparkFleshNoGore[1];
    debris_.spark(currentArena_, point, count, kSparkSize, colour, range,
                  kind == Spark::Hard ? kSparkSlow : 1.0f);
}

void ScriptRuntime::showBarValues(int32_t health, int32_t maxHealth) {
    // C# ShowBarValues (ScriptRuntime.cs:2157-2163).
    barObject = nullptr;
    barHealth = health;
    barMax = maxHealth;
    barTime = health > 0 ? 1.0f : 0.0f;
}

ScriptRuntime::BarValues ScriptRuntime::bar() const {
    // C# GetBar (ScriptRuntime.cs:2165-2174).
    BarValues values{0, 0};
    if (barTime <= 0.0f) {
        return values;
    }
    if (barObject != nullptr) {
        values.health = barObject->health;
        values.max = barObject->max_health;
    } else {
        values.health = barHealth;
        values.max = barMax;
    }
    return values;
}

void ScriptRuntime::updateBar() {
    // C# UpdateBar (ScriptRuntime.cs:2177-2197).
    if (barTime <= 0.0f) {
        return;
    }
    barTime -= kTick;
    if (barObject != nullptr && barObject->dead) {
        barObject = nullptr;
        barTime = 0.0f;
        return;
    }
    const BarValues values = bar();
    if (values.max == 0 || values.max > kMaxBarHealth || values.health < 1) {
        barTime = 0.0f;
    }
}

float ScriptRuntime::aimScore(const formats::Box& bounds, const Vec3& origin, float bestScore) {
    // C# AimScore (ScriptRuntime.cs:2029-2053). The heading is ScriptMath.Heading's libm
    // atan2f (heading(), pinned there): the LUT would move the cone's edge and with it
    // which object the gun hits.
    const float kMinSize = 10.0f;
    const float kFullTurn = 360.0f;
    const float size = mathMax(length3(boxSize(bounds)), kMinSize);
    const Vec3 center = boxCenter(bounds);
    const float distance =
        length3(Vec3{origin.x - center.x, origin.y - center.y, origin.z - center.z});
    if (distance > size + kChainGunReach) {
        return -1.0f;
    }

    const float angle = wrap360(heading(origin, center) - targetYaw_);
    const float cone = (size - 2.0f) * 90.0f / (size - 2.0f + distance);
    if (angle > cone && angle < kFullTurn - cone) {
        return -1.0f;
    }

    const float height = center.z - kurtPosition_.z;
    const float dx = center.x - origin.x;
    const float dy = center.y - origin.y;
    const float score = (dx * dx + dy * dy) + 4.0f * height * height;
    if (bestScore >= 0.0f && score > bestScore) {
        return -1.0f;
    }

    return raycastClear(origin, center) ? score : -1.0f;
}

void ScriptRuntime::fireChainGun() {
    // C# FireChainGun (ScriptRuntime.cs:1818-1900, 0x41a304).
    const int32_t kShotsPerTick = 6;
    const Vec3 origin{kurtPosition_.x, kurtPosition_.y, kurtPosition_.z + kGunEyeHeight};

    // The super chain gun does 6 times the damage while its time lasts.
    Kurt& combat = kurt();
    const bool superGun = combat.superChainGun() > 0;
    const int32_t damage = kChainGunDamage * (superGun ? kSuperChainGunFactor : 1);
    if (superGun) {
        combat.tickSuperChainGun(1);
    }

    stats.shots += kShotsPerTick;
    MdkObject* best = nullptr;
    float bestScore = -1.0f;
    int32_t bestPart = -1;
    formats::Box bestBounds{};
    // The C#'s foreach over Objects: the deque's slots in creation order, skipping the
    // removed ones (`dead`), which is the C# list exactly.
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        MdkObject* obj = &(*it);
        if (obj->dead || obj->arena != currentArena_ || obj->health == 0 ||
            (obj->flags & (MdkObject::FlagNotSolid | MdkObject::FlagNotTarget)) != 0) {
            continue;
        }

        // Weak parts are targets of their own.
        if ((obj->flags & MdkObject::FlagWeakParts) != 0 && obj->model != nullptr) {
            const std::vector<PartBox> parts = partBounds(obj);
            const size_t partCount = obj->model->partList.size();
            for (size_t i = 0; i < partCount; i++) {
                // C#'s `1 << i` takes the shift count modulo 32.
                if ((obj->hidden_parts & (1 << (i & 31))) != 0 ||
                    !isWeakPart(obj, (int32_t)i) || i >= parts.size() || !parts[i].valid) {
                    continue;
                }
                const formats::Box partBox = worldBounds(obj, parts[i].box);
                const float partScore = aimScore(partBox, origin, bestScore);
                if (partScore < 0.0f) {
                    continue;
                }
                best = obj;
                bestScore = partScore;
                bestPart = (int32_t)i;
                bestBounds = partBox;
            }
            if (best == obj) {
                continue;
            }
        }

        const formats::Box bounds = worldBounds(obj);
        const float score = aimScore(bounds, origin, bestScore);
        if (score >= 0.0f) {
            best = obj;
            bestScore = score;
            bestPart = -1;
            bestBounds = bounds;
        }
    }

    if (best != nullptr) {
        chainGunHit(best, bestPart, bestBounds, origin, damage, superGun);
        return;
    }

    // No target: the arena, up to 150 units along Kurt's yaw, level.
    const Vec3 direction = fromAngle(targetYaw_);
    const Vec3 shot{direction.x * kChainGunWallReach, direction.y * kChainGunWallReach, 0.0f};
    DebrisRayHit hit;
    if (!raycastHit(origin, Vec3{origin.x + shot.x, origin.y + shot.y, origin.z + shot.z},
                    &hit)) {
        return;
    }

    const bool reacted =
        (hitGroup(hit.arena, hit.group, damage, kHitChainGun, superGun ? -2 : -1) & 1) != 0;
    // Vector3.Normalize: the vector over its length.
    const float length = length3(shot);
    const Vec3 back{hit.point.x - shot.x / length, hit.point.y - shot.y / length,
                    hit.point.z - shot.z / length};
    sparkAt(back, 1, std::string(), reacted ? Spark::Group : Spark::Hard);
}

void ScriptRuntime::chainGunHit(MdkObject* obj, int32_t part, const formats::Box& bounds,
                                const Vec3& origin, int32_t damage, bool superGun) {
    // C# ChainGunHit (ScriptRuntime.cs:2075-2135). The 1996 demo's hit-part guess
    // (IsBeta && BetaHitPart) is not reached by this runtime (see the IsBeta note by the
    // animation lookup).
    const Vec3 center = boxCenter(bounds);
    const float direction = heading(origin, center);
    stats.shotHits++;
    obj->hit_event = -1;

    if (part >= 0 && part < (int32_t)obj->part_health.size()) {
        const size_t index = (size_t)part;
        obj->part_health[index] -= kurtDamage(obj, damage, obj->part_health[index]);
        if (obj->part_health[index] <= 0) {
            obj->part_health[index] = 0;
            obj->hit_event = part + 1;
        }
        if (index < obj->part_max_health.size() && obj->part_max_health[index] <= kMaxBarHealth) {
            showBarValues(obj->part_health[index], obj->part_max_health[index]);
        }
    }

    if (obj->health < kIndestructible) {
        obj->health -= kurtDamage(obj, damage);
    }

    obj->hit_type = superGun ? -2 : -1;
    obj->hit_direction = direction;
    if (obj->health > 0) {
        if (part < 0 && obj->max_health <= kMaxBarHealth) {
            barObject = obj;
            barTime = 1.0f;
        }

        // Sparks on the side of the box facing Kurt: center - (tx*sx, ty*sy, 0) * 0.5.
        const Vec3 toward = fromAngle(direction);
        const Vec3 size = boxSize(bounds);
        const Vec3 offset{toward.x * size.x * 0.5f, toward.y * size.y * 0.5f, 0.0f * 0.5f};
        const Vec3 point{center.x - offset.x, center.y - offset.y, center.z - offset.z};
        sparkAt(point, 1, obj->labels[1],
                obj->indestructible && part < 0 ? Spark::Hard : Spark::Flesh);
        return;
    }

    obj->health = 0;
    if (superGun) {
        // The super chain gun throws what it kills away.
        const Vec3 push = fromAngle(direction);
        obj->velocity.x += push.x * kSuperChainGunPush;
        obj->velocity.y += push.y * kSuperChainGunPush;
    }

    stats.countEnemy(obj->type_name, GameStats::Killed);
    kill(obj, direction + 180.0f);
}

// Items.cs BlastLift: the blast origin is lifted off the floor so the wall test
// doesn't hit the floor the blast may lie on.
static const float kBlastLift = 1.0f;

int32_t ScriptRuntime::kurtDamage(MdkObject* obj, int32_t damage, int32_t health) {
    // C# KurtDamage (ScriptRuntime.OneHit.cs:21). The one-hit console cheat
    // (Lethality::OneHit) makes a hit take all an enemy's remaining health; that
    // console setting is unported, so Lethality is always Normal and this returns
    // `damage` -- the value the C# returns for every non-cheat game.
    (void)obj;
    (void)health;
    return damage;
}

int32_t ScriptRuntime::kurtDamage(MdkObject* obj, int32_t damage) {
    return kurtDamage(obj, damage, obj->health);
}

int32_t ScriptRuntime::blastHit(MdkObject* obj, int32_t damage, int32_t health, bool counted) {
    // C# BlastHit (Items.cs:783): a counted blast scales by KurtDamage; an
    // uncounted one (Kills.Ignored) deals the raw damage.
    return counted ? kurtDamage(obj, damage, health) : damage;
}

std::pair<int32_t, float> ScriptRuntime::blastDamage(const formats::Box& box, const Vec3& center,
                                                    int32_t damage, float radius) {
    // C# BlastDamage (Items.cs:788): falloff to the box centre less half the box's
    // diagonal; 0 beyond the radius or behind a wall.
    const float sx = box.max.x - box.min.x;
    const float sy = box.max.y - box.min.y;
    const float sz = box.max.z - box.min.z;
    const float size = sqrtf(sx * sx + sy * sy + sz * sz);
    const Vec3 target{(box.min.x + box.max.x) * 0.5f,
                      (box.min.y + box.max.y) * 0.5f,
                      (box.min.z + box.max.z) * 0.5f};
    const Vec3 lifted{center.x, center.y, center.z + kBlastLift};
    const float dx = lifted.x - target.x;
    const float dy = lifted.y - target.y;
    const float dz = lifted.z - target.z;
    const float squared = (dx * dx + dy * dy + dz * dz) - size * size * 0.25f;
    if (squared > radius * radius || !raycastClear(lifted, target)) {
        return std::make_pair(0, 0.0f);
    }
    const float distance = sqrtf(mathMax(squared, 0.0f));
    // ScriptMath.Round is AwayFromZero (ScriptMath.cs:48); C roundf matches it.
    return std::make_pair((int32_t)roundf((float)damage * (1.0f - distance / radius)), distance);
}

// BlastWeakParts (Items.cs:680): the blast on an object's weak parts. For each
// visible, weak, in-range part with a box, take the blast damage; keep the best
// (strict >, so the first max wins), and drop each part's health by the hit,
// recording the last part destroyed (index + 1) as the event.
ScriptRuntime::BlastPartResult ScriptRuntime::blastWeakParts(MdkObject* obj, const Vec3& center,
                                                             int32_t damage, float radius,
                                                             bool counted) {
    BlastPartResult result;
    result.best = 0;
    result.distance = 0.0f;
    result.point = obj->position;
    result.event = 0;
    if ((obj->flags & MdkObject::FlagWeakParts) == 0 || obj->model == nullptr) {
        return result;
    }
    std::vector<PartBox> parts = partBounds(obj);
    const size_t partCount = obj->model->partList.size();
    for (size_t i = 0; i < partCount; ++i) {
        if ((obj->hidden_parts & (1 << i)) != 0 || !isWeakPart(obj, static_cast<int32_t>(i)) ||
            i >= obj->part_health.size() || i >= parts.size() || !parts[i].valid) {
            continue;
        }
        const formats::Box box = worldBounds(obj, parts[i].box);
        const std::pair<int32_t, float> hit = blastDamage(box, center, damage, radius);
        if (hit.first <= 0) {
            continue;
        }
        if (hit.first > result.best) {
            result.best = hit.first;
            result.distance = hit.second;
            result.point = Vec3{(box.min.x + box.max.x) * 0.5f,
                                (box.min.y + box.max.y) * 0.5f,
                                (box.min.z + box.max.z) * 0.5f};
        }
        obj->part_health[i] -= blastHit(obj, hit.first, obj->part_health[i], counted);
        if (obj->part_health[i] < 1) {
            obj->part_health[i] = 0;
            result.event = static_cast<int32_t>(i) + 1;
        }
    }
    return result;
}


void ScriptRuntime::remove(MdkObject* obj) {
    // C# Remove (ScriptRuntime.cs:1648): a live Attached object (a pickup's chute) goes
    // with it FIRST — Remove(obj.Attached) runs before obj is marked dead — then Dead,
    // then out of the list. The deque's slots are never vacated (the pointer model
    // depends on it), so the object is marked removed here and remains readable through
    // surviving references. The oracle clears NO inbound reference — not Linked, not
    // Leader, not CowTarget, not Attached — so none is cleared here either: a widow
    // keeps pointing at its dead partner (link_partners must not re-pair it), a
    // removed chute keeps its leader, and a pickup keeps a chute removed under it.
    // Every read site twins the oracle's test — null-or-dead for linked/leader,
    // null-only for attached, elsewhere a comparison against a live object — so a
    // retained pointer to the permanent slot behaves exactly like the oracle's
    // retained managed reference. A pickup that falls out (FallOut -> remove) takes
    // its chute with it.
    // Rides.Lost (ScriptRuntime.cs:1650, package #9), the C#'s first statement: the
    // ridden object going puts Kurt off it (and hurts him but for the board). A removed
    // object is never the ridden one (its first removal let him off), so this before
    // the idempotence return is the C#'s order without a second effect.
    rides_.lost(obj);
    if (isRemoved(obj)) {
        return;  // C# Objects.Remove is idempotent; do not double-process
    }
    if (obj->attached != nullptr && !obj->attached->dead) {
        remove(obj->attached);
    }
    // The object's sounds go with it (C# Remove: the loop sound, the tracked
    // sound, and every voice that was following it).
    mixer_.stopVoice(obj->loop_sound);
    mixer_.stopVoice(obj->tracked_voice);
    std::map<MdkObject*, std::vector<int32_t> >::iterator following = following_.find(obj);
    if (following != following_.end()) {
        for (size_t i = 0; i < following->second.size(); i++) {
            mixer_.stopVoice(following->second[i]);
        }
        following_.erase(following);
    }
    obj->loop_sound = 0;
    obj->tracked_voice = 0;
    obj->dead = true;
    removed_.push_back(obj);
}

bool ScriptRuntime::isRemoved(const MdkObject* obj) const {
    for (size_t i = 0; i < removed_.size(); i++) {
        if (removed_[i] == obj) {
            return true;
        }
    }
    return false;
}

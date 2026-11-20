// ============================================================================
// hazards.cpp — contact damage and spawned boxes: ScriptRuntime.TouchDamage
// (touch_damage, opcode 158) and ScriptRuntime.SpawnBox / BoxModel (spawn_box,
// opcode 159), src/Mdk.Game/Scripts/ScriptRuntime.cs:1590-1647 and :2755-2820.
//
// Operation for operation against the C#: the same order of tests and the same
// object-list walk (the C#'s ListCopy is a snapshot of the live objects; the walk
// here stops at the size the deque had when it started, so an object a kill
// spawns -- the explosion -- is not visited, and skips the removed slots, which
// the C#'s snapshot holds as Dead). The goldens that pin it are
// tests/scripts/golden/L*_hazards_*.txt, printed by the C# probe
// tests/scripts/hazards_probe and by `tick1 dump hazards`.
// ============================================================================

#include "scripts/pose_cache.h"
#include "scripts/script_runtime.h"

namespace mdk {
namespace scripts {

namespace {

// ScriptMath.Intersects (ScriptMath.cs:78): strict on every axis.
bool touchIntersect(const formats::Box& a, const formats::Box& b) {
    return a.min.x < b.max.x && a.max.x > b.min.x && a.min.y < b.max.y && a.max.y > b.min.y &&
           a.min.z < b.max.z && a.max.z > b.min.z;
}

// C# BoxModel (ScriptRuntime.cs:1621, model_create_box 0x404188): 8 corners, 12
// triangles, every one of them NoMaterial, so nothing of it is drawn.
void buildBoxModel(const Vec3& size, const std::string& texture, formats::Model* out) {
    const int32_t kNoMaterial = -256;
    static const int32_t kTriangles[12][3] = {
        {0, 1, 2}, {1, 2, 3}, {0, 4, 6}, {0, 2, 6}, {0, 1, 5}, {0, 5, 4},
        {1, 5, 7}, {1, 3, 7}, {3, 2, 6}, {3, 7, 6}, {5, 4, 6}, {5, 7, 6},
    };
    const float hx = size.x * 0.5f;
    const float hy = size.y * 0.5f;
    const float hz = size.z * 0.5f;
    formats::Box bounds;
    bounds.min.x = -hx;
    bounds.min.y = -hy;
    bounds.min.z = -hz;
    bounds.max.x = hx;
    bounds.max.y = hy;
    bounds.max.z = hz;

    formats::Model::Part part;
    for (int32_t i = 0; i < 8; i++) {
        formats::Vec3f v;
        v.x = (i & 1) != 0 ? hx : -hx;
        v.y = (i & 2) != 0 ? hy : -hy;
        v.z = (i & 4) != 0 ? hz : -hz;
        part.vertices.push_back(v);
    }
    for (int32_t t = 0; t < 12; t++) {
        for (int32_t k = 0; k < 3; k++) {
            part.triangleIndices.push_back(kTriangles[t][k]);
        }
        part.triangleMaterials.push_back(kNoMaterial);
    }
    part.triangleUvs.assign(12 * 3, formats::Vec2f());
    part.bounds = bounds;

    out->name = texture;
    out->partList.push_back(part);
    out->bounds = bounds;
}

} // namespace

bool ScriptRuntime::touchDamage(MdkObject* obj, int32_t targets, int32_t damage, int32_t flags) {
    // C# TouchDamage (ScriptRuntime.cs:2758).
    const int32_t kHurtsKurt = 1;
    const int32_t kHurtsObjects = 2;
    const int32_t kDiesOnHit = 1;
    const int32_t kUntouchable = 0x820;
    const int32_t kTouchHitEvent = -3;
    const int32_t kTouchHitType = -4;
    const formats::Box box = worldBounds(obj);
    bool hit = false;
    if ((targets & kHurtsKurt) != 0) {
        const formats::Box kurtBounds = kurtBox();
        if (touchIntersect(box, kurtBounds)) {
            // Once per visible part touching him.
            const std::vector<PartBox> parts = partBounds(obj);
            for (size_t i = 0; i < parts.size(); i++) {
                if ((obj->hidden_parts & (1 << (i & 31))) != 0 || !parts[i].valid ||
                    !touchIntersect(worldBounds(obj, parts[i].box), kurtBounds)) {
                    continue;
                }
                kurt().hurt(damage);
                hit = true;
            }
        }
    }

    if ((targets & kHurtsObjects) != 0) {
        // ListCopy<MdkObject>.Of(Objects): the objects as they are now. A kill below
        // can spawn (the explosion); those land past `count` and are not visited.
        const size_t count = objects_.size();
        for (size_t i = 0; i < count; i++) {
            MdkObject* other = &objects_[i];
            if (other == obj || other->dead || other->arena != obj->arena ||
                (other->flags & kUntouchable) != 0) {
                continue;
            }
            if (!touchIntersect(box, worldBounds(other))) {
                continue;
            }
            hit = true;
            other->hit_event = kTouchHitEvent;
            other->hit_type = kTouchHitType;
            other->hit_direction = obj->yaw;
            if (other->health < kIndestructible) {
                other->health -= damage;
            }
            if (other->health < 1) {
                kill(other);
            }
        }
    }

    if (hit && (flags & kDiesOnHit) != 0) {
        kill(obj);
    }
    return hit;
}

MdkObject* ScriptRuntime::spawnBox(MdkObject* parent, const Vec3& position, const Vec3& size,
                                   const std::string& texture, int32_t script) {
    // C# SpawnBox (ScriptRuntime.cs:1593): one model per size and texture.
    const formats::Model* model = nullptr;
    for (size_t i = 0; i < boxModelKeys_.size(); i++) {
        const BoxModelKey& key = boxModelKeys_[i];
        if (key.size.x == size.x && key.size.y == size.y && key.size.z == size.z &&
            key.texture == texture) {
            model = &boxModels_[i];
            break;
        }
    }
    if (model == nullptr) {
        boxModels_.push_back(formats::Model());
        buildBoxModel(size, texture, &boxModels_.back());
        BoxModelKey key;
        key.size = size;
        key.texture = texture;
        boxModelKeys_.push_back(key);
        model = &boxModels_.back();
    }

    MdkObject obj;
    obj.arena = parent->arena;
    obj.type_name = texture;
    obj.model = model;
    obj.instance_id = nextInstance_++;
    obj.position = position;
    obj.spawn_position = position;
    obj.previous_position = position;
    obj.restart = script;
    objects_.push_back(obj);
    MdkObject* created = &objects_.back();

    // _boxes.RemoveAll(b => b.Dead); _boxes.Add(obj).
    size_t kept = 0;
    for (size_t i = 0; i < boxes_.size(); i++) {
        if (!boxes_[i]->dead) {
            boxes_[kept++] = boxes_[i];
        }
    }
    boxes_.resize(kept);
    boxes_.push_back(created);
    return created;
}

void ScriptRuntime::forgetBoxModels() {
    // The pose caches are keyed by model ADDRESS; the box models die with this
    // runtime, so a later runtime's model could land on one of their addresses.
    // Dropping the caches is never wrong (they hold derived data).
    if (!boxModels_.empty()) {
        forgetPoseCaches();
    }
}

} // namespace scripts
} // namespace mdk

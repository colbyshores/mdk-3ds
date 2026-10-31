#include "scripts/object.h"

#include <cmath>
#include <map>
#include <utility>
#include <vector>

#include "formats/model.h"
#include "formats/model_animation.h"
#include "scripts/pose_cache.h"

// The object model is data-only in this slice, so this translation unit carries
// the compile-time checks that pin the flag word — the values most likely to be
// mistyped in a hand port. The runtime field defaults (health=100, the -1 triple,
// friction=64/gravity=32, ...) cannot be static_asserted because MdkObject holds
// std::string/std::vector and is not a literal type; they are pinned instead by
// the C# oracle object-table diff (see the scripts port notes) and the field-by-
// field review.

static_assert(mdk::scripts::MdkObject::FlagGravity == 0x2, "FlagGravity");
static_assert(mdk::scripts::MdkObject::FlagCollides == 0x4, "FlagCollides");
static_assert(mdk::scripts::MdkObject::FlagLoop == 0x8, "FlagLoop");
static_assert(mdk::scripts::MdkObject::FlagNotSolid == 0x10, "FlagNotSolid");
static_assert(mdk::scripts::MdkObject::FlagNotTarget == 0x20, "FlagNotTarget");
static_assert(mdk::scripts::MdkObject::FlagRolling == 0x40, "FlagRolling");
static_assert(mdk::scripts::MdkObject::FlagNoBanking == 0x80, "FlagNoBanking");
static_assert(mdk::scripts::MdkObject::FlagPlatform == 0x100, "FlagPlatform");
static_assert(mdk::scripts::MdkObject::FlagPathOnce == 0x400, "FlagPathOnce");
static_assert(mdk::scripts::MdkObject::FlagNotSolid2 == 0x800, "FlagNotSolid2");
static_assert(mdk::scripts::MdkObject::FlagWeakParts == 0x2000, "FlagWeakParts");
static_assert(mdk::scripts::MdkObject::FlagNoTurning == 0x10000, "FlagNoTurning");
static_assert(mdk::scripts::MdkObject::FlagLanded == 0x20000, "FlagLanded");
static_assert(mdk::scripts::MdkObject::FlagCollected == 0x40000, "FlagCollected");
static_assert(mdk::scripts::MdkObject::FlagChangesArena == 0x80000, "FlagChangesArena");
static_assert(mdk::scripts::MdkObject::FlagDoor == 0x100000, "FlagDoor");
static_assert(mdk::scripts::MdkObject::FlagPickup == 0x200000, "FlagPickup");
static_assert(mdk::scripts::MdkObject::FlagSwinging == 0x400000, "FlagSwinging");
static_assert(mdk::scripts::MdkObject::FlagStandable == 0x800000, "FlagStandable");
static_assert(mdk::scripts::MdkObject::FlagPathPushes == 0x8000000, "FlagPathPushes");
static_assert(mdk::scripts::MdkObject::FlagPathSpeedByKurt == 0x10000000, "FlagPathSpeedByKurt");
static_assert(mdk::scripts::MdkObject::FlagBounces == 0x20000000, "FlagBounces");
static_assert(mdk::scripts::MdkObject::FlagCow == 0x40000000, "FlagCow");
// The C#'s unchecked((int)0x80000000) is INT_MIN; the bits must match exactly.
static_assert(mdk::scripts::MdkObject::FlagNoRootMotion == (-2147483647 - 1), "FlagNoRootMotion");
static_assert(mdk::scripts::MdkObject::FlagNoRootMotion < 0, "FlagNoRootMotion is the sign bit");

static_assert(mdk::scripts::MdkObject::ContactCollided == 0x1, "ContactCollided");
static_assert(mdk::scripts::MdkObject::ContactFloor == 0x2, "ContactFloor");
static_assert(mdk::scripts::MdkObject::ContactTouchedKurt == 0x4, "ContactTouchedKurt");
static_assert(mdk::scripts::MdkObject::ContactStuck == 0x8, "ContactStuck");

static_assert(mdk::scripts::MdkObject::AnimationEnded == -256, "AnimationEnded");
static_assert(mdk::scripts::MdkObject::NoHoldFrame == -1, "NoHoldFrame");

// The rolling basis defaults to identity (the C#'s Matrix4x4.Identity).
static_assert(mdk::Mat4::identity().m[0] == 1.0f && mdk::Mat4::identity().m[5] == 1.0f &&
                  mdk::Mat4::identity().m[10] == 1.0f && mdk::Mat4::identity().m[15] == 1.0f &&
                  mdk::Mat4::identity().m[1] == 0.0f,
              "Mat4::identity");

// ReferencePoint (0x43b65c): the model's reference point `index`, scaled by the
// object's scale and rotated about Z by yaw, added to the position. The C# is
// Position + Vector3.Transform(ReferencePoints[i] * Scale, CreateRotationZ(yaw));
// with the row-vector CreateRotationZ that rotation is (x*cos - y*sin, x*sin +
// y*cos, z). Out of range (or no model) returns the position, as the C# does.
mdk::Mat4 mdk::scripts::MdkObject::rotation() const {
    /* @vfp-exempt R6 — set_rolling captures the gameplay orientation; LUT approximation would alter the saved rolling basis, so retain libm reference trig. measured n/a. Verified 2026-10-26. */
    const float pi = 3.14159265358979323846f;
    // Match float.DegreesToRadians: multiply by pi before dividing by 180.
    const float roll_radians = roll * pi / 180.0f;
    const float pitch_radians = -pitch * pi / 180.0f;
    const float yaw_radians = yaw * pi / 180.0f;
    const float cr = cosf(roll_radians);
    const float sr = sinf(roll_radians);
    const float cp = cosf(pitch_radians);
    const float sp = sinf(pitch_radians);
    const float cy = cosf(yaw_radians);
    const float sy = sinf(yaw_radians);

    // System.Numerics uses row vectors and composes X(roll) * Y(-pitch) * Z(yaw).
    Mat4 rx = Mat4::identity();
    rx.m[5] = cr;
    rx.m[6] = sr;
    rx.m[9] = -sr;
    rx.m[10] = cr;
    Mat4 ry = Mat4::identity();
    ry.m[0] = cp;
    ry.m[2] = -sp;
    ry.m[8] = sp;
    ry.m[10] = cp;
    Mat4 rz = Mat4::identity();
    rz.m[0] = cy;
    rz.m[1] = sy;
    rz.m[4] = -sy;
    rz.m[5] = cy;

    // System.Numerics multiplies each row by the right-hand matrix with one
    // rounded product followed by three fused multiply-adds; mdk::multiply is
    // that arithmetic, shared with MdkObject::transform().
    return mdk::multiply(mdk::multiply(rx, ry), rz);
}

// The C#'s Transform (MdkObject.cs:287): scale, then the orientation (a
// rolling object turns by the basis captured when it started rolling), then
// the translation. The order is the C#'s, left to right, because matrix
// multiplication is not commutative and the last bit depends on it.
mdk::Mat4 mdk::scripts::MdkObject::transform() const {
    const mdk::Mat4 orientation = (flags & FlagRolling) != 0 ? rolling_basis : rotation();
    const mdk::Mat4 lifted = mdk::create_translation(position.x,
                                                     position.y,
                                                     position.z + lift());
    return mdk::multiply(mdk::multiply(mdk::create_scale(scale), orientation), lifted);
}

namespace {

typedef std::vector<std::vector<mdk::formats::Vec3f> > ObjectPose;

// The C#'s static RestPoses and Baked, shared by every object because a level
// has a handful of models drawn by many of them. std::map, not std::vector:
// pose_parts() hands out a reference into these, and a map's elements do not
// move when later entries are inserted. The C# shares the model's own vertex
// arrays; the port copies them once into the cache, which is the same read-only
// data. Single-threaded: the C#'s lock guards its parallel test runner, and the
// port ticks one world at a time.
std::map<const mdk::formats::Model*, ObjectPose> g_restPoses;
std::map<std::pair<const mdk::formats::Model*, const mdk::formats::ModelAnimation*>,
         std::vector<ObjectPose> > g_baked;
const ObjectPose g_noPose;

const std::vector<ObjectPose>& baked(const mdk::formats::Model* model,
                                     const mdk::formats::ModelAnimation* animation)
{
    const std::pair<const mdk::formats::Model*, const mdk::formats::ModelAnimation*> key(model, animation);
    std::map<decltype(key), std::vector<ObjectPose> >::iterator found = g_baked.find(key);
    if (found != g_baked.end()) {
        return found->second;
    }
    // The C# bakes the whole animation once per (model, animation) and keeps
    // every frame, so nothing is baked while the game plays.
    return g_baked.emplace(key, animation->bake(*model)).first->second;
}

} // namespace

const std::vector<std::vector<mdk::formats::Vec3f> >& mdk::scripts::MdkObject::pose_parts() const {
    if (model == nullptr) {
        return g_noPose;
    }
    if (animation == nullptr) {
        std::map<const mdk::formats::Model*, ObjectPose>::iterator found = g_restPoses.find(model);
        if (found != g_restPoses.end()) {
            return found->second;
        }
        return g_restPoses.emplace(model, model->restPose()).first->second;
    }
    const std::vector<ObjectPose>& frames = baked(model, animation);
    if (frames.empty()) {
        return g_noPose;
    }
    // The C# indexes with AnimationFrame and would throw out of range; the
    // clamp is the same guard ScriptRuntime::poseBounds already uses, and it
    // cannot be reached from a real animation (the frame counter is taken
    // modulo the frame count).
    int32_t frame = animation_frame;
    if (frame < 0) {
        frame = 0;
    }
    if (frame >= static_cast<int32_t>(frames.size())) {
        frame = static_cast<int32_t>(frames.size()) - 1;
    }
    return frames[static_cast<size_t>(frame)];
}

void mdk::scripts::forgetPoseCaches() {
    g_restPoses.clear();
    g_baked.clear();
}

void mdk::scripts::MdkObject::forget_poses() {
    // The C#'s own name for the drop; the caches live in this translation unit
    // and scripts/pose_cache.h is the dependency-free handle the game layer
    // (LevelData::load) calls.
    forgetPoseCaches();
}

void mdk::scripts::MdkObject::set_rolling(bool enabled) {
    if (!enabled) {
        flags &= ~FlagRolling;
        return;
    }
    if ((flags & FlagRolling) == 0) {
        flags |= FlagRolling;
        rolling_basis = rotation();
    }
}

// ObjectMotion.Roll (0x4602c8): the model turns by distance / radius radians
// about the horizontal axis ACROSS the motion -- `axis` is the motion turned 90
// degrees in the plane, which is the axis a ball touches the ground along. The
// distance is the horizontal one only (a Vector2), so a boulder rolling off a
// ledge does not spin from the fall. A zero move leaves the basis alone.
//
// The trig is the same libm call rotation() makes through create_from_axis_angle
// (the LUT would move the boundary the basis is built at, and rotation() is
// exempted for the same reason).
void mdk::scripts::MdkObject::update_rolling() {
    const float dx = position.x - previous_position.x;
    const float dy = position.y - previous_position.y;
    // Vector2.Length: the sum of the squares, then one sqrt.
    const float distance = sqrtf(dx * dx + dy * dy);
    if (distance <= 0.0f) {
        return;
    }
    const float radius = roll_radius > 0.0f ? roll_radius : 1.0f;
    // The C# divides the axis vector by the distance componentwise, and
    // CreateFromAxisAngle does NOT normalize, so this is the axis it uses.
    const mdk::Mat4 turn =
        mdk::create_from_axis_angle(-dy / distance, dx / distance, 0.0f, distance / radius);
    rolling_basis = mdk::orthonormal(mdk::multiply(rolling_basis, turn));
}

mdk::Vec3 mdk::scripts::MdkObject::reference_point(int index) const {
    if (model == nullptr || index < 0 ||
        static_cast<size_t>(index) >= model->referencePoints.size()) {
        return position;
    }
    const mdk::formats::Vec3f& p = model->referencePoints[static_cast<size_t>(index)];
    float x = p.x * scale;
    float y = p.y * scale;
    float z = p.z * scale;
    // float.DegreesToRadians is left-associated (yaw * pi / 180f).
    float radians = yaw * 3.14159265358979323846f / 180.0f;
    /* @vfp-exempt R6 — the attach motion's reference-point rotation (per attached
     * object per tick); glibc cosf/sinf match the C# oracle's MathF.Cos/Sin
     * bit-for-bit on this machine (probed), and the conversion is .NET's
     * left-associative spelling; the fastSin/fastCos LUT's error is far larger
     * and would move the attached object's position. measured n/a (corpus
     * empty). Verified 2026-10-23. */
    float c = cosf(radians);
    float s = sinf(radians);
    return Vec3{position.x + (x * c - y * s), position.y + (x * s + y * c), position.z + z};
}

mdk::Vec3 mdk::scripts::MdkObject::partCenter(int index) const {
    if (index < 0 || model == nullptr ||
        static_cast<size_t>(index) >= model->partList.size()) {
        return position;
    }
    const mdk::formats::Box& box = model->partList[static_cast<size_t>(index)].bounds;
    // The C#'s (bounds.Min + bounds.Max) / 2f * Scale: the mean scaled after.
    float centreX = (box.min.x + box.max.x) * 0.5f * scale;
    float centreY = (box.min.y + box.max.y) * 0.5f * scale;
    float centreZ = (box.min.z + box.max.z) * 0.5f * scale;
    // float.DegreesToRadians is left-associated (yaw * pi / 180f).
    float radians = yaw * 3.14159265358979323846f / 180.0f;
    float c = cosf(radians);
    float s = sinf(radians);
    return Vec3{position.x + (centreX * c - centreY * s),
                position.y + (centreX * s + centreY * c), position.z + centreZ};
}

int32_t mdk::scripts::MdkObject::findPart(const std::string& name) const {
    if (model == nullptr) {
        return -1;
    }
    for (size_t i = 0; i < model->partList.size(); i++) {
        const std::string partName = model->partList[i].nameString();
        if (partName.size() != name.size()) {
            continue;
        }
        bool same = true;
        for (size_t k = 0; k < name.size(); k++) {
            char a = partName[k], b = name[k];
            if (a >= 'A' && a <= 'Z') {
                a = (char)(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z') {
                b = (char)(b - 'A' + 'a');
            }
            if (a != b) {
                same = false;
                break;
            }
        }
        if (same) {
            return (int32_t)i;
        }
    }
    return -1;
}

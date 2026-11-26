#include "scripts/object.h"

#include <cmath>
#include <vector>

#include "formats/model.h"
#include "game/frame_blend.h"
#include "formats/model_animation.h"
#include "scripts/pose_cache.h"
#include "scripts/script_runtime.h"

// The pose caches are NOT here. They are in scripts/pose_cache.cpp, which is
// the translation unit whose header the game layer is allowed to call; see that
// file for why the split is a link-time requirement and not a tidiness fix.

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

// The C#'s RotationOf(roll, pitch, yaw) (MdkObject.cs:280-285): the orientation
// from the angles it is handed, so rotation() can pass the object's own and
// transform_at() can pass the blended ones. The row-vector basis is the C#'s:
// CreateRotationX(roll) * CreateRotationY(-pitch) * CreateRotationZ(yaw), and the
// angles are degrees (see the comment on the three matrices below).
mdk::Mat4 mdk::scripts::MdkObject::rotation_of(float roll, float pitch, float yaw) {
    /* @vfp-exempt R6 — set_rolling captures the gameplay orientation and
     * transform_at() turns by the blended copy of it, both of which decide where
     * a model is drawn; LUT approximation would alter the saved rolling basis and
     * every blended frame, so retain libm reference trig. measured n/a. Verified
     * 2026-10-31. */
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

mdk::Mat4 mdk::scripts::MdkObject::rotation() const {
    return rotation_of(roll, pitch, yaw);
}

// The C#'s Remember(): the pose at the start of the tick, which is what the
// frames until the next tick are drawn from. The arena goes with it because a
// connector moves an object between arenas and the C# refuses to blend across
// that (Blends, MdkObject.cs:300). The arena name is copied, not referenced: the
// C# holds the string and an arena's name outlives nothing that could dangle it.
// Every shipped arena name is 7 characters or less, so the copy stays inside
// std::string's small-buffer and the per-tick remember() allocates nothing.
void mdk::scripts::MdkObject::remember() {
    ticked_.position = position;
    ticked_.yaw = yaw;
    ticked_.pitch = pitch;
    ticked_.roll = roll;
    ticked_.arena = arena;
    ticked_.animation = animation;
    ticked_.animation_time = animation_time;
    has_ticked_ = true;
}

// The C#'s Blends(). alpha >= 1 is the frame that IS the tick's pose; a new
// object has no snapshot; a jump is a teleport, which is drawn where it ends and
// not on the way (see game/frame_blend.h for why that test is load-bearing); and
// an object that changed arena since the snapshot is drawn where it arrived.
bool mdk::scripts::MdkObject::blends(float alpha) const {
    return alpha < 1.0f && has_ticked_ && ticked_.arena == arena &&
           !mdk::frame_blend::jumped(ticked_.position, position, scripts::ScriptRuntime::kTick);
}

bool mdk::scripts::MdkObject::frames_at(float alpha, const std::vector<std::vector<formats::Vec3f> >** from,
                                        const std::vector<std::vector<formats::Vec3f> >** to,
                                        float* share) const {
    if (!blends(alpha) || model == nullptr || animation == nullptr || animation->frameCount <= 0 ||
        ticked_.animation != animation) {
        return false;
    }
    const int32_t count = animation->frameCount;
    const float start = ticked_.animation_time;
    float end = animation_time;
    if (end < start && (flags & FlagLoop) != 0) {
        end += (float)count;
    }
    // A restart, or a frame a script set (the frame is not the time's): drawn as
    // it is. The C#'s `% count`s, without a divide (AGENTS.md R11): the times lie
    // within two counts, and anything outside is drawn as it is instead.
    int32_t rounded = (int32_t)std::rint(animation_time);
    if (rounded >= count) {
        rounded -= count;
    }
    if (end < start || rounded != animation_frame) {
        return false;
    }
    const float time = mdk::frame_blend::lerp(start, end, alpha);
    const int32_t frame = (int32_t)time;
    if (frame < 0 || frame >= 2 * count) {
        return false;
    }
    const int32_t first = frame >= count ? frame - count : frame;
    const int32_t second = first + 1 >= count ? first + 1 - count : first + 1;
    *from = &bakedPose(model, animation, first);
    *to = &bakedPose(model, animation, second);
    *share = time - (float)frame;
    return (*from)->size() == (*to)->size();
}

mdk::Vec3 mdk::scripts::MdkObject::position_at(float alpha) const {
    if (!blends(alpha)) {
        return position;
    }
    return mdk::frame_blend::lerp(ticked_.position, position, alpha);
}

float mdk::scripts::MdkObject::yaw_at(float alpha) const {
    if (!blends(alpha)) {
        return yaw;
    }
    return mdk::frame_blend::lerp_angle(ticked_.yaw, yaw, alpha);
}

// The C#'s TransformAt(alpha) (MdkObject.cs:306-318). The scale and the lift are
// NOT blended -- the C# reads Scale and Lift off the object as it is now, and only
// the place and the three angles come from between the ticks. A rolling object
// keeps its basis: the C# reads RollingBasis as it is now, because the basis is
// re-derived from the movement every tick and there is no previous copy of it to
// blend.
mdk::Mat4 mdk::scripts::MdkObject::transform_at(float alpha) const {
    if (!blends(alpha)) {
        return transform();
    }
    const mdk::Mat4 orientation =
        (flags & FlagRolling) != 0
            ? rolling_basis
            : rotation_of(mdk::frame_blend::lerp_angle(ticked_.roll, roll, alpha),
                          mdk::frame_blend::lerp_angle(ticked_.pitch, pitch, alpha),
                          mdk::frame_blend::lerp_angle(ticked_.yaw, yaw, alpha));
    const mdk::Vec3 at = position_at(alpha);
    const mdk::Mat4 lifted = mdk::create_translation(at.x, at.y, at.z + lift());
    return mdk::multiply(mdk::multiply(mdk::create_scale(scale), orientation), lifted);
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

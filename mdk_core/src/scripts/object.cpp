#include "scripts/object.h"

#include <cmath>

#include "formats/model.h"

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

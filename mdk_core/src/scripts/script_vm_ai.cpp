// ============================================================================
// script_vm_ai.cpp — the script VM's aim / turn / face opcode handlers.
//
// These are the opcodes that point an object at its target (the C#'s
// ScriptVm.Aim / TurnToTarget / FaceMode and the distance/ground probes). They
// were cases in ScriptVm::execute; they move here unchanged (a switch `break`
// becomes `return ins.next`) so the aim math lives in one cohesive file. The
// dispatch stays the flat switch in script_vm.cpp (it compiles to a jump table);
// these are direct calls, not a second dispatch.
//
// The @vfp-exempt R6 pins travel with their bodies: this is gameplay aim, and a
// LUT would move the aim boundary. See AGENTS.md 5.
// ============================================================================

#include <math.h>

#include "scripts/script_runtime.h"
#include "scripts/script_vm.h"
#include "scripts/script_vm_internal.h"

namespace mdk {
namespace scripts {

using namespace detail;

// aim_target (101) / aim_target_inaccurate (104): snap the yaw to the target and
// set the pitch to the chest; 104 adds an RNG error to yaw and pitch.
int32_t ScriptVm::opAimTarget(MdkObject* obj, const Instruction& ins) {
    const float kAimHeight = 3.0f;
    const float kMinAcross = 2.0f;
    const float kPerfect = 100.0f;
    const float kPitchErrorScale = 4.0f;
    const float accuracy = ins.opcode == 101 ? kPerfect : fval(op(ins, 0));
    const bool always = ins.opcode == 104;
    const Vec3 target = runtime_.targetPosition();
    const float dx = target.x - obj->position.x;
    const float dy = target.y - obj->position.y;
    const float dz = target.z - obj->position.z;
    /* @vfp-exempt R6 — aim_target's heading (per aiming object per tick);
     * glibc atan2f/sqrtf match the C# oracle's MathF calls bit-for-bit on
     * this machine (probed), and the conversion is .NET's left-associative
     * spelling; the fastAtan2 LUT's error is far larger and would move the
     * aim. measured n/a (corpus empty). Verified 2026-10-24. */
    const float horizontal = sqrtf(dx * dx + dy * dy);
    if (always || horizontal > kMinAcross) {
        obj->yaw = runtime_.yawTo(obj, target);
    }
    obj->pitch = atan2f(target.z + kAimHeight - obj->position.z, horizontal) * 180.0f /
                 3.14159265358979323846f;
    if (accuracy == kPerfect) {
        return ins.next;
    }
    const float distance = sqrtf(dx * dx + dy * dy + dz * dz) + 0.1f;
    obj->yaw = wrap360(obj->yaw + (float)(runtime_.rng().next(20) - 10) *
                                                 (kPerfect - accuracy) / distance);
    obj->pitch += (float)(runtime_.rng().next(20) - 10) * (kPerfect - accuracy) /
                  (distance * kPitchErrorScale);
    return ins.next;
}

// turn_to_target_offset (235) / turn_to_target_offset3d (105): turn toward a
// point offset from the target in the target's frame; 105 adds a z offset and
// pitches.
int32_t ScriptVm::opTurnToTarget(MdkObject* obj, const Instruction& ins) {
    const float kPerfect = 100.0f;
    const float kPitchErrorScale = 4.0f;
    const Vec3 target = runtime_.targetPosition();
    const float rate = fval(op(ins, 0));
    const float accuracy = fval(op(ins, 1));
    const float a = fval(op(ins, 2));
    const float b = fval(op(ins, 3));
    /* @vfp-exempt R6 — turn_to_target_offset's heading (per turning
     * object per tick); glibc sinf/cosf/atan2f/sqrtf match the C#
     * oracle's MathF calls bit-for-bit on this machine (probed), and
     * the conversions are .NET's left-associative spellings; the
     * fastAtan2 LUT's error is far larger and would move the aim.
     * measured n/a (corpus empty). Verified 2026-10-24. */
    const float tRad = runtime_.targetYaw() * 3.14159265358979323846f / 180.0f;
    const float s = sinf(tRad);
    const float c = cosf(tRad);
    Vec3 point;
    point.x = target.x - (a * c + b * s);
    point.y = target.y - (b * c + a * s);
    point.z = target.z;
    const float dx = point.x - obj->position.x;
    const float dy = point.y - obj->position.y;
    const float horizontal = sqrtf(dx * dx + dy * dy);
    float yaw = runtime_.yawTo(obj, point);
    if (accuracy != kPerfect) {
        yaw += (float)(runtime_.rng().next(20) - 10) * (kPerfect - accuracy) /
               (1.0f + horizontal);
    }
    const float maxStep = rate * kDt;
    // Math.Clamp spelled with the C#'s comparisons: a NaN step stays NaN.
    float step = wrapAngle(yaw - obj->yaw);
    step = step < -maxStep ? -maxStep : (step > maxStep ? maxStep : step);
    obj->yaw = wrap360(obj->yaw + step);
    if (ins.opcode == 235) {
        return ins.next;
    }
    const float z = fval(op(ins, 4));
    /* @vfp-exempt R6 — turn_to_target_offset3d's pitch (per turning
     * object per tick); glibc atan2f matches the C# oracle's MathF call
     * bit-for-bit on this machine (probed), and the conversion is .NET's
     * left-associative spelling; the fastAtan2 LUT's error is far larger
     * and would move the aim. measured n/a (corpus empty). Verified 2026-10-24. */
    float pitch = atan2f(target.z + z - obj->position.z + obj->height_offset, horizontal) *
                  180.0f / 3.14159265358979323846f;
    if (accuracy != kPerfect) {
        pitch += (float)(runtime_.rng().next(20) - 10) * (kPerfect - accuracy) /
                 (1.0f + horizontal * kPitchErrorScale);
    }
    float pstep = wrapAngle(pitch - obj->pitch);
    pstep = pstep < -maxStep ? -maxStep : (pstep > maxStep ? maxStep : pstep);
    obj->pitch += pstep;
    return ins.next;
}

// turn_to_yaw (207): turn toward a goal yaw by at most rate*Dt; branches when reached.
int32_t ScriptVm::opTurnToYaw(MdkObject* obj, const Instruction& ins) {
    float goal = fval(op(ins, 1));
    float diff = wrapAngle(goal - obj->yaw);
    float step = fval(op(ins, 0)) * kDt;
    float sign = diff > 0.0f ? 1.0f : (diff < 0.0f ? -1.0f : 0.0f);
    obj->yaw = wrap360(fabsf(diff) <= step ? goal : obj->yaw + sign * step);
    return branch(obj, ins, fabsf(obj->yaw - wrap360(goal)) < 1e-4f);
}

// face_mode (193): 1/2 face the velocity (2 also pitches), 3 copies another's yaw.
int32_t ScriptVm::opFaceMode(MdkObject* obj, const Instruction& ins) {
    const std::vector<ScriptValue>& face = listval(op(ins, 0));
    const int32_t mode = face.empty() ? 0 : ival(face[0]);
    if ((mode == 1 || mode == 2) && obj->velocity.x != 0.0f && obj->velocity.y != 0.0f) {
        /* @vfp-exempt R6 — face_mode turn (per facing alien per tick); glibc
         * atan2f/sinf/cosf/sqrtf match the C# oracle's MathF calls
         * bit-for-bit on this machine (probed), and the conversions are
         * .NET's left-associative spellings; the fastAtan2 LUT's error is
         * far larger and would move those values. measured n/a (corpus
         * empty). Verified 2026-10-23. */
        obj->yaw = wrap360(atan2f(obj->velocity.y, obj->velocity.x) * 180.0f /
                           3.14159265358979323846f);
        if (mode == 2 && obj->velocity.z != 0.0f) {
            const float horiz =
                sqrtf(obj->velocity.x * obj->velocity.x + obj->velocity.y * obj->velocity.y);
            obj->pitch = atan2f(obj->velocity.z, horiz) * 180.0f / 3.14159265358979323846f;
        }
    } else if (mode == 3 && face.size() > 1) {
        MdkObject* other = runtime_.firstOther(obj, strval(face[1]));
        if (other != nullptr) {
            obj->yaw = other->yaw;
        }
    }
    return ins.next;
}

// if_dest_dist (54): distance to the move destination (2D when on a floor).
int32_t ScriptVm::opIfDestDist(MdkObject* obj, const Instruction& ins) {
    Vec3 toDestination;
    toDestination.x = obj->move_destination.x - obj->position.x;
    toDestination.y = obj->move_destination.y - obj->position.y;
    toDestination.z = (obj->contact_flags & MdkObject::ContactFloor) != 0
                          ? 0.0f
                          : obj->move_destination.z - obj->position.z;
    return branch(obj, ins, compare(sqrtf(toDestination.x * toDestination.x +
                                          toDestination.y * toDestination.y +
                                          toDestination.z * toDestination.z),
                                    listval(op(ins, 0))));
}

// if_no_floor_at (236): no up-facing face below a point ahead.
int32_t ScriptVm::opIfNoFloorAt(MdkObject* obj, const Instruction& ins) {
    const float kProbeTop = 3.0f;
    const float kDefaultDrop = 6.0f;
    const float dx = fval(op(ins, 0));
    const float dy = fval(op(ins, 1));
    const float depth = fval(op(ins, 2));
    /* @vfp-exempt R6 — the probe offset's rotation (per probing object per
     * tick); glibc sinf/cosf match the C#'s MathF.SinCos bit-for-bit on this
     * machine (probed), and the conversion is .NET's left-associative
     * spelling; a LUT would move the probe point. measured n/a (corpus
     * empty). Verified 2026-10-23. */
    const float s = sinf(obj->yaw * 3.14159265358979323846f / 180.0f);
    const float c = cosf(obj->yaw * 3.14159265358979323846f / 180.0f);
    Vec3 top;
    top.x = obj->position.x + (-dx * c - dy * s);
    top.y = obj->position.y + (-dy * c - dx * s);
    top.z = obj->position.z + kProbeTop;
    const float bottom = depth != 0.0f ? obj->position.z + depth : top.z - kDefaultDrop;
    Vec3 down = top;
    down.z = bottom;
    return branch(obj, ins, !runtime_.crossesFloor(obj->arena, top, down));
}

} // namespace scripts
} // namespace mdk

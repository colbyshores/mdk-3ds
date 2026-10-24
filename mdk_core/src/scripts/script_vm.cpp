// ============================================================================
// script_vm.cpp — see script_vm.h.
// ============================================================================

#include "scripts/script_vm.h"

#include <math.h>

#include <algorithm>

#include "formats/model.h"
#include "scripts/script_runtime.h"

namespace mdk {
namespace scripts {

using formats::BranchAction;
using formats::Instruction;
using formats::ScriptValue;
using formats::ScriptValueKind;

const float ScriptVm::kDt = 1.0f / 30.0f;
const float ScriptVm::kEpsilon = 0.05f;
const float ScriptVm::kMinWait = 1e-5f;

namespace {

const ScriptValue kNull;
const std::vector<ScriptValue> kEmpty;
const std::string kEmptyString;

// A numeric operand as a float (the C#'s ScriptMath.F).
float fval(const ScriptValue& v) {
    if (v.kind == ScriptValueKind::Float) {
        return v.floatValue;
    }
    if (v.kind == ScriptValueKind::Int) {
        return (float)v.intValue;
    }
    return 0.0f;
}

// A numeric operand as an int (the C#'s ScriptMath.I).
int32_t ival(const ScriptValue& v) {
    if (v.kind == ScriptValueKind::Int) {
        return v.intValue;
    }
    if (v.kind == ScriptValueKind::Float) {
        return (int32_t)v.floatValue;
    }
    return 0;
}

// The C#'s MathF.Max: NaN-propagating. fmaxf is NOT — it returns the numeric side
// when exactly one argument is NaN, so a NaN operand would silently become finite.
float mathMax(float a, float b) {
    if (a != a) {
        return a;
    }
    if (b != b) {
        return b;
    }
    return a > b ? a : b;
}

// The C#'s MathF.Min: NaN-propagating, for the same reason as mathMax.
float mathMin(float a, float b) {
    if (a != a) {
        return a;
    }
    if (b != b) {
        return b;
    }
    return a < b ? a : b;
}

const std::vector<ScriptValue>& listval(const ScriptValue& v) {
    return v.kind == ScriptValueKind::List ? v.items : kEmpty;
}

const std::string& strval(const ScriptValue& v) {
    return v.kind == ScriptValueKind::String ? v.stringValue : kEmptyString;
}

// An instruction's operand, or the null operand when the index is past the end.
const ScriptValue& op(const Instruction& ins, int32_t i) {
    return (i >= 0 && (size_t)i < ins.operands.size()) ? ins.operands[i] : kNull;
}

// The position operand: the first three operands as a vector (the C#'s V(o)).
Vec3 vpos(const Instruction& ins) {
    Vec3 out;
    out.x = fval(op(ins, 0));
    out.y = fval(op(ins, 1));
    out.z = fval(op(ins, 2));
    return out;
}

float wrap360(float degrees) {
    return degrees - 360.0f * floorf(degrees / 360.0f);
}

// Wrap to [-180, 180) (ScriptMath.WrapAngle).
float wrapAngle(float degrees) {
    return degrees - 360.0f * floorf((degrees + 180.0f) / 360.0f);
}

// MdkObject.IsAnimationDone: no animation, or the hold frame marked ended.
bool isAnimDone(const MdkObject& obj) {
    return obj.animation == nullptr || obj.animation_end_frame == MdkObject::AnimationEnded;
}

bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

} // namespace

ScriptVm::ScriptVm(ScriptRuntime& runtime, formats::ScriptDecoder& decoder)
    : runtime_(runtime), decoder_(decoder) {}

void ScriptVm::run(MdkObject* obj) {
    if (obj->restart == 0 || obj->dead) {
        return;
    }

    runtime_.selectTarget(obj);
    int32_t pc = obj->restart;
    if (obj->wait_time > 0.0f) {
        obj->wait_time -= kDt;
        if (obj->wait_time > 0.0f) {
            return;
        }
        obj->wait_time = 0.0f;
        pc = obj->wait_resume;
    }

    int32_t count = 0;
    while (true) {
        Instruction ins;
        if (!decoder_.decode(pc, &ins)) {
            obj->restart = 0;
            return;
        }
        if (ins.opcode == formats::ScriptDecoder::kEnd) {
            obj->hit_event = 0;
            return;
        }

        count++;
        opcodeCount_ = count;
        if (count > kMaxOpcodesPerFrame) {
            obj->restart = 0;
            return;
        }

        pc = execute(obj, ins);
        if (pc == kYield || obj->dead) {
            return;
        }
    }
}

int32_t ScriptVm::branch(MdkObject* obj, const Instruction& ins, bool condition) {
    if (!ins.hasAction) {
        return ins.next;
    }
    switch (ins.action.type) {
        case BranchAction::Kind::Goto:
            return condition ? gotoTarget(obj, ins.action.target) : ins.next;
        case BranchAction::Kind::Gosub:
            return condition ? gosub(obj, ins.next, ins.action.target) : ins.next;
        case BranchAction::Kind::GosubElse:
            return gosub(obj, ins.next, condition ? ins.action.target : ins.action.elseTarget);
        case BranchAction::Kind::Return:
            return condition ? returnFrom(obj) : ins.next;
        default:
            return ins.next;
    }
}

int32_t ScriptVm::gotoTarget(MdkObject* obj, int32_t target) {
    if (target == 0) {
        obj->restart = 0;
        return kYield;
    }
    obj->restart = target;
    obj->level_timers[obj->gosub_returns.size()] = 0.0f;
    return target;
}

int32_t ScriptVm::gosub(MdkObject* obj, int32_t returnPc, int32_t target) {
    if ((int32_t)obj->gosub_returns.size() >= kGosubDepth || target == 0) {
        obj->restart = 0;
        return kYield;
    }
    obj->gosub_returns.push_back(returnPc);
    obj->gosub_restarts.push_back(obj->restart);
    obj->restart = target;
    obj->level_timers[obj->gosub_returns.size()] = 0.0f;
    return target;
}

int32_t ScriptVm::returnFrom(MdkObject* obj) {
    if (obj->gosub_returns.empty()) {
        obj->restart = 0;
        return kYield;
    }
    int32_t restart = obj->gosub_restarts.back();
    obj->gosub_restarts.pop_back();
    int32_t ret = obj->gosub_returns.back();
    obj->gosub_returns.pop_back();
    obj->restart = restart;
    return ret;
}

void ScriptVm::clearStack(MdkObject* obj) {
    obj->gosub_returns.clear();
    obj->gosub_restarts.clear();
}

float ScriptVm::value(MdkObject* obj, const ScriptValue& operand) {
    if (operand.kind != ScriptValueKind::Variable) {
        return fval(operand);
    }
    float* vars = variables(obj, operand.variable.kind);
    int32_t index = operand.variable.index;
    return vars[(index >= 0 && index < kVariableCount) ? index : 0];
}

float* ScriptVm::variables(MdkObject* obj, int32_t source) {
    switch (source) {
        case 0:
            return runtime_.globalVariables.data();
        case 1:
            return runtime_.getArenaState(obj->arena).variables.data();
        case 2:
            return obj->variables.data();
        default:
            if (obj->linked != nullptr) {
                return obj->linked->variables.data();
            }
            // The C# NoVariables() clears its one scratch array on EVERY access, so a
            // write to a missing linked object's variable is discarded, not kept. Clear
            // it here too: without this, add_var/add_var_dt (+=) would accumulate forever
            // in the shared scratch array where the oracle zeroes it each time.
            for (int i = 0; i < kVariableCount; i++) {
                noVariables_[i] = 0.0f;
            }
            return noVariables_.data();
    }
}

bool ScriptVm::isDoor(const MdkObject& obj) {
    return (obj.flags & MdkObject::FlagDoor) != 0;
}

int32_t ScriptVm::getFlags(MdkObject* obj, int32_t source) {
    switch (source) {
        case 0:
            return runtime_.globalFlags;
        case 1:
            return runtime_.getArenaState(obj->arena).flags;
        case 2:
            return obj->script_flags;
        case kDoorFlags:
            return isDoor(*obj) ? obj->door_state : 0;
        default:
            return obj->linked ? obj->linked->script_flags : 0;
    }
}

void ScriptVm::setFlags(MdkObject* obj, int32_t source, int32_t value) {
    switch (source) {
        case 0:
            runtime_.globalFlags = value;
            break;
        case 1:
            runtime_.getArenaState(obj->arena).flags = value;
            break;
        case 2:
            obj->script_flags = value;
            break;
        case kDoorFlags:
            if (isDoor(*obj)) {
                obj->door_state = value;
            }
            break;
        default:
            if (obj->linked) {
                obj->linked->script_flags = value;
            }
            break;
    }
}

bool ScriptVm::compare(float value, const std::vector<ScriptValue>& cond) {
    if (cond.size() < 2) {
        return false;
    }
    const ScriptValue& second = cond.size() > 2 ? cond[2] : kNull;
    return compareOps(value, cond[0], cond[1], second);
}

bool ScriptVm::compareOps(float value, const ScriptValue& opnd, const ScriptValue& first,
                          const ScriptValue& second) {
    float a = fval(first);
    switch (ival(opnd)) {
        case 1: return value < a;
        case 2: return value > a;
        case 3: return value < a + kEpsilon;
        case 4: return value > a - kEpsilon;
        case 5: return fabsf(value - a) < kEpsilon;
        case 6: return fabsf(value - a) >= kEpsilon;
        case 7: return value >= a && value <= fval(second);
        case 8: return value <= a || value >= fval(second);
        default: return false;
    }
}

void ScriptVm::noteUnimplemented(int32_t opcode) {
    for (size_t i = 0; i < unimplemented_.size(); i++) {
        if (unimplemented_[i].first == opcode) {
            unimplemented_[i].second++;
            return;
        }
    }
    unimplemented_.push_back(std::make_pair(opcode, 1));
}

// command_objects (opcode 4), the operand parsing half (C# ScriptVm.CommandObjects):
// the command operand, then for command 7 a branch action whose target is the
// commanded script offset (or for command 43 a near-target offset), then the
// selector and the selector's args.
void ScriptVm::commandObjects(MdkObject* obj, const Instruction& ins) {
    const int32_t kGotoCommand = 7;
    const int32_t kGosubCommand = 0xFC;
    const int32_t kMoveCommand = 43;
    const ScriptValue& commandOperand = op(ins, 0);
    const ScriptValue& actionOperand = op(ins, 1);
    const std::vector<ScriptValue>& args = listval(op(ins, 3));

    int32_t command = ival(commandOperand);
    const int32_t selector = ival(op(ins, 2));
    int32_t target = 0;
    Vec3 destination;
    destination.x = 0.0f;
    destination.y = 0.0f;
    destination.z = 0.0f;
    if (command == kGotoCommand) {
        if (actionOperand.kind != ScriptValueKind::Action) {
            return;
        }
        switch (actionOperand.action.type) {
            case BranchAction::Kind::Gosub:
                command = kGosubCommand;
                target = actionOperand.action.target;
                break;
            case BranchAction::Kind::Goto:
            case BranchAction::Kind::GosubElse:
                target = actionOperand.action.target;
                break;
            default:
                return;
        }
    } else if (command == kMoveCommand) {
        const std::vector<ScriptValue>& offset = listval(actionOperand);
        destination = runtime_.nearTargetDestination(obj, fval(offset.size() > 0 ? offset[0] : kNull),
                                                      fval(offset.size() > 1 ? offset[1] : kNull));
    }

    size_t k = 0;
    float param = 0.0f;
    std::string type;
    int32_t id = 0;
    if (selector == 6 || selector == 10) {
        param = fval(k < args.size() ? args[k] : kNull);
        k++;
    }
    if (selector == 2 || selector == 4 || selector == 5 || selector == 6 || selector == 7 ||
        selector == 10) {
        type = strval(k < args.size() ? args[k] : kNull);
        k++;
    }
    if (selector == 5) {
        id = ival(k < args.size() ? args[k] : kNull);
    }
    runtime_.commandObjects(obj, command, target, destination, selector, type, param, id);
}

// CountOthers (ScriptVm.cs:311): the objects of the same arena whose type matches
// (case-insensitively, as the C# Is), alive for if_count_alive or commandable for
// if_count_objects.
int32_t ScriptVm::countOthers(MdkObject* obj, const ScriptValue& type, bool alive) {
    const std::deque<MdkObject>& objects = runtime_.objects();
    const std::string typeName = strval(type);
    int32_t count = 0;
    for (std::deque<MdkObject>::const_iterator it = objects.begin(); it != objects.end(); ++it) {
        const MdkObject& other = *it;
        if (&other == obj || other.dead || other.arena != obj->arena ||
            !iequals(other.type_name, typeName)) {
            continue;
        }
        if (alive ? other.health > 0 : ScriptRuntime::mayCommand(obj, &other)) {
            count++;
        }
    }
    return count;
}

// The parts mask of a set_parts_mask / set_parts_show operand (the C#'s PartsMask).
static int32_t partsMask(MdkObject* obj, const std::vector<ScriptValue>& entries, int32_t mask,
                         bool hide) {
    if (obj->model == nullptr) {
        return mask;
    }
    for (size_t e = 0; e < entries.size(); e++) {
        const std::vector<ScriptValue>& entry = listval(entries[e]);
        if (entry.empty()) {
            continue;
        }
        const std::string name = strval(entry[0]);
        const std::vector<formats::Model::Part>& parts = obj->model->partList;
        for (size_t i = 0; i < parts.size(); i++) {
            if (!iequals("ALL", name) && !iequals(parts[i].nameString(), name)) {
                continue;
            }
            if (hide) {
                mask |= (1 << i);
            } else if ((obj->locked_parts & (1 << i)) == 0) {
                mask &= ~(1 << i);
            }
        }
    }
    return mask;
}

// A random target of a repeat operand of code offsets ([[target], ...]).
static int32_t randomTarget(Random& rng, const std::vector<ScriptValue>& entries) {
    if (entries.empty()) {
        return 0;
    }
    int32_t pick = rng.next((int)entries.size());
    return ival(listval(entries[pick])[0]);
}

// A weighted random target ([[weight, target], ...]).
static int32_t weightedTarget(Random& rng, const std::vector<ScriptValue>& entries) {
    int32_t total = 0;
    for (size_t i = 0; i < entries.size(); i++) {
        total += ival(listval(entries[i])[0]);
    }
    int32_t r = rng.next(total > 0 ? total : 1);
    for (size_t i = 0; i < entries.size(); i++) {
        r -= ival(listval(entries[i])[0]);
        if (r < 0) {
            return ival(listval(entries[i])[1]);
        }
    }
    return 0;
}

int32_t ScriptVm::execute(MdkObject* obj, const Instruction& ins) {
    switch (ins.opcode) {
        case 4:  // command_objects
            commandObjects(obj, ins);
            break;
        case 1:  // set_restart
            obj->restart = ins.next;
            break;
        case 9:  // stop_script
            obj->restart = 0;
            clearStack(obj);
            return kYield;
        case 12:  // goto (random target)
            return gotoTarget(obj, randomTarget(runtime_.rng(), listval(op(ins, 0))));
        case 94:  // goto_weighted
            return gotoTarget(obj, weightedTarget(runtime_.rng(), listval(op(ins, 0))));
        case 252:  // gosub (random target)
            return gosub(obj, ins.next, randomTarget(runtime_.rng(), listval(op(ins, 0))));
        case 95:  // gosub_weighted
            return gosub(obj, ins.next, weightedTarget(runtime_.rng(), listval(op(ins, 0))));
        case 253:  // return
            return returnFrom(obj);
        case 125:  // clear_gosub_stack
            clearStack(obj);
            break;
        case 64:  // wait
            obj->wait_time = std::max(value(obj, op(ins, 0)), kMinWait);
            obj->wait_resume = ins.next;
            return kYield;
        case 18: {  // if_timer
            size_t level = obj->gosub_returns.size();
            if (fval(op(ins, 0)) * 30.0f > obj->level_timers[level]) {
                obj->level_timers[level] += 1.0f;
                return branch(obj, ins, false);
            }
            obj->level_timers[level] = 0.0f;
            return branch(obj, ins, true);
        }

        // Movement.
        case 78:  // move_to: walk/fly toward the operand position (V(o) = operands 0,1,2)
            runtime_.startMove(obj, 78, vpos(ins));
            break;

        case 88: {  // set_move_x: a straight-line move command (2 = stop and zero the speed)
            const int32_t kStop = 2;
            obj->path = 0;
            if (ival(op(ins, 0)) != kStop) {
                obj->move_command = 88;
                obj->move_parameter = ival(op(ins, 0));
            } else {
                obj->move_command = 0;
                obj->speed = 0.0f;
            }
            break;
        }

        case 93: {  // accel_toward_point: velocity towards a point (Manhattan-normalized)
            Vec3 toPoint;
            toPoint.x = fval(op(ins, 1)) - obj->position.x;
            toPoint.y = fval(op(ins, 2)) - obj->position.y;
            toPoint.z = fval(op(ins, 3)) - obj->position.z;
            const float length = fabsf(toPoint.x) + fabsf(toPoint.y) + fabsf(toPoint.z);
            if (length > 0.0f) {
                obj->velocity.x += toPoint.x / length * fval(op(ins, 0));
                obj->velocity.y += toPoint.y / length * fval(op(ins, 0));
                obj->velocity.z += toPoint.z / length * fval(op(ins, 0));
            }
            break;
        }

        case 200: { // move_to_point: a Manhattan step toward a point (V(o,1)), added to push
            // (0x459555). Unlike move_to this is not a command: it nudges the object each
            // tick it runs and branches true once within Arrived. Gravity leaves Z alone.
            const float kArrived = 0.5f;
            const float kMinDistance = 0.1f;
            Vec3 toPoint;
            toPoint.x = fval(op(ins, 1)) - obj->position.x;
            toPoint.y = fval(op(ins, 2)) - obj->position.y;
            toPoint.z = fval(op(ins, 3)) - obj->position.z;
            if ((obj->flags & MdkObject::FlagGravity) != 0) {
                toPoint.z = 0.0f;
            }
            float distance = fabsf(toPoint.x) + fabsf(toPoint.y) + fabsf(toPoint.z);
            if (distance < kArrived) {
                return branch(obj, ins, true);
            }
            float scale = fval(op(ins, 0)) * kDt / mathMax(distance, kMinDistance);
            float sx = toPoint.x * scale;
            float sy = toPoint.y * scale;
            float sz = toPoint.z * scale;
            // Clamp each axis to |toPoint| so the step cannot overshoot the target.
            float ax = fabsf(toPoint.x);
            float ay = fabsf(toPoint.y);
            float az = fabsf(toPoint.z);
            /* @vfp-exempt R11 — float divide by the compile-time constant kDt, not integer divide; kept as a divide so the push matches the oracle's Clamp(step)/Dt bit-for-bit. measured n/a. Verified 2026-10-23. */
            obj->push.x += (sx < -ax ? -ax : (sx > ax ? ax : sx)) / kDt;
            obj->push.y += (sy < -ay ? -ay : (sy > ay ? ay : sy)) / kDt;
            obj->push.z += (sz < -az ? -az : (sz > az ? az : sz)) / kDt;
            break;
        }

        // Vision.
        case 14:   // if_sees_kurt
        case 57: { // if_not_sees_kurt
            bool sees = runtime_.canSeeKurt(obj, fval(op(ins, 0)), fval(op(ins, 1)));
            return branch(obj, ins, ins.opcode == 14 ? sees : !sees);
        }

        // Flags (set/clear/toggle and the if_flag conditions).
        case 68:  // set_flag
        case 69:  // clear_flag
        case 70: { // toggle_flag
            int32_t source = ival(op(ins, 0));
            int32_t mask = (int32_t)(1u << (ival(op(ins, 1)) & 31));
            int32_t cur = getFlags(obj, source);
            int32_t next = ins.opcode == 68 ? (cur | mask)
                                           : (ins.opcode == 69 ? (cur & ~mask) : (cur ^ mask));
            setFlags(obj, source, next);
            break;
        }
        case 71:  // if_flag_set
        case 72: { // if_flag_clear
            int32_t mask = (int32_t)(1u << (ival(op(ins, 1)) & 31));
            bool set = (getFlags(obj, ival(op(ins, 0))) & mask) != 0;
            return branch(obj, ins, ins.opcode == 71 ? set : !set);
        }
        case 46:  // find_cover_spot
            runtime_.findCoverSpot(obj);
            return branch(obj, ins, obj->move_command == 43);

        // Spawning.
        case 86:   // spawn
            runtime_.spawn(obj, strval(op(ins, 3)), vpos(ins), 0.0f, -1, ival(op(ins, 4)), false);
            break;
        case 161:  // spawn_flagged
            runtime_.spawn(obj, strval(op(ins, 3)), vpos(ins), 0.0f, -1, ival(op(ins, 4)), true);
            break;
        case 230:  // spawn_ex
            runtime_.spawn(obj, strval(op(ins, 5)), vpos(ins), fval(op(ins, 3)), ival(op(ins, 4)),
                           ival(op(ins, 6)), false);
            break;
        case 149:  // spawn_connector: a door to another arena
            runtime_.spawnConnector(obj, strval(op(ins, 5)), vpos(ins), fval(op(ins, 3)),
                                    ival(op(ins, 4)), strval(op(ins, 6)), ival(op(ins, 7)));
            break;
        case 150:  // door_set_anims: opening and closing animations
            obj->door_animations[0] = ival(op(ins, 0));
            obj->door_animations[1] = ival(op(ins, 1));
            break;
        case 151:  // door_set_names: sounds when opening, closing, open, closed
            obj->door_sounds[0] = strval(op(ins, 0));
            obj->door_sounds[1] = strval(op(ins, 1));
            obj->door_sounds[2] = strval(op(ins, 2));
            obj->door_sounds[3] = strval(op(ins, 3));
            break;
        case 152:  // door_set_flags: the low 4 bits are the engine's state
            obj->door_state = (obj->door_state & MdkObject::DoorEngineBits) |
                              (ival(op(ins, 0)) & ~MdkObject::DoorEngineBits);
            break;
        case 153:  // door_set_param: the distance at which the door opens
            obj->door_distance = fval(op(ins, 0));
            break;
        case 113: {  // spawn_relative: at an offset rotated by this object's yaw
            // float.DegreesToRadians is left-associated (yaw * pi / 180f).
            const float rad = obj->yaw * 3.14159265358979323846f / 180.0f;
            /* @vfp-exempt R6 — cold spawn path; the rotation uses libm sinf/cosf
             * because they match the C# oracle's MathF.SinCos bit-for-bit on this
             * machine (probed: .NET's MathF trig routes to the float libm calls),
             * and the conversion is .NET's left-associative spelling; the
             * fastSin/fastCos LUT's error is far larger and would move those
             * values. measured n/a (cold). Verified 2026-10-23. */
            const float s = sinf(rad);
            const float c = cosf(rad);
            const float ox = fval(op(ins, 0));
            const float oy = fval(op(ins, 1));
            Vec3 position;
            position.x = obj->position.x + (ox * c - oy * s);
            position.y = obj->position.y + (ox * s + oy * c);
            position.z = obj->position.z + fval(op(ins, 2));
            runtime_.spawn(obj, strval(op(ins, 3)), position, 0.0f, -1, ival(op(ins, 4)), false);
            break;
        }
        case 2: {  // follow_path: ride a CMI path (positions the object at the path start)
            const int32_t kFlag200 = 0x200;
            int32_t path = ival(op(ins, 0));
            if (path == 0) {
                obj->path = 0;
                break;
            }
            int32_t f1 = ival(op(ins, 1));
            int32_t f2 = ival(op(ins, 2));
            obj->flags = (f1 & 1) != 0 ? (obj->flags | kFlag200) : (obj->flags & ~kFlag200);
            obj->flags = (f1 & 2) != 0 ? (obj->flags | MdkObject::FlagPathPushes)
                                       : (obj->flags & ~MdkObject::FlagPathPushes);
            obj->flags = (f2 & 1) != 0 ? (obj->flags | MdkObject::FlagPathOnce)
                                       : (obj->flags & ~MdkObject::FlagPathOnce);
            float time = (float)ival(op(ins, 3));
            if ((f2 & 2) != 0) {
                obj->path_speed = -1.0f;
                if (ival(op(ins, 3)) == 0) {
                    time = (float)(runtime_.pathKeyFrame(path, runtime_.pathKeyCount(path) - 1) - 1);
                }
            }
            Vec3 origin;
            if (ival(op(ins, 4)) != 0) {
                Vec3 p = runtime_.pathPosition(path, time);
                origin.x = obj->position.x - p.x;
                origin.y = obj->position.y - p.y;
                origin.z = obj->position.z - p.z;
            } else {
                const std::vector<ScriptValue>& v = listval(op(ins, 5));
                origin.x = v.size() > 0 ? fval(v[0]) : 0.0f;
                origin.y = v.size() > 1 ? fval(v[1]) : 0.0f;
                origin.z = v.size() > 2 ? fval(v[2]) : 0.0f;
            }
            runtime_.startPath(obj, path, time, origin);
            break;
        }
        case 100:  // arena_show
            runtime_.showArena(strval(op(ins, 0)));
            break;

        case 111:  // set_instance
            obj->instance_id = ival(op(ins, 0)) & 0xFFFF;
            break;
        case 76:   // set_death_script
            obj->death_script = ival(op(ins, 0));
            break;
        case 16: {  // set_health
            int32_t health = ival(op(ins, 0));
            obj->health = health;
            obj->max_health = health;
            obj->indestructible = health >= ScriptRuntime::kIndestructible;
            if (health == 0) {
                runtime_.kill(obj);
                return kYield;
            }
            break;
        }

        case 25:  // set_string154
        case 26:  // set_string150
            obj->labels[ins.opcode - 25] = strval(op(ins, 0));
            break;
        case 31:  // set_parts_mask
            obj->hidden_parts = partsMask(obj, listval(op(ins, 0)), obj->hidden_parts, true);
            break;
        case 198: {  // set_weak_parts
            obj->flags |= MdkObject::FlagWeakParts;
            obj->weak_prefix = strval(op(ins, 0));
            obj->weak_prefix_length = ival(op(ins, 1));
            obj->part_health.assign(8, ival(op(ins, 2)) & 0xFFFF);
            obj->part_max_health = obj->part_health;
            break;
        }

        // Orientation.
        case 8:  // set_yaw
            obj->yaw = wrap360(fval(op(ins, 0)));
            break;
        case 60:  // face_target: snap the yaw to the heading toward the target
            // C# is obj.YawTo(runtime.TargetPosition): the fmod wrap, not Wrap360.
            obj->yaw = runtime_.yawTo(obj, runtime_.targetPosition());
            break;
        case 101:  // aim_target: Aim(obj, 100, CheckDistance) — snap yaw when more than
        case 104: {  // 2 across, set pitch to the chest; 104 (aim_target_inaccurate)
            // always snaps the yaw and adds an RNG error to yaw and pitch.
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
                break;
            }
            const float distance = sqrtf(dx * dx + dy * dy + dz * dz) + 0.1f;
            obj->yaw = wrap360(obj->yaw + (float)(runtime_.rng().next(20) - 10) *
                                                 (kPerfect - accuracy) / distance);
            obj->pitch += (float)(runtime_.rng().next(20) - 10) * (kPerfect - accuracy) /
                          (distance * kPitchErrorScale);
            break;
        }
        case 40:  // turn_yaw: yaw by rate*Dt (the XD's scripted turn)
            obj->yaw = wrap360(obj->yaw + value(obj, op(ins, 0)) * kDt);
            break;
        case 134:  // turn_roll: roll by rate*Dt (the bolt's spin)
            obj->roll = wrap360(obj->roll + value(obj, op(ins, 0)) * kDt);
            break;
        case 235:  // turn_to_target_offset: turn toward a point offset from the
        case 105: {  // target in the target's frame (105 adds a z offset and pitches)
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
                break;
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
            break;
        }
        case 207: {  // turn_to_yaw: turn toward a goal yaw by at most rate*Dt; branches when reached
            float goal = fval(op(ins, 1));
            float diff = wrapAngle(goal - obj->yaw);
            float step = fval(op(ins, 0)) * kDt;
            float sign = diff > 0.0f ? 1.0f : (diff < 0.0f ? -1.0f : 0.0f);
            obj->yaw = wrap360(fabsf(diff) <= step ? goal : obj->yaw + sign * step);
            return branch(obj, ins, fabsf(obj->yaw - wrap360(goal)) < 1e-4f);
        }
        case 193: {  // face_mode: 1/2 face the velocity (2 also pitches), 3 copies another's yaw
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
            break;
        }

        // Movement parameters.
        case 50:  // set_max_speed
            obj->max_speed = value(obj, op(ins, 0));
            break;
        case 51:  // set_accel
            obj->acceleration = value(obj, op(ins, 0));
            break;
        case 52:  // set_decel
            obj->deceleration = value(obj, op(ins, 0));
            break;
        case 53:  // set_speed
            obj->speed = value(obj, op(ins, 0));
            break;
        case 55:  // set_gravity
            obj->gravity = value(obj, op(ins, 0));
            break;
        case 82:  // set_friction
            obj->friction = value(obj, op(ins, 0));
            break;
        case 84:  // set_height_offset
            obj->height_offset = value(obj, op(ins, 0));
            break;
        case 106:  // set_302: lifetime of projectiles, jump time... (the C#'s Parameter)
            obj->parameter = fval(op(ins, 0));
            break;

        case 65: {  // set_var
            float* vars = variables(obj, ival(op(ins, 0)));
            int32_t index = ival(op(ins, 1));
            if (index < 0) {
                index = 0;
            }
            if (index > kVariableCount - 1) {
                index = kVariableCount - 1;
            }
            vars[index] = fval(op(ins, 2));
            break;
        }
        case 66: {  // add_var
            float* vars = variables(obj, ival(op(ins, 0)));
            int32_t index = ival(op(ins, 1));
            if (index < 0) {
                index = 0;
            }
            if (index > kVariableCount - 1) {
                index = kVariableCount - 1;
            }
            vars[index] += fval(op(ins, 2));
            break;
        }
        case 216: {  // add_var_dt: the per-tick variable step (a spawner countdown)
            float* vars = variables(obj, ival(op(ins, 0)));
            int32_t index = ival(op(ins, 1));
            if (index < 0) {
                index = 0;
            }
            if (index > kVariableCount - 1) {
                index = kVariableCount - 1;
            }
            vars[index] += fval(op(ins, 2)) * kDt;
            break;
        }
        case 67: {  // if_var: compare a variable against the operand condition
            float* vars = variables(obj, ival(op(ins, 0)));
            int32_t index = ival(op(ins, 1));
            if (index < 0) {
                index = 0;
            }
            if (index > kVariableCount - 1) {
                index = kVariableCount - 1;
            }
            return branch(obj, ins, compare(vars[index], listval(op(ins, 2))));
        }
        case 73:  // set_obey_level
            obj->obey_level = ival(op(ins, 0));
            break;
        case 11:  // set_priority
            obj->priority = ival(op(ins, 0));
            break;

        case 35:  // set_flag148_4
            obj->flags = ival(op(ins, 0)) != 0 ? (obj->flags | MdkObject::FlagCollides)
                                               : (obj->flags & ~MdkObject::FlagCollides);
            break;
        case 36:  // set_flag148_2
            obj->flags = ival(op(ins, 0)) != 0 ? (obj->flags | MdkObject::FlagGravity)
                                               : (obj->flags & ~MdkObject::FlagGravity);
            break;
        case 116:  // flags_set
            obj->flags |= ival(op(ins, 0));
            break;
        case 117:  // flags_clear
            obj->flags &= ~ival(op(ins, 0));
            break;
        case 97:  // set_banking (automatic banking, ObjectMotion.Bank)
            if (ival(op(ins, 0)) != 0) {
                obj->flags &= ~MdkObject::FlagNoBanking;
            } else {
                obj->flags |= MdkObject::FlagNoBanking;
                obj->roll = 0.0f;
            }
            break;

        // Animation.
        case 3:   // anim_once
        case 59: { // anim_loop
            runtime_.playAnimation(obj, runtime_.getAnimation(obj, ival(op(ins, 0))), ins.opcode == 59);
            break;
        }
        case 17:  // if_anim_done
            return branch(obj, ins, isAnimDone(*obj));
        case 92:  // if_anim_frame: reached operand frame n - 1 (or the animation is done)
            return branch(obj, ins, isAnimDone(*obj) || obj->animation_frame >= ival(op(ins, 0)) - 1);
        case 19:  // anim_mark_ended
            obj->animation_end_frame = MdkObject::AnimationEnded;
            break;
        case 118: {  // anim_end_frame (s16): the hold frame the animation stops on
            int32_t v = (ival(op(ins, 0)) & 0xFFFF) - 1;
            obj->animation_end_frame = v >= 0x8000 ? v - 0x10000 : v;  // C# S16
            break;
        }
        case 24:  // set_id_and_name: a sound when the animation reaches frame n - 1
            obj->frame_sound_frame = ival(op(ins, 0)) - 1;
            obj->frame_sound = strval(op(ins, 1));
            break;
        case 154: {  // wait_anim_frame: parks restart here and re-runs each frame until reached
            const int32_t kHoldFrame = -1;
            obj->restart = ins.pc;
            const int32_t frame = ival(op(ins, 0));
            const bool reached =
                isAnimDone(*obj) || (frame >= 0 && obj->animation_frame >= frame) ||
                (frame == kHoldFrame &&
                 (obj->animation_end_frame < 0 || obj->animation_frame == obj->animation_end_frame));
            if (!reached) {
                return kYield;
            }
            break;
        }
        case 83: {  // set_scale: a value, or [target, rate] to grow/shrink towards the target
            const ScriptValue& o0 = op(ins, 0);
            const std::vector<ScriptValue>& items = listval(o0);
            if (o0.kind == ScriptValueKind::List && items.size() == 2 &&
                items[0].kind == ScriptValueKind::Float) {
                const float target = items[0].floatValue;
                const float rate = fval(items[1]);
                if (obj->scale < target) {
                    obj->scale = mathMin(obj->scale * (1.0f + rate * kDt), target);
                } else if (obj->scale > target) {
                    obj->scale = mathMax(obj->scale * (1.0f - rate * kDt * 0.5f), target);
                }
            } else {
                obj->scale = value(obj, o0);
            }
            break;
        }

        case 58:  // anim_fps: the animation's playback rate (frames per second)
            obj->animation_rate = value(obj, op(ins, 0));
            break;
        case 186:  // anim_fps_from_speed: the animation keeps up with the walking speed
            obj->animation_rate = fval(op(ins, 1)) != 0.0f
                                      ? obj->speed * ScriptRuntime::kTicksPerSecond / fval(op(ins, 1))
                                      : MdkObject::AnimationFps;
            break;

        // Hits.
        case 22: {  // if_hit: any hit but blasts; the event is cleared when true
            const bool hit = obj->hit_event != 0 && obj->hit_event != -3;
            if (hit) {
                obj->hit_event = 0;
            }
            return branch(obj, ins, hit);
        }
        case 212: {  // if_hit_fd: a blast
            const bool hit = obj->hit_event == -3;
            if (hit) {
                obj->hit_event = 0;
            }
            return branch(obj, ins, hit);
        }
        case 42: {  // if_hit_part: name (cleared when true) or ["", name] (not cleared); "ANY" = any part
            const ScriptValue& nameOp = op(ins, 0);
            const std::vector<ScriptValue>& pair = listval(nameOp);
            const std::string name = nameOp.kind == ScriptValueKind::String
                                         ? nameOp.stringValue
                                         : (pair.size() > 1 ? strval(pair[1]) : kEmptyString);
            const bool hit = obj->model != nullptr && obj->hit_event > 0 &&
                             obj->hit_event <= (int32_t)obj->model->partList.size() &&
                             (iequals("ANY", name) ||
                              iequals(obj->model->partList[obj->hit_event - 1].nameString(), name));
            if (hit && nameOp.kind == ScriptValueKind::String) {
                obj->hit_event = 0;
            }
            return branch(obj, ins, hit);
        }

        // Kurt.
        case 108: {  // if_touching_kurt: a move-61 object reads its contact flag, else box overlap
            if (obj->move_command == 61) {
                return branch(obj, ins, (obj->contact_flags & MdkObject::ContactTouchedKurt) != 0);
            }
            const formats::Box a = runtime_.worldBounds(obj);
            const formats::Box b = runtime_.kurtBox();
            // C# Box.Intersects (Godot AABB rules): strict on every axis.
            const bool hit = a.min.x < b.max.x && a.max.x > b.min.x && a.min.y < b.max.y &&
                             a.max.y > b.min.y && a.min.z < b.max.z && a.max.z > b.min.z;
            return branch(obj, ins, hit);
        }
        case 188: {  // if_target_dist2d: the XY distance to the target
            const Vec3& t = runtime_.targetPosition();
            const float dx = t.x - obj->position.x;
            const float dy = t.y - obj->position.y;
            return branch(obj, ins, compare(sqrtf(dx * dx + dy * dy), listval(op(ins, 0))));
        }
        case 48: {  // if_chance_per_second: operand seconds is the mean period
            const float kMinPeriod = 0.001f;
            return branch(obj, ins,
                          runtime_.rng().next(10000) <
                              kDt / mathMax(fval(op(ins, 0)), kMinPeriod) * 10000.0f);
        }

        // Audio and Kurt effects: the mixer and Kurt's knock-down are later slices.
        // Neither writes object or VM state the golden dumps read (play_sound's Tracked
        // bookkeeping is read only by if_own_sound, itself unported), so these decode
        // and advance exactly as the C# does without a state difference here.
        case 89:   // play_sound
        case 248:  // push_kurt
            break;

        // Movement conditions.
        case 44:  // if_move_done
            return branch(obj, ins, obj->move_command == 0);
        case 102:  // if_path_done
            return branch(obj, ins, obj->path == 0);
        case 54: {  // if_dest_dist: to the move destination (2D when on a floor)
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
        case 45: {  // if_target_dist: the 3D distance to the target
            const Vec3& t = runtime_.targetPosition();
            const float dx = t.x - obj->position.x;
            const float dy = t.y - obj->position.y;
            const float dz = t.z - obj->position.z;
            return branch(obj, ins,
                          compare(sqrtf(dx * dx + dy * dy + dz * dz), listval(op(ins, 0))));
        }
        case 231:  // if_move_idle_flag: the movement gave up (stuck, HandleStuck)
            return branch(obj, ins, obj->move_command == 0 && obj->stuck_count != 0);
        case 61:  // fire: spawn a projectile (flyProjectile runs it each tick)
            runtime_.fire(obj, listval(op(ins, 0)), strval(op(ins, 1)), ival(op(ins, 2)));
            break;
        case 124:  // path_yaw_offset: the yaw added to the path's own heading
            obj->path_yaw_offset = fval(op(ins, 0));
            break;

        // Objects and odds.
        case 10:  // if_count_objects: the count EQUALS the operand (unlike 119's Compare)
            return branch(obj, ins, countOthers(obj, op(ins, 0), false) == ival(op(ins, 1)));
        case 119:  // if_count_alive: live objects of the arena of a type
            return branch(obj, ins, compare((float)countOthers(obj, op(ins, 0), true),
                                            listval(op(ins, 1))));
        case 123:  // if_not_in_player_arena
            return branch(obj, ins, obj->arena != runtime_.currentArena());
        case 47:  // if_chance: percent per tick
            return branch(obj, ins, runtime_.rng().next(10000) < fval(op(ins, 0)) * 100.0f);
        case 222:  // if_opcode_count: this object's opcodes executed this frame
            return branch(obj, ins, compare((float)opcodeCount_, listval(op(ins, 0))));
        case 251: {  // set_target_mode: 1 the aliens' target, 2 always targets Kurt
            const int32_t mode = ival(op(ins, 0));
            obj->target_mode = mode;
            if (mode == 1) {
                runtime_.setAlienTarget(obj);
            } else if (mode == 0 && runtime_.alienTarget() == obj) {
                runtime_.setAlienTarget(nullptr);
            }
            break;
        }
        case 187: {  // random_kick: a shove in a random direction
            /* @vfp-exempt R6 — cold kick path; glibc cosf/sinf match the C#'s
             * MathF.Cos/Sin bit-for-bit on this machine (probed), and the
             * conversion is .NET's left-associative spelling; the RNG draws are
             * taken in the C#'s evaluation order (angle, length, then height).
             * measured n/a (cold). Verified 2026-10-23. */
            const float angle = runtime_.rng().nextSingle() * 360.0f;
            const float mult = 0.5f + runtime_.rng().nextSingle();
            const float rad = angle * 3.14159265358979323846f / 180.0f;
            const float kickX = cosf(rad) * fval(op(ins, 0)) * mult;
            const float kickY = sinf(rad) * fval(op(ins, 0)) * mult;
            const float kickZ = fval(op(ins, 1)) * (0.5f + runtime_.rng().nextSingle());
            obj->velocity.x += kickX;
            obj->velocity.y += kickY;
            obj->velocity.z += kickZ;
            break;
        }

        // The World's Most Interesting Bomb (Items.Bomb; null until the Items slice).
        case 165: {  // if_no_bomb
            const MdkObject* bomb =
                runtime_.itemsBomb != nullptr && !runtime_.itemsBomb->dead ? runtime_.itemsBomb
                                                                          : nullptr;
            return branch(obj, ins, bomb == nullptr);
        }
        case 166: {  // if_bomb_visible: a clear line from 5 above to 5 above the bomb
            const MdkObject* bomb =
                runtime_.itemsBomb != nullptr && !runtime_.itemsBomb->dead ? runtime_.itemsBomb
                                                                          : nullptr;
            bool sees = false;
            if (bomb != nullptr) {
                const float kSightHeight = 5.0f;
                Vec3 from{obj->position.x, obj->position.y, obj->position.z + kSightHeight};
                Vec3 to{bomb->position.x, bomb->position.y, bomb->position.z + kSightHeight};
                sees = runtime_.raycastClear(from, to);
            }
            return branch(obj, ins, sees);
        }
        case 236: {  // if_no_floor_at: no up-facing face below a point ahead
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

        // Movement commands.
        case 43:  // move_near_target (only in Kurt's arena)
            if (obj->arena == runtime_.currentArena()) {
                runtime_.startMove(obj, 43,
                                   runtime_.nearTargetDestination(obj, fval(op(ins, 0)),
                                                                  fval(op(ins, 1))));
            }
            break;
        case 75:  // stop_motion
            obj->move_command = 0;
            break;

        // Flags.
        case 23:  // set_flag148_1 (clearing it also stops the roll)
            if (ival(op(ins, 0)) != 0) {
                obj->flags |= 1;
            } else {
                obj->flags &= ~1;
                obj->roll = 0.0f;
            }
            break;
        case 176:  // if_flag_40000: the pickup was collected
            return branch(obj, ins, (obj->flags & MdkObject::FlagCollected) != 0);

        // Conditions.
        case 96: {  // if_kurt_in_rect
            const Vec3& kurt = runtime_.kurtPosition();
            bool in = kurt.x >= fval(op(ins, 0)) && kurt.x <= fval(op(ins, 2)) &&
                      kurt.y >= fval(op(ins, 1)) && kurt.y <= fval(op(ins, 3));
            return branch(obj, ins, in);
        }
        case 37:  // if_flag14c_2: the object is on a floor (contact flag)
            return branch(obj, ins, (obj->contact_flags & MdkObject::ContactFloor) != 0);
        case 238: {  // if_self_pos: compare one position component (0 x, 1 y, else z)
            int32_t axis = ival(op(ins, 0));
            if (axis < 0) {
                axis = 0;
            }
            if (axis > 2) {
                axis = 2;
            }
            const float component = axis == 0   ? obj->position.x
                                    : axis == 1 ? obj->position.y
                                                : obj->position.z;
            return branch(obj, ins, compare(component, listval(op(ins, 1))));
        }
        case 232:  // if_option
            return branch(obj, ins, ival(op(ins, 0)) == runtime_.option);

        case 98:  // group_set_state
            runtime_.setGroupState(obj->arena, ival(op(ins, 0)), ival(op(ins, 1)));
            break;

        // --- Opcodes ported in the Phase 2 coverage audit (faithful to ScriptVm.cs). ---
        case 79:  // set_pos: place the object at the operand position
            obj->position = vpos(ins);
            break;
        case 211:  // stop_velocity: zero this frame's velocity
            obj->velocity.x = obj->velocity.y = obj->velocity.z = 0.0f;
            break;
        case 120:  // set_pitch: snap the pitch to the operand
            obj->pitch = fval(op(ins, 0));
            break;
        case 122:  // turn_pitch: turn toward a goal pitch by at most rate*Dt
            {
                const float maxStep = fval(op(ins, 0)) * kDt;
                float step = wrapAngle(fval(op(ins, 1)) - obj->pitch);
                // Math.Clamp spelled with the C#'s comparisons: a NaN step stays NaN.
                step = step < -maxStep ? -maxStep : (step > maxStep ? maxStep : step);
                obj->pitch += step;
            }
            break;
        case 56:  // add_health: heal/damage unless indestructible; a kill yields
            if (obj->health < 65000) {  // C# ScriptRuntime.Indestructible
                obj->health += ival(op(ins, 0));
                if (obj->health <= 0) {
                    runtime_.kill(obj);
                    return kYield;
                }
            }
            break;
        case 160:  // if_yaw: compare the wrapped yaw against the operand condition
            return branch(obj, ins, compare(wrap360(obj->yaw), listval(op(ins, 0))));
        case 185:  // if_speed: compare the scalar speed
            return branch(obj, ins, compare(obj->speed, listval(op(ins, 0))));
        case 38:  // if_vel_z: compare the vertical velocity
            return branch(obj, ins, compare(obj->velocity.z, listval(op(ins, 0))));
        case 127:  // if_health: compare the health
            return branch(obj, ins, compare((float)obj->health, listval(op(ins, 0))));
        case 103: {  // if_kurt_in_box: each axis between its min and max
            const Vec3& k = runtime_.kurtPosition();
            bool inside = k.x >= fval(op(ins, 0)) && k.x <= fval(op(ins, 3)) &&
                          k.y >= fval(op(ins, 1)) && k.y <= fval(op(ins, 4)) &&
                          k.z >= fval(op(ins, 2)) && k.z <= fval(op(ins, 5));
            return branch(obj, ins, inside);
        }
        case 237: {  // if_self_in_box: this object's position between min and max
            const Vec3& p = obj->position;
            bool inside = p.x >= fval(op(ins, 0)) && p.y >= fval(op(ins, 1)) &&
                          p.z >= fval(op(ins, 2)) && p.x <= fval(op(ins, 3)) &&
                          p.y <= fval(op(ins, 4)) && p.z <= fval(op(ins, 5));
            return branch(obj, ins, inside);
        }
        case 34: {  // if_no_leader: no leader (or a dead one); clears the move command when true
            bool alone = obj->leader == nullptr || obj->leader->dead;
            int32_t next = branch(obj, ins, alone);
            if (alone) {
                obj->move_command = 0;
            }
            return next;
        }
        case 183: {  // switch_gosub: a gosub picked by a variable, returning after the table
            int32_t slot = ival(op(ins, 1));
            if (slot < 0) {
                slot = 0;
            }
            if (slot > kVariableCount - 1) {
                slot = kVariableCount - 1;
            }
            int32_t index = (int32_t)variables(obj, ival(op(ins, 0)))[slot];
            const std::vector<ScriptValue>& table = listval(op(ins, 2));
            if (index >= 0 && index < (int32_t)table.size()) {
                return gosub(obj, ins.next, ival(listval(table[index])[0]));
            }
            break;
        }
        case 197:  // find_advance_spot: walk toward Kurt via a DTI advance spot
            runtime_.findAdvanceSpot(obj);
            return branch(obj, ins, obj->move_command == 197);
        case 110:  // delete_self: out of the list, and stop this frame
            runtime_.remove(obj);
            return kYield;
        case 218:  // set_f13c: the pitch
            obj->pitch = fval(op(ins, 0));
            break;
        case 164: {  // path_speed_by_kurt: scale path speed by the distance to Kurt
            if (ival(op(ins, 0)) != 0) {
                const std::vector<ScriptValue>& raw = listval(op(ins, 1));
                float middle = 0.0f;
                for (size_t k = 0; k < 4; k++) {
                    float s = k < raw.size() ? fval(raw[k]) : 0.0f;
                    if (k < raw.size()) {
                        obj->path_speeds[k] = s;
                    }
                    if (k == 2) {
                        middle = s;
                    }
                }
                obj->path_speed = middle;
                obj->flags |= MdkObject::FlagPathSpeedByKurt;
            } else {
                obj->flags &= ~MdkObject::FlagPathSpeedByKurt;
            }
            break;
        }
        case 39: {  // push_forward: push along the object's yaw (x,y only)
            /* @vfp-exempt R6 — push_forward heading (per pushing object); glibc
             * cosf/sinf match the C#'s MathF.Cos/Sin bit-for-bit on this machine
             * (probed), and the conversion is .NET's left-associative spelling.
             * measured n/a (cold). Verified 2026-10-24. */
            const float rad = obj->yaw * 3.14159265358979323846f / 180.0f;
            const float mag = value(obj, op(ins, 0));
            obj->push.x += cosf(rad) * mag;
            obj->push.y += sinf(rad) * mag;
            break;
        }
        case 201: {  // push_dir: push along yaw plus an offset (x,y only)
            /* @vfp-exempt R6 — see push_forward. */
            const float rad = (obj->yaw + fval(op(ins, 1))) * 3.14159265358979323846f / 180.0f;
            const float mag = fval(op(ins, 0));
            obj->push.x += cosf(rad) * mag;
            obj->push.y += sinf(rad) * mag;
            break;
        }
        case 80: {  // add_vel_local: add a velocity rotated by the object's yaw
            /* @vfp-exempt R6 — see push_forward. */
            const float rad = obj->yaw * 3.14159265358979323846f / 180.0f;
            const float s = sinf(rad);
            const float c = cosf(rad);
            obj->velocity.x += -fval(op(ins, 0)) * c - fval(op(ins, 1)) * s;
            obj->velocity.y += -fval(op(ins, 1)) * c - fval(op(ins, 0)) * s;
            obj->velocity.z += fval(op(ins, 2));
            break;
        }
        case 126: {  // aim_pitch_target: pitch to the target plus the aim height
            /* @vfp-exempt R6 — see aim_target. */
            const float kAimHeight = 3.0f;
            const Vec3 target = runtime_.targetPosition();
            const float dx = target.x - obj->position.x;
            const float dy = target.y - obj->position.y;
            obj->pitch = atan2f(target.z + kAimHeight - obj->position.z, sqrtf(dx * dx + dy * dy)) *
                         180.0f / 3.14159265358979323846f;
            break;
        }
        case 157: {  // vel_away_from: run from the named object (negative speed moves toward it)
            MdkObject* other = runtime_.firstOther(obj, strval(op(ins, 0)), ival(op(ins, 1)));
            Vec3 away;
            away.x = away.y = away.z = 0.0f;
            if (other != nullptr) {
                away.x = obj->position.x - other->position.x;
                away.y = obj->position.y - other->position.y;
                away.z = obj->position.z - other->position.z;
            }
            if (away.x != 0.0f || away.y != 0.0f || away.z != 0.0f) {
                const float len = sqrtf(away.x * away.x + away.y * away.y + away.z * away.z);
                const float mag = fval(op(ins, 2));
                obj->velocity.x = away.x / len * mag;
                obj->velocity.y = away.y / len * mag;
                obj->velocity.z = away.z / len * mag;
                obj->path = 0;
            }
            break;
        }
        case 41: {  // set_targetable: flag 0x100 (a platform), and 0x800000 with mode 2
            const int32_t kPlatform = 0x100;
            if (ival(op(ins, 0)) != 0) {
                obj->flags |= kPlatform;
            } else {
                obj->flags &= ~kPlatform;
            }
            if (ival(op(ins, 0)) == 2) {
                obj->flags |= MdkObject::FlagStandable;
            } else {
                obj->flags &= ~MdkObject::FlagStandable;
            }
            break;
        }
        case 63:  // set_flag148_10_inv: Kurt goes through the object (0x10)
            if (ival(op(ins, 0)) != 0) {
                obj->flags = (obj->flags & ~MdkObject::FlagNotSolid);
            } else {
                obj->flags = (obj->flags | MdkObject::FlagNotSolid);
            }
            break;
        case 169:  // set_roll_radius
            obj->roll_radius = value(obj, op(ins, 0));
            break;
        case 177:  // set_blast_range: blasts farther than this don't hurt the object
            obj->blast_range = fval(op(ins, 0));
            break;
        case 205:  // set_21f: indestructible
            obj->indestructible = ival(op(ins, 0)) != 0;
            break;
        case 20:  // stop_path
            obj->path = 0;
            break;
        case 21: {  // path_stop_at (-2 = the current frame)
            int32_t stop = ival(op(ins, 0)) & 0xFFFF;
            if ((stop & 0x8000) != 0) {
                stop -= 0x10000;
            }
            obj->path_stop = stop;
            if (obj->path_stop == -2) {
                obj->path_stop = lroundf(obj->path_time);
            }
            break;
        }
        case 33:  // if_path_frame: off a path, or at/past operand frame - 1
            return branch(obj, ins,
                          obj->path == 0 || lroundf(obj->path_time) >= ival(op(ins, 0)) - 1);
        case 91:  // set_path_speed
            obj->path_speed = value(obj, op(ins, 0));
            break;
        case 62: {  // if_target_angle: absolute wrapped yaw to the target
            /* @vfp-exempt R6 — see aim_target. */
            const float rad = atan2f(runtime_.targetPosition().y - obj->position.y,
                                     runtime_.targetPosition().x - obj->position.x);
            const float deg = rad * 180.0f / 3.14159265358979323846f;
            const float angle = deg >= 0.0f ? deg : deg + 360.0f;
            return branch(obj, ins, compare(wrapAngle(angle - obj->yaw), listval(op(ins, 0))));
        }
        case 234:  // if_target_angle_signed: signed wrapped yaw minus yaw to the target
            return branch(obj, ins,
                          compare(wrapAngle(obj->yaw - runtime_.yawTo(obj, runtime_.targetPosition())),
                                  listval(op(ins, 0))));
        case 191: {  // if_kurt_delta: Kurt's offset along an axis, absolute unless +0x80
            int32_t axis = ival(op(ins, 0)) & 0x7F;
            if (axis > 2) {
                axis = 2;
            }
            const Vec3& k = runtime_.kurtPosition();
            float delta = axis == 0   ? k.x - obj->position.x
                            : axis == 1 ? k.y - obj->position.y
                                        : k.z - obj->position.z;
            if ((ival(op(ins, 0)) & 0x80) == 0) {
                delta = delta >= 0.0f ? delta : -delta;
            }
            return branch(obj, ins, compare(delta, listval(op(ins, 1))));
        }

        default:
            noteUnimplemented(ins.opcode);
            break;
    }
    return ins.next;
}

} // namespace scripts
} // namespace mdk

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
            return obj->linked ? obj->linked->variables.data() : noVariables_.data();
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
        case 84:  // set_height_offset
            obj->height_offset = value(obj, op(ins, 0));
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

        // Conditions.
        case 96: {  // if_kurt_in_rect
            const Vec3& kurt = runtime_.kurtPosition();
            bool in = kurt.x >= fval(op(ins, 0)) && kurt.x <= fval(op(ins, 2)) &&
                      kurt.y >= fval(op(ins, 1)) && kurt.y <= fval(op(ins, 3));
            return branch(obj, ins, in);
        }
        case 232:  // if_option
            return branch(obj, ins, ival(op(ins, 0)) == runtime_.option);

        case 98:  // group_set_state
            runtime_.setGroupState(obj->arena, ival(op(ins, 0)), ival(op(ins, 1)));
            break;

        default:
            noteUnimplemented(ins.opcode);
            break;
    }
    return ins.next;
}

} // namespace scripts
} // namespace mdk

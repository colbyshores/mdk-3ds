#pragma once

// ============================================================================
// script_vm_internal.h — the operand/value helpers shared by the script VM's
// dispatch translation units.
//
// These were the anonymous-namespace helpers of script_vm.cpp. They move here
// (unchanged) so the opcode-family handlers that live in their own .cpp files
// (script_vm_move.cpp, script_vm_ai.cpp, ...) can use them without a second
// copy. They are `inline` so every translation unit shares one definition.
//
// They sit in scripts::detail, NOT scripts, so they cannot collide with the
// same-named internal-linkage helpers script_runtime.cpp keeps in its own
// anonymous namespace (wrap360, wrapAngle).
//
// NO VIRTUALS / NO TEMPLATES (AGENTS.md 10): plain inline free functions only.
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

#include "formats/script_decoder.h"
#include "scripts/object.h"

namespace mdk {
namespace scripts {
namespace detail {

using formats::Instruction;
using formats::ScriptValue;
using formats::ScriptValueKind;

inline const ScriptValue kNull;
inline const std::vector<ScriptValue> kEmpty;
inline const std::string kEmptyString;

// A numeric operand as a float (the C#'s ScriptMath.F).
inline float fval(const ScriptValue& v) {
    if (v.kind == ScriptValueKind::Float) {
        return v.floatValue;
    }
    if (v.kind == ScriptValueKind::Int) {
        return (float)v.intValue;
    }
    return 0.0f;
}

// A numeric operand as an int (the C#'s ScriptMath.I).
inline int32_t ival(const ScriptValue& v) {
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
inline float mathMax(float a, float b) {
    if (a != a) {
        return a;
    }
    if (b != b) {
        return b;
    }
    return a > b ? a : b;
}

// The C#'s MathF.Min: NaN-propagating, for the same reason as mathMax.
inline float mathMin(float a, float b) {
    if (a != a) {
        return a;
    }
    if (b != b) {
        return b;
    }
    return a < b ? a : b;
}

inline const std::vector<ScriptValue>& listval(const ScriptValue& v) {
    return v.kind == ScriptValueKind::List ? v.items : kEmpty;
}

inline const std::string& strval(const ScriptValue& v) {
    return v.kind == ScriptValueKind::String ? v.stringValue : kEmptyString;
}

// An instruction's operand, or the null operand when the index is past the end.
inline const ScriptValue& op(const Instruction& ins, int32_t i) {
    return (i >= 0 && (size_t)i < ins.operands.size()) ? ins.operands[i] : kNull;
}

// The position operand: the first three operands as a vector (the C#'s V(o)).
inline Vec3 vpos(const Instruction& ins) {
    Vec3 out;
    out.x = fval(op(ins, 0));
    out.y = fval(op(ins, 1));
    out.z = fval(op(ins, 2));
    return out;
}

inline float wrap360(float degrees) {
    return degrees - 360.0f * floorf(degrees / 360.0f);
}

// Wrap to [-180, 180) (ScriptMath.WrapAngle).
inline float wrapAngle(float degrees) {
    return degrees - 360.0f * floorf((degrees + 180.0f) / 360.0f);
}

// MdkObject.IsAnimationDone: no animation, or the hold frame marked ended.
inline bool isAnimDone(const MdkObject& obj) {
    return obj.animation == nullptr || obj.animation_end_frame == MdkObject::AnimationEnded;
}

inline bool iequals(const std::string& a, const std::string& b) {
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

} // namespace detail
} // namespace scripts
} // namespace mdk

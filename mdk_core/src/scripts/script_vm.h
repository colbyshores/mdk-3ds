#pragma once

// ============================================================================
// script_vm.h — MDK's script interpreter (script_run).
//
// A port of src/Mdk.Game/Scripts/ScriptVm.cs. It runs one object's script for
// one frame: decode at the program counter, execute, advance, until the script
// yields (a wait, a stop, a death) or reaches the end opcode.
//
// THE SLICE. This first slice implements the control-flow core (run, branch,
// goto, gosub, return, value, variables, flags, compare) and the opcodes the
// level-3 tick-1 path needs: the arena controller's spawns and the object-type
// init scripts. An opcode that is decoded but not implemented here does nothing
// and advances to the next instruction, exactly like the C# ("Opcodes not
// implemented do nothing ... but they're always decoded, so scripts never lose
// sync"). The VM records those so a driver can see whether a path it cares
// about hit one.
//
// NO VIRTUALS (house rule): the VM calls a concrete ScriptRuntime, not an
// interface. The runtime owns the VM; the VM holds a reference back.
//
// The decoder is BORROWED: the runtime owns it (it borrows the CMI's bytes),
// and it must outlive the VM.
// ============================================================================

#include <stdint.h>

#include <vector>

#include "formats/script_decoder.h"
#include "scripts/object.h"

namespace mdk {

class Random;

namespace scripts {

class ScriptRuntime;

class ScriptVm {
public:
    static const int32_t kMaxOpcodesPerFrame = 1000;
    static const int32_t kGosubDepth = 4;

    ScriptVm(ScriptRuntime& runtime, formats::ScriptDecoder& decoder);

    // Run one frame of an object's script.
    void run(MdkObject* obj);

    // The opcodes seen that this slice does not implement (opcode to count).
    const std::vector<std::pair<int32_t, int32_t>>& unimplemented() const { return unimplemented_; }

private:
    // The value a handler returns to stop running this object this frame.
    static const int32_t kYield = -1;
    static const float kDt;         // 1/30
    static const float kEpsilon;    // comparison tolerance
    static const float kMinWait;
    static const int32_t kVariableCount = 4;
    static const int32_t kDoorFlags = 5;

    int32_t execute(MdkObject* obj, const formats::Instruction& ins);

    // command_objects (opcode 4): parses the command operand, the branch action (a
    // goto command's target) or the near-target offset, the selector and the args,
    // and hands them to the runtime (C# CommandObjects, ScriptVm.cs:2178).
    void commandObjects(MdkObject* obj, const formats::Instruction& ins);

    int32_t branch(MdkObject* obj, const formats::Instruction& ins, bool condition);
    int32_t gotoTarget(MdkObject* obj, int32_t target);
    int32_t gosub(MdkObject* obj, int32_t returnPc, int32_t target);
    int32_t returnFrom(MdkObject* obj);
    void clearStack(MdkObject* obj);

    float value(MdkObject* obj, const formats::ScriptValue& operand);
    float* variables(MdkObject* obj, int32_t source);
    int32_t getFlags(MdkObject* obj, int32_t source);
    void setFlags(MdkObject* obj, int32_t source, int32_t value);

    static bool compare(float value, const std::vector<formats::ScriptValue>& cond);
    static bool compareOps(float value, const formats::ScriptValue& op,
                           const formats::ScriptValue& first, const formats::ScriptValue& second);

    static bool isDoor(const MdkObject& obj);

    void noteUnimplemented(int32_t opcode);

    ScriptRuntime& runtime_;
    formats::ScriptDecoder& decoder_;

    std::vector<std::pair<int32_t, int32_t>> unimplemented_;
    int32_t opcodeCount_ = 0;
    std::array<float, 4> noVariables_{{0.0f, 0.0f, 0.0f, 0.0f}};
};

} // namespace scripts
} // namespace mdk

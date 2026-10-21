#pragma once

// ============================================================================
// script_decoder.h — MDK's script bytecode.
//
// A 1:1 port of src/Mdk.Formats/Scripts/ScriptDecoder.cs.
//
// The bytecode lives in the CMI file's scripts, and the decoder turns it
// into instructions the game's script VM runs. It is like the Godot port's
// script_decoder.gd and the mdk_script_dis.py tool, and it was validated
// against every script of the game.
//
// THE OFFSETS ARE ABSOLUTE FILE OFFSETS. The bytecode stores its offsets
// from 4, and the decoder adds the 4: an offset of 0 stays 0, because 0
// means "none".
//
// AN OPERAND IS ONE OF A FEW THINGS: an int, a float, a string, a branch
// action, a variable, a nested list, or null. The C# boxes them all into
// `object?`; C++ has no boxed object, so a ScriptValue is a tagged union.
//
// THE DECODER IS A COLD PATH. It runs when a level loads, and then the
// instructions are cached and the VM runs them without decoding again.
// ============================================================================

#include <stdint.h>

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "formats/script_opcodes.h"

namespace mdk {
namespace formats {

// The bytecode of a CMI file: the retail game's, or the 1996 demo's earlier
// one.
enum class ScriptDialect { Retail, Beta1996 };

// What a branch operand does: goto, gosub, gosub one of two (then/else), or
// return. The targets are absolute file offsets.
struct BranchAction {
    enum class Kind { None, Goto, Gosub, GosubElse, Return };

    Kind type = Kind::None;
    int32_t target = 0;
    int32_t elseTarget = 0;
    // The action byte, when the action is none of the known ones.
    int32_t raw = 0;
};

// A script variable operand: kind (0-2) and index.
struct Variable {
    int32_t kind = 0;
    int32_t index = 0;
};

// What a script operand holds.
enum class ScriptValueKind {
    Null,
    Int,
    Float,
    String,
    Variable,
    Action,
    List,
};

// A script operand.
//
// The C#'s operands are `object?[]`: an int, a float, a string, a branch
// action, a variable, a nested list, or null. C++ has no boxed object, so
// the same set is a tagged union, and the lists nest.
struct ScriptValue {
    ScriptValueKind kind = ScriptValueKind::Null;
    int32_t intValue = 0;
    float floatValue = 0.0f;
    std::string stringValue;
    Variable variable;
    BranchAction action;
    std::vector<ScriptValue> items;
};

// The null operand.
inline ScriptValue scriptNull() {
    return ScriptValue();
}

// An int operand.
inline ScriptValue scriptInt(int32_t value) {
    ScriptValue out;
    out.kind = ScriptValueKind::Int;
    out.intValue = value;
    return out;
}

// A float operand.
inline ScriptValue scriptFloat(float value) {
    ScriptValue out;
    out.kind = ScriptValueKind::Float;
    out.floatValue = value;
    return out;
}

// A string operand.
inline ScriptValue scriptString(const std::string& value) {
    ScriptValue out;
    out.kind = ScriptValueKind::String;
    out.stringValue = value;
    return out;
}

// A variable operand.
inline ScriptValue scriptVariable(int32_t kind, int32_t index) {
    ScriptValue out;
    out.kind = ScriptValueKind::Variable;
    out.variable.kind = kind;
    out.variable.index = index;
    return out;
}

// A branch action operand.
inline ScriptValue scriptAction(const BranchAction& value) {
    ScriptValue out;
    out.kind = ScriptValueKind::Action;
    out.action = value;
    return out;
}

// A list operand.
inline ScriptValue scriptList(std::vector<ScriptValue> items) {
    ScriptValue out;
    out.kind = ScriptValueKind::List;
    out.items = items;
    return out;
}

// A decoded instruction. The offsets (the code32 and data32 operands, the
// branch targets) are absolute file offsets, and 0 stays 0.
struct Instruction {
    int32_t pc = 0;
    int32_t opcode = 0;
    std::vector<ScriptValue> operands;
    // The offset of the next instruction.
    int32_t next = 0;
    // Whether the instruction has a branch action. It is SEPARATE from the
    // action's kind: an action byte the decoder does not know is an action
    // of the kind None, and the C# keeps it as a present action.
    bool hasAction = false;
    BranchAction action;
};

// Decodes MDK's script bytecode.
class ScriptDecoder {
public:
    // The opcode that ends a script.
    static const int32_t kEnd = 0xFF;

    // The offsets in the bytecode are from 4, and 0 means "none".
    static const int32_t kFileBase = 4;

    // A decoder of the retail bytecode. The buffer is BORROWED.
    explicit ScriptDecoder(const uint8_t* bytes, int32_t byteCount);

    // A decoder in a dialect. `paths` maps the 1996 demo's path offsets to
    // the converted ones; it is null for the retail dialect.
    ScriptDecoder(const uint8_t* bytes, int32_t byteCount, ScriptDialect dialect,
                  const std::vector<std::pair<int32_t, int32_t>>* paths);

    ScriptDialect dialect() const { return dialect_; }

    // The instruction at the file offset `pc` (cached). Returns false for
    // an invalid opcode, which is what the C#'s null return means.
    bool decode(int32_t pc, Instruction* out);

    // Decodes every instruction reachable from `starts` into the cache (a
    // level's load: the scripts then run without decoding); returns how
    // many. The retail dialect only: the C# returns 0 for the demo's.
    int32_t preload(const std::vector<int32_t>& starts);

    // The instructions decoded so far (after preload: every reachable one).
    const std::map<int32_t, Instruction>& decoded() const { return cache_; }

    // The code offsets an instruction can continue to besides the next one.
    static void targets(const Instruction& instruction, std::vector<int32_t>* out);

private:
    // Read one operand of a layout.
    bool read(OperandCode code, const std::vector<ScriptValue>& prev, ScriptValue* out);

    // The layouts that depend on the earlier operands.
    bool readCommandArgs(const std::vector<ScriptValue>& prev, ScriptValue* out);
    bool readSelectorArgs(int32_t selector, ScriptValue* out);
    bool readOrigin(const std::vector<ScriptValue>& prev, ScriptValue* out);
    bool readParams(const std::vector<ScriptValue>& prev, ScriptValue* out);
    bool readPartName(ScriptValue* out);
    bool readSound(const std::vector<ScriptValue>& prev, ScriptValue* out);
    bool readTypeName(const std::vector<ScriptValue>& prev, ScriptValue* out);
    bool readBox(const std::vector<ScriptValue>& prev, ScriptValue* out);
    bool readParts(const std::vector<ScriptValue>& prev, ScriptValue* out);
    bool readPosition(const std::vector<ScriptValue>& prev, ScriptValue* out);
    bool readHitTarget(const std::vector<ScriptValue>& prev, ScriptValue* out);
    bool readOp89Position(int32_t flags, ScriptValue* out);
    bool readOp61Origin(ScriptValue* out);
    bool readOp83Args(ScriptValue* out);
    bool readOp193Mode(ScriptValue* out);
    bool readOp245Args(ScriptValue* out);
    bool readOp248Args(ScriptValue* out);
    bool readOp172Position(ScriptValue* out);
    bool readOp173Target(ScriptValue* out);
    bool readOp181Source(ScriptValue* out);
    bool readOp228TypeName(ScriptValue* out);

    // A mode byte and then as many floats as the mode asks for.
    bool modeThenFloats(int32_t (*count)(int32_t), ScriptValue* out);

    // The repeats.
    bool readRepeatCode32(const std::vector<ScriptValue>& prev, ScriptValue* out);
    bool readRepeatPstr(const std::vector<ScriptValue>& prev, ScriptValue* out);
    bool readRepeatU8Code32(const std::vector<ScriptValue>& prev, ScriptValue* out);

    // The primitive reads.
    bool canRead(int32_t count) const { return p_ >= 0 && count >= 0 && byteCount_ - p_ >= count; }
    bool readU8(uint8_t* out);
    bool readF32(float* out);
    bool readU32(uint32_t* out);
    bool readS32(int32_t* out);
    bool readOffset(int32_t* out);
    bool readPascalString(std::string* out);
    bool readFloats(int32_t count, std::vector<ScriptValue>* out);
    bool readAction(BranchAction* out);
    bool readValue(ScriptValue* out);

    // The demo's half, in script_decoder_beta.cpp.
    bool decodeBeta(int32_t pc, Instruction* out);
    bool readBetaAction(BranchAction* out);
    bool readBeta(OperandCode code, const std::vector<ScriptValue>& prev, ScriptValue* out);
    bool decodeSpecial(int32_t opcode, std::vector<ScriptValue>* operands, int32_t* outOpcode);
    void retailOperands(int32_t opcode, const std::vector<ScriptValue>& operands,
                        std::vector<ScriptValue>* out) const;

    // The buffer the bytecode is in. BORROWED: the caller owns it and it
    // must outlive the decoder.
    const uint8_t* bytes_ = nullptr;
    int32_t byteCount_ = 0;

    ScriptDialect dialect_ = ScriptDialect::Retail;

    // The 1996 demo's path offsets to the converted ones.
    std::map<int32_t, int32_t> paths_;

    // The instructions decoded so far, by offset.
    std::map<int32_t, Instruction> cache_;

    // The offsets whose opcode is not in the table. The C# caches the null
    // too, so an invalid opcode is only looked up once.
    std::set<int32_t> invalid_;

    // The read position, and the action the last action operand read.
    int32_t p_ = 0;
    BranchAction action_;
    bool hasAction_ = false;
};

} // namespace formats
} // namespace mdk

#pragma once

// ============================================================================
// script_opcodes.h — the operand layouts of the script opcodes.
//
// A 1:1 port of src/Mdk.Formats/Scripts/ScriptOpcodes.g.cs.
//
// The C#'s table is generated from the Godot port's script_opcodes.json, and
// it is the oracle: the decoder's behaviour is "whatever this table says".
// The C++ table is generated from the C# one by
// tools/gen_script_opcodes.py, so the two cannot drift apart by typing.
//
// AN OPERAND CODE is one of:
//   * a plain type: u8, s8, u16, s16, u32, s32, f32, pstr
//   * code32, data32: an offset into the file, absolute, and 0 stays 0
//   * action, value, cond
//   * opN_name: a layout that depends on the earlier operands
//   * repeat:...: a list of items
//
// The repeat codes are split into their three concrete forms (code32, pstr
// and u8,code32) rather than kept as a string to be split at run time: the
// table only ever uses those three, and a run-time split would need a
// string type where an enum will do.
//
// The opcodes that are not in the table are the ones the decoder rejects.
// THAT SET IS NOT THE SET OF OPCODES WITH NO MNEMONIC: opcode 33 has a
// mnemonic and no operands, and opcode 250 has operands and no mnemonic of
// its own in some listings. The C# looks the opcode up in the OPERANDS
// table, and so does this.
// ============================================================================

#include <stdint.h>

namespace mdk {
namespace formats {

// The layout of one operand.
enum class OperandCode {
    None,   // the placeholder of an opcode with no operands

    // The plain types.
    U8,
    S8,
    U16,
    S16,
    U32,
    S32,
    F32,
    Pstr,

    // The offsets. They are absolute file offsets, and 0 stays 0.
    Code32,
    Data32,

    // The branch action, a value (a variable or a float literal) and a
    // comparison.
    Action,
    Value,
    Cond,

    // The layouts that depend on the earlier operands.
    Op2Origin,
    Op4CommandArgs,
    Op4SelectorArgs,
    Op42Part,
    Op61Origin,
    Op83Args,
    Op89Position,
    Op129Parts,
    Op132Position,
    Op158HitTarget,
    Op159Position,
    Op164Params,
    Op172Position,
    Op173Target,
    Op174B,
    Op175B,
    Op178Position,
    Op180Offset,
    Op181Source,
    Op189Args,
    Op193Mode,
    Op203Height,
    Op224Params,
    Op228TypeName,
    Op242Params,
    Op245Args,
    Op248Args,
    Op249Sound,
    Op250TypeName,
    Op250Box,

    // The repeats.
    RepeatCode32,
    RepeatPstr,
    RepeatU8Code32,
};

// What the table says about one opcode.
struct ScriptOpcodeInfo {
    // The operand codes, in order. Null when the opcode is not in the
    // table, which is how the decoder tells a bad opcode from a good one
    // with no operands.
    const OperandCode* operands;
    int32_t operandCount;

    // The opcode's mnemonic, for a disassembler. Null when the opcode has
    // none.
    const char* mnemonic;

    // The opcode never continues with the next instruction.
    bool noFallthrough;
};

// The opcode's layout, or null when the opcode is not in the table.
const ScriptOpcodeInfo* scriptOpcodeInfo(int32_t opcode);

// What the 1996 demo's table says about one of ITS opcodes.
struct BetaOpcodeInfo {
    // The operand codes, in order. Null when the demo's opcode is not in
    // the table, which is how the decoder tells a bad opcode from a good
    // one with no operands.
    const OperandCode* operands;
    int32_t operandCount;

    // The opcode the RETAIL VM runs for it. It is the demo's own opcode
    // for most of them, but the demo has some of its own (numbered above
    // 255) and its gosub and return are the retail's.
    int32_t opcode;
};

// The 1996 demo's opcode's layout, or null when it is not in the table.
const BetaOpcodeInfo* scriptBetaOpcodeInfo(int32_t opcode);

} // namespace formats
} // namespace mdk

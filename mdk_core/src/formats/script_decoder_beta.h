#pragma once

// ============================================================================
// script_decoder_beta.h — the 1996 demo's script bytecode.
//
// A 1:1 port of src/Mdk.Formats/Scripts/ScriptDecoder.Beta.cs.
//
// THE DEMO'S BYTECODE (MDKDEMO.EXE 0x47f6c, the Godot port's docs/beta96.md)
// is decoded into the instructions the retail VM runs: the opcodes 1-92 and
// 128-131 have the retail numbers and mostly the retail operands; the branch
// actions are 0x0C goto, 0xF0 gosub and 0xF1 return; the gosub and the
// return ARE the retail's opcodes 0xF0 and 0xF1. Where an opcode differs,
// the operands the retail handler expects are made here; the names are made
// UPPER CASE.
//
// THE DEMO'S OWN OPCODES are numbered above 255, so they cannot be confused
// with the retail's.
//
// The decoder itself is in script_decoder.h; this is the demo's half of it.
// ============================================================================

#include <stdint.h>

#include <vector>

namespace mdk {
namespace formats {

class Cmi;
class ScriptDecoder;

// The 1996 demo's opcodes that have no retail equivalent, numbered above
// 255.
enum class BetaOpcode {
    // Opcodes 41 (nothing) and 90 (a name and a byte: an arena by name; in
    // no script).
    Nothing = 300,
    // follow_path (2): [path]. A path stopped by opcode 21 goes on; the
    // object turns along it.
    FollowPath = 302,
    // Opcode 5: branches when the object's field +0x108 is 0 (in no
    // script).
    IfField108 = 305,
    // Opcode 28: branches while an object that sounded the alarm was just
    // deleted (0xe21de, 10 ticks).
    IfAlarmEnded = 328,
    // fire (61): [origin, aim, range, accuracy, ?], a bolt without a
    // script.
    Fire = 361,
};

// The decoder of a CMI file's scripts, in its dialect.
ScriptDecoder scriptDecoderFor(const Cmi& cmi);

// A decoder of the demo's bytecode, its paths not converted (the tests and
// the path search).
ScriptDecoder scriptDecoderBeta(const uint8_t* bytes, int32_t byteCount);

// The paths the demo's scripts follow (the raw offsets of opcode 2), found
// by walking every script from its entry point, in the order met.
std::vector<int32_t> scriptBetaPaths(const Cmi& cmi);

} // namespace formats
} // namespace mdk

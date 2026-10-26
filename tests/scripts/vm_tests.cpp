// vm_tests.cpp — the C++ twin of tests/Mdk.Game.Tests/ScriptVmTests.cs.
//
// The C# drives the script VM with tiny hand-assembled programs and a level 7
// runtime around them (no window). This file assembles the same bytes, runs
// them through the C++ VM, and asserts the same values. It is the synthetic
// per-opcode harness the coverage doc calls for: opcodes that never fire in
// the level 3-8 golden corpus (starting with 13, if_cheat_key) land here WITH
// the test that exercises them, instead of as blind ports.
//
// The C# file is the specification: same programs, same assertions, same
// level. A divergence fails here, not in a golden. The parts tests below are
// the exception: no C# counterpart exists, so they pin the oracle's
// ScriptVm.cs directly, with the same byte encoding the C# decoder reads.

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "audio/sound_bank.h"
#include "test_support.h"
#include "formats/case_insensitive_path.h"
#include "formats/cmi.h"
#include "formats/dti.h"
#include "formats/file_system.h"
#include "formats/mdk_data.h"
#include "formats/model.h"
#include "formats/script_decoder.h"
#include "game/level_data.h"
#include "scripts/script_runtime.h"

using namespace mdk;
using namespace mdk::formats;

namespace {

// Opcodes (docs/script_opcodes.md), same numbers as the C# test.
const int32_t kSetRestart = 1;
const int32_t kGoto = 12;
const int32_t kIfCheatKey = 13;
const int32_t kSetPartsMask = 31;
const int32_t kClearPartsMask = 32;
const int32_t kBlowOffParts = 129;
const int32_t kJumpTo = 226;
const int32_t kSet2d0Block = 242;
const int32_t kSpawnAtPoint = 156;
const int32_t kPushHitDir = 81;
const int32_t kWait = 64;
const int32_t kSetVar = 65;
const int32_t kAddVar = 66;
const int32_t kIfVar = 67;
const int32_t kSetFlag = 68;
const int32_t kIfFlagSet = 71;
const int32_t kGosub = 252;
const int32_t kReturn = 253;
// Operand values.
const int32_t kGlobal = 0;
const int32_t kOwn = 2;
// Flag word 5: the object's own door state (ScriptVm.cs:200).
const int32_t kDoor = 5;
const int32_t kLessThan = 1;
const int32_t kLiteral = 3;
const uint8_t kActionGoto = 0x0C;
const uint8_t kEnd = 0xFF;
// ObjectBehaviors.cs:21,25.
const int32_t kDoorClosed = 0x8;
const int32_t kDoorLocked = 0x40;

// Mirrors tick1.cpp's locator: the tests run from tests/scripts, the data can
// sit beside the repo or where MDK_DATA_DIR says.
MdkData locateData() {
    if (const char* env = getenv("MDK_DATA_DIR")) {
        if (env[0] != '\0') {
            const FileSystem* system = fileSystem();
            const std::string marker = caseInsensitiveResolve(system, env, "TRAVERSE/TRAVSPRT.BNI");
            if (system->fileExists(system->user, marker.c_str())) {
                return MdkData(system, env);
            }
        }
    }
    const char* relatives[] = {"../../MDK", "../../../MDK", "MDK"};
    const FileSystem* system = fileSystem();
    for (size_t i = 0; i < sizeof(relatives) / sizeof(relatives[0]); i++) {
        const std::string marker =
            caseInsensitiveResolve(system, relatives[i], "TRAVERSE/TRAVSPRT.BNI");
        if (system->fileExists(system->user, marker.c_str())) {
            return MdkData(system, relatives[i]);
        }
    }
    return MdkData();
}

// A program's bytes: code starts after a gap (offset 0 means "no target"),
// targets are written as stored in a CMI (offset - 4). Mirrors the C# Code.
class Code {
public:
    Code() {
        // FileBase 4, Entry 8: same gap as the C# builder.
        for (int i = 0; i < 8; i++) {
            bytes_.push_back(0);
        }
    }

    int entry() const { return 8; }

    Code& label(const std::string& name) {
        labels_[name] = bytes_.size();
        return *this;
    }

    Code& u8(int32_t value) {
        bytes_.push_back((uint8_t)value);
        return *this;
    }

    Code& f32(float value) {
        uint8_t raw[4];
        memcpy(raw, &value, sizeof(raw));
        for (int i = 0; i < 4; i++) {
            bytes_.push_back(raw[i]);
        }
        return *this;
    }

    Code& pstr(const std::string& value) {
        bytes_.push_back((uint8_t)value.size());
        for (size_t i = 0; i < value.size(); i++) {
            bytes_.push_back((uint8_t)value[i]);
        }
        return *this;
    }

    Code& target(const std::string& label) {
        fixups_.push_back(std::make_pair(bytes_.size(), label));
        for (int i = 0; i < 4; i++) {
            bytes_.push_back(0);
        }
        return *this;
    }

    size_t labelOffset(const std::string& name) const {
        std::map<std::string, size_t>::const_iterator it = labels_.find(name);
        return it == labels_.end() ? 0 : it->second;
    }

    Code& end() { return u8(kEnd); }

    std::vector<uint8_t> build() {
        std::vector<uint8_t> bytes = bytes_;
        for (size_t i = 0; i < fixups_.size(); i++) {
            // Stored as the file offset minus FileBase (script_decoder.h).
            int32_t value = (int32_t)labels_[fixups_[i].second] - 4;
            memcpy(&bytes[fixups_[i].first], &value, sizeof(value));
        }
        return bytes;
    }

private:
    std::vector<uint8_t> bytes_;
    std::map<std::string, size_t> labels_;
    std::vector<std::pair<size_t, std::string> > fixups_;
};

int failures = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            printf("  FAIL line %d: %s\n", __LINE__, #cond);                           \
            failures++;                                                                \
        }                                                                              \
    } while (0)

// The opcodes the last runProgram left unimplemented (the C# twin asserts on
// its own Unimplemented dict; the probe reads the same bucket).
std::vector<int32_t> lastUnimplemented_;

// Runs one program on a fresh object, like the C# Load + Run.
void runProgram(scripts::ScriptRuntime& runtime, Code& code, scripts::MdkObject* obj) {
    std::vector<uint8_t> bytes = code.build();
    ScriptDecoder decoder(bytes.data(), (int32_t)bytes.size());
    scripts::ScriptVm vm(runtime, decoder);
    obj->restart = code.entry();
    vm.run(obj);
    lastUnimplemented_.clear();
    const std::vector<std::pair<int32_t, int32_t> >& un = vm.unimplemented();
    for (size_t i = 0; i < un.size(); i++) {
        lastUnimplemented_.push_back(un[i].first);
    }
}

void testLoopCountsWithVariablesAndGoto(scripts::ScriptRuntime& runtime) {
    Code code;
    code.u8(kSetVar).u8(kOwn).u8(0).f32(0.0f);
    code.label("loop");
    code.u8(kAddVar).u8(kOwn).u8(0).f32(1.0f);
    code.u8(kIfVar).u8(kOwn).u8(0).u8(kLessThan).f32(5.0f).u8(kActionGoto).target("loop");
    code.u8(kSetVar).u8(kGlobal).u8(1).f32(7.0f);
    code.end();
    scripts::MdkObject obj;
    runProgram(runtime, code, &obj);

    CHECK(obj.variables[0] == 5.0f);
    CHECK(runtime.globalVariables[1] == 7.0f);
    CHECK(obj.restart != 0);
}

void testGosubReturnsAfterTheCall(scripts::ScriptRuntime& runtime) {
    Code code;
    code.u8(kGosub).u8(1).target("sub");
    code.u8(kSetVar).u8(kOwn).u8(1).f32(2.0f);
    code.end();
    code.label("sub");
    code.u8(kSetVar).u8(kOwn).u8(0).f32(1.0f);
    code.u8(kReturn);
    code.end();
    scripts::MdkObject obj;
    runProgram(runtime, code, &obj);

    CHECK(obj.variables[0] == 1.0f);
    CHECK(obj.variables[1] == 2.0f);
    CHECK(obj.gosub_returns.empty());
    CHECK(obj.restart == code.entry());
}

void testReturnWithoutGosubStopsTheScript(scripts::ScriptRuntime& runtime) {
    Code code;
    code.u8(kReturn);
    code.end();
    scripts::MdkObject obj;
    runProgram(runtime, code, &obj);

    CHECK(obj.restart == 0);
}

void testWaitResumesAfterItsTicks(scripts::ScriptRuntime& runtime) {
    // 0.09 s: the script goes on at the 4th tick (30 ticks per second).
    Code code;
    code.u8(kSetRestart);
    code.u8(kAddVar).u8(kOwn).u8(0).f32(1.0f);
    code.u8(kWait).u8(kLiteral).f32(0.09f);
    code.u8(kAddVar).u8(kOwn).u8(0).f32(10.0f);
    code.end();
    scripts::MdkObject obj;
    std::vector<uint8_t> bytes = code.build();
    ScriptDecoder decoder(bytes.data(), (int32_t)bytes.size());
    scripts::ScriptVm vm(runtime, decoder);
    obj.restart = code.entry();

    vm.run(&obj);
    CHECK(obj.variables[0] == 1.0f);
    vm.run(&obj);
    vm.run(&obj);
    CHECK(obj.variables[0] == 1.0f);
    vm.run(&obj);
    CHECK(obj.variables[0] == 11.0f);
}

void testFlagsBranchAndGotoZeroStops(scripts::ScriptRuntime& runtime) {
    const int32_t kBit = 3;
    Code code;
    code.u8(kSetFlag).u8(kOwn).u8(kBit);
    code.u8(kIfFlagSet).u8(kOwn).u8(kBit).u8(kActionGoto).target("set");
    code.end();
    code.label("set");
    code.u8(kSetVar).u8(kOwn).u8(2).f32(4.0f);
    code.u8(kGoto).u8(1).u8(0).u8(0).u8(0).u8(0);
    code.end();
    scripts::MdkObject obj;
    runProgram(runtime, code, &obj);

    CHECK(obj.script_flags == (1 << kBit));
    CHECK(obj.variables[2] == 4.0f);
    CHECK(obj.restart == 0);
}

void testFlagWordFiveIsTheDoorState(scripts::ScriptRuntime& runtime) {
    const int32_t kClosedBit = 3;
    const int32_t kLockedBit = 6;
    Code code;
    code.u8(kIfFlagSet).u8(kDoor).u8(kClosedBit).u8(kActionGoto).target("closed");
    code.end();
    code.label("closed");
    code.u8(kSetFlag).u8(kDoor).u8(kLockedBit);
    code.end();
    scripts::MdkObject obj;
    obj.flags |= scripts::MdkObject::FlagDoor;
    obj.door_state = kDoorClosed;
    scripts::MdkObject linked;
    obj.linked = &linked;
    runProgram(runtime, code, &obj);

    CHECK(obj.door_state == (kDoorClosed | kDoorLocked));
    CHECK(linked.script_flags == 0);
}

void testUnimplementedConditionsAreFalse(scripts::ScriptRuntime& runtime) {
    // if_cheat_key (13) never holds: the goto isn't taken. The C# owns the
    // opcode as an explicit false branch (ScriptVm.cs:1709); it must not land
    // in the unimplemented bucket either.
    Code code;
    code.u8(kIfCheatKey).u8(kActionGoto).target("cheat");
    code.u8(kSetVar).u8(kOwn).u8(0).f32(1.0f);
    code.end();
    code.label("cheat");
    code.u8(kSetVar).u8(kOwn).u8(0).f32(2.0f);
    code.end();
    scripts::MdkObject obj;
    runProgram(runtime, code, &obj);

    CHECK(obj.variables[0] == 1.0f);
    bool saw13 = false;
    for (size_t i = 0; i < lastUnimplemented_.size(); i++) {
        if (lastUnimplemented_[i] == kIfCheatKey) {
            saw13 = true;
        }
    }
    CHECK(!saw13);
}

// Whether the last runProgram left the opcode in the unimplemented bucket.
bool sawUnimplemented(int32_t opcode) {
    for (size_t i = 0; i < lastUnimplemented_.size(); i++) {
        if (lastUnimplemented_[i] == opcode) {
            return true;
        }
    }
    return false;
}

// A model with three named parts. PartsMask/FindPart only read the part-name
// list, so this stands in for level data without depending on it.
Model partsModel() {
    Model model;
    const char* names[] = {"HEAD", "TORSO", "LEG"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        Model::Part part;
        snprintf(part.name, sizeof(part.name), "%s", names[i]);
        model.partList.push_back(part);
    }
    return model;
}

// The parts family has no C# ScriptVmTests counterpart (that suite only
// covers 11 opcodes), so these tests pin the oracle directly
// (ScriptVm.cs:1523-1524 for 32, 1746-1769 for 129) with identical program
// bytes. Opcodes 132/136 stay unported: they need the Effects slice.
void testClearPartsMaskShowsPartsAgain(scripts::ScriptRuntime& runtime) {
    Model model = partsModel();
    scripts::MdkObject obj;
    obj.model = &model;

    // Hide HEAD (bit 0) and TORSO (bit 1) by name; LEG stays visible.
    Code hide;
    hide.u8(kSetPartsMask).u8(2).pstr("HEAD").pstr("TORSO");
    hide.end();
    runProgram(runtime, hide, &obj);
    CHECK(obj.hidden_parts == 0x3);
    CHECK(!sawUnimplemented(kSetPartsMask));

    // Show TORSO again by name.
    Code show;
    show.u8(kClearPartsMask).u8(1).pstr("TORSO");
    show.end();
    runProgram(runtime, show, &obj);
    CHECK(obj.hidden_parts == 0x1);
    CHECK(!sawUnimplemented(kClearPartsMask));

    // ALL shows the rest, but a blown-off (locked) HEAD stays hidden.
    obj.locked_parts = 0x1;
    Code all;
    all.u8(kClearPartsMask).u8(1).pstr("ALL");
    all.end();
    runProgram(runtime, all, &obj);
    CHECK(obj.hidden_parts == 0x1);
    CHECK(obj.locked_parts == 0x1);
    CHECK(!sawUnimplemented(kClearPartsMask));
}

void testBlowOffPartsLocksAndHides(scripts::ScriptRuntime& runtime) {
    Model model = partsModel();
    runtime.option = 1;

    // Gore on: mode 2 blows off HEAD (lowercase pins the case-insensitive
    // match) and locks it hidden; NOSE names no part and is skipped.
    scripts::MdkObject obj;
    obj.model = &model;
    Code gore;
    gore.u8(kBlowOffParts).u8(2).u8(2).pstr("head").pstr("NOSE");
    gore.end();
    runProgram(runtime, gore, &obj);
    CHECK(obj.locked_parts == 0x1);
    CHECK(obj.hidden_parts == 0x1);
    CHECK(!sawUnimplemented(kBlowOffParts));

    // Gore off: mode 2 is a no-op (ScriptVm.cs:1752).
    runtime.option = 0;
    scripts::MdkObject shy;
    shy.model = &model;
    runProgram(runtime, gore, &shy);
    CHECK(shy.locked_parts == 0);
    CHECK(shy.hidden_parts == 0);

    // Mode 1 is not gore-gated: it blows off even with the option off.
    scripts::MdkObject plain;
    plain.model = &model;
    Code mode1;
    mode1.u8(kBlowOffParts).u8(1).u8(1).pstr("LEG");
    mode1.end();
    runProgram(runtime, mode1, &plain);
    CHECK(plain.locked_parts == 0x4);
    CHECK(plain.hidden_parts == 0x4);

    // Mode past gore (3): no-op, and the decoder leaves no parts operand.
    scripts::MdkObject wild;
    wild.model = &model;
    Code mode3;
    mode3.u8(kBlowOffParts).u8(3);
    mode3.end();
    runProgram(runtime, mode3, &wild);
    CHECK(wild.locked_parts == 0);
    CHECK(wild.hidden_parts == 0);
    CHECK(!sawUnimplemented(kBlowOffParts));

    runtime.option = 0;
}

void testBlownOffPartsStayHiddenThroughClear(scripts::ScriptRuntime& runtime) {
    // The in-game sequence: 129 blows off HEAD, then 32 shows ALL.
    Model model = partsModel();
    runtime.option = 1;
    scripts::MdkObject obj;
    obj.model = &model;
    Code blow;
    blow.u8(kBlowOffParts).u8(2).u8(1).pstr("HEAD");
    blow.end();
    runProgram(runtime, blow, &obj);
    CHECK(!sawUnimplemented(kBlowOffParts));
    Code all;
    all.u8(kClearPartsMask).u8(1).pstr("ALL");
    all.end();
    runProgram(runtime, all, &obj);
    CHECK(obj.locked_parts == 0x1);
    CHECK(obj.hidden_parts == 0x1);
    CHECK(!sawUnimplemented(kClearPartsMask));
    runtime.option = 0;
}

// The rope-swing pair has no C# ScriptVmTests counterpart either, so these
// pin the oracle directly (ScriptVm.cs:1139-1166) with identical program
// bytes. Opcode 85 (set_rolling) stays unported: SetRolling snapshots the
// full Rotation matrix into RollingBasis, and rotation() is deferred to the
// FrameBlend slice (object.h) — a faithful port needs that arithmetic.
void testJumpToStartsSwingBelowThePivot(scripts::ScriptRuntime& runtime) {
    scripts::MdkObject obj;
    obj.position = mdk::Vec3{10.0f, 20.0f, 5.0f};
    obj.yaw = 30.0f;
    Code code;
    code.u8(kJumpTo).f32(11.0f).f32(22.0f).f32(45.0f).f32(2.5f).f32(0.75f);
    code.end();
    runProgram(runtime, code, &obj);

    CHECK(!sawUnimplemented(kJumpTo));
    CHECK(obj.swing_pivot.x == 11.0f);
    CHECK(obj.swing_pivot.y == 22.0f);
    CHECK(obj.swing_pivot.z == 45.0f);
    CHECK(obj.swing_speed == 2.5f);
    CHECK(obj.swing_gain == 0.75f);
    CHECK(obj.swing_yaw == 30.0f);
    CHECK(obj.swing_length == 40.0f);
    CHECK((obj.flags & scripts::MdkObject::FlagSwinging) != 0);
}

void testSet2d0BlockDrawsTheRopeLines(scripts::ScriptRuntime& runtime) {
    // Count zero clears the mask, even over a stale nonzero one; the colour
    // and the points survive (the C# touches only RopeMask on this path).
    scripts::MdkObject cleared;
    cleared.rope_mask = 0xFF;
    cleared.rope_color = 1;
    cleared.rope_points[0] = mdk::Vec3{9.0f, 9.0f, 9.0f};
    Code zero;
    zero.u8(kSet2d0Block).u8(0);
    zero.end();
    runProgram(runtime, zero, &cleared);
    CHECK(!sawUnimplemented(kSet2d0Block));
    CHECK(cleared.rope_mask == 0);
    CHECK(cleared.rope_color == 1);
    CHECK(cleared.rope_points[0].x == 9.0f);
    CHECK(cleared.rope_points[0].y == 9.0f);
    CHECK(cleared.rope_points[0].z == 9.0f);

    // Count nonzero: colour 1, all lines, all four points in order.
    scripts::MdkObject obj;
    Code code;
    code.u8(kSet2d0Block).u8(1)
        .f32(1.0f).f32(2.0f).f32(3.0f)
        .f32(4.0f).f32(5.0f).f32(6.0f)
        .f32(7.0f).f32(8.0f).f32(9.0f)
        .f32(10.0f).f32(11.0f).f32(12.0f);
    code.end();
    runProgram(runtime, code, &obj);
    CHECK(!sawUnimplemented(kSet2d0Block));
    CHECK(obj.rope_color == 1);
    CHECK(obj.rope_mask == 0xFF);
    CHECK(obj.rope_points[0].x == 1.0f);
    CHECK(obj.rope_points[0].y == 2.0f);
    CHECK(obj.rope_points[0].z == 3.0f);
    CHECK(obj.rope_points[1].x == 4.0f);
    CHECK(obj.rope_points[1].y == 5.0f);
    CHECK(obj.rope_points[1].z == 6.0f);
    CHECK(obj.rope_points[2].x == 7.0f);
    CHECK(obj.rope_points[2].y == 8.0f);
    CHECK(obj.rope_points[2].z == 9.0f);
    CHECK(obj.rope_points[3].x == 10.0f);
    CHECK(obj.rope_points[3].y == 11.0f);
    CHECK(obj.rope_points[3].z == 12.0f);
}

// Opcode 156 (spawn_at_point) has no C# ScriptVmTests counterpart either, so
// this pins the oracle directly (ScriptVm.cs:1573, MdkObject.cs:574-582): the
// named type spawns at the parent's reference point with yaw 0 and instance
// -1, and an out-of-range point falls back to the position. BEAMS is a model
// of the DANT_1 arena (LEVEL7O.MTO) with no init script (no DANT_1$BEAMS key
// in the CMI object scripts), so the spawn runs no nested script and these
// assertions observe only opcode 156.
void testSpawnAtPointSpawnsAtTheReferencePoint(scripts::ScriptRuntime& runtime) {
    Model model;
    Vec3f ref;
    ref.x = 1.0f;
    ref.y = 2.0f;
    ref.z = 3.0f;
    model.referencePoints.push_back(ref);

    const size_t before = runtime.objects().size();
    scripts::MdkObject obj;
    obj.arena = "DANT_1";
    obj.model = &model;
    obj.position = mdk::Vec3{10.0f, 20.0f, 5.0f};
    obj.yaw = 0.0f;
    Code code;
    code.u8(kSpawnAtPoint).u8(0).pstr("BEAMS").target("spawned");
    // The separator keeps the label off the spawn's own fall-through, so the
    // exact-offset pin below distinguishes ival(op(ins, 2)) from ins.next.
    code.u8(kSetVar).u8(kOwn).u8(0).f32(0.0f);
    code.label("spawned");
    code.end();
    runProgram(runtime, code, &obj);
    CHECK(!sawUnimplemented(kSpawnAtPoint));
    CHECK(runtime.objects().size() == before + 1);
    const scripts::MdkObject& created = runtime.objects()[before];
    CHECK(created.type_name == "BEAMS");
    CHECK(created.arena == "DANT_1");
    CHECK(created.position.x == 11.0f);
    CHECK(created.position.y == 22.0f);
    CHECK(created.position.z == 8.0f);
    CHECK(created.spawn_position.x == 11.0f);
    CHECK(created.spawn_position.y == 22.0f);
    CHECK(created.spawn_position.z == 8.0f);
    CHECK(created.yaw == 0.0f);
    CHECK(created.instance_id >= 0);
    const int32_t firstInstance = created.instance_id;
    // The script operand reaches the object: the C++ dispatch passes
    // ival(op(ins, 2)) as the spawn script (script_vm.cpp) and spawn ends
    // with created->restart = script (script_runtime.cpp), mirroring C#
    // I(o[2]) into Spawning (ScriptVm.cs:1574). The exact-offset pin below
    // distinguishes that operand from the spawn's own fall-through (ins.next
    // points at the separator set_var, not the label).
    CHECK(created.restart == (int32_t)code.labelOffset("spawned"));
    CHECK(created.previous_position.x == created.position.x);
    CHECK(created.previous_position.y == created.position.y);
    CHECK(created.previous_position.z == created.position.z);
    {
        std::vector<uint8_t> bytes = code.build();
        const bool restartInRange =
            created.restart >= code.entry() && created.restart < (int32_t)bytes.size();
        CHECK(restartInRange);
        CHECK(restartInRange && bytes[(size_t)created.restart] == (uint8_t)ScriptDecoder::kEnd);
    }

    // An out-of-range reference point falls back to the position, and a
    // second -1 instance takes the next id.
    scripts::MdkObject plain;
    plain.arena = "DANT_1";
    plain.model = &model;
    plain.position = mdk::Vec3{1.0f, 2.0f, 3.0f};
    plain.yaw = 0.0f;
    const size_t again = runtime.objects().size();
    Code fallback;
    fallback.u8(kSpawnAtPoint).u8(5).pstr("BEAMS").target("landed");
    fallback.u8(kSetVar).u8(kOwn).u8(0).f32(0.0f);
    fallback.label("landed");
    fallback.end();
    runProgram(runtime, fallback, &plain);
    CHECK(!sawUnimplemented(kSpawnAtPoint));
    CHECK(runtime.objects().size() == again + 1);
    const scripts::MdkObject& landed = runtime.objects()[again];
    CHECK(landed.position.x == 1.0f);
    CHECK(landed.position.y == 2.0f);
    CHECK(landed.position.z == 3.0f);
    CHECK(landed.instance_id == firstInstance + 1);
    CHECK(landed.restart == (int32_t)fallback.labelOffset("landed"));

    // The rotation/scale half of reference_point: a yawed, scaled parent
    // pins the C# Position + Transform(p * Scale, RotZ(yaw)) row-vector form
    // (MdkObject.cs:574-582), which the axis-aligned case above cannot see.
    // Hand-computed: ref (3,1,0.5) at scale 2 is (6,2,1); RotZ(90) takes
    // (x,y) to (-y,x), so (-2,6,1) at pos (10,20,5) lands on (8,26,6).
    // Tolerance, not equality: cosf(pi/2) is ~-4.4e-8, not exactly 0. Each
    // wrong shape lands elsewhere (no scale: (9,23,5.5); no rotation:
    // (16,22,6); flipped sign: (12,14,6)), so this pins all three.
    Model turned2Model;
    Vec3f ref2;
    ref2.x = 3.0f;
    ref2.y = 1.0f;
    ref2.z = 0.5f;
    turned2Model.referencePoints.push_back(ref2);
    scripts::MdkObject spinner;
    spinner.arena = "DANT_1";
    spinner.model = &turned2Model;
    spinner.position = mdk::Vec3{10.0f, 20.0f, 5.0f};
    spinner.yaw = 90.0f;
    spinner.scale = 2.0f;
    const size_t third = runtime.objects().size();
    Code turned;
    turned.u8(kSpawnAtPoint).u8(0).pstr("BEAMS").target("go");
    turned.u8(kSetVar).u8(kOwn).u8(0).f32(0.0f);
    turned.label("go");
    turned.end();
    runProgram(runtime, turned, &spinner);
    CHECK(!sawUnimplemented(kSpawnAtPoint));
    CHECK(runtime.objects().size() == third + 1);
    const scripts::MdkObject& rotated = runtime.objects()[third];
    CHECK(fabsf(rotated.position.x - 8.0f) < 1e-3f);
    CHECK(fabsf(rotated.position.y - 26.0f) < 1e-3f);
    CHECK(fabsf(rotated.position.z - 6.0f) < 1e-3f);
    CHECK(rotated.previous_position.x == rotated.position.x);
    CHECK(rotated.previous_position.y == rotated.position.y);
    CHECK(rotated.previous_position.z == rotated.position.z);
    CHECK(rotated.restart == (int32_t)turned.labelOffset("go"));
}

// Opcode 81 (push_hit_dir) has no C# ScriptVmTests counterpart either, so this
// pins the oracle directly (ScriptVm.cs:601-607): the Value operand (literal
// or variable), times Dt in mode 1, added along the last-hit heading; z is
// untouched.
void testPushHitDirAddsVelocityAlongTheLastHit(scripts::ScriptRuntime& runtime) {
    // Mode 0, literal speed, heading east: exactly (10, 0).
    {
        Code code;
        code.u8(kPushHitDir).u8(0).u8(kLiteral).f32(10.0f);
        code.end();
        scripts::MdkObject obj;
        obj.hit_direction = 0.0f;
        obj.velocity = mdk::Vec3{1.0f, 2.0f, 3.0f};
        runProgram(runtime, code, &obj);
        CHECK(!sawUnimplemented(kPushHitDir));
        CHECK(obj.velocity.x == 11.0f);
        CHECK(obj.velocity.y == 2.0f);
        CHECK(obj.velocity.z == 3.0f);
    }
    // Mode 1 scales by the tick: 30 * Dt lands within an ulp of 1.
    {
        Code code;
        code.u8(kPushHitDir).u8(1).u8(kLiteral).f32(30.0f);
        code.end();
        scripts::MdkObject obj;
        obj.hit_direction = 0.0f;
        runProgram(runtime, code, &obj);
        CHECK(!sawUnimplemented(kPushHitDir));
        CHECK(fabsf(obj.velocity.x - 1.0f) < 1e-6f);
        CHECK(obj.velocity.y == 0.0f);
        CHECK(obj.velocity.z == 0.0f);
    }
    // A variable speed along heading north lands on +y (tolerance: the C#
    // SinCos and the C++ sinf/cosf can differ by an ulp at 90 degrees).
    {
        Code code;
        code.u8(kSetVar).u8(kOwn).u8(0).f32(5.0f);
        code.u8(kPushHitDir).u8(0).u8(kOwn).u8(0);
        code.end();
        scripts::MdkObject obj;
        obj.hit_direction = 90.0f;
        runProgram(runtime, code, &obj);
        CHECK(!sawUnimplemented(kPushHitDir));
        CHECK(fabsf(obj.velocity.x) < 1e-4f);
        CHECK(fabsf(obj.velocity.y - 5.0f) < 1e-4f);
        CHECK(obj.velocity.z == 0.0f);
    }
}

} // namespace

int main() {
    // The formats library reaches the filesystem through this seam.
    mdk::formats::setFileSystem(mdk::test::host::fileSystem());

    MdkData data = locateData();
    if (!data.found()) {
        printf("SKIP: no MDK data (set MDK_DATA_DIR)\n");
        return 0;
    }
    const int32_t kLevel = 7;
    LevelData levelData;
    if (!LevelData::load(&levelData, &data, kLevel)) {
        printf("FAIL: level %d did not load\n", kLevel);
        return 1;
    }
    char cmiPath[128];
    snprintf(cmiPath, sizeof(cmiPath), "TRAVERSE/LEVEL%d/LEVEL%d.CMI", kLevel, kLevel);
    std::vector<uint8_t> cmiBytes;
    if (!data.read(cmiPath, &cmiBytes)) {
        printf("FAIL: CMI did not read\n");
        return 1;
    }
    Cmi cmi;
    if (!Cmi::parse(&cmi, cmiBytes.data(), (int32_t)cmiBytes.size())) {
        printf("FAIL: CMI did not parse\n");
        return 1;
    }
    audio::SoundBank bank;
    if (!audio::SoundBank::forLevel(&bank, data, kLevel)) {
        printf("FAIL: sound bank did not load\n");
        return 1;
    }
    // No preload: the programs play nothing, and the C# fixture only builds a
    // mixer around an unloaded bank too.

    scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank);
    runtime.option = 0;

    testLoopCountsWithVariablesAndGoto(runtime);
    printf("PASS LoopCountsWithVariablesAndGoto\n");
    testGosubReturnsAfterTheCall(runtime);
    printf("PASS GosubReturnsAfterTheCall\n");
    testReturnWithoutGosubStopsTheScript(runtime);
    printf("PASS ReturnWithoutGosubStopsTheScript\n");
    testWaitResumesAfterItsTicks(runtime);
    printf("PASS WaitResumesAfterItsTicks\n");
    testFlagsBranchAndGotoZeroStops(runtime);
    printf("PASS FlagsBranchAndGotoZeroStops\n");
    testFlagWordFiveIsTheDoorState(runtime);
    printf("PASS FlagWordFiveIsTheDoorState\n");
    testUnimplementedConditionsAreFalse(runtime);
    printf("PASS UnimplementedConditionsAreFalse\n");
    testClearPartsMaskShowsPartsAgain(runtime);
    printf("PASS ClearPartsMaskShowsPartsAgain\n");
    testBlowOffPartsLocksAndHides(runtime);
    printf("PASS BlowOffPartsLocksAndHides\n");
    testBlownOffPartsStayHiddenThroughClear(runtime);
    printf("PASS BlownOffPartsStayHiddenThroughClear\n");
    testJumpToStartsSwingBelowThePivot(runtime);
    printf("PASS JumpToStartsSwingBelowThePivot\n");
    testSet2d0BlockDrawsTheRopeLines(runtime);
    printf("PASS Set2d0BlockDrawsTheRopeLines\n");
    testSpawnAtPointSpawnsAtTheReferencePoint(runtime);
    printf("PASS SpawnAtPointSpawnsAtTheReferencePoint\n");
    testPushHitDirAddsVelocityAlongTheLastHit(runtime);
    printf("PASS PushHitDirAddsVelocityAlongTheLastHit\n");

    if (failures == 0) {
        printf("All VM unit tests match the oracle.\n");
        return 0;
    }
    printf("%d check(s) FAILED\n", failures);
    return 1;
}

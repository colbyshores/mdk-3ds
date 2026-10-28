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

#include <deque>
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
#include "game/player.h"
#include "scripts/script_runtime.h"
#include "util/math_lut.h"

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
const int32_t kSpawnAlongPath = 206;
const int32_t kHeadShots = 217;
const int32_t kIfPointSeesPlayer = 243;
const int32_t kIfPlayerHealth = 241;
const int32_t kIfPartnerDistance = 213;
const int32_t kIfPartnerAngle = 214;
const int32_t kSpawnPartner = 179;
const int32_t kSnapToPartner = 180;
const int32_t kLinkPartners = 228;
const int32_t kSet104 = 199;
const int32_t kSet2c0 = 210;
const int32_t kScreenFlash = 215;
const int32_t kSetHurtFlash = 240;
const int32_t kSetGlobal573bd4 = 244;
const int32_t kSet27c = 239;
const int32_t kDebugMsg = 77;
const int32_t kAlarm = 15;
const int32_t kIfAlarm = 27;
const int32_t kSetRolling = 85;
const int32_t kAttachTo = 74;
const int32_t kSpawnChain = 29;
const int32_t kReleaseChildren = 49;
const int32_t kAvoidObjects = 204;
const int32_t kIfFloorBelow = 121;
const int32_t kIfNoFlatFloorAt = 192;
const int32_t kIfHitWeapon = 195;
const int32_t kRaiseShake = 135;
const int32_t kInheritKurtVel = 170;
const int32_t kSetSkyMode = 202;
const int32_t kTurnAndJump = 229;
// The chain links' movement command (ObjectMotion.cs:370).
const int32_t kChainCommand = 30;
const int32_t kIfNoLeader = 34;
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
const int32_t kLinked = 3;
// Flag word 5: the object's own door state (ScriptVm.cs:200).
const int32_t kDoor = 5;
const int32_t kLessThan = 1;
const int32_t kGreaterThan = 2;
const int32_t kLiteral = 3;
const int32_t kApproxEqual = 5;  // |value - threshold| < epsilon
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

    // A 16-bit operand (the decoder's u16/s16: little-endian, like Bin.U16).
    Code& u16(int32_t value) {
        bytes_.push_back((uint8_t)(value & 0xFF));
        bytes_.push_back((uint8_t)((value >> 8) & 0xFF));
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

    // A raw 32-bit operand (Data32): the decoder adds FileBase back, so callers
    // pass the stored form (file offset minus 4), like target() does.
    Code& u32(int32_t value) {
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
// bytes.
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

// Opcode 170 (inherit_kurt_vel) has no C# ScriptVmTests counterpart, so this
// pins the oracle directly (ScriptVm.cs:1603-1604): the receiver's velocity
// gains Kurt's current velocity multiplied by the numeric operand. The source
// is the live Player (C# Kurt.Velocity, Kurt.cs:132-133), not a copied value.
void testInheritKurtVelocityAddsScaledKurtVelocity(LevelData* levelData, const Cmi* cmi,
                                                    audio::SoundBank* bank) {
    Player player;
    player.yaw = 0.0f;
    player.forwardSpeed = 2.0f;
    player.strafeSpeed = 4.0f;
    player.verticalSpeed = 6.0f;
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, &player);

    // At yaw zero, Player::velocity is (2, -4, 6). A literal scale is applied
    // to every component and added to the receiver's existing velocity.
    {
        Code code;
        code.u8(kInheritKurtVel).f32(0.5f);
        code.u8(kSetVar).u8(kOwn).u8(0).f32(1.0f);
        code.end();
        scripts::MdkObject obj;
        obj.velocity = mdk::Vec3{1.0f, 2.0f, 3.0f};
        runProgram(runtime, code, &obj);
        CHECK(!sawUnimplemented(kInheritKurtVel));
        CHECK(obj.variables[0] == 1.0f);
        CHECK(obj.velocity.x == 2.0f);
        CHECK(obj.velocity.y == 0.0f);
        CHECK(obj.velocity.z == 6.0f);
    }

    // Change the Player after the runtime was constructed. The dispatch must
    // read the live velocity, not a construction-time or previous-tick copy.
    player.forwardSpeed = 10.0f;
    {
        Code code;
        code.u8(kInheritKurtVel).f32(-2.0f);
        code.end();
        scripts::MdkObject obj;
        obj.velocity = mdk::Vec3{1.0f, 2.0f, 3.0f};
        runProgram(runtime, code, &obj);
        CHECK(!sawUnimplemented(kInheritKurtVel));
        CHECK(obj.velocity.x == -19.0f);
        CHECK(obj.velocity.y == 10.0f);
        CHECK(obj.velocity.z == -9.0f);
    }

    // The C# velocity includes the horizontal knock-down push. Its combat
    // state machine is deferred, but omitting this term from Player::velocity
    // would make every inherited knockdown velocity wrong.
    player.addKnockdownPush(mdk::Vec2{3.0f, -1.0f});
    {
        Code code;
        code.u8(kInheritKurtVel).f32(1.0f);
        code.end();
        scripts::MdkObject obj;
        runProgram(runtime, code, &obj);
        CHECK(!sawUnimplemented(kInheritKurtVel));
        CHECK(obj.velocity.x == 13.0f);
        CHECK(obj.velocity.y == -5.0f);
        CHECK(obj.velocity.z == 6.0f);
    }

    // The C# drains the push by 90 u/s^2 (PushDrain). The walk move captures
    // the PRE-DRAIN push (Kurt.cs:221), and only then does DrainPush run before
    // the move is applied and the vertical update runs (Kurt.cs:222-224). Pin
    // both halves: the displacement must be (3, -1) * delta, while the velocity
    // after this one 1/60-second step is (1.5, 0). A drain placed before the
    // move computation would displace only (1.5, 0) * delta.
    player.forwardSpeed = 0.0f;
    player.strafeSpeed = 0.0f;
    player.verticalSpeed = 0.0f;
    const Vec3 beforePushWalk = player.feet;
    Input idle;
    player.update(idle, 1.0f / 60.0f);
    CHECK(fabsf(player.feet.x - beforePushWalk.x - 3.0f * (1.0f / 60.0f)) < 1e-6f);
    CHECK(fabsf(player.feet.y - beforePushWalk.y + 1.0f * (1.0f / 60.0f)) < 1e-6f);
    CHECK(fabsf(player.velocity().x - 1.5f) < 1e-6f);
    CHECK(player.velocity().y == 0.0f);
}

// Opcode 206 (spawn_along_path) has no C# ScriptVmTests counterpart either, so
// this pins the oracle directly (ScriptVm.cs:440-448): from time 0 to before
// the path's last key, one object per step, flagged like spawn_flagged
// (0x2008A6), at the path position for the accumulated float time. Path 53729
// of LEVEL7.CMI has 3 keys at frames [0, 5, 10] (verified against the file,
// and re-pinned below so a data change fails here, not silently elsewhere);
// step 5 spawns exactly at times 0 and 5 (5+5 == 10 ends the accumulation).
void testSpawnAlongPathSpawnsEveryStep(scripts::ScriptRuntime& runtime) {
    const int32_t kPath = 53729;
    CHECK(runtime.pathKeyCount(kPath) == 3);
    CHECK(runtime.pathKeyFrame(kPath, runtime.pathKeyCount(kPath) - 1) == 10);

    const size_t before = runtime.objects().size();
    scripts::MdkObject obj;
    obj.arena = "DANT_1";
    Code code;
    code.u8(kSpawnAlongPath).u32(kPath - 4).f32(5.0f).pstr("BEAMS").target("along");
    code.u8(kSetVar).u8(kOwn).u8(0).f32(0.0f);
    code.label("along");
    code.end();
    runProgram(runtime, code, &obj);
    CHECK(!sawUnimplemented(kSpawnAlongPath));
    CHECK(runtime.objects().size() == before + 2);
    const float times[2] = {0.0f, 5.0f};
    int32_t firstInstance = -1;
    for (int i = 0; i < 2; i++) {
        const scripts::MdkObject& created = runtime.objects()[before + (size_t)i];
        CHECK(created.type_name == "BEAMS");
        CHECK(created.arena == "DANT_1");
        CHECK(created.flags == scripts::ScriptRuntime::kSpawnFlaggedFlags);
        const mdk::Vec3 want = runtime.pathPosition(kPath, times[i]);
        CHECK(created.position.x == want.x);
        CHECK(created.position.y == want.y);
        CHECK(created.position.z == want.z);
        CHECK(created.spawn_position.x == want.x);
        CHECK(created.spawn_position.y == want.y);
        CHECK(created.spawn_position.z == want.z);
        CHECK(created.yaw == 0.0f);
        CHECK(created.instance_id >= 0);
        CHECK(created.restart == (int32_t)code.labelOffset("along"));
        if (i == 0) {
            firstInstance = created.instance_id;
        } else {
            CHECK(created.instance_id == firstInstance + 1);
        }
    }
    // The two steps land at different times: t=0 is (0,0,0), t=5 is (0,0,6).
    const scripts::MdkObject& first = runtime.objects()[before];
    const scripts::MdkObject& second = runtime.objects()[before + 1];
    CHECK(first.position.z != second.position.z);
    // Absolute key-frame positions (not just relative to pathPosition): they
    // pin the position/frame-field reads, not the Hermite tangents (any
    // tangent field reproduces the key positions at u=0 and u=1, so a
    // tangent-assignment regression would pass these silently; tangents are
    // covered by the path-following corpus goldens instead).
    CHECK(first.position.x == 0.0f);
    CHECK(first.position.y == 0.0f);
    CHECK(first.position.z == 0.0f);
    CHECK(second.position.z == 6.0f);

    // A non-exact step pins the accumulation itself. Step 0.1f is not exactly
    // representable, so repeated float addition drifts from i*step (first
    // differing at i=7: accumulated 0x3F333334 vs multiplied 0x3F333333). An
    // integer-count rewrite (time = i*step) passes the exact-step case above
    // unchanged but fails here. Count and every position come from the
    // accumulated sequence, exactly as the oracle loop produces them
    // (ScriptVm.cs:445): 100 spawns, last time 9.9000015f.
    {
        const size_t before2 = runtime.objects().size();
        const float kStep = 0.1f;
        scripts::MdkObject obj2;
        obj2.arena = "DANT_1";
        Code code2;
        code2.u8(kSpawnAlongPath).u32(kPath - 4).f32(kStep).pstr("BEAMS").target("along2");
        code2.u8(kSetVar).u8(kOwn).u8(0).f32(0.0f);
        code2.label("along2");
        code2.end();
        runProgram(runtime, code2, &obj2);
        CHECK(!sawUnimplemented(kSpawnAlongPath));
        CHECK(runtime.objects().size() == before2 + 100);
        float acc = 0.0f;
        int differ = 0;
        for (int i = 0; i < 100; i++) {
            const scripts::MdkObject& created = runtime.objects()[before2 + (size_t)i];
            const mdk::Vec3 want = runtime.pathPosition(kPath, acc);
            CHECK(created.position.x == want.x);
            CHECK(created.position.y == want.y);
            CHECK(created.position.z == want.z);
            if (acc != (float)i * kStep) {
                differ++;
            }
            acc += kStep;
        }
        // The discrimination guard: if accumulation never differed from
        // i*step on this data, the case above would be redundant.
        CHECK(differ > 0);
    }
}

// Opcode 217 (global_573c4c_add) has no C# ScriptVmTests counterpart, so
// this pins the oracle directly (ScriptVm.cs:1606-1611): the head-shot count
// grows by the second operand only when the first is 1. The s32 second
// operand rides the same four bytes the decoder reads (u32 writes the
// identical little-endian encoding for these values).
void testHeadShotsAddOnlyWhenEnabled(scripts::ScriptRuntime& runtime) {
    runtime.stats.headShots = 0;
    scripts::MdkObject obj;
    Code add;
    add.u8(kHeadShots).u8(1).u32(3);
    add.end();
    runProgram(runtime, add, &obj);
    CHECK(!sawUnimplemented(kHeadShots));
    CHECK(runtime.stats.headShots == 3);
    Code more;
    more.u8(kHeadShots).u8(1).u32(2);
    more.end();
    runProgram(runtime, more, &obj);
    CHECK(!sawUnimplemented(kHeadShots));
    CHECK(runtime.stats.headShots == 5);
    // The S32 operand accepts negative values too; adding -5 subtracts.
    Code subtract;
    subtract.u8(kHeadShots).u8(1).u32(-5);
    subtract.end();
    runProgram(runtime, subtract, &obj);
    CHECK(!sawUnimplemented(kHeadShots));
    CHECK(runtime.stats.headShots == 0);
    // First operand 0: no count, and the script stays in sync.
    Code off;
    off.u8(kHeadShots).u8(0).u32(10);
    off.end();
    runProgram(runtime, off, &obj);
    CHECK(!sawUnimplemented(kHeadShots));
    CHECK(runtime.stats.headShots == 0);
    // The struct underneath: enemies match case-insensitively, created and
    // killed count separately, percent truncates and is 0 without shots.
    CHECK(scripts::isEnemyType("xb1"));
    CHECK(!scripts::isEnemyType("KURT"));
    scripts::GameStats counts;
    counts.countEnemy("xb", scripts::GameStats::Created);
    counts.countEnemy("XB1", scripts::GameStats::Killed);
    counts.countEnemy("kurt", scripts::GameStats::Killed);
    CHECK(counts.enemies == 1);
    CHECK(counts.kills == 1);
    CHECK(scripts::GameStats::percent(1, 3) == 33);
    CHECK(scripts::GameStats::percent(2, 3) == 66);
    CHECK(scripts::GameStats::percent(0, 0) == 0);
}

// Opcode 202 (set_574304) has no C# ScriptVmTests counterpart, so this pins
// the oracle directly (ScriptVm.cs:1613-1615): the signed-byte operand is
// written verbatim to the runtime's shared sky mode. The C# renderer consumes
// 0 as shown, 1 as black, and every other value as keep-last-frame; that
// renderer consumer remains a later slice.
void testSetSkyModeWritesSharedLevelState(LevelData* levelData, const Cmi* cmi,
                                          audio::SoundBank* bank) {
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr);
    CHECK(runtime.skyMode == 0);

    scripts::MdkObject obj;
    Code shown;
    shown.u8(kSetSkyMode).u8(0);
    shown.u8(kSetVar).u8(kOwn).u8(0).f32(1.0f);
    shown.end();
    runtime.skyMode = 17;
    runProgram(runtime, shown, &obj);
    CHECK(!sawUnimplemented(kSetSkyMode));
    CHECK(runtime.skyMode == 0);
    CHECK(obj.variables[0] == 1.0f);  // The instruction after 202 still runs.

    Code black;
    black.u8(kSetSkyMode).u8(1);
    black.end();
    runProgram(runtime, black, &obj);
    CHECK(runtime.skyMode == 1);

    // Sky mode is level state, not per-tick state: the oracle does not clear
    // it when the world advances.
    runtime.update(scripts::ScriptRuntime::kTick);
    CHECK(runtime.skyMode == 1);

    // The operand is signed: 0xff is -1, the mode that keeps the last frame.
    Code hidden;
    hidden.u8(kSetSkyMode).u8(0xFF);
    hidden.end();
    runProgram(runtime, hidden, &obj);
    CHECK(runtime.skyMode == -1);

    // Other values are stored unchanged for the renderer's keep-last-frame
    // decision; the VM does not clamp or normalize them.
    Code other;
    other.u8(kSetSkyMode).u8(42);
    other.end();
    runProgram(runtime, other, &obj);
    CHECK(runtime.skyMode == 42);
}

// Opcode 241 (if_player_health) has no C# ScriptVmTests counterpart. Pin its
// comparison against Kurt after damage (99), including true and false branch
// outcomes; opcode 241 does not fire in the level corpus.
void testIfPlayerHealthComparesKurtHealth(LevelData* levelData, const Cmi* cmi,
                                           audio::SoundBank* bank) {
    // A fresh runtime keeps this fixture independent of the shared test state:
    // damaging Kurt here must not leak into any later test.
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr);
    runtime.hurtKurt(1);
    CHECK(runtime.kurt().health() == 99);
    {
        Code code;
        code.u8(kIfPlayerHealth).u8(kLessThan).f32(100.0f).u8(kActionGoto).target("healthy");
        code.u8(kSetVar).u8(kOwn).u8(0).f32(1.0f);
        code.end();
        code.label("healthy");
        code.u8(kSetVar).u8(kOwn).u8(0).f32(2.0f);
        code.end();
        scripts::MdkObject obj;
        // Pinned defensively: the default is already 100, so this only guards
        // a future default change. The default itself rejects a receiver read.
        obj.health = 100;
        runProgram(runtime, code, &obj);
        CHECK(!sawUnimplemented(kIfPlayerHealth));
        CHECK(obj.variables[0] == 2.0f);
    }
    {
        Code code;
        code.u8(kIfPlayerHealth).u8(kGreaterThan).f32(99.0f).u8(kActionGoto).target("low");
        code.u8(kSetVar).u8(kOwn).u8(0).f32(1.0f);
        code.end();
        code.label("low");
        code.u8(kSetVar).u8(kOwn).u8(0).f32(2.0f);
        code.end();
        scripts::MdkObject obj;
        // Pinned defensively: the default is already 100, so this only guards
        // a future default change. The default itself rejects a receiver read.
        obj.health = 100;
        runProgram(runtime, code, &obj);
        CHECK(!sawUnimplemented(kIfPlayerHealth));
        CHECK(obj.variables[0] == 1.0f);
    }
}

// Opcodes 213 (if_partner_distance) and 214 (if_partner_angle) have no C#
// ScriptVmTests counterpart. These pin the oracle directly: both use zero
// without a live linked object; a live partner's distance is the full 3D
// distance from the receiver, floored at one; its angle is
// wrap(receiverYaw - bearingToPartner), the negated turn required to face the
// partner.
int32_t runPartnerCondition(scripts::ScriptRuntime& runtime, int32_t opcode,
                             scripts::MdkObject* linked,
                             const mdk::Vec3& receiverPosition, float receiverYaw,
                             int32_t comparison, float threshold) {
    Code code;
    code.u8(opcode).u8(comparison).f32(threshold).u8(kActionGoto).target("taken");
    code.u8(kSetVar).u8(kOwn).u8(0).f32(1.0f);
    code.end();
    code.label("taken");
    code.u8(kSetVar).u8(kOwn).u8(0).f32(2.0f);
    code.end();
    scripts::MdkObject obj;
    obj.linked = linked;
    obj.position = receiverPosition;
    obj.yaw = receiverYaw;
    runProgram(runtime, code, &obj);
    CHECK(!sawUnimplemented(opcode));
    return obj.variables[0] == 2.0f ? 2 : 1;
}

void testPartnerConditionsUseLiveLinkedObjectAndGeometry(scripts::ScriptRuntime& runtime) {
    scripts::MdkObject partner;
    const mdk::Vec3 receiver{1.0f, 2.0f, 3.0f};

    // No partner and a dead partner both read as distance zero.
    CHECK(runPartnerCondition(runtime, kIfPartnerDistance, nullptr, receiver, 0.0f,
                              kApproxEqual, 0.0f) == 2);
    partner.dead = true;
    partner.position = receiver;
    CHECK(runPartnerCondition(runtime, kIfPartnerDistance, &partner, receiver, 0.0f,
                              kApproxEqual, 0.0f) == 2);

    // A live partner at the same point has distance one, not zero.
    partner.dead = false;
    CHECK(runPartnerCondition(runtime, kIfPartnerDistance, &partner, receiver, 0.0f,
                              kApproxEqual, 1.0f) == 2);

    // The live distance is the full three-dimensional distance, measured from
    // the receiver: 50 units along z, with the receiver away from the origin.
    partner.position = mdk::Vec3{1.0f, 2.0f, 53.0f};
    CHECK(runPartnerCondition(runtime, kIfPartnerDistance, &partner, receiver, 0.0f,
                              kApproxEqual, 50.0f) == 2);

    // The receiver's position is the distance origin, not Kurt's: the receiver
    // is 100 units from Kurt, while the partner is 10 units from the receiver.
    const mdk::Vec3 kurt = runtime.kurtPosition();
    const mdk::Vec3 farReceiver{kurt.x + 100.0f, kurt.y, kurt.z};
    partner.position = mdk::Vec3{farReceiver.x, farReceiver.y, farReceiver.z + 10.0f};
    CHECK(runPartnerCondition(runtime, kIfPartnerDistance, &partner, farReceiver, 0.0f,
                              kApproxEqual, 10.0f) == 2);

    // Without a live partner the angle is zero.
    CHECK(runPartnerCondition(runtime, kIfPartnerAngle, nullptr, receiver, 200.0f,
                              kApproxEqual, 0.0f) == 2);
    partner.dead = true;
    partner.position = mdk::Vec3{11.0f, 2.0f, 3.0f};
    CHECK(runPartnerCondition(runtime, kIfPartnerAngle, &partner, receiver, 200.0f,
                              kApproxEqual, 0.0f) == 2);

    // A partner due north is -90 degrees from the receiver's eastward yaw.
    partner.dead = false;
    partner.position = mdk::Vec3{1.0f, 12.0f, 3.0f};
    CHECK(runPartnerCondition(runtime, kIfPartnerAngle, &partner, receiver, 0.0f,
                              kApproxEqual, -90.0f) == 2);

    // A partner due east has angle zero when the receiver's yaw is zero.
    partner.position = mdk::Vec3{11.0f, 2.0f, 3.0f};
    CHECK(runPartnerCondition(runtime, kIfPartnerAngle, &partner, receiver, 0.0f,
                              kApproxEqual, 0.0f) == 2);

    // A receiver yaw of 200 degrees with a partner due east is -160: the raw
    // 200 must wrap into [-180, 180), and the receiver's yaw must be read.
    CHECK(runPartnerCondition(runtime, kIfPartnerAngle, &partner, receiver, 200.0f,
                              kApproxEqual, -160.0f) == 2);
}

// Opcode 179 (spawn_partner) has no C# ScriptVmTests counterpart. It spawns
// the named type at the receiver's position, gives the new object yaw zero,
// and links the two objects in both directions.
void testSpawnPartnerLinksBothObjects(LevelData* levelData, const Cmi* cmi,
                                      audio::SoundBank* bank) {
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr);
    scripts::MdkObject& controller = runtime.getArenaState("DANT_1").controller;
    scripts::MdkObject* receiver = runtime.spawn(&controller, "BEAMS", Vec3{10.0f, 20.0f, 30.0f},
                                                 45.0f, -1, 0, false);
    CHECK(receiver != nullptr);
    if (receiver == nullptr) {
        return;
    }
    CHECK(receiver->linked == nullptr);

    Code code;
    code.u8(kSpawnPartner).pstr("BEAMS").u32(0);
    code.u8(kSetVar).u8(kOwn).u8(0).f32(9.0f);
    code.end();
    runProgram(runtime, code, receiver);

    CHECK(!sawUnimplemented(kSpawnPartner));
    CHECK(receiver->variables[0] == 9.0f);
    CHECK(receiver->linked != nullptr);
    if (receiver->linked == nullptr) {
        return;
    }
    scripts::MdkObject* partner = receiver->linked;
    CHECK(partner->linked == receiver);
    CHECK(partner->type_name == "BEAMS");
    CHECK(partner->arena == receiver->arena);
    CHECK(partner->position.x == receiver->position.x);
    CHECK(partner->position.y == receiver->position.y);
    CHECK(partner->position.z == receiver->position.z);
    CHECK(partner->yaw == 0.0f);
    CHECK(!partner->dead);
    CHECK(partner->instance_id >= 1000);
}

// Opcode 180 (snap_to_partner) copies a live partner's position. A non-zero
// offset mode adds the following three floats to that copied position; no
// partner, a dead partner, and mode zero leave the receiver where it was.
void testSnapToPartnerUsesOnlyALivePartner(scripts::ScriptRuntime& runtime) {
    scripts::MdkObject receiver;
    scripts::MdkObject partner;
    receiver.position = Vec3{100.0f, 200.0f, 300.0f};
    partner.position = Vec3{7.0f, 8.0f, 9.0f};

    Code noPartner;
    noPartner.u8(kSnapToPartner).u8(0);
    noPartner.u8(kSetVar).u8(kOwn).u8(0).f32(1.0f);
    noPartner.end();
    runProgram(runtime, noPartner, &receiver);
    CHECK(!sawUnimplemented(kSnapToPartner));
    CHECK(receiver.variables[0] == 1.0f);
    CHECK(receiver.position.x == 100.0f);
    CHECK(receiver.position.y == 200.0f);
    CHECK(receiver.position.z == 300.0f);

    receiver.linked = &partner;
    partner.dead = true;
    Code deadPartner;
    deadPartner.u8(kSnapToPartner).u8(0);
    deadPartner.end();
    runProgram(runtime, deadPartner, &receiver);
    CHECK(!sawUnimplemented(kSnapToPartner));
    CHECK(receiver.position.x == 100.0f);
    CHECK(receiver.position.y == 200.0f);
    CHECK(receiver.position.z == 300.0f);

    partner.dead = false;
    Code copy;
    copy.u8(kSnapToPartner).u8(0);
    copy.end();
    runProgram(runtime, copy, &receiver);
    CHECK(!sawUnimplemented(kSnapToPartner));
    CHECK(receiver.position.x == 7.0f);
    CHECK(receiver.position.y == 8.0f);
    CHECK(receiver.position.z == 9.0f);

    receiver.position = Vec3{100.0f, 200.0f, 300.0f};
    Code offset;
    offset.u8(kSnapToPartner).u8(1).f32(1.5f).f32(-2.0f).f32(4.0f);
    offset.end();
    runProgram(runtime, offset, &receiver);
    CHECK(!sawUnimplemented(kSnapToPartner));
    CHECK(receiver.position.x == 8.5f);
    CHECK(receiver.position.y == 6.0f);
    CHECK(receiver.position.z == 13.0f);
}

// Opcode 228 (link_partners) pairs live objects of one type in the same
// arena when their squared distance is below 10000. It follows object-list
// order rather than choosing the nearest candidate. The list form with a
// zero second value clears only the receiver's link.
void testLinkPartnersPairsEligibleObjectsInListOrder(LevelData* levelData, const Cmi* cmi,
                                                      audio::SoundBank* bank) {
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr);
    scripts::MdkObject& controller = runtime.getArenaState("DANT_1").controller;
    // The widow's pair is built by the opcode itself: with only these two
    // BEAMS in the arena, link_partners pairs them (2 units apart).
    scripts::MdkObject* linkedDead = runtime.spawn(&controller, "BEAMS", Vec3{4.0f, 0.0f, 0.0f},
                                                   0.0f, -1, 0, false);
    scripts::MdkObject* deadNear = runtime.spawn(&controller, "BEAMS", Vec3{2.0f, 0.0f, 0.0f},
                                                  0.0f, -1, 0, false);
    CHECK(linkedDead != nullptr && deadNear != nullptr);
    if (linkedDead == nullptr || deadNear == nullptr) {
        return;
    }
    Code widowPair;
    widowPair.u8(kLinkPartners).pstr("beams");
    widowPair.end();
    runProgram(runtime, widowPair, linkedDead);
    CHECK(!sawUnimplemented(kLinkPartners));
    CHECK(linkedDead->linked == deadNear);
    CHECK(deadNear->linked == linkedDead);

    // Remove one member of that real pair. The oracle leaves the survivor's
    // link pointing at the dead partner; link_partners skips every non-null
    // link (as i and as j), so the widow is never re-paired.
    runtime.remove(deadNear);
    CHECK(deadNear->dead);
    CHECK(runtime.isRemoved(deadNear));
    CHECK(linkedDead->linked == deadNear);
    CHECK(deadNear->linked == linkedDead);

    // Variable source 3 still targets the removed partner's persistent array.
    Code writePartnerVariable;
    writePartnerVariable.u8(kSetVar).u8(kLinked).u8(0).f32(4.5f);
    writePartnerVariable.end();
    runProgram(runtime, writePartnerVariable, linkedDead);
    CHECK(deadNear->variables[0] == 4.5f);

    scripts::MdkObject* receiver = runtime.spawn(&controller, "BEAMS", Vec3{0.0f, 0.0f, 0.0f},
                                                 0.0f, -1, 0, false);
    scripts::MdkObject* crossArenaNear = runtime.spawn(&controller, "BEAMS", Vec3{3.0f, 0.0f, 0.0f},
                                                        0.0f, -1, 0, false);
    scripts::MdkObject* alreadyLinked = runtime.spawn(&controller, "BEAMS", Vec3{5.0f, 0.0f, 0.0f},
                                                       0.0f, -1, 0, false);
    scripts::MdkObject* firstInRange = runtime.spawn(&controller, "BEAMS", Vec3{90.0f, 0.0f, 0.0f},
                                                     0.0f, -1, 0, false);
    scripts::MdkObject* nearerLater = runtime.spawn(&controller, "BEAMS", Vec3{10.0f, 0.0f, 0.0f},
                                                    0.0f, -1, 0, false);
    scripts::MdkObject* exactly100 = runtime.spawn(&controller, "BEAMS", Vec3{100.0f, 0.0f, 0.0f},
                                                   0.0f, -1, 0, false);
    scripts::MdkObject* farAway = runtime.spawn(&controller, "BEAMS", Vec3{500.0f, 0.0f, 0.0f},
                                                0.0f, -1, 0, false);
    CHECK(receiver != nullptr && crossArenaNear != nullptr && alreadyLinked != nullptr &&
          firstInRange != nullptr && nearerLater != nullptr && exactly100 != nullptr &&
          farAway != nullptr);
    if (receiver == nullptr || crossArenaNear == nullptr || alreadyLinked == nullptr ||
        firstInRange == nullptr || nearerLater == nullptr || exactly100 == nullptr ||
        farAway == nullptr) {
        return;
    }

    crossArenaNear->arena = "OTHER_ARENA";
    // The already-linked pair is built by an oracle writer too: spawn_partner
    // (179) links its receiver to the spawned partner whatever the type, so a
    // BEAMS can hang on a live BOLT — and link_partners must leave it alone.
    Code boltPartner;
    boltPartner.u8(kSpawnPartner).pstr("BOLT").u32(0);
    boltPartner.end();
    runProgram(runtime, boltPartner, alreadyLinked);
    CHECK(!sawUnimplemented(kSpawnPartner));
    CHECK(alreadyLinked->linked != nullptr);
    if (alreadyLinked->linked == nullptr) {
        return;
    }
    scripts::MdkObject* differentType = alreadyLinked->linked;
    CHECK(differentType->type_name == "BOLT");
    CHECK(differentType->linked == alreadyLinked);

    Code pair;
    // Lower-case type: the oracle compares type names case-insensitively.
    pair.u8(kLinkPartners).pstr("beams");
    pair.u8(kSetVar).u8(kOwn).u8(0).f32(1.0f);
    pair.end();
    runProgram(runtime, pair, receiver);
    CHECK(!sawUnimplemented(kLinkPartners));
    CHECK(receiver->variables[0] == 1.0f);
    // firstInRange comes before nearerLater in the stable object list. The
    // oracle takes the first eligible partner, not the geometrically nearest.
    CHECK(receiver->linked == firstInRange);
    CHECK(firstInRange->linked == receiver);
    // The remaining two eligible objects are then paired with each other in
    // list order; the far object stays alone.
    CHECK(nearerLater->linked == exactly100);
    CHECK(exactly100->linked == nearerLater);
    CHECK(farAway->linked == nullptr);
    CHECK(differentType->linked == alreadyLinked);
    CHECK(alreadyLinked->linked == differentType);
    // The widow is not re-paired: it keeps its dead partner. (Its admission
    // to the loose list is unobservable here — the pairing loop skips every
    // non-null link as i and as j. The retained link is what the source-3
    // write above and the 180/213/214/246 tests read.)
    CHECK(linkedDead->linked == deadNear);

    // A second pair isolates the strict distance boundary: exactly 100 units
    // is outside the oracle's range, while 99 units is inside it.
    runtime.remove(nearerLater);
    runtime.remove(exactly100);
    runtime.remove(farAway);
    scripts::MdkObject* boundaryReceiver = runtime.spawn(&controller, "BEAMS",
                                                         Vec3{0.0f, 0.0f, 0.0f}, 0.0f, -1, 0, false);
    scripts::MdkObject* at100 = runtime.spawn(&controller, "BEAMS", Vec3{100.0f, 0.0f, 0.0f},
                                              0.0f, -1, 0, false);
    scripts::MdkObject* at99 = runtime.spawn(&controller, "BEAMS", Vec3{99.0f, 0.0f, 0.0f},
                                             0.0f, -1, 0, false);
    CHECK(boundaryReceiver != nullptr && at100 != nullptr && at99 != nullptr);
    if (boundaryReceiver == nullptr || at100 == nullptr || at99 == nullptr) {
        return;
    }
    Code boundary;
    boundary.u8(kLinkPartners).pstr("BEAMS");
    boundary.end();
    runProgram(runtime, boundary, boundaryReceiver);
    CHECK(!sawUnimplemented(kLinkPartners));
    CHECK(boundaryReceiver->linked == at99);
    CHECK(at99->linked == boundaryReceiver);
    CHECK(at100->linked == nullptr);

    // The list form with a zero second value clears only the receiver's link.
    // The pair program above left the receiver linked to firstInRange, so the
    // state under test is the oracle's own, not a hand-made one.
    Code clear;
    clear.u8(kLinkPartners).u8(0).u8(0);
    clear.end();
    runProgram(runtime, clear, receiver);
    CHECK(!sawUnimplemented(kLinkPartners));
    CHECK(receiver->linked == nullptr);
    CHECK(firstInRange->linked == receiver);

    // A non-zero list value does not clear the link, and a type-mode call
    // with no eligible pair preserves an existing link. The link is built by
    // an oracle writer (spawn_partner) — the receiver's own new partner.
    Code keepPartner;
    keepPartner.u8(kSpawnPartner).pstr("BOLT").u32(0);
    keepPartner.end();
    runProgram(runtime, keepPartner, receiver);
    CHECK(!sawUnimplemented(kSpawnPartner));
    CHECK(receiver->linked != nullptr);
    if (receiver->linked == nullptr) {
        return;
    }
    scripts::MdkObject* keptPartner = receiver->linked;
    CHECK(keptPartner->linked == receiver);
    Code keep;
    keep.u8(kLinkPartners).u8(0).u8(7);
    keep.end();
    runProgram(runtime, keep, receiver);
    CHECK(!sawUnimplemented(kLinkPartners));
    CHECK(receiver->linked == keptPartner);
    Code preserve;
    preserve.u8(kLinkPartners).pstr("BEAMS");
    preserve.end();
    runProgram(runtime, preserve, receiver);
    CHECK(!sawUnimplemented(kLinkPartners));
    CHECK(receiver->linked == keptPartner);
    CHECK(keptPartner->linked == receiver);
}

// The oracle's Remove clears NO inbound reference (ScriptRuntime.cs:1648-1673:
// it sets Dead and drops the object from the list, and touches nothing else).
// The C++ keeps the deque slot, so a retained pointer stays valid and holds
// the same dead object the oracle's field would hold. Every read site twins
// the oracle's guard — null-or-dead for linked/leader, null-only for attached
// — so the retained reference reads the same way there.
void testRemoveKeepsInboundReferences(LevelData* levelData, const Cmi* cmi,
                                       audio::SoundBank* bank) {
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr);
    scripts::MdkObject& controller = runtime.getArenaState("DANT_1").controller;
    // The fresh runtime's first auto-ID is 1000, so the leader is "BEAMS_1000".
    scripts::MdkObject* leader = runtime.spawn(&controller, "BEAMS", Vec3{10.0f, 0.0f, 0.0f},
                                               0.0f, -1, 0, false);
    scripts::MdkObject* follower = runtime.spawn(&controller, "BEAMS", Vec3{0.0f, 0.0f, 0.0f},
                                                 0.0f, -1, 0, false);
    CHECK(leader != nullptr && follower != nullptr);
    if (leader == nullptr || follower == nullptr) {
        return;
    }
    CHECK(leader->instance_id == 1000);

    // attach_to (74) names the object "<type>_<id>" and makes it the leader.
    Code attach;
    attach.u8(kAttachTo).u8(2).u8(5).pstr("BEAMS_1000");
    attach.end();
    runProgram(runtime, attach, follower);
    CHECK(!sawUnimplemented(kAttachTo));
    CHECK(follower->leader == leader);
    CHECK(follower->move_command == kAttachTo);

    // A live leader is not "no leader": the move command survives.
    Code hasLeader;
    hasLeader.u8(kIfNoLeader).u8(kActionGoto).target("alone");
    hasLeader.u8(kSetVar).u8(kOwn).u8(0).f32(1.0f);
    hasLeader.end();
    hasLeader.label("alone");
    hasLeader.u8(kSetVar).u8(kOwn).u8(0).f32(2.0f);
    hasLeader.end();
    runProgram(runtime, hasLeader, follower);
    CHECK(!sawUnimplemented(kIfNoLeader));
    CHECK(follower->variables[0] == 1.0f);
    CHECK(follower->move_command == kAttachTo);

    runtime.remove(leader);
    CHECK(leader->dead);
    CHECK(runtime.isRemoved(leader));
    // The oracle keeps the reference: Remove clears no inbound field, so the
    // follower still points at the removed leader.
    CHECK(follower->leader == leader);

    // The retained reference still reads as "no leader" everywhere the oracle
    // tests it (a dead leader is a missing leader), so the move command goes.
    Code noLeader;
    noLeader.u8(kIfNoLeader).u8(kActionGoto).target("alone");
    noLeader.u8(kSetVar).u8(kOwn).u8(0).f32(1.0f);
    noLeader.end();
    noLeader.label("alone");
    noLeader.u8(kSetVar).u8(kOwn).u8(0).f32(2.0f);
    noLeader.end();
    runProgram(runtime, noLeader, follower);
    CHECK(!sawUnimplemented(kIfNoLeader));
    CHECK(follower->variables[0] == 2.0f);
    CHECK(follower->move_command == 0);
}

// Opcode 243 (if_point_sees_player) has no C# ScriptVmTests counterpart, so
// this pins the oracle directly (ScriptVm.cs:1703-1708): the REFERENCE point
// (not the position) must be within the u16 range of Kurt's feet, and the
// line from it to his chest (feet + 4) must clear every arena triangle.
// Program: 243 ref, range, goto seen / var0 = 1 / seen: var0 = 2.
void testPointSeesPlayerNeedsRangeAndAClearLine(scripts::ScriptRuntime& runtime) {
    const mdk::Vec3 kurt = runtime.kurtPosition();
    scripts::MdkObject obj;
    obj.arena = "DANT_1";
    obj.position = mdk::Vec3{kurt.x, kurt.y, kurt.z};
    obj.yaw = 0.0f;
    obj.scale = 1.0f;

    // At Kurt's feet, well within range, line feet->chest clear: seen.
    {
        Code code;
        code.u8(kIfPointSeesPlayer).u8(0).u16(1000).u8(kActionGoto).target("seen");
        code.u8(kSetVar).u8(kOwn).u8(0).f32(1.0f);
        code.end();
        code.label("seen");
        code.u8(kSetVar).u8(kOwn).u8(0).f32(2.0f);
        code.end();
        scripts::MdkObject at = obj;
        runProgram(runtime, code, &at);
        CHECK(!sawUnimplemented(kIfPointSeesPlayer));
        CHECK(at.variables[0] == 2.0f);
    }
    // 5000 east, range 100: the distance gate fails first, no raycast.
    {
        Code code;
        code.u8(kIfPointSeesPlayer).u8(0).u16(100).u8(kActionGoto).target("seen");
        code.u8(kSetVar).u8(kOwn).u8(0).f32(1.0f);
        code.end();
        code.label("seen");
        code.u8(kSetVar).u8(kOwn).u8(0).f32(2.0f);
        code.end();
        scripts::MdkObject far = obj;
        far.position = mdk::Vec3{kurt.x + 5000.0f, kurt.y, kurt.z};
        runProgram(runtime, code, &far);
        CHECK(!sawUnimplemented(kIfPointSeesPlayer));
        CHECK(far.variables[0] == 1.0f);
    }
    // The operand selects the reference point, not the position: the body is
    // at Kurt's feet but ref 0 sits 5000 east, so range 100 still misses.
    {
        Model model;
        Vec3f ref;
        ref.x = 5000.0f;
        ref.y = 0.0f;
        ref.z = 0.0f;
        model.referencePoints.push_back(ref);
        Code code;
        code.u8(kIfPointSeesPlayer).u8(0).u16(100).u8(kActionGoto).target("seen");
        code.u8(kSetVar).u8(kOwn).u8(0).f32(1.0f);
        code.end();
        code.label("seen");
        code.u8(kSetVar).u8(kOwn).u8(0).f32(2.0f);
        code.end();
        scripts::MdkObject bodied = obj;
        bodied.model = &model;
        runProgram(runtime, code, &bodied);
        CHECK(!sawUnimplemented(kIfPointSeesPlayer));
        CHECK(bodied.variables[0] == 1.0f);
    }
    // 100 below Kurt's feet, range 1000: in range, but the segment up to his
    // chest crosses the ground he stands on, so the line is blocked.
    {
        Code code;
        code.u8(kIfPointSeesPlayer).u8(0).u16(1000).u8(kActionGoto).target("seen");
        code.u8(kSetVar).u8(kOwn).u8(0).f32(1.0f);
        code.end();
        code.label("seen");
        code.u8(kSetVar).u8(kOwn).u8(0).f32(2.0f);
        code.end();
        scripts::MdkObject under = obj;
        under.position = mdk::Vec3{kurt.x, kurt.y, kurt.z - 100.0f};
        runProgram(runtime, code, &under);
        CHECK(!sawUnimplemented(kIfPointSeesPlayer));
        CHECK(under.variables[0] == 1.0f);
    }
}

// Opcodes 199 (set_104) and 210 (set_2c0) have no C# ScriptVmTests
// counterpart, so this pins the oracle directly (ScriptVm.cs:1663-1668):
// each writes its single Value operand into its own otherwise-unidentified
// object slot. Both slots exist on the C++ object already (object.h); neither
// opcode fires in levels 3-8, so no golden changes.
void testUnidentifiedValueSlotsStoreTheirOperand(scripts::ScriptRuntime& runtime) {
    // Literal operands land bit-for-bit, each in its own slot.
    {
        Code code;
        code.u8(kSet104).u8(kLiteral).f32(7.5f);
        code.u8(kSet2c0).u8(kLiteral).f32(-2.25f);
        code.end();
        scripts::MdkObject obj;
        runProgram(runtime, code, &obj);
        CHECK(!sawUnimplemented(kSet104));
        CHECK(!sawUnimplemented(kSet2c0));
        CHECK(obj.value_104 == 7.5f);
        CHECK(obj.value_2c0 == -2.25f);
    }
    // Variable operands resolve through the same Value() helper as the other
    // value-taking opcodes (ScriptVm.cs:172-180): own var 0 feeds both slots.
    {
        Code code;
        code.u8(kSetVar).u8(kOwn).u8(0).f32(3.0f);
        code.u8(kSet104).u8(kOwn).u8(0);
        code.u8(kSet2c0).u8(kOwn).u8(0);
        code.end();
        scripts::MdkObject obj;
        runProgram(runtime, code, &obj);
        CHECK(!sawUnimplemented(kSet104));
        CHECK(!sawUnimplemented(kSet2c0));
        CHECK(obj.value_104 == 3.0f);
        CHECK(obj.value_2c0 == 3.0f);
    }
}

// Opcodes 215 (screen_flash), 240 (set_hurt_flash) and 244
// (set_global_573bd4) have no C# ScriptVmTests counterpart, so this pins the
// oracle directly (ScriptVm.cs:1584-1598): 215 raises Kurt's white flash to the
// banker's-rounded Value operand; 240 writes his knock-down counter (a literal
// 1 means the default 5) unless he is invulnerable; 244 sets or accumulates his
// invulnerability time. None fires in levels 3-8, so no golden changes.
void testKurtAndScreenWrites(scripts::ScriptRuntime& runtime) {
    // 215: max of the old flash and MathF.Round(Value). From 0, 2.5 rounds to
    // the even 2; 3.5 rounds to 4; a lower 1.5 rounds to 2 and does not lower.
    {
        runtime.kurt().setWhiteFlash(0.0f);
        Code code;
        code.u8(kScreenFlash).u8(kLiteral).f32(2.5f);
        code.end();
        scripts::MdkObject obj;
        runProgram(runtime, code, &obj);
        CHECK(!sawUnimplemented(kScreenFlash));
        CHECK(runtime.kurt().whiteFlash() == 2.0f);
    }
    {
        Code code;
        code.u8(kScreenFlash).u8(kLiteral).f32(3.5f);
        code.end();
        scripts::MdkObject obj;
        runProgram(runtime, code, &obj);
        CHECK(runtime.kurt().whiteFlash() == 4.0f);
    }
    {
        Code code;
        code.u8(kScreenFlash).u8(kLiteral).f32(1.5f);
        code.end();
        scripts::MdkObject obj;
        runProgram(runtime, code, &obj);
        CHECK(runtime.kurt().whiteFlash() == 4.0f);
    }
    // The variable form resolves through the shared Value() helper.
    {
        Code code;
        code.u8(kSetVar).u8(kOwn).u8(0).f32(6.5f);
        code.u8(kScreenFlash).u8(kOwn).u8(0);
        code.end();
        scripts::MdkObject obj;
        runProgram(runtime, code, &obj);
        CHECK(runtime.kurt().whiteFlash() == 6.0f);
    }

    // 240: without invulnerability, a literal 1 means the default 5 and any
    // other byte is used as its float value; while invulnerable it is ignored.
    {
        runtime.kurt().setInvulnerable(0.0f);
        Code code;
        code.u8(kSetHurtFlash).u8(1);
        code.end();
        scripts::MdkObject obj;
        runProgram(runtime, code, &obj);
        CHECK(!sawUnimplemented(kSetHurtFlash));
        CHECK(runtime.kurt().knockDamage() == 5.0f);
    }
    {
        Code code;
        code.u8(kSetHurtFlash).u8(7);
        code.end();
        scripts::MdkObject obj;
        runProgram(runtime, code, &obj);
        CHECK(runtime.kurt().knockDamage() == 7.0f);
    }
    {
        runtime.kurt().setInvulnerable(2.0f);
        Code code;
        code.u8(kSetHurtFlash).u8(9);
        code.end();
        scripts::MdkObject obj;
        runProgram(runtime, code, &obj);
        CHECK(runtime.kurt().knockDamage() == 7.0f);
        runtime.kurt().setInvulnerable(0.0f);
    }

    // 244: a zero mode sets the time, a non-zero mode accumulates it.
    {
        Code code;
        code.u8(kSetGlobal573bd4).u8(0).f32(3.5f);
        code.end();
        scripts::MdkObject obj;
        runProgram(runtime, code, &obj);
        CHECK(!sawUnimplemented(kSetGlobal573bd4));
        CHECK(runtime.kurt().invulnerable() == 3.5f);
    }
    {
        Code code;
        code.u8(kSetGlobal573bd4).u8(1).f32(1.5f);
        code.end();
        scripts::MdkObject obj;
        runProgram(runtime, code, &obj);
        CHECK(runtime.kurt().invulnerable() == 5.0f);
    }
}

// Opcode 85 (set_rolling) captures the object's current Rotation only when
// transitioning from off to on; repeated on and off preserve the saved basis.
void testSetRollingCapturesRotationOnTransition(scripts::ScriptRuntime& runtime) {
    scripts::MdkObject obj;
    obj.yaw = 90.0f;
    Code on;
    on.u8(kSetRolling).u8(1);
    on.end();
    runProgram(runtime, on, &obj);
    CHECK(!sawUnimplemented(kSetRolling));
    CHECK((obj.flags & scripts::MdkObject::FlagRolling) != 0);
    CHECK(fabsf(obj.rolling_basis.m[0]) < 1e-6f);
    CHECK(fabsf(obj.rolling_basis.m[1] - 1.0f) < 1e-6f);
    CHECK(fabsf(obj.rolling_basis.m[4] + 1.0f) < 1e-6f);
    CHECK(fabsf(obj.rolling_basis.m[5]) < 1e-6f);

    obj.yaw = 0.0f;
    runProgram(runtime, on, &obj);
    CHECK(obj.rolling_basis.m[1] > 0.999f); // already rolling: do not recapture

    Code off;
    off.u8(kSetRolling).u8(0);
    off.end();
    runProgram(runtime, off, &obj);
    CHECK((obj.flags & scripts::MdkObject::FlagRolling) == 0);
    CHECK(obj.rolling_basis.m[1] > 0.999f); // stopping preserves the basis
    runProgram(runtime, on, &obj);
    CHECK((obj.flags & scripts::MdkObject::FlagRolling) != 0);
    CHECK(fabsf(obj.rolling_basis.m[0] - 1.0f) < 1e-6f);
    CHECK(fabsf(obj.rolling_basis.m[1]) < 1e-6f);

    // Pin the full Rx(roll) * Ry(-pitch) * Rz(yaw) composition, not just yaw.
    Code stop;
    stop.u8(kSetRolling).u8(0);
    stop.end();
    runProgram(runtime, stop, &obj);
    obj.roll = 30.0f;
    obj.pitch = 20.0f;
    obj.yaw = 40.0f;
    runProgram(runtime, on, &obj);
    // Exact Matrix4x4 bits captured from the .NET 10 C# oracle for
    // roll=30, pitch=20, yaw=40; tolerance checks miss one-ULP divergences.
    const uint32_t expected_bits[16] = {
        0x3F3847D9, 0x3F1AA13D, 0x3EAF1D44, 0x00000000,
        0xBF300B41, 0x3F0DB192, 0x3EF08FB2, 0x00000000,
        0x3DC1857C, 0xBF12CB4E, 0x3F50550B, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x3F800000,
    };
    for (size_t i = 0; i < 16; ++i) {
        uint32_t actual_bits;
        memcpy(&actual_bits, &obj.rolling_basis.m[i], sizeof(actual_bits));
        CHECK(actual_bits == expected_bits[i]);
    }
}

// Opcode 239 is explicitly unidentified and a no-op in the C# oracle
// (ScriptVm.cs:1783). Its six-float operand record must still decode and
// advance cleanly without entering the unimplemented bucket.
void testUnidentifiedSet27cIsNoOp(scripts::ScriptRuntime& runtime) {
    scripts::MdkObject obj;
    obj.variables[0] = 17.0f;
    Code code;
    code.u8(kSet27c).f32(1.0f).f32(2.0f).f32(3.0f).f32(4.0f).f32(5.0f).f32(6.0f);
    code.u8(kSetVar).u8(kOwn).u8(1).f32(9.0f);
    code.end();
    runProgram(runtime, code, &obj);
    CHECK(!sawUnimplemented(kSet27c));
    CHECK(obj.variables[0] == 17.0f);
    CHECK(obj.variables[1] == 9.0f);
}

// Opcode 77's C# case intentionally ignores its debug message (ScriptVm.cs:1713).
// Its U8 and Pascal-string operands still need to be decoded before the next op.
void testDebugMessageIsNoOp(scripts::ScriptRuntime& runtime) {
    scripts::MdkObject obj;
    Code code;
    code.u8(kDebugMsg).u8(3).pstr("debug text");
    code.u8(kSetVar).u8(kOwn).u8(2).f32(11.0f);
    code.end();
    runProgram(runtime, code, &obj);
    CHECK(!sawUnimplemented(kDebugMsg));
    CHECK(obj.variables[2] == 11.0f);
}

// attach_to (ScriptVm.cs:968) uses the oracle's canonical TYPE_ID, excludes
// dead/cross-arena/self objects, retains the last matching live object, and
// leaves all receiver state untouched on a miss.
void testAttachToSelectsNamedArenaObject(scripts::ScriptRuntime& runtime) {
    scripts::MdkObject& controller = runtime.getArenaState("DANT_1").controller;
    // Insert valid candidates first, then self and exclusions. Because the
    // runtime scans in insertion order and retains the last match, each
    // missing exclusion independently changes the selected leader.
    scripts::MdkObject* first = runtime.spawn(&controller, "BEAMS", Vec3{1.0f, 2.0f, 3.0f},
                                               0.0f, -1, 0, false);
    scripts::MdkObject* second = runtime.spawn(&controller, "BEAMS", Vec3{4.0f, 5.0f, 6.0f},
                                                0.0f, 1000, 0, false);
    scripts::MdkObject* differentId = runtime.spawn(&controller, "BEAMS", Vec3{5.0f, 6.0f, 7.0f},
                                                     0.0f, 1001, 0, false);
    // Keep near-matches before self/dead/cross-arena entries so those
    // exclusion checks remain independently sensitive to scan order.
    scripts::MdkObject* lowerNearMatch = runtime.spawn(&controller, "BEAMS", Vec3{0.0f, 0.0f, 0.0f},
                                                        0.0f, 999, 0, false);
    scripts::MdkObject* longerNearMatch = runtime.spawn(&controller, "BEAMS", Vec3{0.0f, 0.0f, 0.0f},
                                                         0.0f, 10001, 0, false);
    // Colliding ID, different type: TYPE_ID must match both components.
    scripts::MdkObject* otherType = runtime.spawn(&controller, "BOLT", Vec3{0.0f, 0.0f, 0.0f},
                                                   0.0f, 1000, 0, false);
    scripts::MdkObject* self = runtime.spawn(&controller, "BEAMS", Vec3{7.0f, 8.0f, 9.0f},
                                              0.0f, 1000, 0, false);
    scripts::MdkObject* dead = runtime.spawn(&controller, "BEAMS", Vec3{0.0f, 0.0f, 0.0f},
                                              0.0f, 1000, 0, false);
    scripts::MdkObject* otherArena = runtime.spawn(&controller, "BEAMS", Vec3{0.0f, 0.0f, 0.0f},
                                                    0.0f, 1000, 0, false);
    CHECK(first != nullptr);
    CHECK(second != nullptr);
    CHECK(differentId != nullptr);
    CHECK(self != nullptr);
    CHECK(dead != nullptr);
    CHECK(otherArena != nullptr);
    CHECK(lowerNearMatch != nullptr);
    CHECK(longerNearMatch != nullptr);
    CHECK(otherType != nullptr);
    if (first == nullptr || second == nullptr || differentId == nullptr || self == nullptr ||
        dead == nullptr || otherArena == nullptr || lowerNearMatch == nullptr ||
        longerNearMatch == nullptr || otherType == nullptr) {
        return;
    }
    dead->dead = true;
    otherArena->arena = "OTHER_ARENA";

    // Oracle-derived identity: the fresh runtime's first auto-ID is 1000;
    // second shares it explicitly to make last-match selection observable.
    CHECK(first->instance_id == 1000);
    CHECK(second->instance_id == 1000);
    CHECK(differentId->instance_id == 1001);
    CHECK(lowerNearMatch->instance_id == 999);
    CHECK(longerNearMatch->instance_id == 10001);
    CHECK(otherType->instance_id == 1000);
    CHECK(otherType->type_name == "BOLT");
    const std::string requested = "BEAMS_1000";
    Code code;
    code.u8(kAttachTo).u8(2).u8(5).pstr(requested);
    code.end();
    runProgram(runtime, code, self);
    CHECK(!sawUnimplemented(kAttachTo));
    CHECK(self->leader == second);
    CHECK(self->attach_point_a == 2);
    CHECK(self->attach_point_b == 5);
    CHECK(self->move_command == kAttachTo);

    Code folded;
    folded.u8(kAttachTo).u8(3).u8(6).pstr("beams_1000");
    folded.end();
    runProgram(runtime, folded, self);
    CHECK(self->leader == second);
    CHECK(self->attach_point_a == 3);
    CHECK(self->attach_point_b == 6);

    Code distinctId;
    distinctId.u8(kAttachTo).u8(4).u8(7).pstr("BEAMS_1001");
    distinctId.end();
    runProgram(runtime, distinctId, self);
    CHECK(self->leader == differentId);
    CHECK(self->attach_point_a == 4);
    CHECK(self->attach_point_b == 7);

    scripts::MdkObject unchanged;
    unchanged.arena = "DANT_1";
    unchanged.leader = first;
    unchanged.attach_point_a = 13;
    unchanged.attach_point_b = 17;
    unchanged.move_command = 43;
    Code miss;
    miss.u8(kAttachTo).u8(7).u8(8).pstr("BEAMS_999999");
    miss.end();
    runProgram(runtime, miss, &unchanged);
    CHECK(!sawUnimplemented(kAttachTo));
    CHECK(unchanged.leader == first);
    CHECK(unchanged.attach_point_a == 13);
    CHECK(unchanged.attach_point_b == 17);
    CHECK(unchanged.move_command == 43);
}

// The alarm state machine: movement command 15 (ObjectMotion.cs:374-385)
// and opcode 27 (ScriptVm.cs:1692-1693). A level-7 runtime is ticked once to
// establish Kurt's current arena; a local object then drives the public motion
// seam exactly as a list object would. The countdown is the runtime's own per-
// tick decrement, so ten ticks pass from "sounded" to "not sounding".
void testAlarmSoundsForTenTicksAndPlaysEveryThirtyTwo(LevelData* levelData,
                                                       const Cmi* cmi,
                                                       audio::SoundBank* bank) {
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr);
    runtime.update(scripts::ScriptRuntime::kTick);
    CHECK(runtime.currentArena().size() != 0);

    scripts::MdkObject obj;
    obj.arena = runtime.currentArena();

    // The dispatch case itself enters movement command 15.
    Code start;
    start.u8(kAlarm);
    start.end();
    runProgram(runtime, start, &obj);
    CHECK(!sawUnimplemented(kAlarm));
    CHECK(obj.move_command == 15);

    // Command 15 on a non-current arena does not sound the alarm.
    scripts::MdkObject foreign;
    foreign.arena = "NOT_CURRENT";
    foreign.move_command = 15;
    runtime.alarmTicks = 0;
    runtime.motionVelocity(&foreign);
    CHECK(runtime.alarmTicks == 0);

    // A current-arena alarm sounds at once and holds if_alarm true.
    CHECK(!runtime.mixer().isPlaying("ALERT"));
    runtime.motionVelocity(&obj);
    CHECK(runtime.alarmTicks == 10);
    CHECK(!runtime.mixer().isPlaying("ALERT"));  // TickCount 1: not an ALERT tick.

    Code alarmBranch;
    alarmBranch.u8(kIfAlarm).u8(kActionGoto).target("alarm");
    alarmBranch.u8(kSetVar).u8(kOwn).u8(0).f32(1.0f);
    alarmBranch.end();
    alarmBranch.label("alarm");
    alarmBranch.u8(kSetVar).u8(kOwn).u8(1).f32(2.0f);
    alarmBranch.end();

    scripts::MdkObject probe;
    runProgram(runtime, alarmBranch, &probe);
    CHECK(!sawUnimplemented(kIfAlarm));
    CHECK(probe.variables[0] == 0.0f);
    CHECK(probe.variables[1] == 2.0f);

    // Nine ticks: the alarm still holds. Ten: it is over.
    for (int i = 0; i < 9; i++) {
        runtime.update(scripts::ScriptRuntime::kTick);
    }
    CHECK(runtime.alarmTicks == 1);
    runProgram(runtime, alarmBranch, &probe);
    CHECK(probe.variables[0] == 0.0f);
    CHECK(probe.variables[1] == 2.0f);

    runtime.update(scripts::ScriptRuntime::kTick);
    CHECK(runtime.alarmTicks == 0);
    scripts::MdkObject quietProbe;
    runProgram(runtime, alarmBranch, &quietProbe);
    CHECK(quietProbe.variables[0] == 1.0f);
    CHECK(quietProbe.variables[1] == 0.0f);

    // ALERT plays on the tick after a multiple of 32 completed ticks
    // (TickCount 32 & 31 == 0). The runtime has completed 11 ticks above;
    // run to 31, sound nothing, complete 32, then sound.
    while (runtime.tickCount() < 31) {
        runtime.update(scripts::ScriptRuntime::kTick);
    }
    runtime.motionVelocity(&obj);
    CHECK(!runtime.mixer().isPlaying("ALERT"));

    runtime.update(scripts::ScriptRuntime::kTick);
    CHECK(runtime.tickCount() == 32);
    runtime.motionVelocity(&obj);
    CHECK(runtime.mixer().isPlaying("ALERT"));
    CHECK(runtime.alarmTicks == 10);
}

// The chain links the scripts hang off an object: opcode 29 (spawn_chain),
// opcode 49 (release_children) and movement command 30 (the chain's motion).
// The oracle's ScriptVm.cs:1006/1732 and ObjectMotion.cs:464 are the spec;
// the C# test file has no counterpart for them.

// The live links that follow the leader, in the object-list order the opcode
// spawned them in.
std::vector<scripts::MdkObject*> linksOf(scripts::ScriptRuntime& runtime,
                                          const scripts::MdkObject* leader) {
    std::vector<scripts::MdkObject*> links;
    std::deque<scripts::MdkObject>& objects = runtime.objects();
    for (std::deque<scripts::MdkObject>::iterator it = objects.begin(); it != objects.end(); ++it) {
        if (!it->dead && it->leader == leader) {
            links.push_back(&(*it));
        }
    }
    return links;
}

// Two points the same to a fraction of a unit.
bool near(const Vec3& a, const Vec3& b) {
    const float kEpsilon = 0.001f;
    return fabsf(a.x - b.x) < kEpsilon && fabsf(a.y - b.y) < kEpsilon &&
           fabsf(a.z - b.z) < kEpsilon;
}

// Opcode 29 (spawn_chain): the links that hang off an object. The links are
// numbered after the highest id of the followers the object already has, and
// the oracle's loop counts down, so the FIRST link spawned has the highest
// id and the last one has id 0 — the link that drives the chain.
void testSpawnChainNumbersItsLinks(LevelData* levelData, const Cmi* cmi,
                                   audio::SoundBank* bank) {
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr);
    scripts::MdkObject& controller = runtime.getArenaState("DANT_1").controller;
    scripts::MdkObject* leader = runtime.spawn(&controller, "X_STRIKB", Vec3{100.0f, 200.0f, 30.0f},
                                               45.0f, -1, 0, false);
    CHECK(leader != nullptr);
    if (leader == nullptr) {
        return;
    }

    Code first;
    first.u8(kSpawnChain).u8(3).pstr("X_STRIKB").u32(0);
    first.end();
    runProgram(runtime, first, leader);
    CHECK(!sawUnimplemented(kSpawnChain));

    std::vector<scripts::MdkObject*> links = linksOf(runtime, leader);
    CHECK(links.size() == 3);
    if (links.size() != 3) {
        return;
    }
    CHECK(links[0]->instance_id == 2);
    CHECK(links[1]->instance_id == 1);
    CHECK(links[2]->instance_id == 0);
    for (size_t i = 0; i < links.size(); i++) {
        CHECK(links[i]->type_name == "X_STRIKB");
        CHECK(links[i]->arena == leader->arena);
        CHECK(links[i]->leader == leader);
        CHECK(links[i]->move_command == kChainCommand);
        CHECK(links[i]->position.x == leader->position.x);
        CHECK(links[i]->position.y == leader->position.y);
        CHECK(links[i]->position.z == leader->position.z);
        CHECK(links[i]->yaw == 0.0f);
        CHECK(!links[i]->dead);
    }

    // A second chain continues after the highest live id (2): the new links
    // are 4 and 3, the first spawned the higher one.
    Code second;
    second.u8(kSpawnChain).u8(2).pstr("X_STRIKB").u32(0);
    second.end();
    runProgram(runtime, second, leader);
    CHECK(!sawUnimplemented(kSpawnChain));
    links = linksOf(runtime, leader);
    CHECK(links.size() == 5);
    if (links.size() != 5) {
        return;
    }
    CHECK(links[3]->instance_id == 4);
    CHECK(links[4]->instance_id == 3);

    // A dead follower does not count: the highest LIVE id is 3, so the next
    // link takes id 4 again — the dead link's id.
    runtime.remove(links[3]);
    CHECK(links[3]->dead);
    Code third;
    third.u8(kSpawnChain).u8(1).pstr("X_STRIKB").u32(0);
    third.end();
    runProgram(runtime, third, leader);
    CHECK(!sawUnimplemented(kSpawnChain));
    links = linksOf(runtime, leader);
    CHECK(links.size() == 5);
    if (links.size() != 5) {
        return;
    }
    CHECK(links[4]->instance_id == 4);

    // The links of another object are not this object's followers: the other
    // object numbers its own chain from zero.
    scripts::MdkObject* other = runtime.spawn(&controller, "X_STRIKB", Vec3{0.0f, 0.0f, 0.0f},
                                              0.0f, -1, 0, false);
    CHECK(other != nullptr);
    if (other == nullptr) {
        return;
    }
    Code otherChain;
    otherChain.u8(kSpawnChain).u8(2).pstr("X_STRIKB").u32(0);
    otherChain.end();
    runProgram(runtime, otherChain, other);
    std::vector<scripts::MdkObject*> otherLinks = linksOf(runtime, other);
    CHECK(otherLinks.size() == 2);
    if (otherLinks.size() != 2) {
        return;
    }
    CHECK(otherLinks[0]->instance_id == 1);
    CHECK(otherLinks[1]->instance_id == 0);
}

// Movement command 30 (the chain's motion): the links hang off their leader,
// each link's reference point 0 at the previous link's reference point 1, and
// they turn 90 degrees apart. Only the link with id 0 moves the chain.
void testChainLinksHangOffTheirLeader(LevelData* levelData, const Cmi* cmi,
                                       audio::SoundBank* bank) {
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr);
    scripts::MdkObject& controller = runtime.getArenaState("DANT_1").controller;
    scripts::MdkObject* leader = runtime.spawn(&controller, "X_STRIKB", Vec3{100.0f, 200.0f, 30.0f},
                                               45.0f, -1, 0, false);
    CHECK(leader != nullptr);
    if (leader == nullptr) {
        return;
    }
    // The model carries two reference points, so the assembly moves the links
    // off the leader instead of leaving them on it.
    CHECK(leader->model != nullptr && leader->model->referencePoints.size() >= 2);

    Code spawn;
    spawn.u8(kSpawnChain).u8(3).pstr("X_STRIKB").u32(0);
    spawn.end();
    runProgram(runtime, spawn, leader);
    std::vector<scripts::MdkObject*> links = linksOf(runtime, leader);
    CHECK(links.size() == 3);
    if (links.size() != 3) {
        return;
    }
    scripts::MdkObject* last = links[0];    // id 2
    scripts::MdkObject* middle = links[1];  // id 1
    scripts::MdkObject* first = links[2];   // id 0

    // Only the link with id 0 drives the chain: the others leave it alone,
    // even before it has been assembled.
    runtime.motionVelocity(middle);
    CHECK(first->position.x == leader->position.x);
    CHECK(first->position.y == leader->position.y);
    CHECK(first->position.z == leader->position.z);
    CHECK(first->yaw == 0.0f);
    CHECK(middle->yaw == 0.0f);
    CHECK(last->yaw == 0.0f);
    CHECK(middle->leader == leader);

    // The tick's motion step runs the chain: the link with id 0 drives it.
    runtime.motionVelocity(first);
    CHECK(first->yaw == 45.0f);
    CHECK(middle->yaw == 135.0f);
    CHECK(last->yaw == 225.0f);
    CHECK(near(first->reference_point(0), leader->reference_point(1)));
    CHECK(near(middle->reference_point(0), first->reference_point(1)));
    CHECK(near(last->reference_point(0), middle->reference_point(1)));
    // The links left the leader's place.
    CHECK(!(first->position.x == leader->position.x &&
            first->position.y == leader->position.y &&
            first->position.z == leader->position.z));

    // A gap in the chain breaks it: the link after the gap lets go.
    runtime.remove(middle);
    CHECK(middle->dead);
    runtime.motionVelocity(last);
    CHECK(last->leader == nullptr);
    CHECK(last->move_command == 0);

    // A dead leader lets the links go too.
    runtime.remove(leader);
    CHECK(leader->dead);
    runtime.motionVelocity(first);
    CHECK(first->leader == nullptr);
    CHECK(first->move_command == 0);
}

// Opcode 49 (release_children): the object lets go of the last links of its
// chain, one per count, and branches TRUE when the chain is empty.
void testReleaseChildrenLetsGoOfTheChain(LevelData* levelData, const Cmi* cmi,
                                          audio::SoundBank* bank) {
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr);
    scripts::MdkObject& controller = runtime.getArenaState("DANT_1").controller;
    scripts::MdkObject* leader = runtime.spawn(&controller, "X_STRIKB", Vec3{100.0f, 200.0f, 30.0f},
                                               45.0f, -1, 0, false);
    CHECK(leader != nullptr);
    if (leader == nullptr) {
        return;
    }
    Code spawn;
    spawn.u8(kSpawnChain).u8(3).pstr("X_STRIKB").u32(0);
    spawn.end();
    runProgram(runtime, spawn, leader);
    std::vector<scripts::MdkObject*> links = linksOf(runtime, leader);
    CHECK(links.size() == 3);
    if (links.size() != 3) {
        return;
    }

    // One link released: the LAST one (the highest id). The chain still has
    // links, so the branch does not jump.
    Code one;
    one.u8(kReleaseChildren).u8(1).u8(kActionGoto).target("released");
    one.u8(kSetVar).u8(kOwn).u8(0).f32(1.0f);
    one.end();
    one.label("released");
    one.u8(kSetVar).u8(kOwn).u8(1).f32(2.0f);
    one.end();
    runProgram(runtime, one, leader);
    CHECK(!sawUnimplemented(kReleaseChildren));
    CHECK(leader->variables[0] == 1.0f);
    CHECK(leader->variables[1] == 0.0f);
    CHECK(links[0]->leader == nullptr);
    CHECK(links[0]->move_command == 0);
    CHECK(links[1]->leader == leader);
    CHECK(links[1]->move_command == kChainCommand);
    CHECK(links[2]->leader == leader);
    CHECK(links[2]->move_command == kChainCommand);

    // More releases than links: the rest let go and the branch jumps.
    Code all;
    all.u8(kReleaseChildren).u8(5).u8(kActionGoto).target("released");
    all.u8(kSetVar).u8(kOwn).u8(2).f32(3.0f);
    all.end();
    all.label("released");
    all.u8(kSetVar).u8(kOwn).u8(3).f32(4.0f);
    all.end();
    runProgram(runtime, all, leader);
    CHECK(!sawUnimplemented(kReleaseChildren));
    CHECK(leader->variables[2] == 0.0f);
    CHECK(leader->variables[3] == 4.0f);
    CHECK(links[1]->leader == nullptr);
    CHECK(links[1]->move_command == 0);
    CHECK(links[2]->leader == nullptr);
    CHECK(links[2]->move_command == 0);
}

// Opcode 204 (avoid_objects) has no C# ScriptVmTests counterpart, so this
// pins the oracle directly (ScriptVm.cs:1715-1729): the receiver turns away
// from the first live, solid object of its arena whose world bounds overlap
// its own. The turn is at most half a turn times one tick, and the object
// list's stable order decides which overlap is used.
void testAvoidObjectsTurnsAwayFromTheFirstSolidOverlap(LevelData* levelData, const Cmi* cmi,
                                                        audio::SoundBank* bank) {
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr);
    Model model;
    model.bounds.min = Vec3f{-1.0f, -1.0f, 0.0f};
    model.bounds.max = Vec3f{1.0f, 1.0f, 2.0f};

    auto resetReceiver = [&]() -> scripts::MdkObject* {
        runtime.objects().clear();
        scripts::MdkObject receiver;
        receiver.arena = "TEST";
        receiver.model = &model;
        runtime.objects().push_back(receiver);
        return &runtime.objects().back();
    };
    auto addObstacle = [&](float x, float y, int32_t flags, const std::string& arena, bool dead) {
        scripts::MdkObject obstacle;
        obstacle.arena = arena;
        obstacle.model = &model;
        obstacle.position = mdk::Vec3{x, y, 0.0f};
        obstacle.flags = flags;
        obstacle.dead = dead;
        runtime.objects().push_back(obstacle);
    };

    Code code;
    code.u8(kAvoidObjects);
    code.end();

    // The receiver is in the object list, but GetArenaObjects excludes it.
    scripts::MdkObject* receiver = resetReceiver();
    runProgram(runtime, code, receiver);
    CHECK(!sawUnimplemented(kAvoidObjects));
    CHECK(receiver->yaw == 0.0f);

    // Dead, cross-arena, non-solid and merely touching objects are all
    // skipped. Their directions differ so an accidental acceptance changes
    // the result rather than coinciding with the valid case below.
    receiver = resetReceiver();
    addObstacle(1.0f, 0.0f, 0, "TEST", true);
    addObstacle(0.0f, -1.0f, 0, "OTHER", false);
    addObstacle(1.0f, 0.0f, scripts::MdkObject::FlagNotSolid, "TEST", false);
    addObstacle(0.0f, -1.0f, scripts::MdkObject::FlagNotSolid2, "TEST", false);
    addObstacle(0.0f, -2.0f, 0, "TEST", false);
    runProgram(runtime, code, receiver);
    CHECK(!sawUnimplemented(kAvoidObjects));
    CHECK(receiver->yaw == 0.0f);

    // A solid overlap east of the receiver turns it six degrees away.
    receiver = resetReceiver();
    addObstacle(1.0f, 0.0f, 0, "TEST", false);
    runProgram(runtime, code, receiver);
    CHECK(!sawUnimplemented(kAvoidObjects));
    CHECK(fabsf(receiver->yaw - 354.0f) < 1e-4f);

    // A solid overlap south of it turns in the other direction.
    receiver = resetReceiver();
    addObstacle(0.0f, -1.0f, 0, "TEST", false);
    runProgram(runtime, code, receiver);
    CHECK(!sawUnimplemented(kAvoidObjects));
    CHECK(fabsf(receiver->yaw - 6.0f) < 1e-4f);

    // The first eligible overlap wins; the oracle stops at that object.
    receiver = resetReceiver();
    addObstacle(1.0f, 0.0f, 0, "TEST", false);
    addObstacle(0.0f, -1.0f, 0, "TEST", false);
    runProgram(runtime, code, receiver);
    CHECK(!sawUnimplemented(kAvoidObjects));
    CHECK(fabsf(receiver->yaw - 354.0f) < 1e-4f);

    // The step is clamped to the available angle, not always six degrees.
    receiver = resetReceiver();
    receiver->yaw = 177.0f;
    addObstacle(1.0f, 0.0f, 0, "TEST", false);
    runProgram(runtime, code, receiver);
    CHECK(!sawUnimplemented(kAvoidObjects));
    CHECK(fabsf(receiver->yaw - 180.0f) < 1e-4f);
}

// Runs opcode 195 (if_hit_weapon) and returns 2 when its branch is taken,
// 1 when it is not. The opcode's first operand is signed-byte encoded.
float runIfHitWeapon(scripts::ScriptRuntime& runtime, scripts::MdkObject* obj,
                     int32_t weapon) {
    Code code;
    code.u8(kIfHitWeapon).u8(weapon).u8(kActionGoto).target("hit");
    code.u8(kSetVar).u8(kOwn).u8(0).f32(1.0f);
    code.end();
    code.label("hit");
    code.u8(kSetVar).u8(kOwn).u8(0).f32(2.0f);
    code.end();
    runProgram(runtime, code, obj);
    CHECK(!sawUnimplemented(kIfHitWeapon));
    return obj->variables[0];
}

// Opcode 195 (if_hit_weapon) has no C# ScriptVmTests counterpart, so this
// pins the oracle directly (ScriptVm.cs:1526-1527): the receiver branches
// when the weapon type of its last hit equals the signed-byte operand. The
// combat writers of that field remain deferred; this case exercises the
// condition over the existing object state.
void testIfHitWeaponBranchesOnExactHitType(scripts::ScriptRuntime& runtime) {
    scripts::MdkObject obj;

    // A grenade hit (-7) matches the same signed operand and takes the true
    // branch; a bomb hit (-9) does not.
    obj.hit_type = -7;
    CHECK(runIfHitWeapon(runtime, &obj, -7) == 2.0f);
    obj.hit_type = -9;
    CHECK(runIfHitWeapon(runtime, &obj, -7) == 1.0f);

    // Positive projectile types are exact too, rather than merely sharing a
    // sign or category.
    obj.hit_type = 3;
    CHECK(runIfHitWeapon(runtime, &obj, 3) == 2.0f);
    obj.hit_type = 4;
    CHECK(runIfHitWeapon(runtime, &obj, 3) == 1.0f);

    // The equality is signed: matching magnitudes with opposite signs do not
    // match. These reject an absolute-value comparison.
    obj.hit_type = 7;
    CHECK(runIfHitWeapon(runtime, &obj, -7) == 1.0f);
    obj.hit_type = -3;
    CHECK(runIfHitWeapon(runtime, &obj, 3) == 1.0f);

    // The oracle's initial hit type is zero, and zero matches zero.
    obj.hit_type = 0;
    CHECK(runIfHitWeapon(runtime, &obj, 0) == 2.0f);
}

// Opcode 135 (raise_573aa8) has no C# ScriptVmTests counterpart, so this pins
// the oracle directly (ScriptVm.cs:1581-1582 and ScriptRuntime.cs:1986-1989):
// each request raises the per-tick screen shake only when it is stronger than
// the current value. The runtime clears that value at the next tick.
void testRaiseShakeKeepsTheTickMaximum(LevelData* levelData, const Cmi* cmi,
                                       audio::SoundBank* bank) {
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr);
    runtime.update(scripts::ScriptRuntime::kTick);
    CHECK(runtime.shake == 0.0f);

    scripts::MdkObject obj;
    Code first;
    first.u8(kRaiseShake).f32(2.5f);
    first.u8(kSetVar).u8(kOwn).u8(0).f32(1.0f);
    first.end();
    runProgram(runtime, first, &obj);
    CHECK(!sawUnimplemented(kRaiseShake));
    CHECK(obj.variables[0] == 1.0f);  // The instruction after 135 still runs.
    CHECK(runtime.shake == 2.5f);

    // A weaker request leaves the tick's maximum alone.
    Code weaker;
    weaker.u8(kRaiseShake).f32(1.0f);
    weaker.end();
    runProgram(runtime, weaker, &obj);
    CHECK(runtime.shake == 2.5f);

    // A stronger request replaces it; a negative request does not lower it.
    Code stronger;
    stronger.u8(kRaiseShake).f32(4.0f);
    stronger.end();
    runProgram(runtime, stronger, &obj);
    CHECK(runtime.shake == 4.0f);
    Code negative;
    negative.u8(kRaiseShake).f32(-1.0f);
    negative.end();
    runProgram(runtime, negative, &obj);
    CHECK(runtime.shake == 4.0f);

    // MathF.Max propagates NaN; fmaxf would instead keep the numeric value.
    Code notNumber;
    notNumber.u8(kRaiseShake).f32(nanf(""));
    notNumber.end();
    runProgram(runtime, notNumber, &obj);
    CHECK(runtime.shake != runtime.shake);

    // MathF.Max(NaN, finite) is still NaN. This pins the current-value arm:
    // returning the finite new value would lose the NaN here.
    Code finiteAfterNaN;
    finiteAfterNaN.u8(kRaiseShake).f32(1.0f);
    finiteAfterNaN.end();
    runProgram(runtime, finiteAfterNaN, &obj);
    CHECK(runtime.shake != runtime.shake);

    // The request belongs to one simulation tick.
    runtime.update(scripts::ScriptRuntime::kTick);
    CHECK(runtime.shake == 0.0f);
}

// Opcode 121 (if_floor_below) has no C# ScriptVmTests counterpart, so this
// pins the oracle directly (ScriptVm.cs:1319-1320): a downward segment from
// the receiver's position crosses any arena triangle, and the condition is
// true when it does. The receiver's arena does not select the ray: the oracle
// uses its active ray arenas, like the existing raycast seam.
void testFloorBelowBranchesOnARayHit(scripts::ScriptRuntime& runtime) {
    const Vec3 kurt = runtime.kurtPosition();

    Code blocked;
    blocked.u8(kIfFloorBelow).f32(10.0f).u8(kActionGoto).target("below");
    blocked.u8(kSetVar).u8(kOwn).u8(0).f32(1.0f);
    blocked.end();
    blocked.label("below");
    blocked.u8(kSetVar).u8(kOwn).u8(0).f32(2.0f);
    blocked.end();

    // The level-7 floor is five below this point. A ten-unit drop crosses it,
    // so the true branch runs.
    {
        scripts::MdkObject obj;
        obj.position = Vec3{kurt.x, kurt.y, kurt.z - 4.0f};
        runProgram(runtime, blocked, &obj);
        CHECK(!sawUnimplemented(kIfFloorBelow));
        CHECK(obj.variables[0] == 2.0f);
    }

    // From the same point, a four-unit drop stops above the floor and takes
    // the false branch. This also pins the operand as the drop distance.
    {
        Code clear;
        clear.u8(kIfFloorBelow).f32(4.0f).u8(kActionGoto).target("below");
        clear.u8(kSetVar).u8(kOwn).u8(0).f32(1.0f);
        clear.end();
        clear.label("below");
        clear.u8(kSetVar).u8(kOwn).u8(0).f32(2.0f);
        clear.end();
        scripts::MdkObject obj;
        obj.position = Vec3{kurt.x, kurt.y, kurt.z - 4.0f};
        runProgram(runtime, clear, &obj);
        CHECK(!sawUnimplemented(kIfFloorBelow));
        CHECK(obj.variables[0] == 1.0f);
    }
}

// Runs opcode 192 (if_no_flat_floor_at) and returns 2 when its branch is
// taken, 1 when it is not. The C# has no ScriptVmTests counterpart for this
// opcode, so the cases below pin ScriptVm.cs:1696-1701 directly.
float runNoFlatFloorAt(scripts::ScriptRuntime& runtime, scripts::MdkObject* obj, float forward,
                       float side, float drop, float flatLimit) {
    Code code;
    code.u8(kIfNoFlatFloorAt).f32(forward).f32(side).f32(drop).f32(flatLimit);
    code.u8(kActionGoto).target("taken");
    code.u8(kSetVar).u8(kOwn).u8(0).f32(1.0f);
    code.end();
    code.label("taken");
    code.u8(kSetVar).u8(kOwn).u8(0).f32(2.0f);
    code.end();
    runProgram(runtime, code, obj);
    CHECK(!sawUnimplemented(kIfNoFlatFloorAt));
    return obj->variables[0];
}

// Opcode 192 (if_no_flat_floor_at) rotates a forward/side offset by the
// receiver's yaw, probes one unit above the receiver's level for a floor, and
// branches when there is no floor or its normal is steeper than the operand
// limit. The test pins all of those inputs against level 7's real geometry.
void testNoFlatFloorAtUsesRotatedPointAndHitNormal(scripts::ScriptRuntime& runtime) {
    const Vec3 kurt = runtime.kurtPosition();

    // The level-7 floor is at zero. A ten-unit drop from the point one above
    // the receiver crosses it; its normal is flat, so the condition is false.
    {
        scripts::MdkObject obj;
        obj.position = kurt;
        CHECK(runNoFlatFloorAt(runtime, &obj, 0.0f, 0.0f, 10.0f, 0.5f) == 1.0f);
    }

    // The same point with a zero drop stops above the floor. This pins both
    // the drop distance and the oracle's extra one-unit clearance.
    {
        scripts::MdkObject obj;
        obj.position = kurt;
        CHECK(runNoFlatFloorAt(runtime, &obj, 0.0f, 0.0f, 0.0f, 0.5f) == 2.0f);
    }

    // The probe starts one unit above the receiver: from just below the
    // floor, that extra unit makes the segment cross the floor.
    {
        scripts::MdkObject obj;
        obj.position = Vec3{kurt.x, kurt.y, -0.5f};
        CHECK(runNoFlatFloorAt(runtime, &obj, 0.0f, 0.0f, 0.0f, 0.5f) == 1.0f);
    }

    // DANT_1 has a sloping face at this point. Its normal's absolute Z is
    // about 0.447214: steep enough for a 0.5 limit, but flat enough for a
    // 0.25 limit. The segment is kept short so this face is the nearest hit.
    {
        scripts::MdkObject obj;
        obj.position = Vec3{-14.666667f, 465.333344f, 22.716666f};
        CHECK(runNoFlatFloorAt(runtime, &obj, 0.0f, 0.0f, 0.0f, 0.5f) == 2.0f);
    }
    {
        scripts::MdkObject obj;
        obj.position = Vec3{-14.666667f, 465.333344f, 22.716666f};
        CHECK(runNoFlatFloorAt(runtime, &obj, 0.0f, 0.0f, 0.0f, 0.25f) == 1.0f);
    }

    // The forward offset is rotated by yaw. At 90 degrees it must move north
    // onto the sloping face above; without the rotation (or with forward and
    // side swapped) the probe misses and takes the other branch.
    {
        scripts::MdkObject obj;
        obj.position = Vec3{-14.666667f, 460.333344f, 22.716666f};
        obj.yaw = 90.0f;
        CHECK(runNoFlatFloorAt(runtime, &obj, 5.0f, 0.0f, 0.0f, 0.25f) == 1.0f);
    }

    // Far from every arena there is no floor at all, so the condition is true.
    {
        scripts::MdkObject obj;
        obj.position = Vec3{10000.0f, 10000.0f, 9.0f};
        CHECK(runNoFlatFloorAt(runtime, &obj, 0.0f, 0.0f, 10.0f, 0.5f) == 2.0f);
    }
}

// Raycast's arena selection (ScriptRuntime.cs:2886-2909) uses Kurt's current
// arena and the second arena when a current arena is set, and every arena only
// when there is no current one. Level 7's DANT_1 and DANT_8 overlap: their
// geometry gives the current arena a hit alone at one point, the second arena
// a hit alone at another, and lets each arena win the nearest-hit contest.
void testRaycastUsesCurrentAndSecondArena(LevelData* levelData, const Cmi* cmi,
                                          audio::SoundBank* bank) {
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr);
    runtime.update(1.0f / 30.0f);
    CHECK(runtime.currentArena() == "DANT_1");

    // DANT_1 alone crosses this ray. This also proves that the current arena
    // has been established before the second one is added.
    {
        Vec3 hit{};
        Vec3 normal{};
        const Vec3 from{-60.0f, 45.0f, 200.0f};
        const Vec3 to{-60.0f, 45.0f, -200.0f};
        CHECK(runtime.raycastPoint(from, to, &hit, &normal));
        CHECK(fabsf(hit.z - 10.962601f) < 1e-4f);
        CHECK(fabsf(normal.x + 0.214408f) < 1e-4f);
        CHECK(fabsf(normal.y + 0.105432f) < 1e-4f);
        CHECK(fabsf(normal.z - 0.971037f) < 1e-4f);
    }

    // DANT_8's geometry crosses this ray, but it is not active yet. Adding it
    // as the second arena makes the previously clear ray hit.
    Vec3 hit{};
    Vec3 normal{};
    CHECK(!runtime.raycastPoint(Vec3{100.0f, 0.0f, 200.0f}, Vec3{100.0f, 0.0f, -200.0f},
                                 &hit, &normal));
    runtime.showArena("DANT_8");
    {
        hit = Vec3{};
        normal = Vec3{};
        const Vec3 from{100.0f, 0.0f, 200.0f};
        const Vec3 to{100.0f, 0.0f, -200.0f};
        CHECK(runtime.raycastPoint(from, to, &hit, &normal));
        CHECK(fabsf(hit.z - 46.946426f) < 1e-4f);
        CHECK(fabsf(normal.x) < 1e-4f);
        CHECK(fabsf(normal.y - 0.379919f) < 1e-4f);
        CHECK(fabsf(normal.z - 0.925020f) < 1e-4f);
    }

    // At Kurt's start both arenas cross the ray. DANT_8's face is nearer, so
    // the second arena's hit point and normal must replace DANT_1's lower one.
    {
        Vec3 hit{};
        Vec3 normal{};
        const Vec3 from{0.0f, 4.0f, 200.0f};
        const Vec3 to{0.0f, 4.0f, -200.0f};
        CHECK(runtime.raycastPoint(from, to, &hit, &normal));
        CHECK(fabsf(hit.z - 24.078888f) < 1e-4f);
        CHECK(fabsf(normal.x + 0.761176f) < 1e-4f);
        CHECK(fabsf(normal.y - 0.124796f) < 1e-4f);
        CHECK(fabsf(normal.z - 0.636426f) < 1e-4f);
    }

    // The contest can also go to the current arena. Here DANT_1's face at
    // 23.300003 is nearer than DANT_8's face at about 4.05.
    {
        Vec3 hit{};
        Vec3 normal{};
        const Vec3 from{0.0f, 40.0f, 200.0f};
        const Vec3 to{0.0f, 40.0f, -200.0f};
        CHECK(runtime.raycastPoint(from, to, &hit, &normal));
        CHECK(fabsf(hit.z - 23.300003f) < 1e-4f);
        CHECK(fabsf(normal.x) < 1e-4f);
        CHECK(fabsf(normal.y + 0.099504f) < 1e-4f);
        CHECK(fabsf(normal.z - 0.995037f) < 1e-4f);
    }
}

// Opcode 229 (turn_and_jump_to_dest) and movement command 229 have no C#
// ScriptVmTests counterpart, so this pins the oracle directly
// (ScriptVm.cs:1136-1138 and ObjectMotion.cs:387-391/426-461). The receiver
// turns by at most rate degrees per second until it faces the destination;
// then the ballistic velocity, arc top and duration are set. The flight
// command stops on floor contact after half of the recorded flight time.
void testTurnAndJumpTurnsLaunchesAndStopsOnFloor(LevelData* levelData, const Cmi* cmi,
                                                  audio::SoundBank* bank) {
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr);
    scripts::MdkObject obj;
    obj.position = Vec3{0.0f, 0.0f, 10.0f};
    obj.move_destination = Vec3{0.0f, 100.0f, 10.0f};
    obj.flags = scripts::MdkObject::FlagGravity;

    Code code;
    code.u8(kTurnAndJump).f32(60.0f).u8(kActionGoto).target("launched");
    code.u8(kSetVar).u8(kOwn).u8(0).f32(1.0f);
    code.end();
    code.label("launched");
    code.u8(kSetVar).u8(kOwn).u8(1).f32(2.0f);
    code.end();

    // The first call turns two degrees (60 degrees per second at 30 Hz), does
    // not launch, and takes the false branch.
    runProgram(runtime, code, &obj);
    CHECK(!sawUnimplemented(kTurnAndJump));
    CHECK(fabsf(obj.yaw - 2.0f) < 1e-4f);
    CHECK(obj.move_command == 0);
    CHECK(obj.velocity.x == 0.0f && obj.velocity.y == 0.0f && obj.velocity.z == 0.0f);
    CHECK(obj.variables[0] == 1.0f);
    CHECK(obj.variables[1] == 0.0f);

    // Forty-four more calls bring the total to forty-five two-degree turns;
    // the last reaches north exactly, launches, and takes the true branch.
    for (int i = 0; i < 44; i++) {
        runProgram(runtime, code, &obj);
    }
    CHECK(fabsf(obj.yaw - 90.0f) < 1e-4f);
    CHECK(obj.move_command == kTurnAndJump);
    CHECK(obj.variables[1] == 2.0f);
    CHECK(obj.parameter_timer == 0.0f);

    // Equal-height ends put the arc top thirty above them: up is
    // sqrt(30 * 2 * 32), and the total flight time is 2 * up / gravity. The
    // horizontal speed includes the distance lost to linear friction.
    CHECK(fabsf(obj.velocity.z - 43.817806f) < 1e-4f);
    CHECK(fabsf(obj.velocity.x) < 1e-4f);
    CHECK(fabsf(obj.velocity.y - 124.150444f) < 1e-4f);
    CHECK(fabsf(obj.parameter - 2.738613f) < 1e-4f);

    // The first motion tick advances the arc: gravity has already reduced the
    // vertical speed, and friction has reduced the horizontal speed.
    runtime.motionVelocity(&obj);
    CHECK(obj.move_command == kTurnAndJump);
    CHECK(fabsf(obj.parameter_timer - scripts::ScriptRuntime::kTick) < 1e-6f);
    CHECK(fabsf(obj.position.y - 4.067237f) < 1e-4f);
    CHECK(fabsf(obj.position.z - 11.425038f) < 1e-4f);

    // Floor contact before half of the recorded flight time does not end the
    // flight. The oracle's condition is Parameter * 0.5f in every case; it
    // is not a geometric turning point.
    obj.contact_flags |= scripts::MdkObject::ContactFloor;
    runtime.motionVelocity(&obj);
    CHECK(obj.move_command == kTurnAndJump);

    // At half the recorded flight time, floor contact ends movement command
    // 229 and clears the velocity. Gravity then acts in the same motion tick,
    // as it does in the oracle's update order.
    obj.parameter_timer = obj.parameter * 0.5f;
    obj.contact_flags |= scripts::MdkObject::ContactFloor;
    runtime.motionVelocity(&obj);
    CHECK(obj.move_command == 0);
    CHECK(obj.velocity.x == 0.0f && obj.velocity.y == 0.0f);
    CHECK(fabsf(obj.velocity.z + 32.0f * scripts::ScriptRuntime::kTick) < 1e-4f);
}

} // namespace

int main() {
    // Player::facing uses the shared trig table; initialize it before the
    // opcode-170 test constructs a Player.
    mdk::mathLutInit();

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

    scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, nullptr);
    runtime.option = 0;

    scripts::ScriptRuntime attachRuntime(&levelData, &cmi, 1, &bank, nullptr);
    testAttachToSelectsNamedArenaObject(attachRuntime);
    printf("PASS AttachToSelectsNamedArenaObject\n");

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
    testInheritKurtVelocityAddsScaledKurtVelocity(&levelData, &cmi, &bank);
    printf("PASS InheritKurtVelocityAddsScaledKurtVelocity\n");
    testSpawnAlongPathSpawnsEveryStep(runtime);
    printf("PASS SpawnAlongPathSpawnsEveryStep\n");
    testHeadShotsAddOnlyWhenEnabled(runtime);
    printf("PASS HeadShotsAddOnlyWhenEnabled\n");
    testSetSkyModeWritesSharedLevelState(&levelData, &cmi, &bank);
    printf("PASS SetSkyModeWritesSharedLevelState\n");
    testPointSeesPlayerNeedsRangeAndAClearLine(runtime);
    printf("PASS PointSeesPlayerNeedsRangeAndAClearLine\n");
    testIfPlayerHealthComparesKurtHealth(&levelData, &cmi, &bank);
    printf("PASS IfPlayerHealthComparesKurtHealth\n");
    testPartnerConditionsUseLiveLinkedObjectAndGeometry(runtime);
    printf("PASS PartnerConditionsUseLiveLinkedObjectAndGeometry\n");
    testIfHitWeaponBranchesOnExactHitType(runtime);
    printf("PASS IfHitWeaponBranchesOnExactHitType\n");
    testRaiseShakeKeepsTheTickMaximum(&levelData, &cmi, &bank);
    printf("PASS RaiseShakeKeepsTheTickMaximum\n");
    testSpawnPartnerLinksBothObjects(&levelData, &cmi, &bank);
    printf("PASS SpawnPartnerLinksBothObjects\n");
    testSnapToPartnerUsesOnlyALivePartner(runtime);
    printf("PASS SnapToPartnerUsesOnlyALivePartner\n");
    testLinkPartnersPairsEligibleObjectsInListOrder(&levelData, &cmi, &bank);
    printf("PASS LinkPartnersPairsEligibleObjectsInListOrder\n");
    testRemoveKeepsInboundReferences(&levelData, &cmi, &bank);
    printf("PASS RemoveKeepsInboundReferences\n");
    testUnidentifiedValueSlotsStoreTheirOperand(runtime);
    printf("PASS UnidentifiedValueSlotsStoreTheirOperand\n");
    testKurtAndScreenWrites(runtime);
    printf("PASS KurtAndScreenWrites\n");
    testUnidentifiedSet27cIsNoOp(runtime);
    printf("PASS UnidentifiedSet27cIsNoOp\n");
    testDebugMessageIsNoOp(runtime);
    printf("PASS DebugMessageIsNoOp\n");
    testSetRollingCapturesRotationOnTransition(runtime);
    printf("PASS SetRollingCapturesRotationOnTransition\n");
    testAlarmSoundsForTenTicksAndPlaysEveryThirtyTwo(&levelData, &cmi, &bank);
    printf("PASS AlarmSoundsForTenTicksAndPlaysEveryThirtyTwo\n");
    testSpawnChainNumbersItsLinks(&levelData, &cmi, &bank);
    printf("PASS SpawnChainNumbersItsLinks\n");
    testChainLinksHangOffTheirLeader(&levelData, &cmi, &bank);
    printf("PASS ChainLinksHangOffTheirLeader\n");
    testReleaseChildrenLetsGoOfTheChain(&levelData, &cmi, &bank);
    printf("PASS ReleaseChildrenLetsGoOfTheChain\n");
    testAvoidObjectsTurnsAwayFromTheFirstSolidOverlap(&levelData, &cmi, &bank);
    printf("PASS AvoidObjectsTurnsAwayFromTheFirstSolidOverlap\n");
    testFloorBelowBranchesOnARayHit(runtime);
    printf("PASS FloorBelowBranchesOnARayHit\n");
    testNoFlatFloorAtUsesRotatedPointAndHitNormal(runtime);
    printf("PASS NoFlatFloorAtUsesRotatedPointAndHitNormal\n");
    testRaycastUsesCurrentAndSecondArena(&levelData, &cmi, &bank);
    printf("PASS RaycastUsesCurrentAndSecondArena\n");
    testTurnAndJumpTurnsLaunchesAndStopsOnFloor(&levelData, &cmi, &bank);
    printf("PASS TurnAndJumpTurnsLaunchesAndStopsOnFloor\n");

    if (failures == 0) {
        printf("All VM unit tests match the oracle.\n");
        return 0;
    }
    printf("%d check(s) FAILED\n", failures);
    return 1;
}

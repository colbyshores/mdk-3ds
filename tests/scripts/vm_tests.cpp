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
#include <sys/stat.h>

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "audio/sound_bank.h"
#include "audio/sound_mixer.h"
#include "test_support.h"
#include "archive_guard_tests.h"
#include "audio_sink_tests.h"
#include "audio_stream_tests.h"
#include "voice_binding_tests.h"
#include "flow_tests.h"
#include "input_3ds_tests.h"
#include "heap_split_tests.h"
#include "kurt_stream_tests.h"
#include "formats/case_insensitive_path.h"
#include "formats/cmi.h"
#include "formats/dti.h"
#include "formats/glb.h"
#include "formats/file_system.h"
#include "formats/mdk_data.h"
#include "formats/model.h"
#include "formats/script_decoder.h"
#include "game/arena_space.h"
#include "game/camera_roll.h"
#include "game/follow_camera.h"
#include "game/effects_view.h"
#include "game/level_data.h"
#include "game/messages.h"
#include "game/mod_models.h"
#include "game/object_view.h"
#include "game/player.h"
#include "util/math_minmax.h"
#include "layer_tests.h"
#include "sky_dome_tests.h"
#include "mve_audio_tests.h"
#include "pica_matrix_tests.h"
#include "scripts/debris.h"
#include "scripts/script_runtime.h"
#include "sprite_tests.h"
#include "system_numerics_tests.h"
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
const int32_t kIfOption = 232;
const int32_t kJumpTo = 226;
const int32_t kSet2d0Block = 242;
const int32_t kSpawnAtPoint = 156;
const int32_t kPushHitDir = 81;
const int32_t kSpawnAlongPath = 206;
const int32_t kHeadShots = 217;
const int32_t kIfPointSeesPlayer = 243;
const int32_t kIfPlayerHealth = 241;
const int32_t kIfIsRidden = 171;
const int32_t kIfAmmo = 174;
const int32_t kBossBar = 181;
const int32_t kIfPlayerOnMeLow = 209;
const int32_t kIfPartnerDistance = 213;
const int32_t kIfPartnerAngle = 214;
const int32_t kSpawnPartner = 179;
const int32_t kSnapToPartner = 180;
const int32_t kLinkPartners = 228;
const int32_t kSet104 = 199;
const int32_t kSet2c0 = 210;
const int32_t kScreenFlash = 215;
const int32_t kPushKurt = 248;
const int32_t kSetHurtFlash = 240;
const int32_t kSetGlobal573bd4 = 244;
const int32_t kSet27c = 239;
const int32_t kArenaTextureFrame = 133;
const int32_t kLobToKurt = 189;
const int32_t kHudMessage = 247;
const int32_t kAttachEffect = 128;
const int32_t kSpawnEffect = 132;
const int32_t kSpawnDebris = 136;
const int32_t kFanCreate = 142;
const int32_t kFanRemove = 143;
const int32_t kFanEnable = 190;
const int32_t kPickWaypoint8 = 221;
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
const int32_t kCameraTrack = 203;
const int32_t kPlaceXNearPlayer = 220;
const int32_t kIfGunAim = 219;
const int32_t kTurnAndJump = 229;
const int32_t kCommandObjects = 4;    // command_objects
const int32_t kMoveToTarget = 6;      // move_to_target: movement command 6
// command_objects' operands: the command and the selector (ScriptVm.CommandObjects).
const int32_t kCommandFormation = 1;
const int32_t kSelectorAllOfType = 2;
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
const int32_t kBetween = 7;
const int32_t kOutside = 8;
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

    runtime.option = 1;
}

// Opcode 232 (if_option): `I(o[0]) == runtime.Option`, ScriptVm.cs:939-940, with
// the operand decoded as a U8 (script_opcodes.cpp:382). Review-6 F2 asked for a
// test that pins Option THROUGH this opcode, because until that finding the whole
// port ran with Option == 0 under a comment claiming that was the oracle's value,
// and the oracle's field initialises to 1 (ScriptRuntime.cs:214) from a gore
// setting that also defaults to on (Settings.cs:75, Viewer.cs:234). Nothing else
// in the suite could see the difference: no golden exercises 232 or a mode-2
// break-up, which is why the wrong default survived into both frontends.
void testIfOptionBranchesOnTheRuntimeOption(LevelData* levelData, Cmi* cmi,
                                            audio::SoundBank* bank) {
    // The default, pinned at the object itself rather than at a driver: a runtime
    // nobody configured is the oracle's default run, which is gore ON. Review-7 F3:
    // this used to take `main()`'s runtime by reference, and that driver assigns
    // `runtime.option = 1` two lines before it calls this test, so the CHECK below
    // observed the DRIVER and not the field default -- reverting script_runtime.h's
    // `int32_t option = 1;` to `= 0` left vm_tests, tick1, both builds and every
    // gate green. A default is only observable on a freshly constructed object, so
    // this test owns one.
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);
    CHECK(runtime.option == 1);

    // One program, both arms: 1 jumps when Option is 1, 0 jumps when it is 0.
    Code code;
    code.u8(kSetVar).u8(kOwn).u8(0).f32(0.0f);
    code.u8(kIfOption).u8(1).u8(kActionGoto).target("gore");
    code.u8(kSetVar).u8(kOwn).u8(0).f32(1.0f);
    code.end();
    code.label("gore");
    code.u8(kAddVar).u8(kOwn).u8(0).f32(2.0f);
    code.end();

    runtime.option = 1;
    scripts::MdkObject on;
    runProgram(runtime, code, &on);
    CHECK(!sawUnimplemented(kIfOption));
    CHECK(on.variables[0] == 2.0f);  // branched: only the target adds 2

    runtime.option = 0;
    scripts::MdkObject off;
    runProgram(runtime, code, &off);
    CHECK(!sawUnimplemented(kIfOption));
    CHECK(off.variables[0] == 1.0f);  // fell through: 0 + 1, the target never ran

    // And the comparison is equality, not "non-zero": an Option of 2 must not take
    // the `if_option 1` branch, which a truthiness test would.
    runtime.option = 2;
    scripts::MdkObject other;
    runProgram(runtime, code, &other);
    CHECK(other.variables[0] == 1.0f);
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
    runtime.option = 1;
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
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, &player, nullptr);

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

// The shipped Linux frontend calls ScriptRuntime::update once per 1/60-second
// gameplay step, after Kurt moves. ScriptRuntime.cs:378-390 keeps a separate
// 1/30-second script clock, so the first 60 Hz step completes no tick and the
// second copies the live Player into the runtime. This pins both the cadence
// and the live binding that opcodes 170/203/219/220 now have in integrated
// play.
void testFrontendTickCadenceCopiesLivePlayer(LevelData* levelData, const Cmi* cmi,
                                              audio::SoundBank* bank) {
    Player player;
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, &player, nullptr);
    player.feet = runtime.kurtPosition();
    const Vec3 start = player.feet;

    runtime.update(1.0f / 60.0f);
    CHECK(runtime.tickCount() == 0);
    CHECK(runtime.kurtPosition().x == start.x);
    CHECK(runtime.kurtPosition().y == start.y);
    CHECK(runtime.kurtPosition().z == start.z);

    Vec3 moved = start;
    moved.x += 2.0f;
    moved.y -= 3.0f;
    moved.z += 0.5f;
    player.feet = moved;
    runtime.update(1.0f / 60.0f);
    CHECK(runtime.tickCount() == 1);
    CHECK(runtime.kurtPosition().x == moved.x);
    CHECK(runtime.kurtPosition().y == moved.y);
    CHECK(runtime.kurtPosition().z == moved.z);

    // A movement between script ticks is not visible until the next complete
    // 1/30-second interval.
    Vec3 later = moved;
    later.x += 4.0f;
    player.feet = later;
    runtime.update(1.0f / 60.0f);
    CHECK(runtime.tickCount() == 1);
    CHECK(runtime.kurtPosition().x == moved.x);
    runtime.update(1.0f / 60.0f);
    CHECK(runtime.tickCount() == 2);
    CHECK(runtime.kurtPosition().x == later.x);
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
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);
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

// Opcode 220 (place_x_near_player) has no C# ScriptVmTests counterpart, so
// this pins the oracle directly (ScriptVm.cs:1121-1123 and
// ScriptRuntime.cs:1136-1186): the first RNG draw picks the branch, only the
// random branch consumes a second draw, the predicted X uses the runtime's
// observed feet velocity, an X below xMin wraps once by the range, and the
// spacing pass pushes X left 12 units from every live same-Y object.
void testPlaceNearKurtChoosesWrapsAndSpaces(LevelData* levelData, const Cmi* cmi,
                                              audio::SoundBank* bank) {
    // The C# public KurtVelocity field is written per tick from the copied
    // feet (ScriptRuntime.cs:945-946). The first tick observes no movement;
    // a moved Player is observed on the next tick.
    {
        Player player;
        scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, &player, nullptr);
        const Vec3 base = runtime.kurtPosition();
        player.feet = base;
        runtime.update(scripts::ScriptRuntime::kTick);
        CHECK(runtime.kurtVelocity.x == 0.0f);
        CHECK(runtime.kurtVelocity.y == 0.0f);
        CHECK(runtime.kurtVelocity.z == 0.0f);

        Vec3 moved = base;
        moved.x += 2.0f;
        moved.y -= 1.0f;
        moved.z += 0.5f;
        player.feet = moved;
        runtime.update(scripts::ScriptRuntime::kTick);
        CHECK(runtime.kurtVelocity.x == 60.0f);
        CHECK(runtime.kurtVelocity.y == -30.0f);
        CHECK(runtime.kurtVelocity.z == 15.0f);
    }

    // .NET's seeded Random gives these branch draws directly: seed 14 starts
    // 4, 9251 (random), seed 3 starts 29, 6975 (Kurt's X / fallback), and seed
    // 0 starts 72 (the predicted X). The range below is 100 units wide.
    const float* start = levelData->dti().startPosition();
    Vec3 kurt;
    kurt.x = start[0];
    kurt.y = start[1];
    kurt.z = start[2];
    const float xMin = kurt.x - 50.0f;
    const float xMax = kurt.x + 50.0f;
    const float yLimit = kurt.y + 1.0f;

    {
        scripts::ScriptRuntime runtime(levelData, cmi, 14, bank, nullptr, nullptr);
        runtime.objects().push_back(scripts::MdkObject());
        scripts::MdkObject* obj = &runtime.objects().back();
        obj->arena = "DANT_1";
        obj->position = kurt;
        Code code;
        code.u8(kPlaceXNearPlayer).f32(xMin).f32(xMax).f32(yLimit);
        code.u8(kSetVar).u8(kOwn).u8(0).f32(1.0f);
        code.end();
        runProgram(runtime, code, obj);
        CHECK(!sawUnimplemented(kPlaceXNearPlayer));
        CHECK(obj->variables[0] == 1.0f);  // The instruction after 220 still runs.
        CHECK(fabsf(obj->position.x - (kurt.x + 42.509995f)) < 0.001f);
        CHECK(obj->position.y == kurt.y);
        CHECK(obj->position.z == kurt.z);
    }

    {
        scripts::ScriptRuntime runtime(levelData, cmi, 3, bank, nullptr, nullptr);
        runtime.objects().push_back(scripts::MdkObject());
        scripts::MdkObject* obj = &runtime.objects().back();
        obj->arena = "DANT_1";
        obj->position = kurt;
        Code code;
        code.u8(kPlaceXNearPlayer).f32(xMin).f32(xMax).f32(yLimit);
        code.end();
        runProgram(runtime, code, obj);
        CHECK(!sawUnimplemented(kPlaceXNearPlayer));
        CHECK(obj->position.x == kurt.x);
    }

    {
        scripts::ScriptRuntime runtime(levelData, cmi, 0, bank, nullptr, nullptr);
        runtime.kurtVelocity = mdk::Vec3{40.0f, 0.0f, 0.0f};
        runtime.objects().push_back(scripts::MdkObject());
        scripts::MdkObject* obj = &runtime.objects().back();
        obj->arena = "DANT_1";
        obj->position = kurt;
        Code code;
        code.u8(kPlaceXNearPlayer).f32(xMin).f32(xMax).f32(yLimit);
        code.end();
        runProgram(runtime, code, obj);
        CHECK(!sawUnimplemented(kPlaceXNearPlayer));
        CHECK(fabsf(obj->position.x - (kurt.x + 106.933205f)) < 0.001f);
    }

    // A predicted X below xMin wraps once by the full range.
    {
        scripts::ScriptRuntime runtime(levelData, cmi, 0, bank, nullptr, nullptr);
        runtime.kurtVelocity = mdk::Vec3{-40.0f, 0.0f, 0.0f};
        runtime.objects().push_back(scripts::MdkObject());
        scripts::MdkObject* obj = &runtime.objects().back();
        obj->arena = "DANT_1";
        obj->position = kurt;
        Code code;
        code.u8(kPlaceXNearPlayer).f32(xMin).f32(xMax).f32(yLimit);
        code.end();
        runProgram(runtime, code, obj);
        CHECK(!sawUnimplemented(kPlaceXNearPlayer));
        CHECK(fabsf(obj->position.x - (kurt.x - 6.9332047f)) < 0.001f);
    }

    // Kurt's X outside the range consumes the random branch even when the
    // first draw would otherwise have taken the X branch (seed 3: 29).
    {
        scripts::ScriptRuntime runtime(levelData, cmi, 3, bank, nullptr, nullptr);
        runtime.objects().push_back(scripts::MdkObject());
        scripts::MdkObject* obj = &runtime.objects().back();
        obj->arena = "DANT_1";
        obj->position = kurt;
        Code code;
        code.u8(kPlaceXNearPlayer).f32(kurt.x + 10.0f).f32(kurt.x + 110.0f).f32(yLimit);
        code.end();
        runProgram(runtime, code, obj);
        CHECK(!sawUnimplemented(kPlaceXNearPlayer));
        CHECK(fabsf(obj->position.x - (kurt.x + 79.75f)) < 0.001f);
    }

    // A Kurt Y above the limit consumes the random branch the same way.
    {
        scripts::ScriptRuntime runtime(levelData, cmi, 3, bank, nullptr, nullptr);
        runtime.objects().push_back(scripts::MdkObject());
        scripts::MdkObject* obj = &runtime.objects().back();
        obj->arena = "DANT_1";
        obj->position = kurt;
        Code code;
        code.u8(kPlaceXNearPlayer).f32(xMin).f32(xMax).f32(kurt.y - 1.0f);
        code.end();
        runProgram(runtime, code, obj);
        CHECK(!sawUnimplemented(kPlaceXNearPlayer));
        CHECK(fabsf(obj->position.x - (kurt.x + 19.75f)) < 0.001f);
    }

    // A dead, cross-arena or different-Y object does not influence the pass,
    // and the receiver itself is skipped.
    {
        scripts::ScriptRuntime runtime(levelData, cmi, 3, bank, nullptr, nullptr);
        runtime.objects().push_back(scripts::MdkObject());
        scripts::MdkObject* obj = &runtime.objects().back();
        obj->arena = "DANT_1";
        obj->position = kurt;

        scripts::MdkObject dead;
        dead.arena = "DANT_1";
        dead.position = kurt;
        dead.position.x += 1.0f;
        dead.dead = true;
        runtime.objects().push_back(dead);
        scripts::MdkObject crossArena;
        crossArena.arena = "NOT_DANT_1";
        crossArena.position = kurt;
        crossArena.position.x += 1.0f;
        runtime.objects().push_back(crossArena);
        scripts::MdkObject differentY;
        differentY.arena = "DANT_1";
        differentY.position = kurt;
        differentY.position.y += 1.0f;
        runtime.objects().push_back(differentY);

        Code code;
        code.u8(kPlaceXNearPlayer).f32(xMin).f32(xMax).f32(yLimit);
        code.end();
        runProgram(runtime, code, obj);
        CHECK(!sawUnimplemented(kPlaceXNearPlayer));
        CHECK(obj->position.x == kurt.x);
    }

    // Live objects on the same Y push X to their LEFT by twelve. Two pushes
    // in one pass are applied in list order and the next pass is stable.
    {
        scripts::ScriptRuntime runtime(levelData, cmi, 3, bank, nullptr, nullptr);
        runtime.objects().push_back(scripts::MdkObject());
        scripts::MdkObject* obj = &runtime.objects().back();
        obj->arena = "DANT_1";
        obj->position = kurt;

        scripts::MdkObject first;
        first.arena = "DANT_1";
        first.position = kurt;
        first.position.x += 1.0f;
        runtime.objects().push_back(first);
        scripts::MdkObject second;
        second.arena = "DANT_1";
        second.position = kurt;
        second.position.x -= 2.0f;
        runtime.objects().push_back(second);

        Code code;
        code.u8(kPlaceXNearPlayer).f32(xMin).f32(xMax).f32(yLimit);
        code.end();
        runProgram(runtime, code, obj);
        CHECK(!sawUnimplemented(kPlaceXNearPlayer));
        CHECK(obj->position.x == kurt.x - 14.0f);
    }
}

// Opcode 219 (if_gun_aim) has no C# ScriptVmTests counterpart, so this pins
// the oracle directly (ScriptVm.cs:1119-1120 and ScriptRuntime.cs:1084-1134).
// The expectations come from the C# runtime itself, run over the same
// geometry on level 7 (whose start position is (0, 4, 9)).
//
// One scenario: the gun's side of Kurt, the targets on the arc south of it,
// and how the targets sit against the target row the script passes.
struct GunAimCase {
    bool north = false;       // the gun is north of Kurt (inside the cone)
    bool followLead = false;  // place the gun on Kurt's predicted lead point
    bool deadTarget = false;  // the targets are dead
    bool crossArena = false;  // the targets are in another arena
    float deltas[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};  // degrees from due south
    int32_t targetCount = 0;
    float dy = 0.0f;          // the targets' offset from the target row
    float dz = 0.0f;          // the targets' offset in Z
};

// One if_gun_aim run: whether the gun fired, the yaw it ended up with, and
// the first RNG draw after the opcode — the oracle's own sequence, so the
// number of draws the opcode took is pinned too.
struct GunAimResult {
    bool fired;
    float yaw;
    int32_t nextDraw;
};

// Places the gun and its targets in a fresh runtime and returns the gun. The
// gun is ten units north of Kurt (inside the 270-degree cone) or ten units
// east (out of it); each target sits ten units out on the arc south of the
// gun, `deltas` degrees from due south. The row the script passes is the
// arc's Y.
scripts::MdkObject* placeGun(scripts::ScriptRuntime& runtime, const GunAimCase& scenario,
                              float* rowY) {
    const mdk::Vec3& kurt = runtime.kurtPosition();
    scripts::MdkObject gun;
    gun.arena = "DANT_1";
    const float leadX = scenario.followLead ? runtime.kurtVelocity.x * 1.67333f : 0.0f;
    gun.position = scenario.north ? mdk::Vec3{kurt.x + leadX, kurt.y + 10.0f, kurt.z}
                                  : mdk::Vec3{kurt.x + 10.0f, kurt.y, kurt.z};
    runtime.objects().push_back(gun);
    scripts::MdkObject* gunPtr = &runtime.objects().back();
    *rowY = gun.position.y - 10.0f;
    for (int32_t i = 0; i < scenario.targetCount; i++) {
        const double angle = (270.0 + (double)scenario.deltas[i]) * 3.14159265358979323846 / 180.0;
        scripts::MdkObject target;
        target.arena = scenario.crossArena ? "NOT_DANT_1" : "DANT_1";
        target.position = mdk::Vec3{gun.position.x + 10.0f * (float)cos(angle),
                                    gun.position.y + 10.0f * (float)sin(angle) + scenario.dy,
                                    gun.position.z + scenario.dz};
        target.dead = scenario.deadTarget;
        runtime.objects().push_back(target);
    }
    return gunPtr;
}

// Runs the if_gun_aim program: it branches to "fired" when the gun fires,
// and the fall-through marks the other outcome.
GunAimResult runGunAim(scripts::ScriptRuntime& runtime, scripts::MdkObject* gun, float rowY,
                        float z) {
    Code code;
    code.u8(kIfGunAim).f32(rowY).f32(z).u8(kActionGoto).target("fired");
    code.u8(kSetVar).u8(kOwn).u8(0).f32(1.0f);
    code.end();
    code.label("fired");
    code.u8(kSetVar).u8(kOwn).u8(1).f32(2.0f);
    code.end();
    runProgram(runtime, code, gun);
    GunAimResult result;
    result.fired = gun->variables[1] == 2.0f;
    result.yaw = gun->yaw;
    result.nextDraw = runtime.rng().next(1000);
    return result;
}

void testGunAimAimsAndFires(LevelData* levelData, const Cmi* cmi, audio::SoundBank* bank) {
    GunAimCase aimed;
    aimed.north = true;

    // Aimed at Kurt (the gun is due north of him, so its bearing to him is
    // the 270 the cone wants) with nothing else in the cone: the gun fires
    // at Kurt with the jittered aim. Seed 0's fire roll (57 of 70) fails,
    // and the gun fires at Kurt anyway; the two draws are the jitter and
    // the roll.
    {
        scripts::ScriptRuntime runtime(levelData, cmi, 0, bank, nullptr, nullptr);
        float rowY = 0.0f;
        scripts::MdkObject* gun = placeGun(runtime, aimed, &rowY);
        GunAimResult result = runGunAim(runtime, gun, rowY, gun->position.z);
        CHECK(!sawUnimplemented(kIfGunAim));
        CHECK(result.fired);
        CHECK(fabsf(result.yaw - 270.113f) < 0.001f);
        CHECK(result.nextDraw == 768);
    }

    // Seed 1's fire roll (7 of 70) succeeds, so the gun fires at Kurt with
    // the jittered aim as well.
    {
        scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);
        float rowY = 0.0f;
        scripts::MdkObject* gun = placeGun(runtime, aimed, &rowY);
        GunAimResult result = runGunAim(runtime, gun, rowY, gun->position.z);
        CHECK(result.fired);
        CHECK(fabsf(result.yaw - 269.874f) < 0.001f);
        CHECK(result.nextDraw == 467);
    }

    // With a raised target in the cone and the fire roll failing (seed 0),
    // the gun aims at the target instead: the target's bearing, and a third
    // draw to pick it.
    {
        GunAimCase scenario = aimed;
        scenario.deltas[0] = 0.0f;
        scenario.targetCount = 1;
        scripts::ScriptRuntime runtime(levelData, cmi, 0, bank, nullptr, nullptr);
        float rowY = 0.0f;
        scripts::MdkObject* gun = placeGun(runtime, scenario, &rowY);
        GunAimResult result = runGunAim(runtime, gun, rowY, gun->position.z);
        CHECK(result.fired);
        CHECK(fabsf(result.yaw - 270.0f) < 0.001f);
        CHECK(result.nextDraw == 558);
    }

    // The same target with the fire roll succeeding (seed 1): the gun fires
    // at Kurt and ignores the target.
    {
        GunAimCase scenario = aimed;
        scenario.targetCount = 1;
        scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);
        float rowY = 0.0f;
        scripts::MdkObject* gun = placeGun(runtime, scenario, &rowY);
        GunAimResult result = runGunAim(runtime, gun, rowY, gun->position.z);
        CHECK(result.fired);
        CHECK(fabsf(result.yaw - 269.874f) < 0.001f);
        CHECK(result.nextDraw == 467);
    }

    // And with seed 2's failing roll (28 of 70), the target again.
    {
        GunAimCase scenario = aimed;
        scenario.targetCount = 1;
        scripts::ScriptRuntime runtime(levelData, cmi, 2, bank, nullptr, nullptr);
        float rowY = 0.0f;
        scripts::MdkObject* gun = placeGun(runtime, scenario, &rowY);
        GunAimResult result = runGunAim(runtime, gun, rowY, gun->position.z);
        CHECK(result.fired);
        CHECK(fabsf(result.yaw - 270.0f) < 0.001f);
        CHECK(result.nextDraw == 985);
    }

    // Out of the cone (the gun is east of Kurt) with nothing in it: the gun
    // faces 270 degrees and fires 29% of the time. Seed 0's roll (81 of 100)
    // fires; seed 1's (11 of 100) does not.
    {
        scripts::ScriptRuntime runtime(levelData, cmi, 0, bank, nullptr, nullptr);
        float rowY = 0.0f;
        scripts::MdkObject* gun = placeGun(runtime, GunAimCase(), &rowY);
        GunAimResult result = runGunAim(runtime, gun, rowY, gun->position.z);
        CHECK(result.fired);
        CHECK(fabsf(result.yaw - 270.0f) < 0.001f);
        CHECK(result.nextDraw == 768);
    }

    {
        scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);
        float rowY = 0.0f;
        scripts::MdkObject* gun = placeGun(runtime, GunAimCase(), &rowY);
        GunAimResult result = runGunAim(runtime, gun, rowY, gun->position.z);
        CHECK(!result.fired);
        CHECK(fabsf(result.yaw - 270.0f) < 0.001f);
        CHECK(result.nextDraw == 467);
    }

    // A raised target in the cone is fired at whatever the 29% roll says.
    {
        GunAimCase scenario;
        scenario.targetCount = 1;
        scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);
        float rowY = 0.0f;
        scripts::MdkObject* gun = placeGun(runtime, scenario, &rowY);
        GunAimResult result = runGunAim(runtime, gun, rowY, gun->position.z);
        CHECK(result.fired);
        CHECK(fabsf(result.yaw - 270.0f) < 0.001f);
        CHECK(result.nextDraw == 467);
    }

    // Two targets in the cone: the gun picks one of their bearings at
    // random. Seed 0 picks the second (271 degrees), seed 1 the first.
    {
        GunAimCase scenario;
        scenario.deltas[0] = 0.0f;
        scenario.deltas[1] = 1.0f;
        scenario.targetCount = 2;
        scripts::ScriptRuntime runtime(levelData, cmi, 0, bank, nullptr, nullptr);
        float rowY = 0.0f;
        scripts::MdkObject* gun = placeGun(runtime, scenario, &rowY);
        GunAimResult result = runGunAim(runtime, gun, rowY, gun->position.z);
        CHECK(result.fired);
        CHECK(fabsf(result.yaw - 271.0f) < 0.001f);
        CHECK(result.nextDraw == 768);
    }

    {
        GunAimCase scenario;
        scenario.deltas[0] = 0.0f;
        scenario.deltas[1] = 1.0f;
        scenario.targetCount = 2;
        scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);
        float rowY = 0.0f;
        scripts::MdkObject* gun = placeGun(runtime, scenario, &rowY);
        GunAimResult result = runGunAim(runtime, gun, rowY, gun->position.z);
        CHECK(result.fired);
        CHECK(fabsf(result.yaw - 270.0f) < 0.001f);
        CHECK(result.nextDraw == 467);
    }

    // Five targets in the cone: the oracle collects at most four, so seed 0
    // picks the fourth (272 degrees) and never the fifth (268).
    {
        GunAimCase scenario;
        scenario.deltas[0] = 0.0f;
        scenario.deltas[1] = 1.0f;
        scenario.deltas[2] = -1.0f;
        scenario.deltas[3] = 2.0f;
        scenario.deltas[4] = -2.0f;
        scenario.targetCount = 5;
        scripts::ScriptRuntime runtime(levelData, cmi, 0, bank, nullptr, nullptr);
        float rowY = 0.0f;
        scripts::MdkObject* gun = placeGun(runtime, scenario, &rowY);
        GunAimResult result = runGunAim(runtime, gun, rowY, gun->position.z);
        CHECK(result.fired);
        CHECK(fabsf(result.yaw - 272.0f) < 0.001f);
        CHECK(result.nextDraw == 768);
    }

    {
        GunAimCase scenario;
        scenario.deltas[0] = 0.0f;
        scenario.deltas[1] = 1.0f;
        scenario.deltas[2] = -1.0f;
        scenario.deltas[3] = 2.0f;
        scenario.deltas[4] = -2.0f;
        scenario.targetCount = 5;
        scripts::ScriptRuntime runtime(levelData, cmi, 2, bank, nullptr, nullptr);
        float rowY = 0.0f;
        scripts::MdkObject* gun = placeGun(runtime, scenario, &rowY);
        GunAimResult result = runGunAim(runtime, gun, rowY, gun->position.z);
        CHECK(result.fired);
        CHECK(fabsf(result.yaw - 271.0f) < 0.001f);
        CHECK(result.nextDraw == 165);
    }

    // A dead target and a target in another arena are not targets: the gun
    // faces 270 degrees and falls back on the 29% roll (seed 1: no).
    {
        GunAimCase scenario;
        scenario.targetCount = 1;
        scenario.deadTarget = true;
        scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);
        float rowY = 0.0f;
        scripts::MdkObject* gun = placeGun(runtime, scenario, &rowY);
        GunAimResult result = runGunAim(runtime, gun, rowY, gun->position.z);
        CHECK(!result.fired);
        CHECK(fabsf(result.yaw - 270.0f) < 0.001f);
        CHECK(result.nextDraw == 467);
    }

    {
        GunAimCase scenario;
        scenario.targetCount = 1;
        scenario.crossArena = true;
        scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);
        float rowY = 0.0f;
        scripts::MdkObject* gun = placeGun(runtime, scenario, &rowY);
        GunAimResult result = runGunAim(runtime, gun, rowY, gun->position.z);
        CHECK(!result.fired);
        CHECK(fabsf(result.yaw - 270.0f) < 0.001f);
        CHECK(result.nextDraw == 467);
    }

    // The target row: two units off it is still a target, two and a half is
    // not (the window is two in Y).
    {
        GunAimCase scenario;
        scenario.targetCount = 1;
        scenario.dy = 2.0f;
        scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);
        float rowY = 0.0f;
        scripts::MdkObject* gun = placeGun(runtime, scenario, &rowY);
        GunAimResult result = runGunAim(runtime, gun, rowY, gun->position.z);
        CHECK(result.fired);
        CHECK(fabsf(result.yaw - 270.0f) < 0.001f);
        CHECK(result.nextDraw == 467);
    }

    {
        GunAimCase scenario;
        scenario.targetCount = 1;
        scenario.dy = 2.5f;
        scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);
        float rowY = 0.0f;
        scripts::MdkObject* gun = placeGun(runtime, scenario, &rowY);
        GunAimResult result = runGunAim(runtime, gun, rowY, gun->position.z);
        CHECK(!result.fired);
        CHECK(fabsf(result.yaw - 270.0f) < 0.001f);
        CHECK(result.nextDraw == 467);
    }

    // And three units off in Z is a target, three and a half is not.
    {
        GunAimCase scenario;
        scenario.targetCount = 1;
        scenario.dz = 3.0f;
        scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);
        float rowY = 0.0f;
        scripts::MdkObject* gun = placeGun(runtime, scenario, &rowY);
        GunAimResult result = runGunAim(runtime, gun, rowY, gun->position.z);
        CHECK(result.fired);
        CHECK(fabsf(result.yaw - 270.0f) < 0.001f);
        CHECK(result.nextDraw == 467);
    }

    {
        GunAimCase scenario;
        scenario.targetCount = 1;
        scenario.dz = 3.5f;
        scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);
        float rowY = 0.0f;
        scripts::MdkObject* gun = placeGun(runtime, scenario, &rowY);
        GunAimResult result = runGunAim(runtime, gun, rowY, gun->position.z);
        CHECK(!result.fired);
        CHECK(fabsf(result.yaw - 270.0f) < 0.001f);
        CHECK(result.nextDraw == 467);
    }

    // Kurt moving sideways at 30 units per second leads the shot 50 units
    // ahead of him, out of the cone: the gun faces 270 degrees and falls
    // back on the 29% roll (seed 0 fires, seed 1 does not).
    {
        scripts::ScriptRuntime runtime(levelData, cmi, 0, bank, nullptr, nullptr);
        runtime.kurtVelocity = mdk::Vec3{30.0f, 0.0f, 0.0f};
        float rowY = 0.0f;
        scripts::MdkObject* gun = placeGun(runtime, aimed, &rowY);
        GunAimResult result = runGunAim(runtime, gun, rowY, gun->position.z);
        CHECK(result.fired);
        CHECK(fabsf(result.yaw - 270.0f) < 0.001f);
        CHECK(result.nextDraw == 768);
    }

    {
        scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);
        runtime.kurtVelocity = mdk::Vec3{30.0f, 0.0f, 0.0f};
        float rowY = 0.0f;
        scripts::MdkObject* gun = placeGun(runtime, aimed, &rowY);
        GunAimResult result = runGunAim(runtime, gun, rowY, gun->position.z);
        CHECK(!result.fired);
        CHECK(fabsf(result.yaw - 270.0f) < 0.001f);
        CHECK(result.nextDraw == 467);
    }

    // At 48.5 units per second, ScriptMath.Round rounds the magnitude away
    // from zero to 49, so the 58-of-70 roll passes the threshold of 59 and
    // Kurt wins over the raised target. The gun sits on the lead point to
    // keep Kurt inside the cone. Seed 58's first roll is exactly 58: a
    // banker's round would make the threshold 58 and select the target.
    {
        GunAimCase scenario = aimed;
        scenario.followLead = true;
        scenario.deltas[0] = 1.0f;
        scenario.targetCount = 1;
        scripts::ScriptRuntime runtime(levelData, cmi, 58, bank, nullptr, nullptr);
        runtime.kurtVelocity = mdk::Vec3{48.5f, 0.0f, 0.0f};
        float rowY = 0.0f;
        scripts::MdkObject* gun = placeGun(runtime, scenario, &rowY);
        GunAimResult result = runGunAim(runtime, gun, rowY, gun->position.z);
        CHECK(result.fired);
        CHECK(fabsf(result.yaw - 269.763f) < 0.001f);
        CHECK(result.nextDraw == 309);
    }
}

// Opcode 203 (camera_track) has no C# ScriptVmTests counterpart, so this pins
// the oracle directly (ScriptVm.cs:1124-1128 and ScriptRuntime.cs:1188-1216):
// mode 1 looks at height * scale above the object, mode 0 at 70% of the way
// to the model's top, the request falls as Kurt turns away and is clamped to
// 30 degrees up, and it remains active for two ticks. The renderer-side
// follow-camera consumer remains deferred.
void testCameraTrackCalculatesPitchAndExpires(LevelData* levelData, const Cmi* cmi,
                                               audio::SoundBank* bank) {
    Player player;
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, &player, nullptr);
    player.feet = runtime.kurtPosition();
    player.yaw = 0.0f;
    runtime.update(scripts::ScriptRuntime::kTick);

    const mdk::Vec3 base = runtime.kurtPosition();
    const float rest = levelData->arenaPitch(runtime.currentArena());
    CHECK(!runtime.currentArena().empty());
    CHECK(rest == 0.0f);  // Level 7's DANT_1 arena pitch.

    // Mode 1: the object is 40 units ahead and its requested top is 20 above
    // Kurt (origin +16, height 2, scale 2). With Kurt facing it, the target
    // is -atan(20/40), or -26.565 degrees.
    scripts::MdkObject obj;
    obj.position = mdk::Vec3{base.x + 40.0f, base.y, base.z + 16.0f};
    obj.scale = 2.0f;
    Code mode1;
    mode1.u8(kCameraTrack).u8(1).f32(2.0f);
    mode1.u8(kSetVar).u8(kOwn).u8(0).f32(1.0f);
    mode1.end();
    runProgram(runtime, mode1, &obj);
    CHECK(!sawUnimplemented(kCameraTrack));
    CHECK(obj.variables[0] == 1.0f);  // The instruction after 203 still runs.
    CHECK(runtime.cameraTrackTicks == 2);
    CHECK(fabsf(runtime.cameraTrackPitch + 26.565051f) < 0.001f);

    // Mode 0: 70% of the way to the model's top. A 20-unit model top 80
    // units away puts the requested top 14 above Kurt, for -9.926 degrees.
    Model model;
    model.bounds.min = Vec3f{0.0f, 0.0f, 0.0f};
    model.bounds.max = Vec3f{0.0f, 0.0f, 20.0f};
    obj.model = &model;
    obj.position = mdk::Vec3{base.x + 80.0f, base.y, base.z};
    obj.scale = 1.0f;
    Code mode0;
    mode0.u8(kCameraTrack).u8(0);
    mode0.u8(kSetVar).u8(kOwn).u8(0).f32(2.0f);
    mode0.end();
    runProgram(runtime, mode0, &obj);
    CHECK(!sawUnimplemented(kCameraTrack));
    CHECK(obj.variables[0] == 2.0f);
    CHECK(runtime.cameraTrackTicks == 2);
    CHECK(fabsf(runtime.cameraTrackPitch + 9.926246f) < 0.001f);

    // The upward limit is 30 degrees, even when the requested top is much
    // higher: mode 1 asks for a top 80 above Kurt, 40 units away.
    obj.model = nullptr;
    obj.position = mdk::Vec3{base.x + 40.0f, base.y, base.z + 76.0f};
    obj.scale = 2.0f;
    Code clamp;
    clamp.u8(kCameraTrack).u8(1).f32(2.0f);
    clamp.end();
    runProgram(runtime, clamp, &obj);
    CHECK(!sawUnimplemented(kCameraTrack));
    CHECK(runtime.cameraTrackPitch == -30.0f);

    // The lifetime is decremented once at the start of each following tick.
    runtime.update(scripts::ScriptRuntime::kTick);
    CHECK(runtime.cameraTrackTicks == 1);
    runtime.update(scripts::ScriptRuntime::kTick);
    CHECK(runtime.cameraTrackTicks == 0);
    CHECK(runtime.cameraTrackPitch == -30.0f);  // The last pitch persists.

    // The live Player yaw is copied at tick start. At 45 degrees the request
    // is scaled by (120 - 45) / 120, so the same 26.565-degree angle becomes
    // 16.603 degrees up.
    player.yaw = 45.0f;
    runtime.update(scripts::ScriptRuntime::kTick);
    obj.position = mdk::Vec3{base.x + 40.0f, base.y, base.z + 16.0f};
    obj.scale = 2.0f;
    Code turned;
    turned.u8(kCameraTrack).u8(1).f32(2.0f);
    turned.end();
    runProgram(runtime, turned, &obj);
    CHECK(!sawUnimplemented(kCameraTrack));
    CHECK(fabsf(runtime.cameraTrackPitch + 16.603157f) < 0.001f);

    // At 180 degrees the object is behind Kurt, so the request falls back to
    // the arena's rest pitch.
    player.yaw = 180.0f;
    runtime.update(scripts::ScriptRuntime::kTick);
    obj.model = nullptr;
    obj.position = mdk::Vec3{base.x + 40.0f, base.y, base.z};
    obj.scale = 1.0f;
    Code behind;
    behind.u8(kCameraTrack).u8(1).f32(2.0f);
    behind.end();
    runProgram(runtime, behind, &obj);
    CHECK(!sawUnimplemented(kCameraTrack));
    CHECK(runtime.cameraTrackTicks == 2);
    CHECK(runtime.cameraTrackPitch == rest);
}

// Opcode 241 (if_player_health) has no C# ScriptVmTests counterpart. Pin its
// comparison against Kurt after damage (99), including true and false branch
// outcomes; opcode 241 does not fire in the level corpus.
void testIfPlayerHealthComparesKurtHealth(LevelData* levelData, const Cmi* cmi,
                                           audio::SoundBank* bank) {
    // A fresh runtime keeps this fixture independent of the shared test state:
    // damaging Kurt here must not leak into any later test.
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);
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

// Opcode 171 (if_is_573c30) branches on whether obj is the object Kurt rides
// (Rides.Ridden == obj, ScriptVm.cs:1711). The ride slice (mount/dismount) is
// unported, so ridesRidden() is null and the branch is false for every object --
// the value the C# reads while Kurt is on foot, which is every tick of the corpus.
// Pin the dispatch and the false branch; the true branch needs the ride slice.
void testIfIsRiddenBranchesOnRidesRidden(LevelData* levelData, const Cmi* cmi,
                                          audio::SoundBank* bank) {
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);
    Code code;
    code.u8(kIfIsRidden).u8(kActionGoto).target("ridden");
    code.u8(kSetVar).u8(kOwn).u8(0).f32(1.0f);
    code.end();
    code.label("ridden");
    code.u8(kSetVar).u8(kOwn).u8(0).f32(2.0f);
    code.end();
    scripts::MdkObject obj;
    runProgram(runtime, code, &obj);
    CHECK(!sawUnimplemented(kIfIsRidden));
    // Nothing is ridden, so the branch is not taken and the fall-through runs.
    CHECK(obj.variables[0] == 1.0f);
}

// BlastDamage (Items.cs:788), the falloff at the core of Items.Blast, ported ahead
// of the dispatcher. Pin it with hand-computed values: full damage at the centre,
// linear falloff to the radius, and 0 beyond it. No arena is current, so the wall
// test raycasts the level's whole arena set (raycastPoint's fallback); the short
// test segments are clear in the shipped geometry, so only the falloff is exercised.
void testBlastDamageFalloff(LevelData* levelData, const Cmi* cmi, audio::SoundBank* bank) {
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);
    // Box centre (1,1,1), size sqrt(12); its half-diagonal (sqrt(3)) covers the
    // 1-unit blast lift, so a blast at the centre reads distance 0 -> full damage.
    const Box box{{0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 2.0f}};
    std::pair<int32_t, float> at = runtime.blastDamage(box, Vec3{1.0f, 1.0f, 1.0f}, 100, 10.0f);
    CHECK(at.first == 100);
    CHECK(at.second == 0.0f);
    // Beyond the radius: zero damage, zero distance.
    std::pair<int32_t, float> far =
        runtime.blastDamage(box, Vec3{100.0f, 100.0f, 100.0f}, 100, 10.0f);
    CHECK(far.first == 0);
    CHECK(far.second == 0.0f);
    // Linear falloff: a point box at the origin (size 0, so squared == distance^2),
    // blast 3 units away in a radius of 6 -> half damage at distance 3.
    const Box point{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
    std::pair<int32_t, float> half =
        runtime.blastDamage(point, Vec3{3.0f, 0.0f, -1.0f}, 100, 6.0f);
    CHECK(half.first == 50);
    CHECK(half.second == 3.0f);
    // Midpoint: 99 * (1 - 3/6) = 49.5. ScriptMath.Round is AwayFromZero -> 50;
    // a truncating cast would give 49, so this pins the rounding, not just the falloff.
    std::pair<int32_t, float> mid =
        runtime.blastDamage(point, Vec3{3.0f, 0.0f, -1.0f}, 99, 6.0f);
    CHECK(mid.first == 50);
    CHECK(mid.second == 3.0f);
}

// The BlastWeakParts primitives (MdkObject.PartBounds, ScriptRuntime.IsWeakPart,
// GetWorldBounds(obj, part)), ported ahead of the dispatcher. No C# ScriptVmTests
// counterpart, so these pin the oracle directly (MdkObject.cs:521-541,
// ScriptRuntime.cs:2059-2070 and 3118-3135).
void testPartBoundsAndWeakParts(LevelData* levelData, const Cmi* cmi, audio::SoundBank* bank) {
    Model model;
    const char* names[] = {"ARM0", "ARM1", "EMPTY", "AB", "XYZ9", "ARM"};
    // Per-part vertices: ARM0 (0..2), ARM1 (1..3), EMPTY none, AB/XYZ9/ARM (0..1).
    const float verts[][6] = {
        {0, 0, 0, 2, 2, 2}, {1, 1, 1, 3, 3, 3}, {}, {0, 0, 0, 1, 1, 1}, {0, 0, 0, 1, 1, 1},
        {0, 0, 0, 1, 1, 1}};
    for (size_t i = 0; i < 6; i++) {
        Model::Part part;
        snprintf(part.name, sizeof(part.name), "%s", names[i]);
        const int n = i == 2 ? 0 : 2;  // EMPTY has no vertices
        for (int v = 0; v < n; v++) {
            part.vertices.push_back(Vec3f{verts[i][v * 3], verts[i][v * 3 + 1], verts[i][v * 3 + 2]});
        }
        model.partList.push_back(part);
    }

    scripts::MdkObject obj;
    obj.model = &model;
    obj.weak_prefix = "ARM";
    obj.weak_prefix_length = 3;
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);

    // PartBounds: per-part boxes in the rest pose; the empty part is invalid.
    std::vector<scripts::ScriptRuntime::PartBox> pb = runtime.partBounds(&obj);
    CHECK(pb.size() == 6);
    CHECK(pb[0].valid && pb[0].box.min.x == 0.0f && pb[0].box.max.z == 2.0f);
    CHECK(pb[1].valid && pb[1].box.min.x == 1.0f && pb[1].box.max.z == 3.0f);
    CHECK(!pb[2].valid);
    CHECK(pb[3].valid && pb[3].box.max.x == 1.0f);
    CHECK(pb[4].valid && pb[4].box.max.x == 1.0f);
    CHECK(pb[5].valid && pb[5].box.max.x == 1.0f);

    // IsWeakPart: digit at index 3 and the "ARM" prefix. ARM0/ARM1 weak; EMPTY has
    // no digit at 3; AB is shorter than the prefix length; XYZ9 has the digit but
    // not the prefix; ARM is exactly the prefix length (name.Length <= length).
    CHECK(scripts::ScriptRuntime::isWeakPart(&obj, 0));
    CHECK(scripts::ScriptRuntime::isWeakPart(&obj, 1));
    CHECK(!scripts::ScriptRuntime::isWeakPart(&obj, 2));
    CHECK(!scripts::ScriptRuntime::isWeakPart(&obj, 3));
    CHECK(!scripts::ScriptRuntime::isWeakPart(&obj, 4));
    CHECK(!scripts::ScriptRuntime::isWeakPart(&obj, 5));

    // GetWorldBounds(obj, part): the part box scaled by 2 and placed at the position
    // (yaw 0, no lift). ARM0 (0..2) -> (10..14, 20..24, 30..34).
    obj.position = Vec3{10.0, 20.0, 30.0};
    obj.yaw = 0.0f;
    obj.scale = 2.0f;
    Box world = runtime.worldBounds(&obj, pb[0].box);
    CHECK(world.min.x == 10.0f && world.min.y == 20.0f && world.min.z == 30.0f);
    CHECK(world.max.x == 14.0f && world.max.y == 24.0f && world.max.z == 34.0f);
    // The whole-object worldBounds still aggregates the visible parts (0..3 -> *2 + pos).
    Box whole = runtime.worldBounds(&obj);
    CHECK(whole.min.x == 10.0f && whole.max.x == 16.0f && whole.max.z == 36.0f);

    // The pose cache is keyed by Model address; this test's model is a stack local,
    // so drop the cache (object.h:261-265) or a later test reusing this address
    // would read this model's pose.
    scripts::MdkObject::forget_poses();
}

// Items.BlastWeakParts (Items.cs:680), the weak-part core of the blast, ported
// ahead of the dispatcher. Pins the guard, the best-damage/point selection, the
// per-part health drop, and the destroyed-part event. The blast rays are short
// segments near the origin, clear in the shipped level-7 geometry (as in the
// blastDamage falloff test), so only the falloff and part accounting are exercised.
void testBlastWeakParts(LevelData* levelData, const Cmi* cmi, audio::SoundBank* bank) {
    Model model;
    const char* names[] = {"ARM0", "ARM1", "EMPTY"};
    const float verts[][6] = {{0, 0, 0, 2, 2, 2}, {1, 1, 1, 3, 3, 3}, {}};
    for (int i = 0; i < 3; i++) {
        Model::Part part;
        snprintf(part.name, sizeof(part.name), "%s", names[i]);
        const int n = i == 2 ? 0 : 2;
        for (int v = 0; v < n; v++) {
            part.vertices.push_back(Vec3f{verts[i][v * 3], verts[i][v * 3 + 1], verts[i][v * 3 + 2]});
        }
        model.partList.push_back(part);
    }
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);

    // Guard: without FlagWeakParts nothing happens and the point is the position.
    scripts::MdkObject plain;
    plain.model = &model;
    plain.position = Vec3{7.0, 7.0, 7.0};
    plain.part_health = {50, 50, 50};
    scripts::ScriptRuntime::BlastPartResult g =
        runtime.blastWeakParts(&plain, Vec3{0.0f, 0.0f, 0.0f}, 100, 10.0f, true);
    CHECK(g.best == 0);
    CHECK(g.point.x == 7.0f);
    CHECK(g.event == 0);
    CHECK(plain.part_health[0] == 50);

    // Guard: FlagWeakParts set but no model -> default result, no health change.
    scripts::MdkObject nomodel;
    nomodel.flags |= scripts::MdkObject::FlagWeakParts;
    nomodel.position = Vec3{3.0, 3.0, 3.0};
    nomodel.part_health = {50};
    scripts::ScriptRuntime::BlastPartResult nm =
        runtime.blastWeakParts(&nomodel, Vec3{0.0f, 0.0f, 0.0f}, 100, 10.0f, true);
    CHECK(nm.best == 0);
    CHECK(nm.point.x == 3.0f);
    CHECK(nm.event == 0);
    CHECK(nomodel.part_health[0] == 50);

    // Weak blast at (1,1,2). ARM0 (box 0..2, centre (1,1,1)) takes 90 (distance 1);
    // ARM1 (box 1..3, centre (2,2,2)) takes 100 (distance 0). ARM1 is the strict max,
    // so the point is ARM1's box centre (2,2,2) -- NOT the blast point -- pinning
    // Point = box.Center(). ARM0 health 91 -> 1 survives (pins the `< 1` boundary:
    // `<= 1` would destroy it); ARM1 health 100 -> 0 destroyed, event = index+1 = 2.
    // EMPTY is not a weak part and keeps its health.
    scripts::MdkObject obj;
    obj.model = &model;
    obj.flags |= scripts::MdkObject::FlagWeakParts;
    obj.weak_prefix = "ARM";
    obj.weak_prefix_length = 3;
    obj.position = Vec3{0.0, 0.0, 0.0};
    obj.yaw = 0.0f;
    obj.scale = 1.0f;
    obj.part_health = {91, 100, 50};
    scripts::ScriptRuntime::BlastPartResult r =
        runtime.blastWeakParts(&obj, Vec3{1.0f, 1.0f, 2.0f}, 100, 10.0f, true);
    CHECK(r.best == 100);
    CHECK(r.distance == 0.0f);
    CHECK(r.point.x == 2.0f && r.point.y == 2.0f && r.point.z == 2.0f);
    CHECK(r.event == 2);
    CHECK(obj.part_health[0] == 1);
    CHECK(obj.part_health[1] == 0);
    CHECK(obj.part_health[2] == 50);

    scripts::MdkObject::forget_poses();
}


// Opcode 174 (if_ammo) has no C# ScriptVmTests counterpart, so this pins the
// oracle directly (ScriptVm.cs:999-1005): kind 0 is the super chain gun's
// remaining tick count, kinds 1-5 are the sniper-ammo entries, and every
// other kind reads as zero. The comparison is the same one used by 175.
float runIfAmmo(scripts::ScriptRuntime& runtime, int32_t kind, int32_t comparison,
                float first, float second, bool hasSecond) {
    Code code;
    code.u8(kIfAmmo).u8(kind).u8(comparison).f32(first);
    if (hasSecond) {
        code.f32(second);
    }
    code.u8(kActionGoto).target("taken");
    code.u8(kSetVar).u8(kOwn).u8(0).f32(1.0f);
    code.end();
    code.label("taken");
    code.u8(kSetVar).u8(kOwn).u8(0).f32(2.0f);
    code.end();
    scripts::MdkObject obj;
    runProgram(runtime, code, &obj);
    CHECK(!sawUnimplemented(kIfAmmo));
    return obj.variables[0];
}

void testIfAmmoReadsSuperChainGunAndSniperAmmo(LevelData* levelData, const Cmi* cmi,
                                                audio::SoundBank* bank) {
    // A fresh runtime keeps the injected inventory values out of other tests.
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);

    // A fresh inventory has no super-chain-gun ticks and no sniper ammo.
    CHECK(runIfAmmo(runtime, 0, kLessThan, 1.0f, 0.0f, false) == 2.0f);
    CHECK(runIfAmmo(runtime, 1, kGreaterThan, 0.0f, 0.0f, false) == 1.0f);

    runtime.kurt().setSuperChainGun(100);
    runtime.kurt().setAmmo(1, 10);
    runtime.kurt().setAmmo(2, 20);
    runtime.kurt().setAmmo(3, 30);
    runtime.kurt().setAmmo(4, 40);
    runtime.kurt().setAmmo(5, 50);

    // Kind 0 reads the super chain gun, not the first sniper-ammo entry.
    CHECK(runIfAmmo(runtime, 0, kApproxEqual, 100.0f, 0.0f, false) == 2.0f);
    CHECK(runIfAmmo(runtime, 0, kApproxEqual, 10.0f, 0.0f, false) == 1.0f);
    CHECK(runIfAmmo(runtime, 0, kLessThan, 101.0f, 0.0f, false) == 2.0f);
    CHECK(runIfAmmo(runtime, 0, kLessThan, 100.0f, 0.0f, false) == 1.0f);

    // Kinds 1-5 map one-to-one onto the five sniper-ammo entries.
    for (int32_t type = 1; type <= 5; type++) {
        CHECK(runIfAmmo(runtime, type, kApproxEqual, (float)(type * 10), 0.0f, false) == 2.0f);
    }
    CHECK(runIfAmmo(runtime, 1, kApproxEqual, 20.0f, 0.0f, false) == 1.0f);
    CHECK(runIfAmmo(runtime, 5, kApproxEqual, 40.0f, 0.0f, false) == 1.0f);
    CHECK(runIfAmmo(runtime, 1, kGreaterThan, 9.0f, 0.0f, false) == 2.0f);
    CHECK(runIfAmmo(runtime, 1, kGreaterThan, 10.0f, 0.0f, false) == 1.0f);

    // The oracle's bounds guard leaves kinds above the five ammo entries at zero.
    CHECK(runIfAmmo(runtime, 6, kApproxEqual, 0.0f, 0.0f, false) == 2.0f);
    CHECK(runIfAmmo(runtime, 255, kApproxEqual, 0.0f, 0.0f, false) == 2.0f);

    // The between and outside comparisons consume their second bound.
    CHECK(runIfAmmo(runtime, 0, kBetween, 50.0f, 100.0f, true) == 2.0f);
    CHECK(runIfAmmo(runtime, 0, kBetween, 101.0f, 200.0f, true) == 1.0f);
    CHECK(runIfAmmo(runtime, 0, kOutside, 99.0f, 101.0f, true) == 1.0f);
    CHECK(runIfAmmo(runtime, 0, kOutside, 50.0f, 99.0f, true) == 2.0f);

    // An unknown comparison is false in the oracle, and the following
    // instruction still runs.
    CHECK(runIfAmmo(runtime, 0, 0, 0.0f, 0.0f, false) == 1.0f);
}

// Opcode 181 (boss_bar) has no C# ScriptVmTests counterpart, so this pins the
// oracle directly (ScriptVm.cs BossBar, :1854-1878): the bar shows ONLY while Kurt
// fires; mode 0 is the maximum minus a group counter, mode 1 an arena variable going
// from high (0) to low (the maximum), rounded away from zero; the value is capped at
// the maximum; and a value of 0 or less hides the bar (ShowBarValues).
void runBossBar(scripts::ScriptRuntime& runtime, scripts::MdkObject* obj, int32_t mode,
                int32_t index, float low, float high, int32_t maxHealth) {
    Code code;
    code.u8(kBossBar).u8(mode).u8(index);
    if (mode == 1) {
        code.f32(low).f32(high);
    }
    code.u32(maxHealth);
    code.end();
    runProgram(runtime, code, obj);
    CHECK(!sawUnimplemented(kBossBar));
}

void testBossBarShowsOnlyWhileKurtFires(LevelData* levelData, const Cmi* cmi,
                                        audio::SoundBank* bank) {
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);
    scripts::MdkObject obj;
    obj.arena = "BOSSBAR";
    runtime.setGroupCounter("BOSSBAR", 3, 40);

    // Not firing: the opcode leaves the bar alone.
    runBossBar(runtime, &obj, 0, 3, 0.0f, 0.0f, 100);
    CHECK(runtime.bar().health == 0 && runtime.bar().max == 0);
    CHECK(runtime.barTime == 0.0f);

    // Firing: max - counter of group 3.
    runtime.kurt().setFiring(true);
    runBossBar(runtime, &obj, 0, 3, 0.0f, 0.0f, 100);
    CHECK(runtime.bar().health == 60 && runtime.bar().max == 100);
    CHECK(runtime.barTime == 1.0f);
    CHECK(runtime.barObject == nullptr);

    // Mode 1: variable 2 at 4 between low 0 and high 10, of 50 -> (10-4)/(10-0)*50 = 30.
    runtime.getArenaState("BOSSBAR").variables[2] = 4.0f;
    runBossBar(runtime, &obj, 1, 2, 0.0f, 10.0f, 50);
    CHECK(runtime.bar().health == 30 && runtime.bar().max == 50);
    // Away from zero, not to even: (10-7.5)/(10-0)*10 is exactly 2.5 -> 3 (banker's: 2).
    runtime.getArenaState("BOSSBAR").variables[2] = 7.5f;
    runBossBar(runtime, &obj, 1, 2, 0.0f, 10.0f, 10);
    CHECK(runtime.bar().health == 3 && runtime.bar().max == 10);

    // A counter below zero would show more than the maximum: capped at it.
    runtime.setGroupCounter("BOSSBAR", 3, -25);
    runBossBar(runtime, &obj, 0, 3, 0.0f, 0.0f, 100);
    CHECK(runtime.bar().health == 100 && runtime.bar().max == 100);

    // A counter at the maximum is a value of 0: ShowBarValues hides the bar.
    runtime.setGroupCounter("BOSSBAR", 3, 100);
    runBossBar(runtime, &obj, 0, 3, 0.0f, 0.0f, 100);
    CHECK(runtime.barTime == 0.0f);
    CHECK(runtime.bar().health == 0 && runtime.bar().max == 0);

    // A maximum of 0 shows nothing even while firing.
    runtime.setGroupCounter("BOSSBAR", 3, 0);
    runBossBar(runtime, &obj, 0, 3, 0.0f, 0.0f, 0);
    CHECK(runtime.barTime == 0.0f);
}

// Opcode 209 (if_player_on_me_low): Kurt stands on the object, fires, and is not above
// its top (ScriptVm.cs:1269-1273). Kurt.Platform is not ported, so the platform half is
// null and the branch is not taken even while he fires; what this pins is that the
// opcode is dispatched (not the unimplemented bucket) and reads the live firing flag
// through getKurtPlatform, which a platform port will make the whole condition.
void testIfPlayerOnMeLowNeedsThePlatform(LevelData* levelData, const Cmi* cmi,
                                         audio::SoundBank* bank) {
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);
    runtime.kurt().setFiring(true);
    Code code;
    code.u8(kIfPlayerOnMeLow).u8(kActionGoto).target("taken");
    code.u8(kSetVar).u8(kOwn).u8(0).f32(1.0f);
    code.end();
    code.label("taken");
    code.u8(kSetVar).u8(kOwn).u8(0).f32(2.0f);
    code.end();
    scripts::MdkObject obj;
    runProgram(runtime, code, &obj);
    CHECK(!sawUnimplemented(kIfPlayerOnMeLow));
    CHECK(runtime.getKurtPlatform() == nullptr);
    CHECK(obj.variables[0] == 1.0f);
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
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);
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
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);
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
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);
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

    // The other half of that counter (review-12 E): a hurt the guard REFUSES does not
    // merely leave KnockDamage alone, it zeroes it (Kurt.Combat.cs:127). Both arms of
    // the guard are pinned here, because the oracle golden cannot: a hurt that reaches
    // the zeroing is by definition a hurt that never reaches the Damage path the
    // golden prints. The state arm is the one review-11 R11-3 was about.
    {
        runtime.kurt().setHealth(100);
        runtime.kurt().setInvulnerable(2.0f);
        runtime.kurt().setKnockDamage(7.0f);
        const int32_t health = runtime.kurt().health();
        runtime.kurt().hurt(5);
        CHECK(runtime.kurt().knockDamage() == 0.0f);
        CHECK(runtime.kurt().health() == health);
        runtime.kurt().setInvulnerable(0.0f);
    }
    {
        runtime.kurt().setHealth(100);
        runtime.kurt().setState(Kurt::HardLand);
        runtime.kurt().setKnockDamage(6.0f);
        const int32_t health = runtime.kurt().health();
        runtime.kurt().hurt(5);
        CHECK(runtime.kurt().knockDamage() == 0.0f);
        CHECK(runtime.kurt().health() == health);
        runtime.kurt().setState(Kurt::Still);
    }

    // The decline itself (Kurt.Combat.cs:230-231): clamp into [0, KnockdownDamage]
    // FIRST, then drain KnockdownDrain per second, floored at zero. The oracle pins
    // the same two rules in golden/L3_kurt_knock.txt; these pin the two shapes that
    // fixture cannot reach without a knock-down state machine: a counter above the
    // threshold standing on the floor (the oracle would have knocked him down), and a
    // counter at or below zero, which must not go negative.
    {
        runtime.kurt().setKnockDamage(9.0f);
        runtime.kurt().declineKnockDamage(1.0f / 60.0f);
        CHECK(fabsf(runtime.kurt().knockDamage() - (5.0f - 2.0f * (1.0f / 60.0f))) < 1e-6f);
        for (int i = 0; i < 200; i++) {
            runtime.kurt().declineKnockDamage(1.0f / 60.0f);
        }
        CHECK(runtime.kurt().knockDamage() == 0.0f);
        runtime.kurt().declineKnockDamage(1.0f / 60.0f);
        CHECK(runtime.kurt().knockDamage() == 0.0f);
        runtime.kurt().setKnockDamage(-1.0f);
        runtime.kurt().declineKnockDamage(1.0f / 60.0f);
        CHECK(runtime.kurt().knockDamage() == 0.0f);
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

    // The timer's own decline (Kurt.cs:139, Update's second statement). The oracle
    // golden L3_kurt_invuln.txt pins its rate and its position in Update; these pin the two
    // shapes that fixture cannot show, because a hurt that reaches the Damage path is by
    // definition a hurt the timer already released: the timer must not go negative, and
    // opcode 244's accumulate arm must add to a floored timer, not to a negative one.
    {
        runtime.kurt().setInvulnerable(3.0f);
        runtime.kurt().declineInvulnerability(1.0f / 60.0f);
        CHECK(fabsf(runtime.kurt().invulnerable() - (3.0f - 1.0f / 60.0f)) < 1e-6f);
        for (int i = 0; i < 200; i++) {
            runtime.kurt().declineInvulnerability(1.0f / 60.0f);
        }
        CHECK(runtime.kurt().invulnerable() == 0.0f);
        runtime.kurt().declineInvulnerability(1.0f / 60.0f);
        CHECK(runtime.kurt().invulnerable() == 0.0f);
    }
    {
        // What the floor at zero is FOR: a timer that drained past it would make "1.5 s
        // more" mean less than 1.5 s.
        runtime.kurt().setInvulnerable(0.5f);
        runtime.kurt().declineInvulnerability(1.0f);
        CHECK(runtime.kurt().invulnerable() == 0.0f);
        Code code;
        code.u8(kSetGlobal573bd4).u8(1).f32(1.5f);
        code.end();
        scripts::MdkObject obj;
        runProgram(runtime, code, &obj);
        CHECK(runtime.kurt().invulnerable() == 1.5f);
    }

    // The reason the decline matters at all: opcode 244 is a timer, not a switch. While it
    // runs, hurt() refuses (Kurt.Combat.cs:125); once it has run out, the same hurt lands.
    // Without the decline the first two CHECKs pass forever and a script that ever set the
    // timer made Kurt immortal for the rest of the level.
    {
        runtime.kurt().setHealth(100);
        runtime.kurt().setKnockDamage(0.0f);
        runtime.kurt().setInvulnerable(0.5f);
        runtime.kurt().hurt(10);
        CHECK(runtime.kurt().health() == 100);
        for (int i = 0; i < 29; i++) {  // 29/60 s is under 0.5 s, so still invulnerable
            runtime.kurt().declineInvulnerability(1.0f / 60.0f);
        }
        CHECK(runtime.kurt().invulnerable() > 0.0f);
        runtime.kurt().hurt(10);
        CHECK(runtime.kurt().health() == 100);
        for (int i = 0; i < 60; i++) {
            runtime.kurt().declineInvulnerability(1.0f / 60.0f);
        }
        CHECK(runtime.kurt().invulnerable() == 0.0f);
        runtime.kurt().hurt(10);
        CHECK(runtime.kurt().health() == 90);
        runtime.kurt().setHealth(100);
        runtime.kurt().setKnockDamage(0.0f);
    }
}

// The two flash fields as state, not as write-only numbers: the clamp arms of the red one
// (Kurt.Combat.cs:145, which the golden reaches only from the hard landing's side), the
// fade both of them fall by (:203-206), and the skull's end (:238-253). The oracle golden
// L3_kurt_flashes.txt pins the same rules through the level -- the fade's rate, the slam's
// 180, the death transition's zero, the step Died fires on -- and these pin the two shapes
// that fixture cannot reach: a flash driven past its clamp by a second hit, and a fade
// asked to go below zero.
void testKurtFlashes(scripts::ScriptRuntime& runtime) {
    // Damage's clamp, both arms (0x573b70): +25 per point, kept within 75-180. A hit of 1
    // is 25 raw and still flashes 75; a hit of 10 is 250 raw and stops at 180; a second
    // hit on top of a full flash cannot raise it further, and cannot lower it either.
    runtime.kurt().setHurtFlash(0.0f);
    runtime.kurt().setHealth(100);
    runtime.kurt().damage(1);
    CHECK(runtime.kurt().hurtFlash() == Kurt::kHurtFlashMin);
    runtime.kurt().setHurtFlash(0.0f);
    runtime.kurt().damage(10);
    CHECK(runtime.kurt().hurtFlash() == Kurt::kHurtFlashMax);
    runtime.kurt().damage(10);
    CHECK(runtime.kurt().hurtFlash() == Kurt::kHurtFlashMax);
    runtime.kurt().setHealth(100);

    // The fade (Kurt.Combat.cs:204-205): both fields fall by kFlashFade per second and
    // stop at zero -- the floor is what keeps a finished flash off the screen, because
    // HudView.DrawFlashes tests `> 0f` and opcode 215 maxes onto whatever is there.
    runtime.kurt().setHurtFlash(100.0f);
    runtime.kurt().setWhiteFlash(60.0f);
    runtime.kurt().fadeFlashes(1.0f / 60.0f);
    CHECK(fabsf(runtime.kurt().hurtFlash() - (100.0f - Kurt::kFlashFade / 60.0f)) < 1e-4f);
    CHECK(fabsf(runtime.kurt().whiteFlash() - (60.0f - Kurt::kFlashFade / 60.0f)) < 1e-4f);
    for (int i = 0; i < 200; i++) {
        runtime.kurt().fadeFlashes(1.0f / 60.0f);
    }
    CHECK(runtime.kurt().hurtFlash() == 0.0f);
    CHECK(runtime.kurt().whiteFlash() == 0.0f);
    runtime.kurt().fadeFlashes(1.0f / 60.0f);
    CHECK(runtime.kurt().hurtFlash() == 0.0f);
    CHECK(runtime.kurt().whiteFlash() == 0.0f);

    // The skull: it rises kSkullFade per second, it is not finished AT SkullEnd (the
    // oracle's guard is a strict >, Kurt.Combat.cs:238), and the step that passes it is
    // the one that reports the C#'s Died event -- once, because the guard stops the rise
    // and nothing re-crosses it. The golden pins the same crossing at step 587 of its own
    // schedule; this pins that it is the crossing and not the value 256 that matters.
    runtime.kurt().setHurtFlash(0.0f);
    CHECK(!runtime.kurt().skullFinished());
    int died = 0;
    for (int i = 0; i < 300; i++) {
        // The guard UpdateDeath runs before the rise (Kurt.Combat.cs:238). It is what
        // makes the event fire once: without it every later step re-crosses SkullEnd.
        if (runtime.kurt().skullFinished()) {
            continue;
        }
        if (runtime.kurt().fadeSkullIn(1.0f / 60.0f)) {
            died++;
        }
    }
    CHECK(runtime.kurt().hurtFlash() > Kurt::kSkullEnd);
    CHECK(runtime.kurt().skullFinished());
    CHECK(died == 1);
    runtime.kurt().setHurtFlash(Kurt::kSkullEnd);
    CHECK(!runtime.kurt().skullFinished());
    runtime.kurt().setHurtFlash(0.0f);
    CHECK(!runtime.kurt().fadeSkullIn(0.0f));
    // Leave the combat half as the cases after this one expect it: damage() above moved
    // the health and the knock counter on the way to the flash.
    runtime.kurt().setKnockDamage(0.0f);
    runtime.kurt().setHealth(100);
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

// Opcode 133 (arena_texture_frame) drives the runtime's AnimatedTextures
// state (AnimatedTextures.cs): mode 1 adds value*dt frames, wrapping into
// the texture's frame count; mode 0 sets value-1. M_COMM in level 7's
// DANT_5 arena is the oracle's own animated-texture example.
void testArenaTextureFrameAnimates(scripts::ScriptRuntime& runtime) {
    scripts::MdkObject obj;
    obj.arena = "DANT_5";
    Code code;
    code.u8(kArenaTextureFrame).pstr("M_COMM").u8(0).f32(2.0f);
    code.end();
    runProgram(runtime, code, &obj);
    CHECK(!sawUnimplemented(kArenaTextureFrame));
    scripts::AnimatedTextures& animated = runtime.animatedTextures;
    const int32_t first = animated.frameOf(obj.arena, "M_COMM");
    CHECK(first == 1);

    Code more;
    more.u8(kArenaTextureFrame).pstr("M_COMM").u8(2).f32(1.5f);
    more.end();
    runProgram(runtime, more, &obj);
    CHECK(animated.frameOf(obj.arena, "M_COMM") == 2);

    // An unknown texture has no frames to animate: a clean no-op.
    Code missing;
    missing.u8(kArenaTextureFrame).pstr("NO_SUCH_TEXTURE").u8(0).f32(2.0f);
    missing.end();
    runProgram(runtime, missing, &obj);
    CHECK(animated.frameOf(obj.arena, "NO_SUCH_TEXTURE") == 0);
}

// Opcode 189 (lob_to_kurt) throws the receiver so it lands on Kurt: the
// horizontal velocity solves the ballistic fall to Kurt's height, capped
// by the operand's speed. Mode 2 (and anything above 1) does nothing.
void testLobToKurtThrowsAtKurt(scripts::ScriptRuntime& runtime) {
    scripts::MdkObject obj;
    obj.position = runtime.kurtPosition();
    obj.position.x += 20.0f;  // aside from Kurt, ten units above his height
    obj.position.z += 10.0f;
    obj.velocity = mdk::Vec3{0.0f, 0.0f, 0.0f};
    obj.gravity = 32.0f;
    obj.friction = 64.0f;
    Code code;
    // Mode 0: land at the height operand -- Kurt's own height here.
    code.u8(kLobToKurt).u8(0).f32(1000.0f).f32(runtime.kurtPosition().z);
    code.end();
    runProgram(runtime, code, &obj);
    CHECK(!sawUnimplemented(kLobToKurt));
    CHECK(obj.velocity.x < 0.0f);  // thrown back toward Kurt
    CHECK(obj.velocity.y == 0.0f);

    // Mode 2 does nothing at all.
    obj.velocity = mdk::Vec3{1.0f, 2.0f, 3.0f};
    Code noop;
    noop.u8(kLobToKurt).u8(2);
    noop.end();
    runProgram(runtime, noop, &obj);
    CHECK(obj.velocity.x == 1.0f && obj.velocity.y == 2.0f && obj.velocity.z == 3.0f);
}

// Opcode 247 (hud_message) queues a HUD message on the runtime (flags,
// text, seconds), in order.
void testHudMessageQueues(scripts::ScriptRuntime& runtime) {
    const size_t before = runtime.messages().size();
    scripts::MdkObject obj;
    Code code;
    code.u8(kHudMessage).u8(3).pstr("HELLO").f32(2.5f);
    code.u8(kHudMessage).u8(0).pstr("WORLD").f32(1.0f);
    code.end();
    runProgram(runtime, code, &obj);
    CHECK(!sawUnimplemented(kHudMessage));
    CHECK(runtime.messages().size() == before + 2);
    CHECK(runtime.messages()[before].text == "HELLO");
    CHECK(runtime.messages()[before].flags == 3);
    CHECK(runtime.messages()[before].seconds == 2.5f);
    CHECK(runtime.messages()[before + 1].text == "WORLD");
}

// Opcodes 142/143/190 (fan_create/remove/enable) drive the runtime's
// Fans: a fan on a real type-7 hotspot lifts what its box holds, an
// enabled-off fan holds nothing for Kurt's mask, and a removed fan is
// gone. The hotspot is the level's own first type-7 record, found by
// scanning the DTI the runtime was built on.
void testFanLifecycle(scripts::ScriptRuntime& runtime, LevelData& level) {
    std::string arena;
    int32_t hotspot = -1;
    Vec3 boxMid{0.0f, 0.0f, 0.0f};
    const std::vector<Dti::ArenaEntry>& arenas = level.dti().arenas();
    for (size_t i = 0; i < arenas.size() && hotspot < 0; i++) {
        for (size_t r = 0; r < arenas[i].records.size(); r++) {
            const Dti::Record& record = arenas[i].records[r];
            if (record.type == 7) {
                arena = arenas[i].name;
                hotspot = record.id;
                boxMid = Vec3{(record.position[0] + record.boxEnd[0]) * 0.5f,
                              (record.position[1] + record.boxEnd[1]) * 0.5f,
                              (record.position[2] + record.boxEnd[2]) * 0.5f};
                break;
            }
        }
    }
    CHECK(hotspot >= 0);

    const int32_t before = runtime.fans.count();
    scripts::MdkObject obj;
    obj.arena = arena;
    Code create;
    create.u8(kFanCreate).u8(hotspot).pstr("TESTFAN").u8(0).u8(3).f32(20.0f);
    create.end();
    runProgram(runtime, create, &obj);
    CHECK(!sawUnimplemented(kFanCreate));
    CHECK(runtime.fans.count() == before + 1);
    int32_t mask = 0;
    float strength = 0.0f;
    CHECK(runtime.fans.fan(arena, "TESTFAN", &mask, &strength));
    CHECK(strength == 20.0f);

    // Inside the box, the fan lifts: a still point gains upward speed.
    const float lifted =
        runtime.fans.query(arena, boxMid, 0.0f, scripts::Fans::kMaskKurt, 1.0f / 30.0f);
    CHECK(lifted == lifted);  // not NaN
    CHECK(lifted > 0.0f);

    // fan_enable off clears bit 0: Kurt's mask no longer matches.
    Code off;
    off.u8(kFanEnable).u8(0).pstr("TESTFAN");
    off.end();
    runProgram(runtime, off, &obj);
    CHECK(!sawUnimplemented(kFanEnable));
    const float held =
        runtime.fans.query(arena, boxMid, 0.0f, scripts::Fans::kMaskKurt, 1.0f / 30.0f);
    CHECK(held != held);  // NaN: no fan holds it

    // fan_remove takes it away entirely.
    Code remove;
    remove.u8(kFanRemove).pstr("TESTFAN");
    remove.end();
    runProgram(runtime, remove, &obj);
    CHECK(!sawUnimplemented(kFanRemove));
    CHECK(runtime.fans.count() == before);
}

// Opcode 221 (pick_waypoint8): a weighted-random pick of the arena's DTI type-8
// waypoints becomes the destination (command 221), and the opcode BRANCHES on
// whether one was found (ScriptVm.cs:1268 `return Branch(..., MoveCommand == 221)`).
// The synthetic arena "WPTEST" (injected in main() before any runtime is built)
// holds six waypoints. The object sits at the origin; no player is bound, so Kurt
// stays at the level-7 start (0,4,9) (the runtime seeds kurtPosition_ from the DTI
// start and runProgram never moves him), so the weights are the C#'s 500-distance
// form with the toKurt / near-Kurt terms evaluated against that start, NOT against
// the object. Each scenario runs on a fresh runtime (seed 1) so the RNG state at
// the draw is known and the pick is reproducible. Positions are chosen so one
// waypoint dominates each mode's weight total (id 1 in mode 0; id 0/id 1 in mode
// 1), which is what lets the pinned draw sequences reject a uniform picker as well
// as a first/last picker.
//   id 0  (30,0,0)    dist 30   mode0 no (<50)   mode1 yes (nearest, anchor)
//   id 1  (50,0,0)    dist 50   mode0 yes (dom)  mode1 yes (dom)
//   id 2  (0,499,0)   dist 499  mode0 yes        mode1 yes
//   id 3  (0,498,0)   dist 498  mode0 yes        mode1 yes
//   id 9  (0,497,0)   dist 497  mode0 yes        mode1 no  (id 9 from nearest)
//   id 21 (600,0,0)   dist 600  mode0 no (>500)  mode1 no  (id 21 from nearest)
void testPickWaypoint8(LevelData* levelData, Cmi* cmi, audio::SoundBank* bank) {
    const std::string arena = "WPTEST";
    auto inSet = [](const Vec3& p, const Vec3* set, size_t n) {
        for (size_t i = 0; i < n; i++) {
            if (p.x == set[i].x && p.y == set[i].y && p.z == set[i].z) {
                return true;
            }
        }
        return false;
    };
    auto same = [](const Vec3& p, const float* q) {
        return p.x == q[0] && p.y == q[1] && p.z == q[2];
    };
    // mode 0 keeps the 50-500 window (drops id 0 at 30 and id 21 at 600); mode 1
    // keeps the ids within 3 of the nearest (id 0), so it keeps id 0 (30,0,0) but
    // drops id 9 (0,497,0). The two sets differ on exactly those two waypoints.
    const Vec3 mode0Set[] = {Vec3{50, 0, 0}, Vec3{0, 499, 0}, Vec3{0, 498, 0}, Vec3{0, 497, 0}};
    const Vec3 mode1Set[] = {Vec3{30, 0, 0}, Vec3{50, 0, 0}, Vec3{0, 499, 0}, Vec3{0, 498, 0}};

    // pick: reset global[1], run pick_waypoint8 `mode`, branch to "found" when a
    // waypoint is chosen (global[1] = 1) else fall through (global[1] stays 0).
    auto runPick = [&](scripts::ScriptRuntime& rt, int32_t mode, scripts::MdkObject& obj) {
        Code code;
        code.u8(kSetVar).u8(kGlobal).u8(1).f32(0.0f);
        code.u8(kPickWaypoint8).u8(mode).u8(kActionGoto).target("found");
        code.end();
        code.label("found");
        code.u8(kSetVar).u8(kGlobal).u8(1).f32(1.0f);
        code.end();
        runProgram(rt, code, &obj);
    };
    auto fresh = [&]() { return scripts::ScriptRuntime(levelData, cmi, 1, bank, nullptr, nullptr); };

    // Mode 0: a waypoint in the 50-500 window is chosen, the branch is taken, the
    // destination is one of the four qualifying waypoints, and the stuck flag clears.
    {
        scripts::ScriptRuntime rt = fresh();
        scripts::MdkObject obj;
        obj.arena = arena;
        obj.position = Vec3{0.0f, 0.0f, 0.0f};
        obj.contact_flags = scripts::MdkObject::ContactStuck;
        runPick(rt, 0, obj);
        CHECK(!sawUnimplemented(kPickWaypoint8));
        CHECK(obj.move_command == 221);
        CHECK(rt.globalVariables[1] == 1.0f);  // branch taken
        CHECK(inSet(obj.move_destination, mode0Set, 4));
        CHECK(obj.waypoint.x == obj.move_destination.x &&
              obj.waypoint.y == obj.move_destination.y &&
              obj.waypoint.z == obj.move_destination.z);
        CHECK((obj.contact_flags & scripts::MdkObject::ContactStuck) == 0);
        // The weighted draw is the opcode's substance, so pin it, not just the
        // qualifying set: on one seed-1 runtime the successive draws (the RNG
        // advances each call) must reproduce the oracle's weighted sequence. Here
        // id 1 dominates the weight total, so every draw is (50,0,0); a uniform
        // picker (which would scatter over all four qualifying waypoints) and a
        // last-qualifying picker (always (0,497,0)) both fail this. A first-
        // qualifying picker coincidentally matches here (id 1 is also first in DTI
        // order), so mode 1 below is what rejects that case. This is the oracle's
        // output for this fixture.
        const float seq[8][3] = {{50, 0, 0}, {50, 0, 0}, {50, 0, 0}, {50, 0, 0},
                                 {50, 0, 0}, {50, 0, 0}, {50, 0, 0}, {50, 0, 0}};
        for (int d = 0; d < 8; d++) {
            runPick(rt, 0, obj);
            CHECK(obj.move_command == 221);
            CHECK(same(obj.move_destination, seq[d]));
        }
    }

    // Mode 1: only the waypoints within 3 of the nearest id (0) qualify, so the
    // destination is never the id-9 waypoint at (0,497,0).
    {
        scripts::ScriptRuntime rt = fresh();
        scripts::MdkObject obj;
        obj.arena = arena;
        obj.position = Vec3{0.0f, 0.0f, 0.0f};
        runPick(rt, 1, obj);
        CHECK(obj.move_command == 221);
        CHECK(inSet(obj.move_destination, mode1Set, 4));
        // Same weight pin on the mode-1 id-window set (id 0/1/2/3). id 0 and id 1
        // carry the weight and the two far id-window waypoints are near-floor (id 2
        // weight-1, id 3 weight-2), so the sequence mixes only (30,0,0) and
        // (50,0,0). This rejects a first-qualifying picker (always (30,0,0)), a
        // last-qualifying picker (always (0,498,0)), and a uniform picker (which
        // would reach id 2/id 3).
        const float seq[8][3] = {{30, 0, 0}, {30, 0, 0}, {50, 0, 0}, {50, 0, 0},
                                 {30, 0, 0}, {30, 0, 0}, {50, 0, 0}, {30, 0, 0}};
        for (int d = 0; d < 8; d++) {
            runPick(rt, 1, obj);
            CHECK(obj.move_command == 221);
            CHECK(same(obj.move_destination, seq[d]));
        }
    }

    // Formula-sensitive companion on arena WPTEST2. WPTEST's dominant fixture pins
    // the selection distribution (it rejects a uniform picker) but is invariant to
    // the weight-formula terms. WPTEST2's waypoints sit on the x-axis so the +250
    // bump moves the cumulative pick boundaries: disabling the +250 bump reddens
    // the pinned seed-1 sequence below (verified by shadow build) while leaving
    // WPTEST green. The toKurt<200 near-Kurt term and the 500-distance base are
    // exercised here but NOT pinned by these draws -- removing either leaves the
    // seed-1 sequence byte-identical (the draws do not cross the shifted
    // boundaries), so those two terms are guarded by no fixture.
    {
        const std::string arena2 = "WPTEST2";
        {
            scripts::ScriptRuntime rt = fresh();
            scripts::MdkObject obj;
            obj.arena = arena2;
            obj.position = Vec3{0.0f, 0.0f, 0.0f};
            runPick(rt, 0, obj);
            CHECK(obj.move_command == 221);
            CHECK(rt.globalVariables[1] == 1.0f);
            CHECK(same(obj.move_destination, (const float[]){100, 0, 0}));
            const float seq[8][3] = {{100, 0, 0}, {300, 0, 0}, {150, 0, 0}, {120, 0, 0},
                                     {300, 0, 0}, {300, 0, 0}, {150, 0, 0}, {100, 0, 0}};
            for (int d = 0; d < 8; d++) {
                runPick(rt, 0, obj);
                CHECK(obj.move_command == 221);
                CHECK(same(obj.move_destination, seq[d]));
            }
        }
        {
            scripts::ScriptRuntime rt = fresh();
            scripts::MdkObject obj;
            obj.arena = arena2;
            obj.position = Vec3{0.0f, 0.0f, 0.0f};
            runPick(rt, 1, obj);
            CHECK(obj.move_command == 221);
            CHECK(same(obj.move_destination, (const float[]){30, 0, 0}));
            const float seq[8][3] = {{30, 0, 0}, {100, 0, 0}, {120, 0, 0}, {300, 0, 0},
                                     {100, 0, 0}, {100, 0, 0}, {120, 0, 0}, {30, 0, 0}};
            for (int d = 0; d < 8; d++) {
                runPick(rt, 1, obj);
                CHECK(obj.move_command == 221);
                CHECK(same(obj.move_destination, seq[d]));
            }
        }
    }

    // An unknown mode (2) takes the mode-0 window (the C#'s `mode != 1`).
    {
        scripts::ScriptRuntime rt = fresh();
        scripts::MdkObject obj;
        obj.arena = arena;
        obj.position = Vec3{0.0f, 0.0f, 0.0f};
        runPick(rt, 2, obj);
        CHECK(obj.move_command == 221);
        CHECK(inSet(obj.move_destination, mode0Set, 4));
    }

    // No qualifying waypoint (an arena with no type-8 records): MoveCommand is
    // cleared to 0 and the branch is NOT taken.
    {
        scripts::ScriptRuntime rt = fresh();
        scripts::MdkObject none;
        none.arena = "NO_SUCH_ARENA";
        none.position = Vec3{0.0f, 0.0f, 0.0f};
        none.move_command = 5;  // a prior command the opcode must clear
        runPick(rt, 0, none);
        CHECK(none.move_command == 0);
        CHECK(rt.globalVariables[1] == 0.0f);  // branch not taken
    }

    // Determinism: two fresh runtimes on the same seed make the same pick.
    {
        scripts::ScriptRuntime rt1 = fresh();
        scripts::ScriptRuntime rt2 = fresh();
        scripts::MdkObject a, b;
        a.arena = b.arena = arena;
        a.position = b.position = Vec3{0.0f, 0.0f, 0.0f};
        runPick(rt1, 0, a);
        runPick(rt2, 0, b);
        CHECK(a.move_command == 221 && b.move_command == 221);
        CHECK(a.move_destination.x == b.move_destination.x);
        CHECK(a.move_destination.y == b.move_destination.y);
        CHECK(a.move_destination.z == b.move_destination.z);
    }
}

// Opcodes 128/132/136 (attach_effect, spawn_effect, spawn_debris)
// drive the runtime's Effects pool: drops spawn and expire, a bubble
// spawns at a point, a wound attaches and detaches by "OFF".
void testEffectsLifecycle(scripts::ScriptRuntime& runtime) {
    scripts::MdkObject obj;
    obj.arena = "DANT_1";
    obj.position = Vec3{0.0f, 0.0f, 100.0f};
    const int32_t before = runtime.effects.count();

    // spawn_debris with the detail on (the runtime's default option):
    // three drops at the object's position.
    Code drops;
    drops.u8(kSpawnDebris).u8(3).f32(0.0f).f32(0.0f).f32(0.0f).f32(0.0f).u8(0)
        .f32(0.0f).f32(0.0f).f32(0.0f).f32(4.0f);
    drops.end();
    runProgram(runtime, drops, &obj);
    CHECK(!sawUnimplemented(kSpawnDebris));
    CHECK(runtime.effects.count() == before + 3);
    CHECK(runtime.effects.hasEffects("DANT_1"));

    // They fall and expire: a few hundred updates clear them.
    for (int i = 0; i < 400; i++) {
        runtime.effects.update(1.0f);
    }
    CHECK(runtime.effects.count() == before);

    // spawn_effect at an explicit point (255): a bubble appears.
    Code bubble;
    bubble.u8(kSpawnEffect).u8(100).u8(255).f32(1.0f).f32(2.0f).f32(3.0f);
    bubble.end();
    runProgram(runtime, bubble, &obj);
    CHECK(!sawUnimplemented(kSpawnEffect));
    CHECK(runtime.effects.count() == before + 1);

    // attach_effect starts a wound; "OFF" with the same point stops it.
    Code attach;
    attach.u8(kAttachEffect).pstr("WOUND").u8(0).u8(1);
    attach.end();
    runProgram(runtime, attach, &obj);
    CHECK(!sawUnimplemented(kAttachEffect));
    CHECK(runtime.effects.count() == before + 2);
    Code off;
    off.u8(kAttachEffect).pstr("OFF").u8(0).u8(0);
    off.end();
    runProgram(runtime, off, &obj);
    CHECK(runtime.effects.count() == before + 1);
}

// EffectsView's geometry (Objects/EffectsView.cs): the billboard basis
// read out of the view-projection, the sprite quad's corners and UVs,
// and a spark's facing-shaded palette index.
void testEffectsViewGeometry() {
    // A view-projection whose first column is +X and whose fourth is
    // +Z: right +X, forward +Z, up = right x forward = -Y.
    Mat4 vp = Mat4::identity();
    vp.m[11] = 1.0f;
    Vec3 right;
    Vec3 up;
    Vec3 forward;
    effectsBillboardBasis(vp, &right, &up, &forward);
    CHECK(right.x == 1.0f && right.y == 0.0f && right.z == 0.0f);
    CHECK(forward.x == 0.0f && forward.y == 0.0f && forward.z == 1.0f);
    CHECK(up.x == 0.0f && up.y == -1.0f && up.z == 0.0f);

    // A 2x2-texel sprite at the origin, one texel = one unit each way.
    Vertex quad[6];
    effectsQuadVertices(Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, 2, 2,
                        quad);
    CHECK(quad[0].pos.x == -1.0f && quad[0].pos.y == 1.0f && quad[0].uv.x == 0.0f &&
          quad[0].uv.y == 0.0f);
    CHECK(quad[1].pos.x == 1.0f && quad[1].pos.y == 1.0f && quad[1].uv.x == 1.0f &&
          quad[1].uv.y == 0.0f);
    CHECK(quad[2].pos.x == 1.0f && quad[2].pos.y == -1.0f && quad[2].uv.x == 1.0f &&
          quad[2].uv.y == 1.0f);
    CHECK(quad[3].pos.x == quad[0].pos.x && quad[3].pos.y == quad[0].pos.y);
    CHECK(quad[5].pos.x == -1.0f && quad[5].pos.y == -1.0f && quad[5].uv.x == 0.0f &&
          quad[5].uv.y == 1.0f);

    // THE GLOW OF A SPRITE TEXTURE (the 3DS's billboards): the share of opaque
    // texels whose LINEAR colour passes the PC bloom's 0.8 in any channel, and
    // their mean sRGB colour. Entry 1 is white (passes), 2 a mid grey (0.5 sRGB is
    // 0.22 linear: does not), 3 a red at 0.95 sRGB (0.89 linear: passes on red
    // alone); index 0 is transparent and is not counted at all.
    {
        uint8_t palette[256 * 4] = {};
        const uint8_t white[4] = {255, 255, 255, 255};
        const uint8_t grey[4] = {128, 128, 128, 255};
        const uint8_t red[4] = {242, 20, 20, 255};
        memcpy(palette + 4, white, 4);
        memcpy(palette + 8, grey, 4);
        memcpy(palette + 12, red, 4);
        const uint8_t texels[8] = {0, 0, 1, 2, 2, 3, 0, 2};   // 5 opaque: 1 white, 1 red, 3 grey
        float glow = -1.0f;
        float colour[3] = {-1.0f, -1.0f, -1.0f};
        effectsGlowOf(texels, 8, palette, &glow, colour);
        CHECK(fabsf(glow - 2.0f / 5.0f) < 1e-6f);
        // The mean of max(linear - 0.8, 0): white 0.2 on every channel, the red
        // (242/255)^2.2 - 0.8 on red and nothing on green.
        const float redExcess = powf(242.0f / 255.0f, 2.2f) - 0.8f;
        CHECK(fabsf(colour[0] - (0.2f + redExcess) / 2.0f) < 1e-4f);
        CHECK(fabsf(colour[1] - 0.2f / 2.0f) < 1e-4f);
        const uint8_t dim[4] = {2, 2, 0, 0};                    // grey and transparent only
        effectsGlowOf(dim, 4, palette, &glow, colour);
        CHECK(glow == 0.0f && colour[0] == 0.0f && colour[1] == 0.0f && colour[2] == 0.0f);
        effectsGlowOf(nullptr, 4, palette, &glow, colour);    // pixels not resident: none
        CHECK(glow == 0.0f);
        const uint8_t clear[2] = {0, 0};                        // nothing opaque: none
        effectsGlowOf(clear, 2, palette, &glow, colour);
        CHECK(glow == 0.0f);
    }

    // A spark face turned to the camera takes base + range; edge-on and
    // degenerate faces take base; the index clamps to the palette.
    const Vec3 a{0.0f, 0.0f, 0.0f};
    const Vec3 b{1.0f, 0.0f, 0.0f};
    const Vec3 c{0.0f, 1.0f, 0.0f};
    CHECK(effectsSparkColourIndex(10, 100, a, b, c, Vec3{0.0f, 0.0f, 1.0f}) == 110);
    CHECK(effectsSparkColourIndex(10, 100, a, b, c, Vec3{1.0f, 0.0f, 0.0f}) == 10);
    CHECK(effectsSparkColourIndex(10, 100, a, a, a, Vec3{0.0f, 0.0f, 1.0f}) == 10);
    CHECK(effectsSparkColourIndex(200, 100, a, b, c, Vec3{0.0f, 0.0f, 1.0f}) == 255);
    CHECK(effectsSparkColourIndex(-5, 0, a, b, c, Vec3{0.0f, 0.0f, 1.0f}) == 0);
}

// The ribbon's draw half (EffectsView.AddRibbon over Ribbon.Triangles): nothing for
// one position, four segments of six corners for five, the oldest edges shrunk to a
// point, and the UVs in WMIBTEX's texels scaled to 0-1. The run-level corners are
// pinned against the oracle by tick1's L8_ribbon_tornado; this pins the vertex pass.
void testEffectsRibbonVertices() {
    scripts::Ribbon ribbon;
    Vertex out[scripts::Ribbon::kMaxCorners];
    ribbon.push(Vec3{0.0f, 0.0f, 10.0f});
    CHECK(effectsRibbonVertices(ribbon, 16, 48, out) == 0);
    for (int32_t i = 1; i < 6; i++) {
        ribbon.push(Vec3{(float)i, 0.0f, 10.0f});
    }
    CHECK(ribbon.count() == 5);
    CHECK(effectsRibbonVertices(ribbon, 16, 48, out) == 24);
    // The first segment (edges 0 and 1, the oldest kept being x 1, both scale 0): its
    // bottoms and tops are the centres.
    CHECK(out[0].pos.x == 1.0f && out[0].pos.z == 10.0f && out[1].pos.z == 10.0f);
    // The last segment (edges 3 and 4, both full): 2 high around the centre.
    CHECK(out[18].pos.x == 4.0f && out[18].pos.z == 9.0f);
    CHECK(out[19].pos.x == 4.0f && out[19].pos.z == 11.0f);
    CHECK(out[20].pos.x == 5.0f && out[20].pos.z == 11.0f);
    // UVs: columns 0.5 and 15.5 of 16, the first segment rows 0.5-16 of 48, the last 32-47.5.
    CHECK(out[0].uv.x == 0.5f * (1.0f / 16.0f) && out[0].uv.y == 0.5f * (1.0f / 48.0f));
    CHECK(out[2].uv.x == 15.5f * (1.0f / 16.0f) && out[2].uv.y == 16.0f * (1.0f / 48.0f));
    CHECK(out[20].uv.y == 47.5f * (1.0f / 48.0f) && out[22].uv.x == 0.5f * (1.0f / 16.0f));
    CHECK(out[0].colour == kVertexWhite);
}

// The message queue without an FTI (Messages.PushText): two lines split at the
// "\n" escape and cut at 35 bytes, four deep, a fifth at the back dropped, a front
// one pushing the last out; the first update takes the front message.
void testMessagesQueue() {
    Messages messages;
    const char* two = "AB\\nCDEFGHIJKLMNOPQRSTUVWXYZ0123456789XYZ";
    CHECK(messages.pushText((const uint8_t*)two, (int32_t)strlen(two), Messages::kFlagZoom, 2.0f));
    CHECK(messages.queued() == 1);
    CHECK(messages.queuedAt(0).lineCount == 2 && messages.queuedAt(0).length[0] == 2 &&
          messages.queuedAt(0).length[1] == 35);
    CHECK(!messages.pushText((const uint8_t*)"", 0, 0, 1.0f));
    CHECK(!messages.push("NO_SUCH", 0, 1.0f));
    const uint8_t b = 'B';
    const uint8_t c = 'C';
    const uint8_t d = 'D';
    const uint8_t e = 'E';
    const uint8_t f = 'F';
    messages.pushText(&b, 1, 0, 1.0f);
    messages.pushText(&c, 1, 0, 1.0f);
    messages.pushText(&d, 1, 0, 1.0f);
    messages.pushText(&e, 1, 0, 1.0f);   // the fifth at the back: dropped
    CHECK(messages.queued() == 4 && messages.queuedAt(3).text[0][0] == 'D');
    messages.pushText(&f, 1, Messages::kFlagFront, 1.0f);   // first; D goes
    CHECK(messages.queued() == 4 && messages.queuedAt(0).text[0][0] == 'F' &&
          messages.queuedAt(3).text[0][0] == 'C');
    messages.update(1.0f / 60.0f);
    CHECK(messages.current() != nullptr && messages.current()->text[0][0] == 'F');
    CHECK(messages.queued() == 3 && messages.time() == 1.0f);
    messages.reset();
    CHECK(messages.current() == nullptr && messages.queued() == 0);
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
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);
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
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);
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
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);
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
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);
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
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);
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
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);
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
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);
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
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);
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

// Movement command 6 (move_to_target, ObjectMotion.cs:337 -> Chase 0x45e448 at
// ObjectMotion.cs:894): the object flies at the runtime's target 8 above it, at
// `Speed` UNITS PER TICK — not per second, and not through Push, so neither friction
// nor gravity's ApplyVelocity step scales it. Every number below comes from the C#
// constants (Behind 90, Far 30, Ahead 22.5, Gain 0.0444444/6, MaxChase 2.3333333,
// Brake 0.05, MinChase 0.3333333, Climb 0.25, TurnSpeed 180 deg/s = 6 deg per tick)
// applied by hand to the oracle's formula, not read off the port.
void testChaseFliesAtTheRuntimeTarget(LevelData* levelData, const Cmi* cmi, audio::SoundBank* bank) {
    Player player;
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, &player, nullptr);

    // The chase point is the runtime's target, which is Kurt's feet, 8 above them.
    player.feet = Vec3{0.0f, 40.0f, 20.0f};
    runtime.update(scripts::ScriptRuntime::kTick);
    CHECK(runtime.targetPosition().x == 0.0f);
    CHECK(runtime.targetPosition().y == 40.0f);
    CHECK(runtime.targetPosition().z == 20.0f);

    // Straight ahead: the gain branch. Speed 1 + (22.5 - 0) * (0.0444444/6), and the
    // step is that speed in units, so the feet move 1.1666665 along +Y. The climb is
    // a quarter of the step toward 28, and it does not overshoot.
    scripts::MdkObject ahead;
    ahead.position = Vec3{0.0f, 0.0f, 10.0f};
    ahead.yaw = 90.0f;
    ahead.speed = 1.0f;
    ahead.move_command = 6;
    runtime.motionVelocity(&ahead);
    CHECK(ahead.move_command == 6);   // the command survives: chase never ends itself
    CHECK(fabsf(ahead.speed - 1.1666665f) < 1e-6f);
    CHECK(fabsf(ahead.yaw - 90.0f) < 1e-5f);
    CHECK(fabsf(ahead.position.x) < 1e-6f);
    CHECK(fabsf(ahead.position.y - 1.1666665f) < 1e-6f);
    CHECK(fabsf(ahead.position.z - 10.2916670f) < 1e-6f);
    CHECK(ahead.push.x == 0.0f && ahead.push.y == 0.0f && ahead.push.z == 0.0f);

    // Thirty of those ticks put the speed on the MaxChase clamp exactly, which only a
    // gain branch that keeps running can reach.
    scripts::MdkObject fast;
    fast.position = Vec3{0.0f, 0.0f, 10.0f};
    fast.yaw = 90.0f;
    fast.speed = 1.0f;
    fast.move_command = 6;
    for (int i = 0; i < 30; i++) {
        runtime.motionVelocity(&fast);
    }
    CHECK(fast.speed == 2.3333333f);
    // And a hundred close the Z exactly: MoveToward lands on the point, it does not
    // circle it. (The feet have flown past 40 and turned back by then; only the climb
    // is exact, which is the point of the assertion.)
    for (int i = 0; i < 70; i++) {
        runtime.motionVelocity(&fast);
    }
    CHECK(fast.position.z == 28.0f);

    // Behind and near (exactly 90 degrees off, 10 units away): the oracle turns and
    // brakes only when the point is ahead of it or far away, so at offAngle == 90 and
    // |dy| == 10 neither test passes. The object keeps its own heading and its own
    // speed and simply steps along them.
    player.feet = Vec3{0.0f, -10.0f, -8.0f};   // the chase point is (0,-10,0)
    runtime.update(scripts::ScriptRuntime::kTick);
    scripts::MdkObject behind;
    behind.position = Vec3{0.0f, 0.0f, 0.0f};
    behind.yaw = 0.0f;
    behind.speed = 2.0f;
    behind.move_command = 6;
    runtime.motionVelocity(&behind);
    CHECK(behind.yaw == 0.0f);
    CHECK(behind.speed == 2.0f);
    CHECK(fabsf(behind.position.x - 2.0f) < 1e-6f);
    CHECK(behind.position.y == 0.0f);
    CHECK(behind.position.z == 0.0f);

    // The same 90 degrees, but 40 units to the side: past Far, so it turns — 6
    // degrees, and the brake branch (offAngle > Ahead) takes 0.05 off the speed.
    player.feet = Vec3{0.0f, -40.0f, -8.0f};   // the chase point is (0,-40,0)
    runtime.update(scripts::ScriptRuntime::kTick);
    scripts::MdkObject aside;
    aside.position = Vec3{0.0f, 0.0f, 0.0f};
    aside.yaw = 0.0f;
    aside.speed = 2.0f;
    aside.move_command = 6;
    runtime.motionVelocity(&aside);
    CHECK(fabsf(aside.yaw - 354.0f) < 1e-5f);
    CHECK(fabsf(aside.speed - 1.95f) < 1e-6f);
    CHECK(fabsf(aside.position.x - 1.9393177f) < 1e-6f);
    CHECK(fabsf(aside.position.y + 0.2038309f) < 1e-6f);

    // Directly behind and far: the wrap puts the tie at -180, so the turn goes the
    // short way DOWN through 354, 348, ... and the brake runs every tick until the
    // MinChase floor catches it on the fourth.
    player.feet = Vec3{-40.0f, 0.0f, -8.0f};   // the chase point is (-40,0,0)
    runtime.update(scripts::ScriptRuntime::kTick);
    scripts::MdkObject astern;
    astern.position = Vec3{0.0f, 0.0f, 0.0f};
    astern.yaw = 0.0f;
    astern.speed = 0.5f;
    astern.move_command = 6;
    for (int i = 0; i < 4; i++) {
        runtime.motionVelocity(&astern);
    }
    CHECK(fabsf(astern.yaw - 336.0f) < 1e-4f);
    CHECK(astern.speed == 0.3333333f);
    runtime.motionVelocity(&astern);
    CHECK(astern.speed == 0.3333333f);   // the floor holds

    // The point directly overhead is not atan2(0, 0)'s answer: the C# keeps the
    // object's own heading when the point shares its X and Y, so this is still the
    // straight-ahead gain branch (speed 1.1666665, heading 90) and not a 90-degree
    // swing to yaw 0 with the brake instead of the gain.
    player.feet = Vec3{0.0f, 0.0f, 20.0f};   // the chase point is (0,0,28)
    runtime.update(scripts::ScriptRuntime::kTick);
    scripts::MdkObject over;
    over.position = Vec3{0.0f, 0.0f, 10.0f};
    over.yaw = 90.0f;
    over.speed = 1.0f;
    over.move_command = 6;
    runtime.motionVelocity(&over);
    CHECK(fabsf(over.speed - 1.1666665f) < 1e-6f);
    CHECK(fabsf(over.yaw - 90.0f) < 1e-6f);
    CHECK(fabsf(over.position.y - 1.1666665f) < 1e-6f);
    CHECK(fabsf(over.position.z - 10.2916670f) < 1e-6f);
}

// A follower in the slot (0,-10,0) behind a leader at the origin facing +X.
void startFollower(scripts::MdkObject* obj, scripts::MdkObject* leader, const Vec3& position,
                   float yaw) {
    obj->move_command = 1;
    obj->leader = leader;
    obj->waypoint = Vec3{0.0f, -10.0f, 0.0f};
    obj->position = position;
    obj->yaw = yaw;
}

// Movement command 1 (command_objects' formation, FollowFormation at
// ObjectMotion.cs:929): the slot is the leader's FRAME, so the slot for waypoint
// (0,-10,0) behind a leader facing +X is (10,0,0) — FormationPosition's off-diagonal
// signs, not a plain rotation. Inside one step (MaxSpeed * Dt) the follower is
// PLACED on the slot; outside it the follower pushes the unscaled heading * MaxSpeed,
// which ApplyVelocity turns into one frame of MaxSpeed. Numbers from the C#.
void testFormationFollowersKeepTheirSlot(scripts::ScriptRuntime& runtime) {
    // No leader: let go of the command, and do not move first.
    scripts::MdkObject alone;
    alone.move_command = 1;
    alone.position = Vec3{0.0f, -40.0f, 0.0f};
    runtime.motionVelocity(&alone);
    CHECK(alone.move_command == 0);
    CHECK(alone.position.y == -40.0f);

    scripts::MdkObject leader;
    leader.position = Vec3{0.0f, 0.0f, 0.0f};
    leader.yaw = 0.0f;

    // A dead leader is no leader.
    leader.dead = true;
    scripts::MdkObject orphan;
    startFollower(&orphan, &leader, Vec3{0.0f, -40.0f, 0.0f}, 90.0f);
    runtime.motionVelocity(&orphan);
    CHECK(orphan.move_command == 0);
    CHECK(orphan.position.y == -40.0f);
    leader.dead = false;

    // Far from the slot: 90 degrees off, so it turns the full 6 degrees per tick
    // (90 -> 84) and pushes along the NEW heading: (cos 84, sin 84) * 50, one frame
    // of that = (0.17421404, 1.6575366). The slot's Z equals its own, so the vertical
    // takes the third arm and pushes the exact remaining gap: zero.
    scripts::MdkObject distant;
    startFollower(&distant, &leader, Vec3{0.0f, -40.0f, 0.0f}, 90.0f);
    runtime.motionVelocity(&distant);
    CHECK(distant.move_command == 1);
    CHECK(distant.move_destination.x == 10.0f && distant.move_destination.y == 0.0f &&
          distant.move_destination.z == 0.0f);
    CHECK(fabsf(distant.yaw - 84.0f) < 1e-5f);
    CHECK(fabsf(distant.position.x - 0.17421404f) < 1e-6f);
    CHECK(fabsf(distant.position.y + 38.3424644f) < 1e-6f);
    CHECK(distant.position.z == 0.0f);
    CHECK(distant.push.x == 0.0f && distant.push.y == 0.0f && distant.push.z == 0.0f);

    // A whole unit below the slot climbs a unit per tick; a whole unit above it sinks
    // one; half a unit away closes the exact gap. All three arms are the C#'s, and the
    // first two are ±1 per TICK, not per second.
    scripts::MdkObject below;
    startFollower(&below, &leader, Vec3{0.0f, -40.0f, -5.0f}, 90.0f);
    runtime.motionVelocity(&below);
    CHECK(fabsf(below.position.z + 4.9666667f) < 1e-6f);
    scripts::MdkObject above;
    startFollower(&above, &leader, Vec3{0.0f, -40.0f, 5.0f}, 90.0f);
    runtime.motionVelocity(&above);
    CHECK(fabsf(above.position.z - 4.9666667f) < 1e-6f);
    scripts::MdkObject halfup;
    startFollower(&halfup, &leader, Vec3{0.0f, -40.0f, 0.5f}, 90.0f);
    runtime.motionVelocity(&halfup);
    CHECK(fabsf(halfup.position.z - 0.4833333f) < 1e-6f);

    // Inside one step of the slot it is placed exactly on it, and its yaw moves
    // toward the leader's by at most 6 degrees: 4 away reaches 0 exactly, 20 away
    // comes to 14. Nothing moves in the same tick as the placement.
    scripts::MdkObject placed;
    startFollower(&placed, &leader, Vec3{10.0f, 1.0f, 0.0f}, 4.0f);
    runtime.motionVelocity(&placed);
    CHECK(placed.position.x == 10.0f && placed.position.y == 0.0f && placed.position.z == 0.0f);
    CHECK(placed.yaw == 0.0f);
    CHECK(placed.move_command == 1);
    scripts::MdkObject turning;
    startFollower(&turning, &leader, Vec3{10.0f, 1.0f, 0.0f}, 20.0f);
    runtime.motionVelocity(&turning);
    CHECK(turning.position.y == 0.0f);
    CHECK(fabsf(turning.yaw - 14.0f) < 1e-5f);
}

// Opcode 6 (move_to_target) through the real bytecode dispatch, then the motion
// step: the batch that ported `chase` set move_command from two dispatches, and
// until this test neither end had ever been joined by a test. The numbers are the
// straight-ahead gain case of ObjectMotion.cs:894, computed the same way as
// ChaseFliesAtTheRuntimeTarget's (speed 1 + (22.5 - 0) * (0.0444444/6) = 1.1666665,
// the step is that many units, the climb a quarter of it toward the point's Z).
void testMoveToTargetOpcodeRunsChase(LevelData* levelData, const Cmi* cmi,
                                      audio::SoundBank* bank) {
    Player player;
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, &player, nullptr);
    player.feet = Vec3{0.0f, 40.0f, 20.0f};   // the chase point is (0,40,28)
    runtime.update(scripts::ScriptRuntime::kTick);
    CHECK(runtime.targetPosition().x == 0.0f);
    CHECK(runtime.targetPosition().y == 40.0f);
    CHECK(runtime.targetPosition().z == 20.0f);

    Code code;
    code.u8(kMoveToTarget);   // opcode 6 takes no operands
    code.end();

    // The command starts cleared, so the only thing that can set it is the dispatch.
    scripts::MdkObject obj;
    obj.position = Vec3{0.0f, 0.0f, 10.0f};
    obj.yaw = 90.0f;
    obj.speed = 1.0f;
    obj.move_command = 0;
    runProgram(runtime, code, &obj);
    CHECK(obj.move_command == 6);

    // And the motion step reads it: the same tick ChaseFliesAtTheRuntimeTarget
    // measures, from the same starting pose.
    runtime.motionVelocity(&obj);
    CHECK(obj.move_command == 6);   // chase never ends itself
    CHECK(fabsf(obj.speed - 1.1666665f) < 1e-6f);
    CHECK(fabsf(obj.position.x) < 1e-6f);
    CHECK(fabsf(obj.position.y - 1.1666665f) < 1e-6f);
    CHECK(fabsf(obj.position.z - 10.2916670f) < 1e-6f);
    CHECK(obj.push.x == 0.0f && obj.push.y == 0.0f && obj.push.z == 0.0f);
}

// Opcode 4 (command_objects) with command 1, through the real bytecode dispatch:
// the selector walk, the slot, and the motion arm in one line. The C# installs
// Waypoint (ScriptRuntime.cs:3071) and FollowFormation (ObjectMotion.cs:929) is the
// only reader of it, so a dispatch that dropped the offset would put every follower
// on the leader's own slot and no hand-built follower could notice.
//
// The slots come from the C#: formationIndex 0 and 1 of a non-XE pair are
// side * (index/2 + 1) * 5 * 1 with side (-1, +1), y = 1 * -4, z = 8, so
// (-5,-4,8) and (5,-4,8); FormationPosition with the leader at the origin facing
// +X (s=0, c=1) maps (ox,oy,oz) to (ox*0 - oy*1, -oy*0 - ox*1, oz+8) = (4, 5, 8)
// and (4, -5, 8).
void testCommandObjectsFormationArmInstallsTheSlot(LevelData* levelData, const Cmi* cmi,
                                                   audio::SoundBank* bank) {
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);
    scripts::MdkObject& sender = runtime.getArenaState("DANT_1").controller;
    sender.position = Vec3{0.0f, 0.0f, 0.0f};
    sender.yaw = 0.0f;
    scripts::MdkObject* left =
        runtime.spawn(&sender, "X_STRIKB", Vec3{0.0f, -40.0f, 0.0f}, 0.0f, -1, 0, false);
    scripts::MdkObject* right =
        runtime.spawn(&sender, "X_STRIKB", Vec3{0.0f, -60.0f, 0.0f}, 0.0f, -1, 0, false);
    CHECK(left != nullptr && right != nullptr);
    if (left == nullptr || right == nullptr) {
        return;
    }

    // Clear whatever the type's init script left, so every value asserted below can
    // only have come from the dispatch, and pin the motion inputs the C# reads.
    scripts::MdkObject* followers[] = {left, right};
    for (size_t i = 0; i < 2; i++) {
        scripts::MdkObject* obj = followers[i];
        obj->move_command = 0;
        obj->leader = nullptr;
        obj->waypoint = Vec3{0.0f, 0.0f, 0.0f};
        obj->move_destination = Vec3{0.0f, 0.0f, 0.0f};
        obj->path = 0;
        obj->flags = 0;
        obj->speed = 0.0f;
        obj->push = Vec3{0.0f, 0.0f, 0.0f};
        obj->velocity = Vec3{0.0f, 0.0f, 0.0f};
        obj->friction = 0.0f;
        obj->max_speed = 50.0f;
        obj->yaw = 0.0f;
    }

    // command 1, selector 2 (all of a type), the type as its only selector arg.
    Code code;
    code.u8(kCommandObjects).u8(kCommandFormation).u8(kSelectorAllOfType).pstr("X_STRIKB");
    code.end();
    runProgram(runtime, code, &sender);

    CHECK(left->move_command == 1);
    CHECK(right->move_command == 1);
    CHECK(left->leader == &sender);
    CHECK(right->leader == &sender);
    CHECK(left->path == 0);
    CHECK(left->waypoint.x == -5.0f && left->waypoint.y == -4.0f && left->waypoint.z == 8.0f);
    CHECK(right->waypoint.x == 5.0f && right->waypoint.y == -4.0f && right->waypoint.z == 8.0f);
    CHECK(left->move_destination.x == 4.0f && left->move_destination.y == 5.0f &&
          left->move_destination.z == 8.0f);
    CHECK(right->move_destination.x == 4.0f && right->move_destination.y == -5.0f &&
          right->move_destination.z == 8.0f);

    // The arm: the left follower is 45 units from its slot, so it is outside one
    // step (MaxSpeed * Dt = 1.66667), turns the full 6 degrees toward it
    // (atan2(45, 4) = 84.9204 degrees away, past the 6-degree cap), and pushes the
    // unscaled heading * MaxSpeed, which ApplyVelocity scales by Dt: (cos 6, sin 6) *
    // 50 / 30 = (1.6575365, 0.1742141). Its Z is a whole unit below the slot's, so the
    // vertical push is +Ticks and the climb is 1/30.
    runtime.motionVelocity(left);
    CHECK(left->move_command == 1);
    CHECK(left->move_destination.x == 4.0f && left->move_destination.y == 5.0f &&
          left->move_destination.z == 8.0f);
    CHECK(fabsf(left->yaw - 6.0f) < 1e-5f);
    CHECK(fabsf(left->position.x - 1.6575365f) < 1e-6f);
    CHECK(fabsf(left->position.y + 39.8257859f) < 1e-6f);
    CHECK(fabsf(left->position.z - 0.0333333f) < 1e-6f);

    // The right follower moves toward ITS OWN slot, not the left one's: the sign of
    // the turn is the assertion. Its heading is atan2(55, 4) = 85.8404 degrees, a
    // different angle from the left follower's 84.9204, but past the same 6-degree
    // cap, so it too turns exactly +6 and pushes the same vector.
    runtime.motionVelocity(right);
    CHECK(fabsf(right->yaw - 6.0f) < 1e-5f);
    CHECK(fabsf(right->position.x - 1.6575365f) < 1e-6f);
    CHECK(fabsf(right->position.y + 59.8257859f) < 1e-6f);
}

// The oracle's speed clamps are MathF.Min/MathF.Max (ObjectMotion.cs:784,788,916,917)
// and both propagate NaN, where fminf/fmaxf return the NUMERIC side and would turn a
// NaN speed into the clamp bound. Nothing in the shipped data can put NaN in Speed:
// opcodes 50-53 copy a script literal into max_speed/acceleration/deceleration/speed,
// and the VM's only variable arithmetic is += a literal and += a literal x Dt, so a
// NaN would have to be a NaN literal in the bytecode. This pins the semantics at the
// site, not a reachable scenario.
void testChaseSpeedClampPropagatesNaNLikeTheOracle(LevelData* levelData, const Cmi* cmi,
                                                   audio::SoundBank* bank) {
    const uint32_t kNaNBits = 0x7fc00000u;
    float kNaN = 0.0f;
    memcpy(&kNaN, &kNaNBits, sizeof(kNaN));

    Player player;
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, &player, nullptr);
    player.feet = Vec3{0.0f, 40.0f, 20.0f};   // straight ahead of a yaw-90 object
    runtime.update(scripts::ScriptRuntime::kTick);

    // The gain branch: MathF.Min(NaN + gain, MaxChase) is NaN, not MaxChase.
    scripts::MdkObject gain;
    gain.position = Vec3{0.0f, 0.0f, 10.0f};
    gain.yaw = 90.0f;
    gain.speed = kNaN;
    gain.move_command = 6;
    runtime.motionVelocity(&gain);
    CHECK(gain.speed != gain.speed);
    CHECK(gain.position.y != gain.position.y);   // the step is NaN units too

    // The brake branch: MathF.Max(NaN - brake, MinChase) is NaN, not MinChase. The
    // point 45 degrees off passes the turn test (Behind is 90) and fails Ahead.
    player.feet = Vec3{10.0f, 10.0f, -8.0f};   // the chase point is (10,10,0)
    runtime.update(scripts::ScriptRuntime::kTick);
    scripts::MdkObject brake;
    brake.position = Vec3{0.0f, 0.0f, 0.0f};
    brake.yaw = 0.0f;
    brake.speed = kNaN;
    brake.move_command = 6;
    runtime.motionVelocity(&brake);
    CHECK(brake.speed != brake.speed);
}

// A quiet NaN float32 — the value a script literal would carry into a movement field.
static float nanFloat() {
    const uint32_t bits = 0x7fc00000u;
    float value = 0.0f;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

// The other six speed-clamp call sites are the oracle's Accelerate (ObjectMotion.cs:
// 780-790), which MoveForward (command 88) and Walk and Fly (commands 43/78/197) each
// run twice, once per branch. A NaN Speed does NOT discriminate them, and the reason is
// worth writing down: Accelerate's branches are `Speed > target` and `Speed < target`,
// both false for NaN, so a NaN speed walks out of it untouched whichever clamp is in the
// body. What does reach a clamp is a NaN Acceleration or Deceleration — opcodes 51/52
// copy a script literal into them exactly as 53 does into Speed — and there
// MathF.Min/MathF.Max propagate the NaN where fminf/fmaxf return the finite target and
// silently un-NaN the speed. Both sides were run, not assumed: .NET 10 gives
// MathF.Min(NaN,10) = MathF.Max(NaN,10) = NaN, glibc gives fminf(NaN,10) =
// fmaxf(NaN,10) = 10. Same caveat as the chase pin above: this pins the semantics at the
// site, not a scenario the shipped data reaches.
void testMoveForwardClampPropagatesNaNLikeTheOracle(LevelData* levelData, const Cmi* cmi,
                                                    audio::SoundBank* bank) {
    const float kNaN = nanFloat();
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);

    // Gain: MathF.Min(5 + NaN × Dt, 10) is NaN, not the 10 the target is. Gravity keeps
    // the push horizontal (the other branch adds a pitch trig), and a non-zero parameter
    // means "accelerate to MaxSpeed" rather than "brake to a stop".
    scripts::MdkObject gain;
    gain.flags = scripts::MdkObject::FlagGravity;
    gain.move_command = 88;
    gain.move_parameter = 1;
    gain.max_speed = 10.0f;
    gain.speed = 5.0f;
    gain.acceleration = kNaN;
    gain.deceleration = 1.0f;
    runtime.motionVelocity(&gain);
    CHECK(gain.speed != gain.speed);
    CHECK(gain.position.x != gain.position.x);   // the NaN reaches the tick's motion too
    CHECK(gain.move_command == 88);             // speed == target is false for NaN, so
                                                // the stop never claims the command

    // Brake: MathF.Max(15 - NaN × Dt, 10) is NaN, not 10.
    scripts::MdkObject brake;
    brake.flags = scripts::MdkObject::FlagGravity;
    brake.move_command = 88;
    brake.move_parameter = 1;
    brake.max_speed = 10.0f;
    brake.speed = 15.0f;
    brake.acceleration = 1.0f;
    brake.deceleration = kNaN;
    runtime.motionVelocity(&brake);
    CHECK(brake.speed != brake.speed);
    CHECK(brake.position.x != brake.position.x);
    CHECK(brake.move_command == 88);
}

void testWalkClampPropagatesNaNLikeTheOracle(LevelData* levelData, const Cmi* cmi,
                                             audio::SoundBank* bank) {
    const float kNaN = nanFloat();
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);

    // Walk's target is MaxSpeed × TurnSpeedFactor(diff). The waypoint is straight ahead
    // of the yaw and 100 units away, so diff is 0, the factor is 1 and the target is
    // MaxSpeed; and it is the final leg (Waypoint == MoveDestination) 100 away, so Walk
    // does not Stop() — arrival is Manhattan < 5 — and the command survives to assert.
    scripts::MdkObject gain;
    gain.flags = scripts::MdkObject::FlagGravity;   // FlagGravity is what makes this arm walk
    gain.move_command = 43;
    gain.waypoint = Vec3{100.0f, 0.0f, 0.0f};
    gain.move_destination = Vec3{100.0f, 0.0f, 0.0f};
    gain.max_speed = 10.0f;
    gain.speed = 5.0f;
    gain.acceleration = kNaN;
    gain.deceleration = 1.0f;
    runtime.motionVelocity(&gain);
    CHECK(gain.speed != gain.speed);
    CHECK(gain.position.x != gain.position.x);
    CHECK(gain.move_command == 43);

    // Brake: same pose, Speed above the target, NaN Deceleration.
    scripts::MdkObject brake;
    brake.flags = scripts::MdkObject::FlagGravity;
    brake.move_command = 43;
    brake.waypoint = Vec3{100.0f, 0.0f, 0.0f};
    brake.move_destination = Vec3{100.0f, 0.0f, 0.0f};
    brake.max_speed = 10.0f;
    brake.speed = 15.0f;
    brake.acceleration = 1.0f;
    brake.deceleration = kNaN;
    runtime.motionVelocity(&brake);
    CHECK(brake.speed != brake.speed);
    CHECK(brake.position.x != brake.position.x);
    CHECK(brake.move_command == 43);
}

void testFlyClampPropagatesNaNLikeTheOracle(LevelData* levelData, const Cmi* cmi,
                                            audio::SoundBank* bank) {
    const float kNaN = nanFloat();
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);

    // Fly (no gravity flag) runs the same Accelerate with the target taken from the
    // pre-turn diff. The waypoint is straight ahead, 100 across and 5 up: past TurnFrom
    // (3) so the turn runs, diff 0 so the factor is 1, and nowhere near the arrival test
    // (horizontal < 4), so the command is not stopped.
    scripts::MdkObject gain;
    gain.move_command = 78;
    gain.waypoint = Vec3{100.0f, 0.0f, 5.0f};
    gain.move_destination = Vec3{100.0f, 0.0f, 5.0f};
    gain.max_speed = 10.0f;
    gain.speed = 5.0f;
    gain.acceleration = kNaN;
    gain.deceleration = 1.0f;
    runtime.motionVelocity(&gain);
    CHECK(gain.speed != gain.speed);
    CHECK(gain.position.x != gain.position.x);
    CHECK(gain.position.z != gain.position.z);   // the climb carries it too
    CHECK(gain.move_command == 78);

    scripts::MdkObject brake;
    brake.move_command = 78;
    brake.waypoint = Vec3{100.0f, 0.0f, 5.0f};
    brake.move_destination = Vec3{100.0f, 0.0f, 5.0f};
    brake.max_speed = 10.0f;
    brake.speed = 15.0f;
    brake.acceleration = 1.0f;
    brake.deceleration = kNaN;
    runtime.motionVelocity(&brake);
    CHECK(brake.speed != brake.speed);
    CHECK(brake.position.x != brake.position.x);
    CHECK(brake.move_command == 78);
}

// The oracle has ONE speed step, ObjectMotion.Accelerate (ObjectMotion.cs:780-790), and it
// has exactly three callers: MoveForward (:979), Walk (:819) and Fly (:840). The port had
// inlined it at all three, and review-21 F7 is the hazard that shape creates: a fidelity fix
// applied to one copy and not the others, which no gate sees (batch 49 changed 2 of the 8
// clamp sites that way and review-20 caught it). script_runtime.cpp now has one accelerate()
// and all three arms call it, so this pins the property the collapse is meant to hold:
//
//   * every arm lands on the SAME speed from the SAME state, so an arm that drifts away from
//     the shared helper fails here even when no other test looks at that arm; and
//   * that speed is the oracle's bit for bit — the expected bits below are what .NET 10
//     prints from the private static ObjectMotion.Accelerate on these same inputs (probe
//     mode "accel"), not what the C++ produces.
//
// Each arm is posed so its own target IS the row's target: MoveForward takes MaxSpeed when
// MoveParameter is non-zero, and Walk/Fly take MaxSpeed × TurnSpeedFactor(diff), which is
// MaxSpeed with the waypoint straight ahead and MaxSpeed × 0.5 with it 90 degrees off. So a
// `half` row hands those two arms twice the target as MaxSpeed and pins, along with the
// step, the factor the target came from. Chase is deliberately NOT in this table: its clamp
// pair is the C#'s own Chase formula (ObjectMotion.cs:916-917), a second oracle function and
// not a copy of this one, and it has its own pin.
struct AccelRow {
    const char* label;
    float speed;   // the object's Speed on entry
    float target;  // the number every arm must aim at, however it derives it
    float accel;
    float decel;
    bool half;     // Walk/Fly reach `target` through TurnSpeedFactor's 0.5
    bool nan;      // the oracle's answer is NaN (NaN-ness pinned, not the payload)
    uint32_t bits; // the oracle's IEEE-754 bits when the answer is finite
};

static uint32_t floatBits(float value) {
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// arm 0 = MoveForward (command 88), arm 1 = Walk (43), arm 2 = Fly (78).
static void accelArmPose(scripts::MdkObject* obj, int arm, const AccelRow& row) {
    obj->speed = row.speed;
    obj->acceleration = row.accel;
    obj->deceleration = row.decel;
    obj->yaw = 0.0f;
    if (arm == 0) {
        obj->flags = scripts::MdkObject::FlagGravity;
        obj->move_command = 88;
        obj->move_parameter = 1;            // non-zero means "to MaxSpeed", not "to a stop"
        obj->max_speed = row.target;
    } else {
        obj->flags = arm == 1 ? scripts::MdkObject::FlagGravity : 0;  // gravity is what walks
        obj->move_command = arm == 1 ? 43 : 78;
        obj->max_speed = row.half ? row.target * 2.0f : row.target;
        // Straight ahead (factor 1) or 90 degrees off (factor 0.5), and far either way, so
        // neither arrival branch runs and the command survives to be asserted below.
        obj->waypoint = row.half ? Vec3{0.0f, 200.0f, 0.0f} : Vec3{100.0f, 0.0f, 0.0f};
        obj->move_destination = obj->waypoint;
    }
}

void testAccelerateIsOneImplementationAcrossTheArms(LevelData* levelData, const Cmi* cmi,
                                                    audio::SoundBank* bank) {
    const float kNaN = nanFloat();
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);
    const AccelRow rows[] = {
        //  label             speed  target  accel  decel  half   nan    oracle bits
        {"below",             5.0f,  10.0f,   1.0f,   1.0f, false, false, 0x40A11111u},
        {"above",            15.0f,  10.0f,   1.0f,   1.0f, false, false, 0x416F7777u},
        {"equal",            10.0f,  10.0f,   1.0f,   1.0f, false, false, 0x41200000u},
        {"overshoot_up",      9.9f,  10.0f, 100.0f,   1.0f, false, false, 0x41200000u},
        {"overshoot_down",   10.1f,  10.0f,   1.0f, 100.0f, false, false, 0x41200000u},
        {"zero_accel",        5.0f,  10.0f,   0.0f,   0.0f, false, false, 0x40A00000u},
        {"negative",         -2.0f,  10.0f,   1.0f,   1.0f, false, false, 0xBFFBBBBCu},
        {"stop_to_zero",      4.0f,   0.0f,   1.0f,   1.0f, false, false, 0x407DDDDEu},
        {"half_below",        5.0f,  10.0f,   1.0f,   1.0f, true,  false, 0x40A11111u},
        {"half_above",       15.0f,  10.0f,   1.0f,   1.0f, true,  false, 0x416F7777u},
        {"nan_accel_below",   5.0f,  10.0f,   kNaN,   1.0f, false, true,  0u},
        {"nan_decel_above",  15.0f,  10.0f,   1.0f,   kNaN, false, true,  0u},
        {"nan_speed",         kNaN,  10.0f,   1.0f,   1.0f, false, true,  0u},
        // The last two are the branch selector: the comparison picks the branch, not the
        // clamp, so a NaN in the field the OTHER branch reads must not touch the speed.
        {"nan_accel_above",  15.0f,  10.0f,   kNaN,   1.0f, false, false, 0x416F7777u},
        {"nan_decel_below",   5.0f,  10.0f,   1.0f,   kNaN, false, false, 0x40A11111u},
        // And these two pin the STRICTness of both comparisons: at Speed == target neither
        // branch may run, so a NaN in either field has to stay out of the speed. A `>=` or a
        // `<=` would take a branch here and come back NaN where the oracle returns 10.
        {"equal_nan_decel",  10.0f,  10.0f,   1.0f,   kNaN, false, false, 0x41200000u},
        {"equal_nan_accel",  10.0f,  10.0f,   kNaN,   1.0f, false, false, 0x41200000u},
    };
    const int kArms = 3;
    const int kCommands[kArms] = {88, 43, 78};
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        const AccelRow& row = rows[i];
        const int failuresBefore = failures;
        float landed[kArms] = {0.0f, 0.0f, 0.0f};
        for (int arm = 0; arm < kArms; ++arm) {
            scripts::MdkObject obj;
            accelArmPose(&obj, arm, row);
            runtime.motionVelocity(&obj);
            // The command survived, so the arm ran to its end rather than arriving and
            // stopping — which would leave the speed untouched and look like a pass.
            CHECK(obj.move_command == kCommands[arm]);
            if (row.nan) {
                CHECK(obj.speed != obj.speed);
            } else {
                CHECK(floatBits(obj.speed) == row.bits);
            }
            landed[arm] = obj.speed;
        }
        // The property itself, stated once: one step, three callers.
        if (!row.nan) {
            CHECK(floatBits(landed[0]) == floatBits(landed[1]));
            CHECK(floatBits(landed[1]) == floatBits(landed[2]));
        }
        if (failures != failuresBefore) {
            printf("  row %d (%s)\n", static_cast<int>(i), row.label);
        }
    }
}

// Review-20 F4 read the oracle as running DetectStuck/HandleStuck unconditionally after
// Walk/Fly, and cited ObjectMotion.cs:354-357 — the lines that hold the guard it means.
// The oracle HAS the guard: `if (obj.MoveCommand != 0) { DetectStuck(...); HandleStuck(
// ...); }`. So on the tick a command arrives, Stop() zeroes MoveCommand and Speed and
// clears ContactStuck WITHOUT touching StuckCount (792-797), and BOTH calls are skipped,
// so a stuck_count left by an earlier replan SURVIVES the arrival instead of being reset.
// opcode 231 (if_move_idle_flag, ScriptVm.cs:1243) reads exactly that pair — MoveCommand
// == 0 && StuckCount != 0 — so the direction is observable to a script, not just in a
// field. Review-21 F2: the walk case alone does not pin the GUARD, because Walk returns
// from its own body (811-813) before the switch reaches it; the guard is what skips the
// calls on a FLY arrival (Fly returns to the switch at :860). Both arrivals are pinned
// here, plus the other direction — a fly tick that does NOT arrive must still run
// DetectStuck — so neither deleting the guard nor inverting it survives.
void testArrivalKeepsTheStuckCountTheOracleKeeps(LevelData* levelData, const Cmi* cmi,
                                                 audio::SoundBank* bank) {
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);
    scripts::MdkObject obj;
    obj.flags = scripts::MdkObject::FlagGravity;   // FlagGravity is what makes this arm walk
    obj.move_command = 43;
    obj.waypoint = Vec3{2.0f, 0.0f, 0.0f};         // Manhattan 2, inside Arrived (5) ...
    obj.move_destination = Vec3{2.0f, 0.0f, 0.0f}; // ... and it IS the final leg
    obj.speed = 4.0f;
    obj.stuck_count = 2;                           // what a replan left behind
    runtime.motionVelocity(&obj);
    CHECK(obj.move_command == 0);                  // Walk's Stop()
    CHECK(obj.speed == 0.0f);
    CHECK(obj.stuck_count == 2);                   // DetectStuck never ran, so no reset

    // And the same tick the VM would read: 231 branches true here, in both trees.
    CHECK(obj.move_command == 0 && obj.stuck_count != 0);

    // The FLY arrival, which is the one the guard decides. No gravity flag -> Fly; the
    // waypoint is Manhattan 2 across and 0 up, so the turn is skipped (TurnFrom 3), the
    // arrival test passes (horizontal < 4, vertical < 3), it is the final leg, and Fly
    // Stops and returns to the SWITCH (no early return from the arm). Without the guard,
    // DetectStuck runs with Speed already 0, takes its !collided || speed <= 2 branch,
    // ResetStuck zeroes the count, and 231 would branch the other way.
    scripts::MdkObject fly;
    fly.move_command = 78;
    fly.waypoint = Vec3{2.0f, 0.0f, 0.0f};
    fly.move_destination = Vec3{2.0f, 0.0f, 0.0f};
    fly.max_speed = 4.0f;
    fly.speed = 4.0f;                              // == target, so Accelerate is a no-op
    fly.stuck_count = 2;                           // what a replan left behind
    runtime.motionVelocity(&fly);
    CHECK(fly.move_command == 0);                  // Fly's Stop()
    CHECK(fly.speed == 0.0f);
    CHECK(fly.stuck_count == 2);                   // the guard skipped DetectStuck's reset
    CHECK(fly.move_command == 0 && fly.stuck_count != 0);

    // And the other direction, so the guard cannot be inverted or moved into the arrival
    // branch: a fly tick that does NOT arrive keeps its command, so DetectStuck MUST run —
    // collided, faster than StuckSpeed (2) and inside the 16-update window, it counts the
    // update (stuck_ticks += 1, ObjectMotion.Ticks = 1f) and leaves the count alone.
    scripts::MdkObject moving;
    moving.move_command = 78;
    moving.waypoint = Vec3{100.0f, 0.0f, 0.0f};      // far: no arrival, the command survives
    moving.move_destination = Vec3{100.0f, 0.0f, 0.0f};
    moving.max_speed = 5.0f;
    moving.speed = 5.0f;
    moving.contact_flags = scripts::MdkObject::ContactCollided;
    moving.stuck_ticks = 0.0f;
    moving.stuck_count = 0;
    runtime.motionVelocity(&moving);
    CHECK(moving.move_command == 78);
    CHECK(moving.stuck_ticks == 1.0f);               // DetectStuck ran
    CHECK(moving.stuck_count == 0);                  // HandleStuck: no ContactStuck yet
}

// The last four MathF.Min/MathF.Max sites in script_runtime.cpp, which the port used to
// spell fminf/fmaxf. Every expected number below is the ORACLE's, measured on .NET 10 by
// the headless probe (mode `nanclamp`, transcript in docs/PROGRESS_LOG.md), not by the
// port: ApplyVelocity's friction factor (ObjectMotion.cs:1066), SegmentEntry's slab
// (ScriptMath.cs:110-111), CanSeeKurt's cone floor (ScriptRuntime.cs:3244) and Sweep's
// half height (ObjectMotion.cs:1138). Same rule as the eight speed clamps (review-20 F2):
// fminf/fmaxf return the NUMERIC side when one operand is NaN, so they silently turn a NaN
// into a finite bound, which is a different game.
void testFrictionClampPropagatesNaNLikeTheOracle(LevelData* levelData, const Cmi* cmi,
                                                 audio::SoundBank* bank) {
    const float kNaN = nanFloat();
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);

    // The control row first, in the oracle's own bits: velocity (3,4,0) is length 5, a
    // friction of 60 spends 60 × 1/30 = 2 of it, the factor is 3/5, and the step is that
    // velocity × Dt. FRICTION finite vel=1.8000001,2.4,0 bits=3FE66667,4019999A,00000000
    // pos=0.060000006,0.080000006,0.
    scripts::MdkObject finite;
    finite.velocity = Vec3{3.0f, 4.0f, 0.0f};
    finite.friction = 60.0f;
    runtime.motionVelocity(&finite);
    CHECK(floatBits(finite.velocity.x) == 0x3FE66667u);
    CHECK(floatBits(finite.velocity.y) == 0x4019999Au);
    CHECK(floatBits(finite.velocity.z) == 0x00000000u);
    CHECK(floatBits(finite.position.x) == 0x3D75C291u);
    CHECK(floatBits(finite.position.y) == 0x3DA3D70Bu);

    // The row that decides it: MathF.Max(5 - NaN, 0)/5 is NaN, so the velocity is NaN and
    // the object moves by NaN. fmaxf would return the 0, park the object at exactly
    // (0,0,0) with a zeroed velocity, and no finite test would notice.
    // FRICTION nan vel=NaN,NaN,NaN pos=NaN,NaN,NaN.
    scripts::MdkObject nanObject;
    nanObject.velocity = Vec3{3.0f, 4.0f, 0.0f};
    nanObject.friction = kNaN;
    runtime.motionVelocity(&nanObject);
    CHECK(nanObject.velocity.x != nanObject.velocity.x);
    CHECK(nanObject.velocity.y != nanObject.velocity.y);
    CHECK(nanObject.velocity.z != nanObject.velocity.z);
    CHECK(nanObject.position.x != nanObject.position.x);
    CHECK(nanObject.position.y != nanObject.position.y);
}

// SegmentEntry's slab, inlined in flyProjectile. The discriminating input is a NaN BOX
// with a finite step: a NaN STEP does not discriminate, because fmaxf's enter of 0 still
// multiplies a NaN delta (0 x NaN is NaN) and both spellings end up NaN anyway. The
// oracle's row is SEGMENT nan_box hit=True point=NaN,NaN,NaN: the NaN enter survives the
// NaN > NaN test (false, so it is still a hit) and the entry point is from + d x NaN.
// fmaxf/fminf would return enter 0 and leave the shot at its own start.
void testSegmentEntrySlabPropagatesNaNLikeTheOracle(LevelData* levelData, const Cmi* cmi,
                                                    audio::SoundBank* bank) {
    const float kNaN = nanFloat();
    Player player;
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, &player, nullptr);
    player.feet = Vec3{0.0f, 4.0f, 9.0f};      // level 7's start, where the ray is free
    runtime.update(scripts::ScriptRuntime::kTick);
    player.feet = Vec3{kNaN, kNaN, kNaN};      // the box the slab is tested against
    runtime.update(scripts::ScriptRuntime::kTick);  // copies the feet into the runtime

    // One unit along the yaw, so the segment is finite and only the box is NaN.
    scripts::MdkObject shot;
    shot.arena = "DANT_1";
    shot.position = Vec3{0.0f, 4.0f, 9.0f};
    shot.yaw = 0.0f;
    shot.speed = 30.0f;                       // 30 x 1/30 = one unit
    shot.parameter = 1.0f;                    // outlives this tick
    shot.move_command = 61;
    runtime.motionVelocity(&shot);
    CHECK(shot.contact_flags & scripts::MdkObject::ContactTouchedKurt);
    CHECK(shot.position.x != shot.position.x);
    CHECK(shot.position.y != shot.position.y);
    CHECK(shot.position.z != shot.position.z);

    // And the finite control, so this is a NaN a normal shot does not produce: the same
    // shot against a box 26 units further along misses, and the shot lands exactly one
    // unit from where it started.
    Player sane;
    scripts::ScriptRuntime saneRuntime(levelData, cmi, 1, bank, &sane, nullptr);
    sane.feet = Vec3{26.0f, 4.0f, 9.0f};
    saneRuntime.update(scripts::ScriptRuntime::kTick);
    scripts::MdkObject miss;
    miss.arena = "DANT_1";
    miss.position = Vec3{0.0f, 4.0f, 9.0f};
    miss.yaw = 0.0f;
    miss.speed = 30.0f;
    miss.parameter = 1.0f;
    miss.move_command = 61;
    saneRuntime.motionVelocity(&miss);
    CHECK(!(miss.contact_flags & scripts::MdkObject::ContactTouchedKurt));
    CHECK(miss.position.x == 1.0f);
}

// CanSeeKurt's MathF.Max(cone, MinCone) cannot be reached with a NaN, and this pins WHY:
// the `cone < HalfTurn` guard in front of it is false for a NaN, so a NaN cone is the
// no-cone-test case. The oracle's rows (probe `nanclamp`, level 7), an object 10 along +X
// from Kurt facing +X, so Kurt is 180 degrees off its yaw and the ray between them is
// clear: CONE x10 c45=False cNaN=True c200=True. The NaN row is the 200 row, not the 45
// one. If anyone moves the clamp in front of that guard, this fails.
void testConeFloorIsSkippedForNaNBeforeTheClampLikeTheOracle(LevelData* levelData,
                                                             const Cmi* cmi,
                                                             audio::SoundBank* bank) {
    const float kNaN = nanFloat();
    Player player;
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, &player, nullptr);
    player.feet = Vec3{0.0f, 4.0f, 9.0f};
    runtime.update(scripts::ScriptRuntime::kTick);

    scripts::MdkObject alien;
    alien.arena = "DANT_1";
    alien.position = Vec3{10.0f, 4.0f, 9.0f};
    alien.yaw = 0.0f;
    CHECK(!runtime.canSeeKurt(&alien, 1000.0f, 45.0f));   // Kurt is behind it
    CHECK(runtime.canSeeKurt(&alien, 1000.0f, 200.0f));   // no cone test at all
    CHECK(runtime.canSeeKurt(&alien, 1000.0f, kNaN));     // the same answer, not the 45 one
}

// Sweep's MathF.Max(bounds.Max.Z x Scale + Lift, MinHeight) x 0.5 (ObjectMotion.cs:1138).
// FlagRolling is what makes a NaN reachable at all: lift() then reads height_offset, so the
// half-height goes NaN while the X/Y halves stay finite. The oracle's rows (probe
// `nanclamp`, level 7), an object at the level start swept 20 along +X with the default
// (no model) bounds:
//   SWEEP finite   pos=20,4,9     bits=41A00000,40800000,41100000
//   SWEEP nan_lift pos=20,4,NaN   bits=41A00000,40800000,7FC00000
// The Z does NOT cancel: the centre is NaN, so end.Z - a.Z is NaN even though the sweep
// met no face, and fmaxf's 0.25 would have kept the whole thing finite.
void testSweepHalfHeightPropagatesNaNLikeTheOracle(LevelData* levelData, const Cmi* cmi,
                                                   audio::SoundBank* bank) {
    const float kNaN = nanFloat();
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, nullptr, nullptr);

    scripts::MdkObject finite;
    finite.arena = "DANT_1";
    finite.position = Vec3{0.0f, 4.0f, 9.0f};
    finite.flags = scripts::MdkObject::FlagCollides | scripts::MdkObject::FlagRolling;
    finite.friction = 0.0f;
    finite.height_offset = 1.0f;
    finite.velocity = Vec3{600.0f, 0.0f, 0.0f};   // 20 units at 1/30
    runtime.motionVelocity(&finite);
    CHECK(finite.position.x > 19.9f && finite.position.x < 20.1f);
    CHECK(finite.position.z == 9.0f);             // finite: the sweep ran on a real box

    scripts::MdkObject nanObject;
    nanObject.arena = "DANT_1";
    nanObject.position = Vec3{0.0f, 4.0f, 9.0f};
    nanObject.flags = scripts::MdkObject::FlagCollides | scripts::MdkObject::FlagRolling;
    nanObject.friction = 0.0f;
    nanObject.height_offset = kNaN;
    nanObject.velocity = Vec3{600.0f, 0.0f, 0.0f};
    runtime.motionVelocity(&nanObject);
    CHECK(nanObject.position.z != nanObject.position.z);  // the oracle keeps the NaN
    CHECK(nanObject.position.x > 19.9f && nanObject.position.x < 20.1f);

    // The same object with a NaN height_offset but no FlagRolling: lift() is 0, the
    // half-height is the ordinary max, and nothing is NaN. Without FlagRolling there is no
    // NaN route to this clamp, which is why the pin above has to set the flag.
    scripts::MdkObject notRolling;
    notRolling.arena = "DANT_1";
    notRolling.position = Vec3{0.0f, 4.0f, 9.0f};
    notRolling.flags = scripts::MdkObject::FlagCollides;
    notRolling.friction = 0.0f;
    notRolling.height_offset = kNaN;
    notRolling.velocity = Vec3{600.0f, 0.0f, 0.0f};
    runtime.motionVelocity(&notRolling);
    CHECK(notRolling.position.z == 9.0f);
}

// The MathF.Min / MathF.Max twins themselves, pinned in the ORACLE'S OWN BITS.
//
// Every expected number below is a `MINMAX` row printed by the C# probe (mode
// `nanclamp`, level 7; the level-3 run prints the identical rows, because these are
// BCL functions and not game data), so this is the oracle's behaviour and not the
// port's. The rows exist because review-25 F1 measured that the tie arm the port had
// (`a < b ? a : b`) returns the WRONG ZERO for one of the two argument orders: the
// oracle sorts -0.0 BELOW +0.0, and a float compare cannot see a difference between
// them. fminf/fmaxf agreed with .NET on this; the ternary did not.
//
// It is data-independent, so it runs BEFORE the game-data check in main() and cannot
// skip on a machine with no files -- the same ordering the archive guards use.
struct MinMaxRow {
    const char* label;
    uint32_t a;
    uint32_t b;
    uint32_t min;   // the oracle's MathF.Min bits, or kNaNAnswer
    uint32_t max;   // the oracle's MathF.Max bits, or kNaNAnswer
};

// Sentinel, not a payload: the oracle's answer is a NaN, and which NaN it hands back
// is not part of the contract (probe rows MINMAX nan_one / one_nan keep the operand's
// own bits, but the pins assert NaN-ness rather than a payload).
static const uint32_t kNaNAnswer = 0xFFFFFFFFu;

static float floatFromBits(uint32_t bits) {
    float value = 0.0f;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

void testMathMinMaxMatchTheOracleOnNaNAndSignedZero() {
    const MinMaxRow rows[] = {
        //  label              a          b          MathF.Min  MathF.Max
        {"negzero_poszero", 0x80000000u, 0x00000000u, 0x80000000u, 0x00000000u},
        {"poszero_negzero", 0x00000000u, 0x80000000u, 0x80000000u, 0x00000000u},
        {"negzero_negzero", 0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u},
        {"poszero_poszero", 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u},
        {"negtie",          0xC0000000u, 0xC0000000u, 0xC0000000u, 0xC0000000u},
        {"postie",          0x40000000u, 0x40000000u, 0x40000000u, 0x40000000u},
        {"negone_poszero",  0xBF800000u, 0x00000000u, 0xBF800000u, 0x00000000u},
        {"poszero_negone",  0x00000000u, 0xBF800000u, 0xBF800000u, 0x00000000u},
        {"negzero_posone",  0x80000000u, 0x3F800000u, 0x80000000u, 0x3F800000u},
        {"poszero_posone",  0x00000000u, 0x3F800000u, 0x00000000u, 0x3F800000u},
        {"neginf_neginf",   0xFF800000u, 0xFF800000u, 0xFF800000u, 0xFF800000u},
        {"neginf_posinf",   0xFF800000u, 0x7F800000u, 0xFF800000u, 0x7F800000u},
        {"posinf_posinf",   0x7F800000u, 0x7F800000u, 0x7F800000u, 0x7F800000u},
        {"nan_one",         0x7FC00000u, 0x3F800000u, kNaNAnswer,  kNaNAnswer},
        {"one_nan",         0x3F800000u, 0x7FC00000u, kNaNAnswer,  kNaNAnswer},
        {"nan_nan",         0x7FC00000u, 0x7FC00000u, kNaNAnswer,  kNaNAnswer},
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        const MinMaxRow& row = rows[i];
        const float a = floatFromBits(row.a);
        const float b = floatFromBits(row.b);
        const float lo = mathMin(a, b);
        const float hi = mathMax(a, b);
        if (row.min == kNaNAnswer) {
            CHECK(lo != lo);
        } else {
            CHECK(floatBits(lo) == row.min);
        }
        if (row.max == kNaNAnswer) {
            CHECK(hi != hi);
        } else {
            CHECK(floatBits(hi) == row.max);
        }
    }

    // The sign test the tie arm depends on: -0.0 only, and not -0.0 confused with any
    // other negative, and not +0.0.
    CHECK(isNegativeZero(floatFromBits(0x80000000u)));
    CHECK(!isNegativeZero(0.0f));
    CHECK(!isNegativeZero(-2.0f));
    CHECK(!isNegativeZero(floatFromBits(0x7FC00000u)));

    // And the two arms review-25 F1 named, spelled out: the order that used to return
    // the wrong zero, in both directions, for both functions.
    CHECK(floatBits(mathMin(-0.0f, 0.0f)) == 0x80000000u);
    CHECK(floatBits(mathMin(0.0f, -0.0f)) == 0x80000000u);
    CHECK(floatBits(mathMax(-0.0f, 0.0f)) == 0x00000000u);
    CHECK(floatBits(mathMax(0.0f, -0.0f)) == 0x00000000u);
}

// ---------------------------------------------------------------------------
// The shatter's group box.
//
// Debris.cs:160-161 folds each facet through a NESTED `Vector3.Min`/`Vector3.Max`, and
// that box is the only thing that decides `Shot.Far` (:166), which scales every piece's
// speed and spin. The port spelled those components `fminf`/`fmaxf`, which are
// NaN-SILENT, so a NaN corner dropped out of the box, `Far` stayed finite, and every
// piece in the group got a different speed: the oracle's first piece of the nan_corner
// group moves at exactly the shatter's speed (1.5, bits 3FC00000, because `Far > 0` is
// false for a NaN and the falloff `f` is left at exactly 1.0) while the fminf port had
// 1.0810 (3F8A5E98). The expectations below are the ORACLE's own pieces: the C#
// `Debris` is constructible on its own (a public ctor taking the Random, a public
// `GroupFacets` delegate, a public `Pieces`), so the probe drove it with the same
// synthetic group, the same seed and the same Shatter arguments and printed the bits
// (probe mode `shatter`; the transcript is in docs/PROGRESS_LOG.md, "Iteration 84").
//
// The spin quaternion IS pinned here now, and it is the reason this batch exists:
// `degreesToRadians` was the single-rounding `degrees * (float)(pi/180)`, while .NET's
// `float.DegreesToRadians` is `degrees * (float)Math.PI / 180f` -- two roundings. The
// factor VALUE is the same float either way, so every earlier proof of the factor passed;
// the rounding was not the same, and 8 of the first 12 shatter angles came out 1 ULP low in
// magnitude. `w` hid it (a 1-ULP angle move is ~0.3 ULP of a near-1 cosine product) while
// x/y/z moved 1-2 ULP. See the comment at debris.cpp's degreesToRadians.
// ---------------------------------------------------------------------------

// One piece of the oracle's output: its index and the bits of its centre, velocity and spin.
struct ShatterRow {
    int32_t index;
    uint32_t center[3];
    uint32_t velocity[3];
    uint32_t spin[4];
};

struct ShatterCase {
    const char* label;
    std::vector<scripts::DebrisFacet> facets;
    Vec3 point;
    Vec3 direction;
    int32_t pieces;
    const ShatterRow* rows;
    int32_t rowCount;
    // When non-null, EVERY piece's velocity.y must be exactly these bits. That is the
    // whole point of the NaN rows: `f` is exactly 1.0 for every piece of the group, so
    // the assertion covers all 129 pieces of `big_nan` without 129 rows.
    const uint32_t* everyVelocityY;
};

// The GroupFacets seam, fed from the case under test.
static bool shatterGroupFacets(void* ctx, const std::string& arena, int32_t group,
                               scripts::DebrisGroup* out) {
    (void)arena;
    (void)group;
    *out = *static_cast<scripts::DebrisGroup*>(ctx);
    return true;
}

static scripts::DebrisFacet shatterFacet(const float a[3], const float b[3], const float c[3],
                                         int32_t material) {
    scripts::DebrisFacet facet;
    facet.a = Vec3{a[0], a[1], a[2]};
    facet.b = Vec3{b[0], b[1], b[2]};
    facet.c = Vec3{c[0], c[1], c[2]};
    facet.uvA = Vec2{0.0f, 0.0f};
    facet.uvB = Vec2{1.0f, 0.0f};
    facet.uvC = Vec2{0.0f, 1.0f};
    facet.material = material;
    return facet;
}

static scripts::DebrisFacet shatterFacetBits(uint32_t ax, uint32_t ay, uint32_t az,
                                             uint32_t bx, uint32_t by, uint32_t bz,
                                             uint32_t cx, uint32_t cy, uint32_t cz,
                                             int32_t material) {
    const float a[3] = {floatFromBits(ax), floatFromBits(ay), floatFromBits(az)};
    const float b[3] = {floatFromBits(bx), floatFromBits(by), floatFromBits(bz)};
    const float c[3] = {floatFromBits(cx), floatFromBits(cy), floatFromBits(cz)};
    return shatterFacet(a, b, c, material);
}

void testDebrisGroupBoxPropagatesNaNLikeTheOracle() {
    const uint32_t nan = 0x7fc00000u;
    const uint32_t zero = 0x00000000u;
    const uint32_t negzero = 0x80000000u;
    const uint32_t fullSpeed = 0x3fc00000u;  // the shatter's speed, 1.5: f == 1.0 exactly

    // The control: an all-finite group. 18 pieces, every one of them a different
    // fraction of the speed, because Far is finite and real. Without this row set the
    // NaN rows could be matched by a box that simply always produced a NaN.
    const ShatterRow finiteRows[] = {
        {0, {0x3F400000u, 0x3EC00000u, 0x00000000u}, {0x00000000u, 0x3F92F4A6u, 0x00000000u}, {0xBD992301u, 0xBD4D152Au, 0xBC4BB726u, 0x3F7EF106u}},
        {1, {0x3E800000u, 0x3F900000u, 0x00000000u}, {0x00000000u, 0x3F5FEBD3u, 0x00000000u}, {0x3CA4348Eu, 0x3D175FB1u, 0xBC492938u, 0x3F7FC11Cu}},
        {2, {0x3E000000u, 0xBF4AAAABu, 0x3F955555u}, {0x00000000u, 0x3FABCBE0u, 0x00000000u}, {0x3DC5D103u, 0xBCD64320u, 0xBDB0D465u, 0x3F7DC110u}},
        {3, {0x3F2AAAABu, 0xBF200000u, 0x3F400000u}, {0x00000000u, 0x3FB06186u, 0x00000000u}, {0xBDE19600u, 0x3CB5BF1Fu, 0xBD65C796u, 0x3F7DF91Fu}},
        {4, {0xBF255555u, 0x00000000u, 0x3FD00000u}, {0x00000000u, 0x3F83A968u, 0x00000000u}, {0x3DA127D8u, 0xBD0D9898u, 0x3CFB61B1u, 0x3F7EEE83u}},
        {5, {0xBEA00000u, 0xBF155555u, 0x3FBAAAABu}, {0x00000000u, 0x3F9CD4B4u, 0x00000000u}, {0xBD3A57E4u, 0x3CEE1A95u, 0x3CB07CB4u, 0x3F7F9131u}},
        {6, {0x3EB55555u, 0xBFE00000u, 0x3F900000u}, {0x00000000u, 0x3FB96A5Au, 0x00000000u}, {0x3D476F53u, 0x3D24412Eu, 0x3DCDDDD2u, 0x3F7E30E3u}},
        {7, {0x3F300000u, 0xC0155555u, 0x3F755555u}, {0x00000000u, 0x3FBCD4B4u, 0x00000000u}, {0xBDA5F63Au, 0xBDD32570u, 0xBD2386FBu, 0x3F7D955Eu}},
        {8, {0x3F800000u, 0xBF9AAAABu, 0x3F155555u}, {0x00000000u, 0x3FBB348Cu, 0x00000000u}, {0xBD9DD33Du, 0x3D94F43Fu, 0x3D93168Eu, 0x3F7DE4D2u}},
        {9, {0x3EEAAAABu, 0xBFB00000u, 0x3F800000u}, {0x00000000u, 0x3FB8EEC4u, 0x00000000u}, {0x3D8B7E2Fu, 0xBD5FC632u, 0x3DB9F03Bu, 0x3F7DF5FFu}},
        {10, {0x3F7AAAABu, 0x3F000000u, 0x3EC00000u}, {0x00000000u, 0x3F90967Du, 0x00000000u}, {0x3D19F197u, 0x3BD66921u, 0x3D0A8535u, 0x3F7FAAC7u}},
        {11, {0x3FA80000u, 0xBDAAAAABu, 0x3E555555u}, {0x00000000u, 0x3FA2D22Eu, 0x00000000u}, {0x3DBC56D2u, 0xBDDEA30Bu, 0x3D8FB71Eu, 0x3F7CC102u}},
        {12, {0x3FD00000u, 0x3F855555u, 0xBE2AAAABu}, {0x00000000u, 0x3F675B38u, 0x00000000u}, {0xBD87C2D1u, 0x3D4F832Cu, 0x3B630412u, 0x3F7F1B1Du}},
        {13, {0x3F8AAAABu, 0x3F600000u, 0x3E800000u}, {0x00000000u, 0x3F80EEC4u, 0x00000000u}, {0xBD20C743u, 0x3BA386BCu, 0x3DA12CBDu, 0x3F7F0142u}},
        {14, {0x00000000u, 0x3F0AAAABu, 0x3F8AAAABu}, {0x00000000u, 0x3F84FA89u, 0x00000000u}, {0xBCB37362u, 0x3CF369ADu, 0xBC51701Fu, 0x3F7FCDF9u}},
        {15, {0xBF0AAAABu, 0x3EC00000u, 0x3FC00000u}, {0x00000000u, 0x3F778A4Du, 0x00000000u}, {0x3CD1B5AFu, 0x3CD1D9C0u, 0xBBE8459Eu, 0x3F7FD35Cu}},
        {16, {0x3F455555u, 0xBE800000u, 0x3F200000u}, {0x00000000u, 0x3FA84FDEu, 0x00000000u}, {0xBD0CFE22u, 0xBD6B8FD5u, 0x3A3661A4u, 0x3F7F6C9Du}},
        {17, {0x3EE00000u, 0x3EAAAAABu, 0x3F4AAAABu}, {0x00000000u, 0x3F93C44Fu, 0x00000000u}, {0xBD094510u, 0xBD823B37u, 0x3D005C5Au, 0x3F7F3634u}},
    };
    // A NaN on the MIN side of one corner. The box goes NaN, Far goes NaN, the
    // `Far > 0` falloff test is false, and every piece keeps the full speed. Note the
    // piece COUNT also changes (3, not 18): a NaN triangle compares false against
    // maxCross, so it is never split. Both halves are the oracle's.
    const ShatterRow nanCornerRows[] = {
        {0, {0x3F400000u, 0x3EC00000u, 0x00000000u}, {0x00000000u, 0x3FC00000u, 0x00000000u}, {0xBDC768FBu, 0xBD86331Du, 0xBC90FFE4u, 0x3F7E30BDu}},
        {1, {0x3E800000u, 0x3F900000u, 0x00000000u}, {0x00000000u, 0x3FC00000u, 0x00000000u}, {0x3D0A72A6u, 0x3D82428Cu, 0xBCB3BE06u, 0x3F7F45F8u}},
        {2, {0x7FC00000u, 0xBEAAAAABu, 0x3F555555u}, {0x00000000u, 0x3FC00000u, 0x00000000u}, {0x3DDD9516u, 0xBCE5BF52u, 0xBDC46E8Fu, 0x3F7D354Bu}},
    };
    // The same on the MAX side, and on the other corner, so a one-sided fold fails.
    const ShatterRow nanHighRows[] = {
        {0, {0x3F400000u, 0x3EC00000u, 0x00000000u}, {0x00000000u, 0x3FC00000u, 0x00000000u}, {0xBDC768FBu, 0xBD86331Du, 0xBC90FFE4u, 0x3F7E30BDu}},
        {1, {0x3E800000u, 0x3F900000u, 0x00000000u}, {0x00000000u, 0x3FC00000u, 0x00000000u}, {0x3D0A72A6u, 0x3D82428Cu, 0xBCB3BE06u, 0x3F7F45F8u}},
        {2, {0xBF555555u, 0x7FC00000u, 0x3F955555u}, {0x00000000u, 0x3FC00000u, 0x00000000u}, {0x3DDD9516u, 0xBCE5BF52u, 0xBDC46E8Fu, 0x3F7D354Bu}},
    };
    // The ±0 half of the helper is NOT observable at this site, and that is measured, not
    // argued: Far is `Max(Abs(min - point), Abs(max - point)).LengthSquared()` and Abs
    // erases the sign of a zero, so this group and the identical one with plain 0.0 shatter
    // into bit-identical pieces (probe rows `signed_zero` / `signed_zero_plain`). The two
    // cases below assert the same rows for both groups.
    const ShatterRow signedZeroRows[] = {
        {0, {0x3EAAAAABu, 0x00000000u, 0x3E2AAAABu}, {0x00000000u, 0x3F381382u, 0x00000000u}, {0xBD407DB2u, 0xBD001177u, 0xBBE30A8Bu, 0x3F7F95F1u}},
        {1, {0xBE2AAAABu, 0x3E2AAAABu, 0x3E2AAAABu}, {0x00000000u, 0x3EE2BE2Bu, 0x00000000u}, {0x3C281C7Cu, 0x3C98DAD5u, 0xBBC5AA23u, 0x3F7FEFF3u}},
    };
    // A NaN in a group big enough to split: 129 pieces, every one of them at full speed.
    const ShatterRow bigNanRows[] = {
        {0, {0x40980000u, 0x40100000u, 0x00000000u}, {0x00000000u, 0x3FC00000u, 0x00000000u}, {0xBDC768FBu, 0xBD86331Du, 0xBC90FFE4u, 0x3F7E30BDu}},
        {1, {0x40A80000u, 0x40300000u, 0x00000000u}, {0x00000000u, 0x3FC00000u, 0x00000000u}, {0x3D0A72A6u, 0x3D82428Cu, 0xBCB3BE06u, 0x3F7F45F8u}},
        {2, {0x40700000u, 0x40300000u, 0x00000000u}, {0x00000000u, 0x3FC00000u, 0x00000000u}, {0x3DDD9516u, 0xBCE5BF52u, 0xBDC46E8Fu, 0x3F7D354Bu}},
        {3, {0x40880000u, 0x40100000u, 0x00000000u}, {0x00000000u, 0x3FC00000u, 0x00000000u}, {0xBDF5B555u, 0x3CC063DCu, 0xBD788FA7u, 0x3F7D9AB9u}},
    };
    // Which SIDE of the box each axis takes its far corner from is pinned from both ends.
    // `inside` sits next to the min corner, so every axis must reach the MAX side; the
    // same group from a point next to the MAX corner must reach the MIN side instead. With
    // only the mid-group point above, a box that lost its min side on one axis still
    // shattered identically -- measured, not assumed (the first version of this pin let
    // exactly that mutant through, and these two cases are what closed it).
    const ShatterRow insideRows[] = {
        {0, {0x3F400000u, 0x3EC00000u, 0x00000000u}, {0x00000000u, 0x3F82D70Fu, 0x00000000u}, {0xBD887DFDu, 0xBD367434u, 0xBC2F872Cu, 0x3F7F2953u}},
        {1, {0x3E800000u, 0x3F900000u, 0x00000000u}, {0x00000000u, 0x3F613CBDu, 0x00000000u}, {0x3CA525F4u, 0x3D1844AEu, 0xBC4A6A01u, 0x3F7FC05Du}},
        {2, {0x3E000000u, 0xBF4AAAABu, 0x3F955555u}, {0x00000000u, 0x3FA20FE6u, 0x00000000u}, {0x3DBA643Cu, 0xBCCE0E42u, 0xBDA74927u, 0x3F7DFF36u}},
        {3, {0x3F2AAAABu, 0xBF200000u, 0x3F400000u}, {0x00000000u, 0x3F9AFC40u, 0x00000000u}, {0xBDC609B4u, 0x3CA5B25Cu, 0xBD4B9AABu, 0x3F7E6DFFu}},
    };
    const ShatterRow nearMaxRows[] = {
        {0, {0x3F400000u, 0x3EC00000u, 0x00000000u}, {0x00000000u, 0x3F9C4244u, 0x00000000u}, {0xBDA2B989u, 0xBD5A2746u, 0xBC5CA22Cu, 0x3F7ECD85u}},
        {1, {0x3E800000u, 0x3F900000u, 0x00000000u}, {0x00000000u, 0x3F9E8027u, 0x00000000u}, {0x3CE63AE3u, 0x3D56C1F6u, 0xBC91E3F9u, 0x3F7F8187u}},
        {2, {0x3E000000u, 0xBF4AAAABu, 0x3F955555u}, {0x00000000u, 0x3F89CFDAu, 0x00000000u}, {0x3D9E039Bu, 0xBCB78429u, 0xBD8F3CD4u, 0x3F7E8B24u}},
        {3, {0x3F2AAAABu, 0xBF200000u, 0x3F400000u}, {0x00000000u, 0x3F920F3Du, 0x00000000u}, {0xBDBA8DA4u, 0x3C9E8060u, 0xBD4088DCu, 0x3F7E9A77u}},
    };
    const Vec3 mid{1.0f, -2.0f, 0.5f};
    const Vec3 insidePoint{-1.0f, -2.5f, 0.0f};
    const Vec3 nearMaxPoint{1.9f, 2.0f, 1.9f};

    std::vector<scripts::DebrisFacet> finiteFacets;
    finiteFacets.push_back(shatterFacetBits(zero, zero, zero, 0x3FC00000u, zero, zero,
                                            zero, 0x40100000u, zero, 3));
    finiteFacets.push_back(shatterFacetBits(0xBFA00000u, 0x3F000000u, 0x40000000u, 0x3F400000u,
                                            0xC0400000u, 0x3F800000u, 0x40000000u, 0x3FC00000u,
                                            0xBF000000u, 7));
    std::vector<scripts::DebrisFacet> nanCornerFacets;
    nanCornerFacets.push_back(finiteFacets[0]);
    nanCornerFacets.push_back(shatterFacetBits(nan, 0x3F000000u, 0x40000000u, 0x3F400000u,
                                               0xC0400000u, 0x3F800000u, 0x40000000u,
                                               0x3FC00000u, 0xBF000000u, 7));
    std::vector<scripts::DebrisFacet> nanHighFacets;
    nanHighFacets.push_back(finiteFacets[0]);
    nanHighFacets.push_back(shatterFacetBits(0xBFA00000u, 0x3F000000u, 0x40000000u, 0x3F400000u,
                                             nan, 0x3F800000u, 0xC0000000u, 0xBFC00000u,
                                             0x3F000000u, 7));
    std::vector<scripts::DebrisFacet> signedZeroFacets;
    signedZeroFacets.push_back(shatterFacetBits(negzero, negzero, zero, 0x3F000000u, negzero,
                                                0x3F000000u, 0x3F000000u, zero, negzero, 5));
    signedZeroFacets.push_back(shatterFacetBits(0xBF000000u, zero, negzero, zero, negzero,
                                                0x3F000000u, zero, 0x3F000000u, zero, 9));
    std::vector<scripts::DebrisFacet> signedZeroPlainFacets;
    signedZeroPlainFacets.push_back(shatterFacetBits(zero, zero, zero, 0x3F000000u, zero,
                                                     0x3F000000u, 0x3F000000u, zero, zero, 5));
    signedZeroPlainFacets.push_back(shatterFacetBits(0xBF000000u, zero, zero, zero, zero,
                                                     0x3F000000u, zero, 0x3F000000u, zero, 9));
    std::vector<scripts::DebrisFacet> bigNanFacets;
    bigNanFacets.push_back(shatterFacetBits(zero, zero, zero, 0x41400000u, zero, zero, zero,
                                            0x41400000u, zero, 3));
    bigNanFacets.push_back(nanCornerFacets[1]);

    // An ASSOCIATION case. `Shot.Far` is `Vector3.LengthSquared()` of the box's far corner,
    // and whether that sums the per-axis squares as ((x*x + y*y) + z*z) or ((x*x + z*z) + y*y)
    // is not visible from any other row here -- review-27 F6 reproduced every earlier row under
    // BOTH groupings. This group is searched so that its far vector discriminates: left-assoc
    // gives 3DAF1718, the other grouping gives 3DAF1719, and the triangle's centroid sits off
    // the point so `f = 1 - DistanceSquared(center,point)/far` actually reads `far`. The oracle
    // prints `ASSOC far 3E0D8004,3DE4D6F4,3E6DC3C0 lensq 3DAF1718 left 3DAF1718 right 3DAF1719`
    // (probe mode `shatter`), so `Vector3.LengthSquared`/`Dot`/`DistanceSquared` are all
    // LEFT-assoc, which is what debris.cpp's vdot writes and what the rows below now pin.
    std::vector<scripts::DebrisFacet> assocFacets;
    assocFacets.push_back(shatterFacetBits(0xBD0059C6u, 0xBCE16BA3u, 0xBE02F83Cu, 0x3D0059C6u,
                                           0x3CE16BA3u, 0x3E02F83Cu, 0x3D0059C6u, 0xBCE16BA3u,
                                           0x3E02F83Cu, 4));
    const Vec3 assocPoint = {floatFromBits(0x3DDAD325u), floatFromBits(0xBDAC7C0Bu),
                             floatFromBits(0xBDD59707u)};
    const ShatterRow assocRows[] = {
        {0, {0x3C2B225Du, 0xBC1647C2u, 0x3D2EA050u}, {0x00000000u, 0x3F5BFFE1u, 0x00000000u},
         {0xBD65CF1Du, 0xBD193934u, 0xBC0D30E8u, 0x3F7F6862u}},
    };

    // The velocity is `(direction OR radial) * Speed * f`, and the C# associates that LEFT:
    // `shot.Direction * shot.Speed * f` is `(d * Speed) * f`, two multiplies. The port had
    // `d * (Speed * f)`, one multiply by a pre-multiplied scalar, and those are different
    // floats. An axis-aligned direction hides it -- multiplying by 1.0 is exact, so the
    // `dir` rows above pass under either spelling -- which is why these two groups exist.
    // They are the same finite group with a direction that has three real components, so
    // every axis of both the directional and the radial path is pinned to the oracle's bits.
    const ShatterRow radialRows[] = {
        {0, {0x3F400000u, 0x3EC00000u, 0x00000000u}, {0xBDF0EBA9u, 0x3F8F0BECu, 0xBE70EBA9u}, {0xBD992301u, 0xBD4D152Au, 0xBC4BB726u, 0x3F7EF106u}},
        {1, {0x3E800000u, 0x3F900000u, 0x00000000u}, {0xBE4E8B34u, 0x3F572656u, 0xBE09B222u}, {0x3CA4348Eu, 0x3D175FB1u, 0xBC492938u, 0x3F7FC11Cu}},
        {2, {0x3E000000u, 0xBF4AAAABu, 0x3F955555u}, {0xBF37FC76u, 0x3F7E137Du, 0x3F0C2E10u}, {0x3DC5D103u, 0xBCD64320u, 0xBDB0D465u, 0x3F7DC110u}},
        {3, {0x3F2AAAABu, 0xBF200000u, 0x3F400000u}, {0xBEA3AF89u, 0x3FA8CD06u, 0x3E75874Du}, {0xBDE19600u, 0x3CB5BF1Fu, 0xBD65C796u, 0x3F7DF91Fu}},
        {4, {0xBF255555u, 0x00000000u, 0x3FD00000u}, {0xBF197899u, 0x3F3A7F1Du, 0x3ED1CF01u}, {0x3DA127D8u, 0xBD0D9898u, 0x3CFB61B1u, 0x3F7EEE83u}},
        {5, {0xBEA00000u, 0xBF155555u, 0x3FBAAAABu}, {0xBF3EF419u, 0x3F4E1BCAu, 0x3F0B6D26u}, {0xBD3A57E4u, 0x3CEE1A95u, 0x3CB07CB4u, 0x3F7F9131u}},
    };
    const ShatterRow obliqueRows[] = {
        {0, {0x3F400000u, 0x3EC00000u, 0x00000000u}, {0x3F0045FFu, 0xBF8045FFu, 0x3E8045FFu}, {0xBD992301u, 0xBD4D152Au, 0xBC4BB726u, 0x3F7EF106u}},
        {1, {0x3E800000u, 0x3F900000u, 0x00000000u}, {0x3EC37456u, 0xBF437456u, 0x3E437456u}, {0x3CA4348Eu, 0x3D175FB1u, 0xBC492938u, 0x3F7FC11Cu}},
        {2, {0x3E000000u, 0xBF4AAAABu, 0x3F955555u}, {0x3F15F4C9u, 0xBF95F4C9u, 0x3E95F4C9u}, {0x3DC5D103u, 0xBCD64320u, 0xBDB0D465u, 0x3F7DC110u}},
        {3, {0x3F2AAAABu, 0xBF200000u, 0x3F400000u}, {0x3F19F53Au, 0xBF99F53Au, 0x3E99F53Au}, {0xBDE19600u, 0x3CB5BF1Fu, 0xBD65C796u, 0x3F7DF91Fu}},
        {4, {0xBF255555u, 0x00000000u, 0x3FD00000u}, {0x3EE5D8FAu, 0xBF65D8FAu, 0x3E65D8FAu}, {0x3DA127D8u, 0xBD0D9898u, 0x3CFB61B1u, 0x3F7EEE83u}},
        {5, {0xBEA00000u, 0xBF155555u, 0x3FBAAAABu}, {0x3F08E4A9u, 0xBF88E4A9u, 0x3E88E4A9u}, {0xBD3A57E4u, 0x3CEE1A95u, 0x3CB07CB4u, 0x3F7F9131u}},
    };
    const ShatterRow oblique2Rows[] = {
        {0, {0x3F400000u, 0x3EC00000u, 0x00000000u}, {0xBEC1CCA6u, 0x3E688F2Du, 0x3F87A8DAu}, {0xBD992301u, 0xBD4D152Au, 0xBC4BB726u, 0x3F7EF106u}},
        {1, {0x3E800000u, 0x3F900000u, 0x00000000u}, {0xBE93A636u, 0x3E312DDAu, 0x3F4EB57Fu}, {0x3CA4348Eu, 0x3D175FB1u, 0xBC492938u, 0x3F7FC11Cu}},
        {2, {0x3E000000u, 0xBF4AAAABu, 0x3F955555u}, {0xBEE28EF3u, 0x3E87EF5Eu, 0x3F9E9743u}, {0x3DC5D103u, 0xBCD64320u, 0xBDB0D465u, 0x3F7DC110u}},
        {3, {0x3F2AAAABu, 0xBF200000u, 0x3F400000u}, {0xBEE89AB5u, 0x3E8B9006u, 0x3FA2D2B1u}, {0xBDE19600u, 0x3CB5BF1Fu, 0xBD65C796u, 0x3F7DF91Fu}},
        {4, {0xBF255555u, 0x00000000u, 0x3FD00000u}, {0xBEADA160u, 0x3E505B3Fu, 0x3F73151Eu}, {0x3DA127D8u, 0xBD0D9898u, 0x3CFB61B1u, 0x3F7EEE83u}},
        {5, {0xBEA00000u, 0xBF155555u, 0x3FBAAAABu}, {0xBECED28Bu, 0x3E782FD9u, 0x3F90C694u}, {0xBD3A57E4u, 0x3CEE1A95u, 0x3CB07CB4u, 0x3F7F9131u}},
    };
    const Vec3 obliqueDir{0.5f, -1.0f, 0.25f};
    const Vec3 oblique2Dir{-1.25f, 0.75f, 3.5f};
    const Vec3 up{0.0f, 1.0f, 0.0f};
    const Vec3 noDir{0.0f, 0.0f, 0.0f};

    const ShatterCase cases[] = {
        //  label              facets                point     direction pieces rows          count every v.y
        {"finite", finiteFacets, mid, up, 18, finiteRows, 18, nullptr},
        {"nan_corner", nanCornerFacets, mid, up, 3, nanCornerRows, 3, &fullSpeed},
        {"nan_high", nanHighFacets, mid, up, 3, nanHighRows, 3, &fullSpeed},
        {"signed_zero", signedZeroFacets, mid, up, 2, signedZeroRows, 2, nullptr},
        {"signed_zero_plain", signedZeroPlainFacets, mid, up, 2, signedZeroRows, 2, nullptr},
        {"big_nan", bigNanFacets, mid, up, 129, bigNanRows, 4, &fullSpeed},
        {"inside", finiteFacets, insidePoint, up, 18, insideRows, 4, nullptr},
        {"near_max", finiteFacets, nearMaxPoint, up, 18, nearMaxRows, 4, nullptr},
        {"oblique", finiteFacets, mid, obliqueDir, 18, obliqueRows, 6, nullptr},
        {"oblique2", finiteFacets, mid, oblique2Dir, 18, oblique2Rows, 6, nullptr},
        {"radial", finiteFacets, mid, noDir, 18, radialRows, 6, nullptr},
        {"assoc", assocFacets, assocPoint, up, 1, assocRows, 1, nullptr},
    };

    std::vector<std::string> names;
    names.push_back("MAT_A");
    names.push_back("MAT_B");

    for (size_t ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ci++) {
        const ShatterCase& c = cases[ci];
        Random rng(1);
        scripts::DebrisGroup group;
        group.names = &names;
        group.facets = c.facets;
        scripts::Debris debris(rng);
        debris.setCallbacks(&group, nullptr, nullptr, nullptr, shatterGroupFacets);
        debris.shatter("TEST", 3, 2.5f, 4.0f, 1.5f, c.point, c.direction);
        if ((int)debris.pieces().size() != c.pieces) {
            printf("  FAIL: %s made %d pieces, the oracle made %d\n", c.label,
                   (int)debris.pieces().size(), (int)c.pieces);
            failures++;
            continue;
        }
        for (int32_t r = 0; r < c.rowCount; r++) {
            const ShatterRow& row = c.rows[r];
            const scripts::Debris::Piece* p = debris.pieces()[row.index];
            const uint32_t center[3] = {floatBits(p->center.x), floatBits(p->center.y),
                                        floatBits(p->center.z)};
            const uint32_t velocity[3] = {floatBits(p->velocity.x), floatBits(p->velocity.y),
                                          floatBits(p->velocity.z)};
            const uint32_t spin[4] = {floatBits(p->spin[0]), floatBits(p->spin[1]),
                                      floatBits(p->spin[2]), floatBits(p->spin[3])};
            for (int k = 0; k < 3; k++) {
                if (center[k] != row.center[k]) {
                    printf("  FAIL: %s piece %d centre.%c is %08X, the oracle's is %08X\n",
                           c.label, (int)row.index, 'x' + k, center[k], row.center[k]);
                    failures++;
                }
                if (velocity[k] != row.velocity[k]) {
                    printf("  FAIL: %s piece %d velocity.%c is %08X, the oracle's is %08X\n",
                           c.label, (int)row.index, 'x' + k, velocity[k], row.velocity[k]);
                    failures++;
                }
            }
            // The spin is the piece's own quaternion, from three RNG draws and the same `f`
            // the velocity used, so it fails on an angle-rounding change the velocity alone
            // cannot see.
            for (int k = 0; k < 4; k++) {
                if (spin[k] != row.spin[k]) {
                    printf("  FAIL: %s piece %d spin.%c is %08X, the oracle's is %08X\n",
                           c.label, (int)row.index, "xyzw"[k], spin[k], row.spin[k]);
                    failures++;
                }
            }
        }
        if (c.everyVelocityY != nullptr) {
            for (size_t pi = 0; pi < debris.pieces().size(); pi++) {
                if (floatBits(debris.pieces()[pi]->velocity.y) != *c.everyVelocityY) {
                    printf("  FAIL: %s piece %zu velocity.y is %08X, the oracle's is %08X\n",
                           c.label, pi, floatBits(debris.pieces()[pi]->velocity.y),
                           *c.everyVelocityY);
                    failures++;
                    break;
                }
            }
        }
    }
}

// Small vector helpers for the camera pin below. vm_tests has no geometry of its own,
// and the ones follow_camera.cpp uses are file-static there.
static Vec3 vmCross(const Vec3& a, const Vec3& b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

static float vmDot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static Vec3 vmNormalize(const Vec3& v) {
    const float length = sqrtf(vmDot(v, v));
    return length > 0.0f ? Vec3{v.x / length, v.y / length, v.z / length} : v;
}

static bool nearFloat(float actual, float expected, float tolerance) {
    return fabsf(actual - expected) <= tolerance;
}

// The follow camera's roll is the C#'s Turn (FollowCamera.cs:196-212): the up vector
// swings towards the view's own right by the bank, the forward is untouched, and an
// exact zero takes the C#'s early return. The oracle's own KurtRollsTheCameraTurningRight
// (KurtMovesTests.cs:368) asserts the same property this checks -- Dot(Up, right) equals
// Sin(DegreesToRadians(Roll)) -- to three decimals; this checks it tighter. And
// golden/L3_kurt_camroll.txt pins the DEGREES exactly against the oracle; this pins what
// those degrees do to the view, which no golden can, because the rotation goes through
// the project's one trig path (fastSin/fastCos, AGENTS.md R6) and so sits within the
// table's error of MathF.Sin/MathF.Cos rather than on top of it.
//
// WHY THE TOLERANCE IS 2e-5 AND NOT TIGHTER. Measured at the bank limit (10 degrees =
// 0.174533 rad), on this tree, against libm: fastCos(+r) is off by 4.53e-06 and
// fastSin(+r) by 8.20e-07 -- inside the header's documented 4.76e-06 -- but the SAME
// table on a NEGATIVE argument is off by 1.19e-05 (cos) and 1.88e-06 (sin), because
// `(int)scaled` truncates toward zero, so a negative argument interpolates with a
// negative fraction along the NEXT segment instead of the one it lies in. That is a
// real asymmetry in math_lut.h, it is 2.5x its own documented bound, and it is NOT
// this batch's to change (AGENTS.md §7: do not touch the LUT as part of a pass); it is
// recorded in the progress log instead. 2e-5 is that measurement plus room, and it is
// still four orders below anything the eye resolves while failing every wrong sign, a
// swapped sine and cosine, or a roll about the wrong axis, all of which are O(0.1).
void testCameraRollBanksTheFollowCamera() {
    const Vec3 feet{0.0f, 0.0f, 0.0f};
    const Vec3 facing{1.0f, 0.0f, 0.0f};
    const float kAspect = 4.0f / 3.0f;
    const float kPivot = follow_camera::kDefaultPivot;
    const float kBank = 10.0f;   // the walk writer's own limit, in degrees

    Camera level;
    Camera negativeZero;
    Camera bankedRight;
    Camera bankedLeft;
    CHECK(buildFollowCamera(&level, feet, facing, follow_camera::kDefaultPitch, kPivot,
                            kAspect, 0.0f));
    CHECK(buildFollowCamera(&negativeZero, feet, facing, follow_camera::kDefaultPitch, kPivot,
                            kAspect, -0.0f));
    CHECK(buildFollowCamera(&bankedRight, feet, facing, follow_camera::kDefaultPitch, kPivot,
                            kAspect, kBank));
    CHECK(buildFollowCamera(&bankedLeft, feet, facing, follow_camera::kDefaultPitch, kPivot,
                            kAspect, -kBank));

    // The C#'s early return is `if (roll == 0f) return;`, which -0.0 satisfies too, so a
    // level view is bit-for-bit what Orient just produced and not a rotation by zero.
    CHECK(negativeZero.up.x == level.up.x && negativeZero.up.y == level.up.y &&
          negativeZero.up.z == level.up.z);

    // A bank tilts the picture; it does not turn it. Forward is untouched.
    CHECK(bankedRight.forward.x == level.forward.x &&
          bankedRight.forward.y == level.forward.y &&
          bankedRight.forward.z == level.forward.z);

    // The C#'s Right, from Orient (FollowCamera.cs:151): Cross(Forward, UnitZ). Up
    // swings towards it for a positive bank and away for a negative one, by the bank's
    // own sine, and it keeps its cosine along the level up.
    const Vec3 right = vmNormalize(vmCross(level.forward, Vec3{0.0f, 0.0f, 1.0f}));
    const float radians = kBank * 3.14159265358979323846f / 180.0f;
    const float kTolerance = 2e-5f;
    CHECK(nearFloat(vmDot(bankedRight.up, right), sinf(radians), kTolerance));
    CHECK(nearFloat(vmDot(bankedLeft.up, right), -sinf(radians), kTolerance));
    CHECK(nearFloat(vmDot(bankedRight.up, level.up), cosf(radians), kTolerance));
    CHECK(nearFloat(vmDot(bankedLeft.up, level.up), cosf(radians), kTolerance));

    // And the basis stays orthonormal, or the look-at would shear the view.
    CHECK(nearFloat(vmDot(bankedRight.up, bankedRight.up), 1.0f, kTolerance));
    CHECK(nearFloat(vmDot(bankedRight.up, bankedRight.forward), 0.0f, kTolerance));

    // THE WIRING, not just the maths. FollowCamera::update has to read the bank off the
    // Player the way the C# reads it off Kurt (FollowCamera.cs:139); a build that calls
    // the roll helper with a literal 0 passes every check above and shows Kurt a level
    // view forever. The property is pitch-independent, which is what makes it usable
    // here: `up` is rotated about `forward`, and the view's own right is built from
    // that same forward, so the component of `up` along it is the bank's sine whatever
    // the arena pitch, the climb and the air tilt have done to the pitch.
    Player player;
    player.feet = feet;
    Input input;
    input.down[kKeyForward] = true;
    input.down[kKeyTurnRight] = true;   // the C#'s -Axis(TurnLeft, TurnRight) is +1 here
    for (int i = 0; i < 20; i++) {
        player.update(input, 1.0f / 60.0f);
    }
    const float bank = player.cameraRoll().roll();
    CHECK(bank > 0.0f);
    FollowCamera follow;
    Camera stepped;
    follow.update(&stepped, nullptr, player, follow_camera::kDefaultPitch,
                  1.0f / 60.0f, kAspect);
    const Vec3 steppedRight = vmNormalize(vmCross(stepped.forward, Vec3{0.0f, 0.0f, 1.0f}));
    CHECK(nearFloat(vmDot(stepped.up, steppedRight),
                    sinf(bank * 3.14159265358979323846f / 180.0f), kTolerance));
}

// The follow camera's between-step blend (C# FollowCamera.Blend, FollowCamera.cs
// 170-186): a frame drawn between two steps places the view at Kurt AS DRAWN --
// the caller's feet and yaw, because the oracle takes them from FrameBlend -- with
// its pitch, pivot and roll interpolated between the last two steps' Aim.
//
// WHAT THIS PINS AND WHY EACH PART IS WHERE IT IS.
//
// * The oracle rows are the probe's own (`mdkprobe 3 60 1 followblend`): eight
//   steps with the feet, yaw, vertical speed and mouse driven by hand -- the camera
//   reads only those and OnFloor off Kurt, and the C++ Player's own Update is not
//   part of this slice -- then the blend at alpha 0, .25, .5, .75 and 1, and again
//   with the feet moved 1 along +X. The oracle's own test (FrameBlendTests
//   .TheCameraIsPlacedForKurtAsDrawn) asserts only the +X row and only to four
//   decimals; this pins every row, and it pins the orientation as well as the place,
//   because a blend that forgets Orient or Turn leaves the frame drawing on the
//   previous step's view with no visible symptom in the place alone.
//
// * WHY A TOLERANCE AND NOT BITS. The blend's trig goes through the project's one
//   trig path (fastSin/fastCos, AGENTS.md R6) and its normalize takes a reciprocal,
//   where the oracle uses MathF.Cos/MathF.Sin and divides by the length; the view is
//   therefore within the table's error of the oracle rather than on top of it, the
//   same reason golden/L3_kurt_camroll.txt pins degrees and this file's roll test
//   pins 2e-5. The tolerance below is RELATIVE, because the place is ~100 units and
//   one ULP of that is already 8e-6.
//
// * WHAT IS BIT-EXACT ANYWAY. alpha = 1 returns the current Aim untouched (the
//   oracle's own `t >= 1` arm), so the blend at 1 with the step's own feet and yaw
//   IS the step's view, bit for bit, and after the first step the previous Aim and
//   the current one are the same, so every alpha of the first frame is that frame's
//   step. The oracle says exactly this for all eight steps and all five alphas of
//   step 1; the port has to, or its blend is not the same function. That is the
//   strongest check here and it needs no oracle table to be wrong about.
void testFollowCameraBlendPlacesTheFrame() {
    // The step's own view (pos, forward, up), raw bits, one row per step.
    static const uint32_t kStep[8][9] = {
        {0xC0FAB62D, 0xBF2ACA7E, 0x42D22CA0, 0x3F7F127E, 0x3D55E263, 0xBD898320, 0x3D8952E1, 0x3B664C75, 0x3F7F6C1B},   // step 1
        {0xC0F5C352, 0xBFAAAFAE, 0x42D2373E, 0x3F7E1581, 0x3DD5A477, 0xBD8205AA, 0x3D814F53, 0x3BD974CA, 0x3F7F7BCB},   // step 2
        {0xC0EFE08C, 0xBFFF92DD, 0x42D2663F, 0x3F7C4029, 0x3E1FCF70, 0xBD8CB8D2, 0x3D8AFD4B, 0x3C301C30, 0x3F7F651B},   // step 3
        {0xC0E96975, 0xC02A073E, 0x42D28480, 0x3F79CB50, 0x3E5461B0, 0xBD8F0CEE, 0x3D8BECAD, 0x3C6DEF5D, 0x3F7F5FF0},   // step 4
        {0xC0E284A6, 0xC0541BD3, 0x42D27B9C, 0x3F76CF5E, 0x3E8443DD, 0xBD7B9C63, 0x3D730997, 0x3C823E4E, 0x3F7F843C},   // step 5
        {0xC0DAA483, 0xC07DADC2, 0x42D2A270, 0x3F72F606, 0x3E9DE2BA, 0xBD846AC4, 0x3D7BDF48, 0x3CA3AD2D, 0x3F7F76DF},   // step 6
        {0xC0D23F94, 0xC0937479, 0x42D2B155, 0x3F6E8941, 0x3EB72183, 0xBD7E21E3, 0x3D6D40C4, 0x3CB62553, 0x3F7F81BD},   // step 7
        {0xC0C91AFC, 0xC0A7C904, 0x42D2D017, 0x3F69660A, 0x3ECFD4DD, 0xBD81A51E, 0x3D6CDF88, 0x3CD2ECEC, 0x3F7F7C8F},   // step 8
    };
    // The blended view at alpha = 0, .25, .5, .75, 1 (pos, forward, up).
    static const uint32_t kBlend[8][5][9] = {
        {   // step 1
            {0xC0FAB62D, 0xBF2ACA7E, 0x42D22CA0, 0x3F7F127E, 0x3D55E263, 0xBD898320, 0x3D8952E1, 0x3B664C75, 0x3F7F6C1B},
            {0xC0FAB62D, 0xBF2ACA7E, 0x42D22CA0, 0x3F7F127E, 0x3D55E263, 0xBD898320, 0x3D8952E1, 0x3B664C75, 0x3F7F6C1B},
            {0xC0FAB62D, 0xBF2ACA7E, 0x42D22CA0, 0x3F7F127E, 0x3D55E263, 0xBD898320, 0x3D8952E1, 0x3B664C75, 0x3F7F6C1B},
            {0xC0FAB62D, 0xBF2ACA7E, 0x42D22CA0, 0x3F7F127E, 0x3D55E263, 0xBD898320, 0x3D8952E1, 0x3B664C75, 0x3F7F6C1B},
            {0xC0FAB62D, 0xBF2ACA7E, 0x42D22CA0, 0x3F7F127E, 0x3D55E263, 0xBD898320, 0x3D8952E1, 0x3B664C75, 0x3F7F6C1B},
        },
        {   // step 2
            {0xC0F5BC3A, 0xBFAAACB3, 0x42D23B86, 0x3F7E1123, 0x3DD5A0CA, 0xBD842984, 0x3D83702C, 0x3BDD090A, 0x3F7F7766},
            {0xC0F5BE04, 0xBFAAAD73, 0x42D23A74, 0x3F7E123C, 0x3DD5A1B7, 0xBD83A08E, 0x3D82E7F6, 0x3BDC23FB, 0x3F7F7880},
            {0xC0F5BFC9, 0xBFAAAE32, 0x42D23962, 0x3F7E1354, 0x3DD5A2A3, 0xBD831798, 0x3D825FC1, 0x3BDB3EED, 0x3F7F799A},
            {0xC0F5C190, 0xBFAAAEF1, 0x42D23850, 0x3F7E146B, 0x3DD5A38D, 0xBD828EA0, 0x3D81D788, 0x3BDA59DA, 0x3F7F7AB3},
            {0xC0F5C352, 0xBFAAAFAE, 0x42D2373E, 0x3F7E1581, 0x3DD5A477, 0xBD8205AA, 0x3D814F53, 0x3BD974CA, 0x3F7F7BCB},
        },
        {   // step 3
            {0xC0EFE08C, 0xBFFF92DD, 0x42D2663F, 0x3F7C4029, 0x3E1FCF70, 0xBD8CB8D2, 0x3D8AFD4B, 0x3C301C30, 0x3F7F651B},
            {0xC0EFE08C, 0xBFFF92DD, 0x42D2663F, 0x3F7C4029, 0x3E1FCF70, 0xBD8CB8D2, 0x3D8AFD4B, 0x3C301C30, 0x3F7F651B},
            {0xC0EFE08C, 0xBFFF92DD, 0x42D2663F, 0x3F7C4029, 0x3E1FCF70, 0xBD8CB8D2, 0x3D8AFD4B, 0x3C301C30, 0x3F7F651B},
            {0xC0EFE08C, 0xBFFF92DD, 0x42D2663F, 0x3F7C4029, 0x3E1FCF70, 0xBD8CB8D2, 0x3D8AFD4B, 0x3C301C30, 0x3F7F651B},
            {0xC0EFE08C, 0xBFFF92DD, 0x42D2663F, 0x3F7C4029, 0x3E1FCF70, 0xBD8CB8D2, 0x3D8AFD4B, 0x3C301C30, 0x3F7F651B},
        },
        {   // step 4
            {0xC0E98411, 0xC02A128E, 0x42D27525, 0x3F79DBB0, 0x3E546F9B, 0xBD875F5B, 0x3D846A0D, 0x3C612A06, 0x3F7F70AD},
            {0xC0E97D8D, 0xC02A0FC9, 0x42D278FC, 0x3F79D7AE, 0x3E546C33, 0xBD894ACC, 0x3D864AC2, 0x3C645B71, 0x3F7F6C93},
            {0xC0E976F2, 0xC02A0CFA, 0x42D27CD2, 0x3F79D39D, 0x3E5468BE, 0xBD8B3635, 0x3D882B6E, 0x3C678CCD, 0x3F7F686B},
            {0xC0E97040, 0xC02A0A22, 0x42D280A9, 0x3F79CF7F, 0x3E54653D, 0xBD8D2195, 0x3D8A0C10, 0x3C6ABE18, 0x3F7F6434},
            {0xC0E96975, 0xC02A073E, 0x42D28480, 0x3F79CB50, 0x3E5461B0, 0xBD8F0CEE, 0x3D8BECAD, 0x3C6DEF5D, 0x3F7F5FF0},
        },
        {   // step 5
            {0xC0E25E21, 0xC054072E, 0x42D29367, 0x3F76B7AA, 0x3E843729, 0xBD89B394, 0x3D850268, 0x3C8E8F1E, 0x3F7F6BB1},
            {0xC0E26815, 0xC0540C84, 0x42D28D75, 0x3F76BDCA, 0x3E843A72, 0xBD86BA58, 0x3D82231C, 0x3C8B7B08, 0x3F7F7209},
            {0xC0E271D3, 0xC05411BC, 0x42D28782, 0x3F76C3C9, 0x3E843DA9, 0xBD83C108, 0x3D7E877B, 0x3C8866DD, 0x3F7F783E},
            {0xC0E27B58, 0xC05416D6, 0x42D2818F, 0x3F76C9A4, 0x3E8440CC, 0xBD80C7A5, 0x3D78C899, 0x3C85529E, 0x3F7F7E4E},
            {0xC0E284A6, 0xC0541BD3, 0x42D27B9C, 0x3F76CF5E, 0x3E8443DD, 0xBD7B9C63, 0x3D730997, 0x3C823E4E, 0x3F7F843C},
        },
        {   // step 6
            {0xC0DA9733, 0xC07DA51B, 0x42D2AA9D, 0x3F72EDD5, 0x3E9DDD67, 0xBD8881B9, 0x3D81D35C, 0x3CA8BB4A, 0x3F7F6E42},
            {0xC0DA9A92, 0xC07DA74C, 0x42D2A892, 0x3F72EFE8, 0x3E9DDEC0, 0xBD877BFF, 0x3D80DA71, 0x3CA777C6, 0x3F7F706F},
            {0xC0DA9DE9, 0xC07DA977, 0x42D2A686, 0x3F72F1F6, 0x3E9DE016, 0xBD867644, 0x3D7FC309, 0x3CA63441, 0x3F7F7299},
            {0xC0DAA139, 0xC07DAB9F, 0x42D2A47B, 0x3F72F400, 0x3E9DE169, 0xBD857085, 0x3D7DD12B, 0x3CA4F0B9, 0x3F7F74BE},
            {0xC0DAA483, 0xC07DADC2, 0x42D2A270, 0x3F72F606, 0x3E9DE2BA, 0xBD846AC4, 0x3D7BDF48, 0x3CA3AD2D, 0x3F7F76DF},
        },
        {   // step 7
            {0xC0D23F94, 0xC0937479, 0x42D2B155, 0x3F6E8941, 0x3EB72183, 0xBD7E21E3, 0x3D6D40C4, 0x3CB62553, 0x3F7F81BD},
            {0xC0D23F94, 0xC0937479, 0x42D2B155, 0x3F6E8941, 0x3EB72183, 0xBD7E21E3, 0x3D6D40C4, 0x3CB62553, 0x3F7F81BD},
            {0xC0D23F94, 0xC0937479, 0x42D2B155, 0x3F6E8941, 0x3EB72183, 0xBD7E21E3, 0x3D6D40C4, 0x3CB62553, 0x3F7F81BD},
            {0xC0D23F94, 0xC0937479, 0x42D2B155, 0x3F6E8941, 0x3EB72183, 0xBD7E21E3, 0x3D6D40C4, 0x3CB62553, 0x3F7F81BD},
            {0xC0D23F94, 0xC0937479, 0x42D2B155, 0x3F6E8941, 0x3EB72183, 0xBD7E21E3, 0x3D6D40C4, 0x3CB62553, 0x3F7F81BD},
        },
        {   // step 8
            {0xC0C93226, 0xC0A7D354, 0x42D2C03B, 0x3F69744A, 0x3ECFE18D, 0xBD736DCB, 0x3D5E6224, 0x3CC605DD, 0x3F7F8C2A},
            {0xC0C92C7E, 0xC0A7D0D0, 0x42D2C432, 0x3F6970D0, 0x3ECFDE75, 0xBD7764FF, 0x3D620191, 0x3CC93FB3, 0x3F7F885A},
            {0xC0C926BF, 0xC0A7CE40, 0x42D2C829, 0x3F696D47, 0x3ECFDB4F, 0xBD7B5C23, 0x3D65A0F2, 0x3CCC797D, 0x3F7F847B},
            {0xC0C920EA, 0xC0A7CBA8, 0x42D2CC20, 0x3F6969AF, 0x3ECFD81C, 0xBD7F5335, 0x3D694043, 0x3CCFB339, 0x3F7F808D},
            {0xC0C91AFC, 0xC0A7C904, 0x42D2D017, 0x3F69660A, 0x3ECFD4DD, 0xBD81A51E, 0x3D6CDF88, 0x3CD2ECEC, 0x3F7F7C8F},
        },
    };
    // The same five blends with the feet moved 1 along +X: the place moves with
    // them (the oracle asserts exactly this), the orientation does not.
    static const uint32_t kShift[8][5][3] = {
        {   // step 1
            {0xC0DAB62D, 0xBF2ACA7E, 0x42D22CA0},
            {0xC0DAB62D, 0xBF2ACA7E, 0x42D22CA0},
            {0xC0DAB62D, 0xBF2ACA7E, 0x42D22CA0},
            {0xC0DAB62D, 0xBF2ACA7E, 0x42D22CA0},
            {0xC0DAB62D, 0xBF2ACA7E, 0x42D22CA0},
        },
        {   // step 2
            {0xC0D5BC3A, 0xBFAAACB3, 0x42D23B86},
            {0xC0D5BE04, 0xBFAAAD73, 0x42D23A74},
            {0xC0D5BFC9, 0xBFAAAE32, 0x42D23962},
            {0xC0D5C190, 0xBFAAAEF1, 0x42D23850},
            {0xC0D5C352, 0xBFAAAFAE, 0x42D2373E},
        },
        {   // step 3
            {0xC0CFE08C, 0xBFFF92DD, 0x42D2663F},
            {0xC0CFE08C, 0xBFFF92DD, 0x42D2663F},
            {0xC0CFE08C, 0xBFFF92DD, 0x42D2663F},
            {0xC0CFE08C, 0xBFFF92DD, 0x42D2663F},
            {0xC0CFE08C, 0xBFFF92DD, 0x42D2663F},
        },
        {   // step 4
            {0xC0C98411, 0xC02A128E, 0x42D27525},
            {0xC0C97D8D, 0xC02A0FC9, 0x42D278FC},
            {0xC0C976F2, 0xC02A0CFA, 0x42D27CD2},
            {0xC0C97040, 0xC02A0A22, 0x42D280A9},
            {0xC0C96975, 0xC02A073E, 0x42D28480},
        },
        {   // step 5
            {0xC0C25E21, 0xC054072E, 0x42D29367},
            {0xC0C26815, 0xC0540C84, 0x42D28D75},
            {0xC0C271D3, 0xC05411BC, 0x42D28782},
            {0xC0C27B58, 0xC05416D6, 0x42D2818F},
            {0xC0C284A6, 0xC0541BD3, 0x42D27B9C},
        },
        {   // step 6
            {0xC0BA9733, 0xC07DA51B, 0x42D2AA9D},
            {0xC0BA9A92, 0xC07DA74C, 0x42D2A892},
            {0xC0BA9DE9, 0xC07DA977, 0x42D2A686},
            {0xC0BAA139, 0xC07DAB9F, 0x42D2A47B},
            {0xC0BAA483, 0xC07DADC2, 0x42D2A270},
        },
        {   // step 7
            {0xC0B23F94, 0xC0937479, 0x42D2B155},
            {0xC0B23F94, 0xC0937479, 0x42D2B155},
            {0xC0B23F94, 0xC0937479, 0x42D2B155},
            {0xC0B23F94, 0xC0937479, 0x42D2B155},
            {0xC0B23F94, 0xC0937479, 0x42D2B155},
        },
        {   // step 8
            {0xC0A93226, 0xC0A7D354, 0x42D2C03B},
            {0xC0A92C7E, 0xC0A7D0D0, 0x42D2C432},
            {0xC0A926BF, 0xC0A7CE40, 0x42D2C829},
            {0xC0A920EA, 0xC0A7CBA8, 0x42D2CC20},
            {0xC0A91AFC, 0xC0A7C904, 0x42D2D017},
        },
    };

    // Measured on this tree against every row above: the worst relative delta over
    // the 8 steps x 5 alphas x 9 view values is 1.29377659e-05, and over the +X
    // place rows it is 1.50669503e-05. The place is ~100 units, so this is the
    // project's LUT trig and reciprocal normalize rather than the blend. The
    // 2e-05 relative tolerance therefore has 1.55x headroom over the view rows and
    // 1.33x over the worst (+X place) row, and remains far below what a wrong sign, a
    // swapped sine and cosine, a missing term or a dropped pivot/roll interpolation
    // would produce.
    const float kTolerance = 2e-5f;
    auto nearOracle = [&](float actual, uint32_t expectedBits) {
        uint32_t e = expectedBits;
        float want;
        memcpy(&want, &e, sizeof(want));
        const float scale = fabsf(want) > 1.0f ? fabsf(want) : 1.0f;
        return fabsf(actual - want) <= kTolerance * scale;
    };
    auto checkView = [&](const Camera& camera, const uint32_t* want, const char* what, int step,
                         int alpha) {
        const float got[9] = {camera.position.x, camera.position.y, camera.position.z,
                              camera.forward.x, camera.forward.y, camera.forward.z,
                              camera.up.x,       camera.up.y,       camera.up.z};
        for (int i = 0; i < 9; i++) {
            if (!nearOracle(got[i], want[i])) {
                printf("  FAIL: %s step %d alpha %d value %d was %.9g, the oracle has %08X\n", what,
                       step, alpha, i, got[i], want[i]);
                failures++;
                return;
            }
        }
    };
    auto sameBits = [](const Vec3& a, const Vec3& b) {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    };

    const float kAspect = 4.0f / 3.0f;
    const float kStepSeconds = 1.0f / 60.0f;
    const float kAlphas[5] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};

    FollowCamera follow;
    Player player;
    Camera step;
    Camera blend;

    // Before the first step there is no Aim to interpolate, and the C# returns with
    // the camera untouched (FollowCamera.cs:179-182, `if (_previousAim is not {})`).
    // A blend that placed the view here would show a frame of a camera that has never
    // seen Kurt at all -- the blank-first-frame class of bug.
    Camera untouched;
    CHECK(!follow.blend(&untouched, Vec3{0.0f, 0.0f, 0.0f}, 0.0f, 0.5f, kAspect));
    Camera fresh;
    CHECK(memcmp(&untouched, &fresh, sizeof(Camera)) == 0);

    for (int s = 1; s <= 8; s++) {
        player.feet = Vec3{s * 0.125f, -s * 0.25f, 100.0f + s * 0.05f};
        player.yaw = s * 3.0f;
        player.verticalSpeed = (s % 4 == 0) ? -20.0f : 0.0f;
        player.onFloor = (s % 4) != 0;
        Input input;
        input.mouseY = (s % 3 == 0) ? 2.0f : -1.0f;
        // The mouse goes in through mouseLook, which consumes it, so the step adds no
        // mouse of its own -- the side the port takes, and the side the probe drives.
        follow.mouseLook(input);
        follow.update(&step, nullptr, player, 4.0f, kStepSeconds, kAspect);
        checkView(step, kStep[s - 1], "step", s, -1);

        for (int i = 0; i < 5; i++) {
            CHECK(follow.blend(&blend, player.feet, player.yaw, kAlphas[i], kAspect));
            checkView(blend, kBlend[s - 1][i], "blend", s, i);
            if (s == 1 || kAlphas[i] == 1.0f) {
                CHECK(sameBits(blend.position, step.position));
                CHECK(sameBits(blend.forward, step.forward));
                CHECK(sameBits(blend.up, step.up));
            }

            // Kurt drawn 1 further along +X: the place moves with him and the
            // orientation does not (the oracle's own assertion, to four decimals;
            // the port lands on the same bits the oracle prints).
            CHECK(follow.blend(&blend, Vec3{player.feet.x + 1.0f, player.feet.y, player.feet.z},
                               player.yaw, kAlphas[i], kAspect));
            const float shifted[3] = {blend.position.x, blend.position.y, blend.position.z};
            for (int v = 0; v < 3; v++) {
                if (!nearOracle(shifted[v], kShift[s - 1][i][v])) {
                    printf("  FAIL: shifted step %d alpha %d place %d was %.9g, the oracle has %08X\n",
                           s, i, v, shifted[v], kShift[s - 1][i][v]);
                    failures++;
                }
            }
        }
    }
}

// The Aim triple's other two components have to move, or a blend that drops the
// pivot or the roll still passes the test above. This is a second oracle probe
// (`followblend2`, .NET 10): the first step has the default pivot 4.5 and no
// bank, then the pivot moves to 7.25 and three CameraRoll.Walk calls bank the
// view by 0.375 degrees. The second step records that Aim, and the blends below
// interpolate all three components. A mutant that restores the default pivot or
// a zero roll changes the place or the up vector by far more than the trig
// tolerance below.
void testFollowCameraBlendInterpolatesThePivotAndRoll() {
    static const uint32_t kRows[5][9] = {
        {0xC0689B08, 0xC0289B08, 0x4110EDC8, 0x3F349411, 0x3F349411, 0xBD8EDC7B,
         0x3D4A094C, 0x3D4A094C, 0x3F7F605B},
        {0xC0689B08, 0xC0289B08, 0x411BEDC8, 0x3F349411, 0x3F349411, 0xBD8EDC7B,
         0x3D4EC66F, 0x3D454C07, 0x3F7F6045},
        {0xC0689B08, 0xC0289B08, 0x4126EDC8, 0x3F349411, 0x3F349411, 0xBD8EDC7B,
         0x3D53836C, 0x3D408E9E, 0x3F7F6001},
        {0xC0689B08, 0xC0289B08, 0x4131EDC8, 0x3F349411, 0x3F349411, 0xBD8EDC7B,
         0x3D584045, 0x3D3BD115, 0x3F7F5F91},
        {0xC0689B08, 0xC0289B08, 0x413CEDC8, 0x3F349411, 0x3F349411, 0xBD8EDC7B,
         0x3D5CFCF8, 0x3D37136A, 0x3F7F5EF5},
    };
    const float kAlphas[5] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    const float kTolerance = 2e-5f;
    const float kAspect = 4.0f / 3.0f;
    const float kStep = 1.0f / 60.0f;

    FollowCamera follow;
    Player player;
    Camera stepped;
    Camera blended;
    player.feet = Vec3{2.0f, 3.0f, 4.0f};
    player.yaw = 45.0f;
    player.onFloor = true;
    follow.update(&stepped, nullptr, player, 4.0f, kStep, kAspect);

    // The oracle drives CameraRoll.Walk directly. The port's one writer is
    // Player::update, so three held walk steps make the same 0.375-degree bank.
    Input input;
    input.down[kKeyForward] = true;
    input.down[kKeyTurnRight] = true;
    for (int i = 0; i < 3; i++) {
        player.update(input, kStep);
    }
    player.feet = Vec3{2.0f, 3.0f, 4.0f};
    player.yaw = 45.0f;
    player.verticalSpeed = 0.0f;
    player.onFloor = true;
    player.cameraPivot = 7.25f;
    CHECK(floatBits(player.cameraRoll().roll()) == 0x3EC00000u);
    follow.update(&stepped, nullptr, player, 4.0f, kStep, kAspect);

    for (int i = 0; i < 5; i++) {
        CHECK(follow.blend(&blended, player.feet, player.yaw, kAlphas[i], kAspect));
        const float got[9] = {blended.position.x, blended.position.y, blended.position.z,
                              blended.forward.x, blended.forward.y, blended.forward.z,
                              blended.up.x,       blended.up.y,       blended.up.z};
        for (int v = 0; v < 9; v++) {
            uint32_t bits = kRows[i][v];
            float want;
            memcpy(&want, &bits, sizeof(want));
            const float scale = fabsf(want) > 1.0f ? fabsf(want) : 1.0f;
            if (fabsf(got[v] - want) > kTolerance * scale) {
                printf("  FAIL: pivot/roll blend alpha %d value %d was %.9g, the oracle has %08X\n",
                       i, v, got[v], kRows[i][v]);
                failures++;
                return;
            }
        }
    }
}

// FollowCamera::update's arena-pitch ease is the other live float.Lerp site.
// These are the oracle's own bits after the targets 12, -3, 40, 0 and 17.5
// degrees (`followblend2` reads the C#'s private _arenaPitch by reflection).
// The fused spelling and the old twice-rounded `target + (old-target)*ease`
// differ on steps 3, 4 and 5 (re-measured 2026-11-05: 40DAC5F8/40C9B2F4/40E5A9B0
// against the three rows below), so this pin fails if that exact defect returns;
// the view-level tolerance above cannot see a one-ULP pitch shift.
//
// These five rows do NOT separate the fused form from the twice-rounded product
// sum `target*(1-ease) + old*ease`: both reproduce all five bits. The huge-target
// pair below DOES separate them at this camera site. It is the same 1e30/-1e30
// pair as the discriminating edge row in system_numerics_tests.cpp; after the
// first update the old pitch is 0x6F7C2DC4, and the second update's oracle bits
// are 0xED9D738D. The product sum gives 0xED9D7390 there. The two rows were
// measured from the C# oracle on .NET 10 (probe mode `cameraminmax`, 2026-11-05),
// not read off this port.
void testFollowCameraArenaPitchUsesTheOraclesEaseRows() {
    static const uint32_t kExpected[5] = {
        0x4093FACBu, 0x4080F02Au, 0x40DAC5FDu, 0x40C9B2F8u, 0x40E5A9B3u};
    static const float kTargets[5] = {12.0f, -3.0f, 40.0f, 0.0f, 17.5f};

    FollowCamera follow;
    Player player;
    Camera camera;
    player.onFloor = true;
    for (int i = 0; i < 5; i++) {
        follow.update(&camera, nullptr, player, kTargets[i], 1.0f / 60.0f,
                      4.0f / 3.0f);
        if (floatBits(follow.arenaPitch()) != kExpected[i]) {
            printf("  FAIL: arena pitch step %d was %08X, the oracle has %08X\n", i,
                   floatBits(follow.arenaPitch()), kExpected[i]);
            failures++;
            return;
        }
    }

    FollowCamera huge;
    huge.update(&camera, nullptr, player, 1.0e30f, 1.0f / 60.0f, 4.0f / 3.0f);
    if (floatBits(huge.arenaPitch()) != 0x6F7C2DC4u) {
        printf("  FAIL: huge arena pitch step 1 was %08X, the oracle has 6F7C2DC4\n",
               floatBits(huge.arenaPitch()));
        failures++;
        return;
    }
    huge.update(&camera, nullptr, player, -1.0e30f, 1.0f / 60.0f, 4.0f / 3.0f);
    if (floatBits(huge.arenaPitch()) != 0xED9D738Du) {
        printf("  FAIL: huge arena pitch step 2 was %08X, the oracle has ED9D738D "
               "(the product sum gives ED9D7390)\n",
               floatBits(huge.arenaPitch()));
        failures++;
        return;
    }
}

// FollowCamera's five MathF.Min/MathF.Max sites are the C#'s MathF operations,
// not ordinary std::min/std::max. The helper propagates a NaN and keeps the
// oracle's sign-of-zero order; the C++ standard pair does neither. These rows are
// the C# oracle's own bits (probe mode `cameraminmax`, .NET 10, 2026-11-05).
//
// The signed-zero row reaches FollowCamera.Climb's MathF.Min arm: the initial
// climb times 0.97f is exactly -0.02f, adding one return step is exactly +0.0f,
// and the rise is -0.0f. MathF.Min(+0.0f, -0.0f) is -0.0f (0x80000000), while
// std::min returns its first argument there and therefore gives +0.0f.
//
// The NaN row reaches the air-pitch MathF.Max arm: a NaN step makes the air time
// NaN, the inner MathF.Min is NaN, and the outer MathF.Max must stay NaN.
// std::max(_airPitch, NaN) instead returns the finite first argument, zero.
// A NaN rise does NOT reach Climb's MathF.Min: the oracle's `rise >= 0f` guard
// is false for NaN, so Climb returns its already-NaN eased value. The direct
// NaN-climb row below pins that path too.
void testFollowCameraMathFMinMaxMatchTheOracle() {
    const float kNaN = nanFloat();

    const float signedClimb =
        FollowCamera::climbStep(-0.020618556067347527f, -0.0f, 1.0f);
    CHECK(floatBits(signedClimb) == 0x80000000u);

    const float nanClimb = FollowCamera::climbStep(-0.1f, kNaN, 1.0f);
    CHECK(nanClimb != nanClimb);

    FollowCamera follow;
    Player player;
    Camera camera;
    player.verticalSpeed = -20.0f;
    player.onFloor = false;
    follow.update(&camera, nullptr, player, 4.0f, kNaN, 4.0f / 3.0f);
    CHECK(follow.airPitch() != follow.airPitch());

    FollowCamera finite;
    player.verticalSpeed = 0.0f;
    player.onFloor = true;
    finite.update(&camera, nullptr, player, 4.0f, 1.0f / 60.0f, 4.0f / 3.0f);
    CHECK(finite.airPitch() == 0.0f);
}

// CameraRoll's two clamps are MathF.Min/MathF.Max in the oracle (CameraRoll.cs:47,52),
// so a NaN bank STAYS NaN instead of snapping to +/-10 the way fminf/fmaxf would. No
// schedule reaches this -- nothing in the golden can make the step NaN -- which is why
// it lives here and not in the golden, and it is the same rule and the same mutant as
// the eight speed clamps in script_runtime.cpp (review-20 F2): swap the pair for the
// fminf/fmaxf ones and the first check below fails, because fminf(NaN,10) is 10.
//
// The name says "where the oracle throws" because the LAST check is NOT parity: the
// leveller reaches MathF.Sign(NaN), and MathF.Sign(NaN) throws ArithmeticException
// (measured on .NET 10, probe `nanclamp` row `SIGN nan`). This port's signf returns 0, so
// the roll stays NaN and the game runs where the oracle would have faulted. That is a
// documented divergence (camera_roll.cpp's signf comment), not a claim that the oracle
// produces NaN here -- review-24 F3, so do not rename this back to "...LikeTheOracle".
void testCameraRollNaNStaysNaNWhereTheOracleThrows() {
    const float kNaN = nanFloat();
    const float kStep = 1.0f / 60.0f;

    CameraRoll roll;
    roll.walk(1.0f, 1.0f, kNaN);              // the gain arm: MathF.Min(NaN, 10)
    CHECK(roll.roll() != roll.roll());
    roll.walk(1.0f, 1.0f, kStep);             // a normal step after it does not heal it
    CHECK(roll.roll() != roll.roll());
    roll.walk(-1.0f, 1.0f, kStep);            // the brake arm: MathF.Max(NaN, -10)
    CHECK(roll.roll() != roll.roll());
    roll.settle(kStep);                       // the leveller neither fixes nor hides it:
                                              // signf returns 0 where the oracle throws
    CHECK(roll.roll() != roll.roll());

    // The other half of the same claim: with a finite step the clamps are ordinary
    // clamps, so this is not a NaN that would survive anything.
    CameraRoll finite;
    for (int i = 0; i < 200; i++) {
        finite.walk(1.0f, 1.0f, kStep);
    }
    CHECK(finite.roll() == 10.0f);            // the +limit, held against
    finite.settle(kStep);                     // the held step spends itself
    for (int i = 0; i < 400; i++) {
        finite.settle(kStep);
    }
    CHECK(finite.roll() == 0.0f);             // and the leveller reaches EXACTLY zero
}

// The oracle's OWN unit test for this class is tests/Mdk.Game.Tests/KurtMovesTests.cs's
// RunningAndTurningRollsTheView (Tick = 1/30), and it is the specification the C++ twin
// has to meet assertion for assertion -- the same relationship vm_tests.cpp has to
// ScriptVmTests.cs. Every number below is that test's, in its order:
//   1.0   -- four (walk, settle) pairs at 0.25 degrees a tick, the settle each time
//            spent on the held step rather than levelling;
//   10.0  -- a hundred more, against the +10 limit;
//   7.75  -- one step the other way: -0.25 for the step and -2 for the reverse jump;
//   5.25  -- two settles, the first the held step again and the second the 2.5 clamp;
//   0.0   -- a hundred settles reach EXACTLY zero, not a small number;
//   0.0   -- and turning on the spot (no walk axis) does not roll at all.
// It is data-independent, so it runs on a machine with no game files, which the golden
// cannot claim.
void testCameraRollMatchesTheOraclesOwnUnitTest() {
    const float kTick = 1.0f / 30.0f;   // KurtMovesTests.Tick
    const float kTolerance = 1e-4f;     // Assert.Equal(x, y, 4)

    CameraRoll roll;
    for (int i = 0; i < 4; i++) {
        roll.walk(1.0f, 1.0f, kTick);
        roll.settle(kTick);
    }
    CHECK(nearFloat(roll.roll(), 1.0f, kTolerance));
    for (int i = 0; i < 100; i++) {
        roll.walk(1.0f, 1.0f, kTick);
        roll.settle(kTick);
    }
    CHECK(nearFloat(roll.roll(), 10.0f, kTolerance));
    roll.walk(-1.0f, 1.0f, kTick);
    CHECK(nearFloat(roll.roll(), 7.75f, kTolerance));
    roll.settle(kTick);
    roll.settle(kTick);
    CHECK(nearFloat(roll.roll(), 5.25f, kTolerance));
    for (int i = 0; i < 100; i++) {
        roll.settle(kTick);
    }
    CHECK(roll.roll() == 0.0f);
    roll.walk(1.0f, 0.0f, kTick);       // turning on the spot
    roll.settle(kTick);
    CHECK(roll.roll() == 0.0f);
}

// The animation wrap is the C# Wrap's exact reduction: a positive divisor
// leaves the first remainder in (-count, count), so one conditional add is
// enough. Zero or a negative count is the C# divide-by-zero path; the port
// returns frame 0 rather than crashing.
void testWrapAnimationFrameIsTheOracleReduction() {
    CHECK(Player::wrapAnimationFrame(0, 5) == 0);
    CHECK(Player::wrapAnimationFrame(4, 5) == 4);
    CHECK(Player::wrapAnimationFrame(5, 5) == 0);
    CHECK(Player::wrapAnimationFrame(7, 5) == 2);
    CHECK(Player::wrapAnimationFrame(-1, 5) == 4);
    CHECK(Player::wrapAnimationFrame(-6, 5) == 4);
    CHECK(Player::wrapAnimationFrame(0, 0) == 0);
    CHECK(Player::wrapAnimationFrame(-1, 0) == 0);
}

// Death uses the C#'s complete Fall operation, not only its move. FallMove's
// degenerate sweep hits nothing and clears OnFloor, and the post-move
// fall-out test then decides what happens next. On a real floor the corpse
// re-lands on the next step; below the arena the fall-out test sets OnFloor
// back to true and the oracle holds the corpse at the death point.
void testDeathUsesTheOraclesCompleteFallOperation(LevelData* levelData) {
    ArenaSpace space;
    for (const formats::Arena* arena : levelData->arenas()) {
        if (levelData->isReachable(arena->name())) {
            space.add(arena);
        }
    }

    const float* start = levelData->dti().startPosition();
    const Vec startPoint{start[0], start[1], start[2]};
    const std::string* arenaName = space.arenaAt(startPoint);
    CHECK(arenaName != nullptr);
    if (arenaName == nullptr) {
        return;
    }
    space.setSolid({*arenaName});

    Input input;
    const float delta = 1.0f / 30.0f;

    // A corpse on a real floor clears OnFloor in the first death step. Its
    // next gravity move lands again and the creep snap restores the feet.
    ArenaSpace::Floor floor;
    const bool haveFloor = space.floorCrossing(
        Vec{start[0], start[1], start[2] + 100.0f},
        Vec{start[0], start[1], start[2] - 100.0f}, &floor);
    CHECK(haveFloor);
    if (haveFloor) {
        Player player;
        player.feet = Vec3{floor.point.x, floor.point.y, floor.point.z};
        player.setSpace(&space);
        player.setHealth(0);
        player.dead = true;
        player.onFloor = true;

        player.update(input, delta);
        CHECK(!player.onFloor);
        CHECK(fabsf(player.feet.z - floor.point.z) < 1e-4f);

        player.update(input, delta);
        CHECK(player.onFloor);
        CHECK(fabsf(player.feet.z - floor.point.z) < 1e-4f);
    }

    // Falling out of the arena sets OnFloor so the death plays. The oracle's
    // Fall then repeats that post-move assignment every death step, so the
    // corpse stays at the death point instead of falling farther.
    float bottom = 0.0f;
    CHECK(space.bottom(startPoint, &bottom));
    if (!space.bottom(startPoint, &bottom)) {
        return;
    }
    Player fallen;
    fallen.feet = Vec3{start[0], start[1], bottom - 51.0f};
    fallen.setSpace(&space);

    fallen.update(input, delta);
    CHECK(fallen.dead);
    CHECK(fallen.health() == 0);
    CHECK(fallen.onFloor);
    const float deathZ = fallen.feet.z;

    for (int i = 0; i < 10; i++) {
        fallen.update(input, delta);
        CHECK(fallen.onFloor);
        CHECK(fabsf(fallen.feet.z - deathZ) < 1e-6f);
    }
}

// The knock-down's own rules, without any geometry: what KnockDown writes, what stops it
// from firing twice, and what hurt() does with a Kurt who is on the floor of his animation.
// The golden pins the same rules in the game; these pin the branches a schedule cannot
// reach -- a Player with no arena space at all, and a counter raised again while he is
// already down.
void testKnockDownSpendsTheCounterAndOpensTheTimer() {
    Player::FrameCounts counts;
    // The archive's real counts (the ones L3_kurt_steps.txt prints in its header:
    // Knocked=25, GetUp=27), so one step at 1/60 leaves him inside K_BANG.
    counts.values[Kurt::Knocked] = 25;
    counts.values[Kurt::GetUp] = 27;

    Input input;
    Player player;
    player.setFrameCounts(counts);
    player.onFloor = true;
    player.combat().setKnockDamage(Kurt::kKnockdownDamage);
    player.update(input, 1.0f / 60.0f);
    CHECK(player.state() == Kurt::Knocked);
    CHECK(player.combat().knockDamage() == 0.0f);   // the counter is spent, not left to drain
    CHECK(player.combat().invulnerable() == Kurt::kKnockdownInvulnerability);
    CHECK(player.health() == Kurt::kMaxHealth);     // a knock-down costs no health

    // The C#'s !Knocked guard: a counter raised again while he is already down must not
    // re-open the timer or restart the animation, and it must still drain.
    player.combat().setKnockDamage(Kurt::kKnockdownDamage);
    player.update(input, 1.0f / 60.0f);
    CHECK(player.state() == Kurt::Knocked);
    CHECK(player.combat().invulnerable() < Kurt::kKnockdownInvulnerability);
    CHECK(player.combat().knockDamage() < Kurt::kKnockdownDamage);

    // hurt() refuses him for the STATE, with the timer it opened written down to zero --
    // the arm of Kurt.Combat.cs:125 batch 41 could not reach because nothing entered
    // Knocked. The golden reaches it in the game; this pins it without one.
    player.combat().setInvulnerable(0.0f);
    player.combat().hurt(20);
    CHECK(player.health() == Kurt::kMaxHealth);
    CHECK(player.combat().knockDamage() == 0.0f);   // hurt()'s guard zeroes the counter

    // In the air with no arena space there is no floor to find, so no slam and no
    // knock-down: the counter just drains. The C# always has a space to ask; an empty
    // arena list is its answer, and this is that answer, not a substitute for the query.
    Player air;
    air.setFrameCounts(counts);
    air.onFloor = false;
    air.combat().setKnockDamage(Kurt::kKnockdownDamage);
    air.update(input, 1.0f / 60.0f);
    CHECK(air.state() != Kurt::Knocked);
    CHECK(air.combat().knockDamage() < Kurt::kKnockdownDamage);
}

// The airborne arm of the trigger, on real geometry: a floor inside the 13-unit probe
// slams him to it at KnockdownSlamSpeed and knocks him down; the same height over no
// geometry does neither. This is the arm golden/L3_kurt_knockdown.txt reaches at step 157,
// pinned here without the schedule around it.
void testAirborneKnockDownSlamsToTheFloorTheProbeReaches(LevelData* levelData) {
    ArenaSpace space;
    for (const formats::Arena* arena : levelData->arenas()) {
        if (levelData->isReachable(arena->name())) {
            space.add(arena);
        }
    }
    const float* start = levelData->dti().startPosition();
    // Where the floor actually is, found the way the death test finds it: the level's own
    // start does not necessarily sit on its floor, and a fixture that assumed it would
    // assert nothing. Then the knock-down's own height: 6 above that surface, probed 13
    // down from there.
    const Vec high{start[0], start[1], start[2] + 100.0f};
    const Vec low{start[0], start[1], start[2] - 100.0f};
    Vec pad = {0.0f, 0.0f, 0.0f};
    CHECK(space.floor(high, low, &pad));
    const Vec at{pad.x, pad.y, pad.z + 6.0f};
    const Vec down{at.x, at.y, at.z - Kurt::kKnockdownFloorDistance};

    // The setup has to be real or the test would prove nothing: a floor must be inside the
    // probe, which is what makes the next CHECK a verdict and not a skip.
    Vec found = {0.0f, 0.0f, 0.0f};
    CHECK(space.floor(at, down, &found));
    CHECK(fabsf(found.z - pad.z) < 1e-3f);

    Player::FrameCounts counts;
    counts.values[Kurt::Knocked] = 25;
    counts.values[Kurt::GetUp] = 27;
    Input input;
    Player player;
    player.setSpace(&space);
    player.setFrameCounts(counts);
    player.feet = Vec3{at.x, at.y, at.z};
    player.onFloor = false;
    player.combat().setKnockDamage(Kurt::kKnockdownDamage);
    player.update(input, 1.0f / 60.0f);
    CHECK(player.state() == Kurt::Knocked);
    // The slam: min(verticalSpeed, -64) and then the step's own gravity, so he is going
    // at least as fast as the slam speed by the end of the step that knocked him down.
    CHECK(player.verticalSpeed <= Kurt::kKnockdownSlamSpeed);
}

// Opcode 248 must pass the decoder's variable-arity operand list through the VM into the
// live Player. The L3 golden drives ScriptRuntime::pushKurt directly to cover multiple
// branches in one long schedule; this executes the real bytecode dispatch and its mode-1
// list shape, which a regression back to an unimplemented case would otherwise miss.
void testPushKurtDispatchesDecodedListToPlayer(LevelData* levelData, const Cmi* cmi,
                                                audio::SoundBank* bank) {
    Player player;
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, &player, nullptr);
    player.feet = runtime.kurtPosition();
    scripts::MdkObject obj;
    obj.position = Vec3{player.feet.x - 5.0f, player.feet.y, player.feet.z};

    Code code;
    // readOp248Args: mode=1, a=15, up=8 -> [1, 15, 8]. The object is 5 units
    // behind Kurt, so the C# away vector is (+5, 0), normalized and scaled to 15.
    code.u8(kPushKurt).u8(1).f32(15.0f).f32(8.0f).end();
    runProgram(runtime, code, &obj);

    CHECK(!sawUnimplemented(kPushKurt));
    CHECK(player.state() == Kurt::Knocked);
    CHECK(player.combat().invulnerable() == Kurt::kKnockdownInvulnerability);
    const Vec3 velocity = player.velocity();
    CHECK(fabsf(velocity.x - 15.0f) < 1e-5f);
    CHECK(fabsf(velocity.y) < 1e-5f);
    CHECK(fabsf(velocity.z - 8.0f) < 1e-5f);

    // C# Vector2.Normalize(away) * a divides each component by the length before
    // multiplying. This chosen input distinguishes that from away * (a / length)
    // by one float ULP; retaining only four printed decimals would hide the change.
    Player ordered;
    scripts::ScriptRuntime orderedRuntime(levelData, cmi, 1, bank, &ordered, nullptr);
    ordered.feet = orderedRuntime.kurtPosition();
    // Put both vectors at moderate magnitudes: subtraction from the level's ~190-unit
    // Z coordinate would erase the low bits that make this exact-rounding case useful.
    ordered.feet.x = -456.5494384765625f;
    ordered.feet.y = 56.5562744140625f;
    orderedRuntime.update(1.0f / 30.0f);  // runTick snapshots these feet as KurtPosition
    ordered.feet.y += 10.0f;  // live feet now differ; opcode must still read the snapshot
    scripts::MdkObject offset;
    offset.position = Vec3{0.0f, 0.0f, ordered.feet.z};
    // kurtPosition - object.position = (-456.5494, +56.5563); negative a makes +X.
    // Use the C#'s divide-then-multiply order and assert the exact float bits via equality.
    std::vector<formats::ScriptValue> args;
    args.push_back(formats::scriptInt(1));
    args.push_back(formats::scriptFloat(-978.97198486328125f));
    args.push_back(formats::scriptFloat(0.0f));
    orderedRuntime.pushKurt(&offset, args);
    const Vec3 orderedVelocity = ordered.velocity();
    CHECK(orderedVelocity.x == 971.54583740234375f);
    CHECK(orderedVelocity.y == -120.35282897949219f);
}

// The exact-float twin of the golden's step 20, and the reason it exists is a real one:
// the golden prints four decimals, and with operands whose b and up are both zero the
// oracle's asymmetric frame formula, its args[3] lift, and the "obvious" alternatives all
// print the same thing. Two mutants -- "correct" the formula into a real rotation, or read
// the lift from args[2] instead of args[3] -- survived the whole golden and the whole VM
// suite until this case was written with every operand non-zero. These are exact equality
// on the C# probe's own round-trip output for this operand list.
void testPushKurtModeZeroFrameAndLiftAreExact(LevelData* levelData, const Cmi* cmi,
                                              audio::SoundBank* bank) {
    Player player;
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, &player, nullptr);
    player.feet = runtime.kurtPosition();
    scripts::MdkObject obj;
    obj.yaw = 37.0f;
    obj.position = runtime.kurtPosition();

    // readOp248Args with mode 0 reads TWO floats after a: [0, a, b, up]. The arity is
    // part of what is pinned -- a fixed three-element list would feed b as the lift.
    Code code;
    code.u8(kPushKurt).u8(0).f32(20.0f).f32(5.0f).f32(3.0f).end();
    runProgram(runtime, code, &obj);

    CHECK(!sawUnimplemented(kPushKurt));
    CHECK(player.state() == Kurt::Knocked);
    const Vec3 velocity = player.velocity();
    // x = -a*c - b*s and y = -b*c - a*s at yaw 37 (asymmetric: swapping a and b moves
    // both), and the lift is args[3]'s 3, NOT args[2]'s 5.
    CHECK(velocity.x == -18.981785f);
    CHECK(velocity.y == -16.029478f);
    CHECK(velocity.z == 3.0f);
}

// ArenaSpace::floor's own contract, the parts a schedule cannot show: it answers the
// trigger's question (a floor below, and where it is), and when nothing is found it leaves
// the out point at the segment's own end, which is the value the C# initialises `point`
// to before it searches (ArenaSpace.cs:193). The nearest-vs-first and both-ends rules that
// separate it from FloorCrossing are pinned by golden/L3_floor_probes.txt, which prints
// both probes over every reachable arena and does show them part company on two of the
// level's long segments.
void testFloorProbeAnswersWhatTheTriggerAsks(LevelData* levelData) {
    ArenaSpace space;
    for (const formats::Arena* arena : levelData->arenas()) {
        if (levelData->isReachable(arena->name())) {
            space.add(arena);
        }
    }
    const float* start = levelData->dti().startPosition();
    const Vec high{start[0], start[1], start[2] + 100.0f};
    const Vec low{start[0], start[1], start[2] - 100.0f};

    Vec hit = {0.0f, 0.0f, 0.0f};
    CHECK(space.floor(high, low, &hit));
    CHECK(hit.z < high.z);

    ArenaSpace empty;
    Vec kept = {1.0f, 2.0f, 3.0f};
    CHECK(!empty.floor(high, low, &kept));
    CHECK(kept.x == low.x && kept.y == low.y && kept.z == low.z);
}

// The solid-arena pin is STICKY, and that is the whole reason the frontend has to
// refresh it as Kurt moves (review-8 F1). While `_solid` is set, `move()` collides
// with it and only it (arena_space.cpp:118) and `kurts()` returns it and only it
// (:174-181), so the arenas around the feet are not consulted at all; `bottom()`
// keeps the pinned arena even when the feet are in no arena (:160), which is what
// lets a fall out of the world reach a bottom and die. Pinned here because the
// frontend that got it wrong was invisible to every host gate: `main_3ds.cpp`
// includes <3ds.h>, so the shape is pinned by main_3ds_flow_test.sh and the
// SEMANTICS it depends on are pinned here.
void testTheSolidArenaPinIsStickyAndReplacesTheArenasAroundTheFeet(LevelData* levelData) {
    ArenaSpace space;
    for (const formats::Arena* arena : levelData->arenas()) {
        if (levelData->isReachable(arena->name())) {
            space.add(arena);
        }
    }
    const float* start = levelData->dti().startPosition();
    // The frontend's own spawn: the start position raised by StartDrop.
    const Vec at{start[0], start[1], start[2] + 3.0f};
    const std::string* spawn = space.arenaAt(at);
    CHECK(spawn != nullptr);
    if (spawn == nullptr) {
        return;
    }

    // Non-vacuity: with the pin on the arena that actually holds him, a downward
    // move meets geometry. Level 7 spawns Kurt 12 units above DANT_1's floor at
    // z=0, so the probe has to be longer than that; a 1-unit probe hits nothing
    // and would make the comparison below prove nothing.
    space.setSolid({*spawn});
    const ArenaSpace::Result own = space.move(at, Vec{0.0f, 0.0f, -50.0f},
                                              ArenaSpace::kMotionFall, 0.0f);
    CHECK(own.hit());
    if (!own.hit()) {
        return;
    }

    // Pin a DIFFERENT arena and the same move at the same feet loses its floor:
    // the pin replaces the by-bounds arenas, it does not add to them. This is the
    // defect from the other side — a pin left at the spawn does to the arena Kurt
    // walked into exactly what this does to the arena he is standing in.
    const std::string* other = nullptr;
    for (const formats::Arena* arena : levelData->arenas()) {
        if (levelData->isReachable(arena->name()) && arena->name() != *spawn) {
            other = &arena->name();
            break;
        }
    }
    CHECK(other != nullptr);
    if (other == nullptr) {
        return;
    }
    space.setSolid({*other});
    const ArenaSpace::Result stale = space.move(at, Vec{0.0f, 0.0f, -50.0f},
                                                ArenaSpace::kMotionFall, 0.0f);
    CHECK(!stale.hit());

    // And the pin is what a Kurt outside every arena still has. Below the world,
    // no arena contains his feet, so with no pin bottom() answers false and the
    // fall-out death never fires; pinned, it still answers with his arena's bottom.
    const Vec below{at.x, at.y, -1000.0f};
    float bottom = 0.0f;
    space.setSolid({*spawn});
    CHECK(space.bottom(below, &bottom));
    const float pinned = bottom;
    ArenaSpace unpinned;
    for (const formats::Arena* arena : levelData->arenas()) {
        if (levelData->isReachable(arena->name())) {
            unpinned.add(arena);
        }
    }
    float lost = 0.0f;
    CHECK(!unpinned.bottom(below, &lost));
    CHECK(pinned > -1000.0f);
}

// The oracle's collision world is a LIST, and this is the only place the port
// EXECUTES the claims that list rests on: that it is the runtime's DRAWN one
// (current arena plus the active second, ScriptRuntime.cs:265-282 and UpdateArenas
// :658-664), that ArenaSpace pins both names, and that the list is a SNAPSHOT taken at
// the tick's UpdateArenas point rather than a live read of the three fields.
// The frontends' own gates (main_3ds_flow_test.sh,
// frontend_solid_pin_test.sh) can only read the shape of the call; what they cannot
// see is whether the value it passes is the oracle's. Review-6 F1 is the failure
// this pins: batch 99 passed ONE name where the oracle passes two, and the room
// behind the door went un-solid under Kurt, which is the last move here, executed.
//
// The walk is the oracle's own DoorArenaTests geometry on level 7 (the same one
// golden/L7_crossing.txt records): the room DANT_1, the tunnel DANT_2, the doorway
// connection 1001 on the plane y = 673, floor z = -3.
void testSolidArenasAreTheDrawnListAndThePinTakesBoth(LevelData* levelData, const Cmi* cmi,
                                                     audio::SoundBank* bank) {
    const float kFloor = -3.0f;   // DoorArenaTests.Floor
    ArenaSpace space;
    for (const formats::Arena* arena : levelData->arenas()) {
        if (levelData->isReachable(arena->name())) {
            space.add(arena);
        }
    }
    audio::SoundMixer mixer(bank);
    Player player;
    player.setSpace(&space);
    player.setMixer(&mixer);
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, &player, nullptr);
    runtime.option = 1;  // the oracle's default (ScriptRuntime.cs:214)

    // BEFORE THE FIRST TICK the oracle has no list AT ALL: _drawnArenas is written only
    // by UpdateArenas (:694-695) and that has not run yet, so SetSolid gets an empty
    // list and Kurt collides with the arenas around his feet. The assertion is the
    // oracle's value (empty), not the shape an implementation happens to produce --
    // review-4 F2 is that this block used to pin the derivation's single "" instead, so
    // the snapshot fix that makes it the oracle's value would have reddened it. The
    // collision effect is proved by the second half of the block either way.
    player.feet = Vec3{0.0f, 655.0f, kFloor};
    CHECK(runtime.solidArenas().empty());
    space.setSolid(runtime.solidArenas());
    float bottom = 0.0f;
    const Vec belowTheWorld{0.0f, 655.0f, -1000.0f};
    CHECK(!space.bottom(belowTheWorld, &bottom));

    // THE FIRST TICK places him (ScriptRuntime.cs:918-919: while CurrentArena is
    // empty it is the arena around him), so the list is his arena alone and the
    // second slot is still inactive. He starts in the corridor, CDANT_1, and the walk
    // below crosses into the room DANT_2 -- the same pair golden/L7_crossing.txt
    // records at s=675, where the crossing rule says DANT_2 and the box under his
    // feet still says CDANT_1.
    runtime.update(1.0f / 30.0f);
    CHECK(runtime.currentArena() == "CDANT_1");
    CHECK(runtime.solidArenas().size() == 1);
    CHECK(runtime.solidArenas()[0] == "CDANT_1");

    // The oracle's walk: one tick per unit of y up to but not including 688. The
    // doorway plane is y = 673 and the predicate is strict (`to.Y > min.Y`), so the
    // crossing lands on the step to 674.
    for (float y = 655.0f; y < 688.0f; y += 1.0f) {
        player.feet = Vec3{0.0f, y, kFloor};
        runtime.update(1.0f / 30.0f);
    }
    CHECK(runtime.currentArena() == "DANT_2");
    CHECK(runtime.secondActive());
    CHECK(runtime.secondArena() == "CDANT_1");

    // The list, in the C#'s order: Kurt's arena first (ArenaSpace's move sweeps
    // element 0 as his own and the rest as the free pass) and the active second
    // behind it.
    CHECK(runtime.solidArenas().size() == 2);
    CHECK(runtime.solidArenas()[0] == "DANT_2");
    CHECK(runtime.solidArenas()[1] == "CDANT_1");

    // And the two names are what makes the room behind the door solid. Standing back
    // inside the room he left -- the state a slide or a knock-back can put him in one
    // tick before the crossing could follow -- a downward move finds the floor only
    // while BOTH names are pinned. Pinned with the current arena alone, the shape
    // review-6 F1 shipped, the same move falls through it: that is the defect this
    // batch's list exists to prevent, executed rather than argued.
    const Vec insideTheRoomHeLeft{0.0f, 660.0f, kFloor};
    space.setSolid(runtime.solidArenas());
    const ArenaSpace::Result both =
        space.move(insideTheRoomHeLeft, Vec{0.0f, 0.0f, -50.0f},
                   ArenaSpace::kMotionFall, 0.0f);
    CHECK(both.hit());

    const std::vector<std::string> onlyTheCurrent{runtime.currentArena()};
    space.setSolid(onlyTheCurrent);
    const ArenaSpace::Result alone =
        space.move(insideTheRoomHeLeft, Vec{0.0f, 0.0f, -50.0f},
                   ArenaSpace::kMotionFall, 0.0f);
    CHECK(!alone.hit());

    // An inactive second is not drawn and not solid (UpdateArenas :663-666), and a
    // name no arena has resolves to nothing instead of clearing what came before it
    // (C# Named(), ArenaSpace.cs:45-55).
    const std::vector<std::string> unknown{"DANT_2", "NOT_AN_ARENA"};
    space.setSolid(unknown);
    const ArenaSpace::Result unknownKept =
        space.move(insideTheRoomHeLeft, Vec{0.0f, 0.0f, -50.0f},
                   ArenaSpace::kMotionFall, 0.0f);
    CHECK(!unknownKept.hit());
    const std::vector<std::string> empty;
    space.setSolid(empty);
    CHECK(space.bottom(insideTheRoomHeLeft, &bottom));

    // THE PIN IS A SNAPSHOT, NOT THE FIELDS (review-4 F1). UpdateArenas runs EARLY in
    // the oracle's RunTick (:935, right after the crossing and CheckTriggers) and it is
    // the only writer of _drawnArenas, so a showArena that lands later in the same tick
    // -- a door closing (ObjectBehaviors.cs:99) or opening (:411), an arena_show opcode
    // (ScriptVm.cs:474) -- moves CurrentArena/SecondArena/SecondActive and the pin does
    // NOT follow it until the next tick's snapshot. Deriving the list from the fields
    // instead drops the active second a whole tick early: over CDANT_1 the next move()
    // then finds no floor and bottom() answers with the wrong arena's minimum Z.
    //
    // showArena("") is exactly what a closing door calls, and this harness has no hook
    // to run it INSIDE a tick, so it is called between them; the property under test is
    // the one both spellings exercise -- the list changes only at the snapshot, never
    // between reads -- and no golden exercises the mid-tick door path (recorded as
    // not-evidenced, not claimed here).
    runtime.showArena("");
    CHECK(!runtime.secondActive());              // the fields moved...
    CHECK(runtime.secondArena().empty());
    CHECK(runtime.solidArenas().size() == 2);    // ...the pin did not
    CHECK(runtime.solidArenas()[1] == "CDANT_1");
    space.setSolid(runtime.solidArenas());
    const ArenaSpace::Result stillSolid =
        space.move(insideTheRoomHeLeft, Vec{0.0f, 0.0f, -50.0f},
                   ArenaSpace::kMotionFall, 0.0f);
    CHECK(stillSolid.hit());

    // And the NEXT tick's snapshot is where it goes, no earlier.
    runtime.update(1.0f / 30.0f);
    CHECK(runtime.solidArenas().size() == 1);
    CHECK(runtime.solidArenas()[0] == "DANT_2");
}

// THE DTI TRIGGER BOXES AND THE ARENA-LOAD PATH THEY DRIVE (CheckTriggers 0x41bf1c,
// arena_load 0x419d00, the door move 0x43ca00).
//
// WHY THIS WAS NEXT. CheckTriggers (ScriptRuntime.cs:707-740) is the LAST writer above the
// drawn-list snapshot: the oracle's tick is `CheckTriggers(); UpdateArenas();` (:934-935),
// so the arena a trigger box preloads belongs to THIS tick's collision pin, while a door or
// an arena_show that runs in the object loop below belongs to the next one's. It is also the
// only per-tick caller of PreloadArena (:602-612), which is the only caller of LoadArena
// besides ShowArena's own (:582-589), and LoadArena is the only caller of PullDoors
// (:628-643) -- the door move whose absence is the "the doorway showed the sky" bug the
// oracle's DoorArenaTests comment opens with. None of the four was ported.
//
// THE GEOMETRY IS SHIPPED, NOT CHOSEN. Level 7's corridor CDANT_1 writes down two type-3
// trigger boxes and the DTI golden decodes them (golden/L7_dti.txt:49-50, big-endian float
// bits, min/max taken the way :718-719 does):
//
//     id=1  x[103,169] y[582,592]        id=0  x[103,169] y[569,580]
//
// with the arena list order the DTI's own (:2 DANT_1, :4 DANT_2), so the record's id is an
// INDEX INTO THAT LIST (:726). Neither box has anything to do with Z -- the record's third
// coordinate is -42..-36, under a tunnel whose floor is -3, and :718-720 reads X and Y only
// -- so the walk below fires from the floor, which any Z test would break.
//
// WHY THE PIN IS THE DRAWN LIST. Everything else that names an arena -- a door's arena_show
// while it stands open and its ShowArena("") when it finishes closing, an arena controller's
// opcode 100 -- runs in the object loop, BELOW the snapshot. So by the end of a tick
// SecondArena/SecondActive/residentArena are whatever those scripts last said, not what the
// box did. The snapshot is taken between the two, and it is what the collision pin and the
// renderer use, so the drawn list is the one thing that shows a trigger box firing ON THE
// TICK IT FIRED. That is what the position of checkTriggers() in runTick is for, and moving
// that call below updateDrawnArenas() reddens the drawnArenas() line below.
void testTriggerBoxesPreloadAndPullDoors(LevelData* levelData, const Cmi* cmi,
                                         audio::SoundBank* bank) {
    const float kFloor = -3.0f;   // DoorArenaTests.Floor: the tunnel's floor
    const float kBoxX = 136.0f;   // inside both CDANT_1 boxes' x[103,169]
    ArenaSpace space;
    for (const formats::Arena* arena : levelData->arenas()) {
        if (levelData->isReachable(arena->name())) {
            space.add(arena);
        }
    }
    audio::SoundMixer mixer(bank);
    Player player;
    player.setSpace(&space);
    player.setMixer(&mixer);
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, &player, nullptr);
    runtime.option = 1;  // the oracle's default (ScriptRuntime.cs:214)

    player.feet = Vec3{0.0f, 655.0f, kFloor};
    runtime.update(1.0f / 30.0f);
    CHECK(runtime.currentArena() == "CDANT_1");

    // Sideways into the boxes' column, one tick per unit as the oracle's own walks do.
    // Nothing above y = 592 is a box. The corridor's own scripts name DANT_2 while the tunnel
    // door stands open, and when that door finishes closing it clears the second with
    // ShowArena("") (ObjectBehaviors, already ported), so Kurt ends up in the corridor alone
    // with DANT_2 still the resident arena -- the one the corridor showed while it could.
    for (float x = 1.0f; x <= kBoxX; x += 1.0f) {
        player.feet = Vec3{x, 655.0f, kFloor};
        runtime.update(1.0f / 30.0f);
    }
    CHECK(runtime.currentArena() == "CDANT_1");
    CHECK(runtime.secondArena().empty());
    CHECK(!runtime.secondActive());
    CHECK(runtime.drawnArenas().size() == 1);
    CHECK(runtime.residentArena() == "DANT_2");

    // DOWN TO THE UPPER BOX. The step 593 -> 592 does not land INSIDE it (`to.Y < max.Y` is
    // strict, :720): it touches the box's y = 592 edge, and it is Crosses (:743-755) that
    // fires -- the half of the predicate a point-in-box-only port drops. The preload takes the
    // second slot and leaves it INACTIVE (:609-611), and the MTO has DANT_2, so DANT_2 becomes
    // the resident arena: the record's id is an INDEX INTO THE ARENA LIST (:726) and arenas[1]
    // is DANT_2 (golden/L7_dti.txt:4), which is what the residentArena() line below reads.
    for (float y = 654.0f; y >= 581.0f; y -= 1.0f) {
        player.feet = Vec3{kBoxX, y, kFloor};
        runtime.update(1.0f / 30.0f);
    }
    CHECK(runtime.secondArena() == "DANT_2");
    CHECK(!runtime.secondActive());
    CHECK(runtime.residentArena() == "DANT_2");
    CHECK(runtime.drawnArenas().size() == 1);   // a preload does not DRAW the room

    // A THIRD arena, and ACTIVE, so the next box has something to displace and the tick it
    // fires on is visible. showArena is opcode 100 (arena_show).
    runtime.showArena("DANT_3");
    CHECK(runtime.secondActive());
    CHECK(runtime.residentArena() == "DANT_3");

    // THE STEP 581 -> 580 FIRES THE id=0 BOX, and it lands in THIS tick's snapshot and not in
    // the next one's: the second becomes the INACTIVE DANT_1, so the drawn list is the
    // corridor alone, and arenas[0] is DANT_1 so the resident arena changes with it. Moving
    // checkTriggers() below updateDrawnArenas() reddens the drawnArenas() line.
    player.feet = Vec3{kBoxX, 580.0f, kFloor};
    runtime.update(1.0f / 30.0f);
    CHECK(runtime.drawnArenas().size() == 1);
    CHECK(runtime.drawnArenas()[0] == "CDANT_1");
    CHECK(runtime.secondArena() == "DANT_1");
    CHECK(!runtime.secondActive());
    CHECK(runtime.residentArena() == "DANT_1");

    // AND IT KEEPS FIRING, because the box is still crossed on every step down.
    for (float y = 579.0f; y >= 575.0f; y -= 1.0f) {
        player.feet = Vec3{kBoxX, y, kFloor};
        runtime.update(1.0f / 30.0f);
        CHECK(runtime.drawnArenas().size() == 1);
        CHECK(runtime.secondArena() == "DANT_1");
    }

    // PULLDOORS (:628-643): the doors leading INTO an arena being loaded come out of the
    // neighbour that holds them and turn around, EXCEPT from Kurt's own arena and the active
    // second. The oracle's own assertion is DoorArenaTests.LoadedArenaPullsItsDoors, and it
    // reaches the state with TeleportKurt -- unported -- because on foot UpdateDoor's own move
    // (ObjectBehaviors.cs:99, ported in updateDoor()) has already turned the door by the time
    // Kurt stands in the third arena. So THESE doors are built, not spawned, and what is
    // under test is the rule: which doors move, which are exempt, and the turn. No ticks
    // below, so nothing but the calls named here changes anything.
    Player walker;
    walker.setSpace(&space);
    walker.setMixer(&mixer);
    scripts::ScriptRuntime pull(levelData, cmi, 1, bank, &walker, nullptr);
    pull.option = 1;
    walker.feet = Vec3{0.0f, 655.0f, kFloor};
    pull.update(1.0f / 30.0f);
    CHECK(pull.currentArena() == "CDANT_1");

    scripts::MdkObject kurtSide;
    kurtSide.arena = "CDANT_1";
    kurtSide.connects = "DANT_2";
    kurtSide.flags = scripts::MdkObject::FlagDoor;
    kurtSide.door_state = kDoorClosed;  // shut, and shut it stays: Kurt is far from it
    kurtSide.yaw = 90.0f;               // DoorArenaTests.DoorYaw, the tunnel door's own
    kurtSide.position = Vec3{0.0f, 673.0f, kFloor};
    pull.objects().push_back(kurtSide);
    scripts::MdkObject* kurtDoor = &pull.objects().back();

    scripts::MdkObject otherSide;
    otherSide.arena = "DANT_2";
    otherSide.connects = "CDANT_2";
    otherSide.flags = scripts::MdkObject::FlagDoor;
    otherSide.door_state = kDoorClosed;
    otherSide.yaw = 90.0f;
    otherSide.position = Vec3{75.0f, 1004.0f, -6.0f};
    pull.objects().push_back(otherSide);
    scripts::MdkObject* farDoor = &pull.objects().back();

    // Kurt's own arena is exempt (:632-636): loading DANT_2 would take the door out of the
    // corridor he is standing in and put it in the room he is not. The C# refuses, and the
    // door keeps its arena, its link and its yaw.
    pull.showArena("DANT_2");
    CHECK(pull.residentArena() == "DANT_2");
    CHECK(kurtDoor->arena == "CDANT_1");
    CHECK(kurtDoor->connects == "DANT_2");
    CHECK(kurtDoor->yaw == 90.0f);

    // Preloading the arena that is already the second changes nothing, not even the flag.
    pull.preloadArena("DANT_2");
    CHECK(pull.secondArena() == "DANT_2");
    CHECK(pull.secondActive());

    // A corridor is not in the MTO, so loading one does NOT replace the resident arena --
    // DANT_2 stays resident (:617-620) -- but it still pulls its doors, and DANT_2 is
    // neither Kurt's arena nor the active second now that the second has been cleared.
    // The turn is the oracle's own number: DoorArenaTests.TurnedYaw, 270 to 0.01.
    pull.showArena("");
    CHECK(pull.secondArena().empty());
    pull.showArena("CDANT_2");
    CHECK(pull.residentArena() == "DANT_2");
    CHECK(farDoor->arena == "CDANT_2");
    CHECK(farDoor->connects == "DANT_2");
    CHECK(fabsf(farDoor->yaw - 270.0f) < 0.01f);

    // OPCODE 223 (arena_set_neighbour) is the scripts' own door onto this path, and it was
    // unimplemented until this batch: the operand names the arena to load ahead, and it lands
    // in the second slot INACTIVE exactly as the trigger box's preload does -- so the room is
    // resident and its doors are pulled without being drawn.
    const int32_t kArenaSetNeighbour = 223;
    Code ahead;
    ahead.u8(kArenaSetNeighbour).pstr("DANT_1");
    ahead.end();
    runProgram(pull, ahead, farDoor);
    CHECK(pull.secondArena() == "DANT_1");
    CHECK(!pull.secondActive());
    CHECK(pull.residentArena() == "DANT_1");
    CHECK(!sawUnimplemented(kArenaSetNeighbour));
}

// THE OTHER TWO TRIGGER ARMS: THE SHOW BOX, AND THE id = -1 CLEAR.
//
// WHY THIS WAS NEXT. Review-6 F5 and CONTINUATION priority (2) are the same item:
// batch 139 walked CDANT_1, whose two boxes are both type 3, so the
// `TriggerShow -> ShowArena(arena)` arm (:727-730) and the "names no arena ->
// ShowArena("")" arm (:731-734) had never been EXECUTED. That is not a small gap.
// SHOW and LOAD are the two halves of the trigger rule and they do DIFFERENT things:
// a preload takes the second slot INACTIVE and does not draw it (batch 139's pin),
// while a show takes it ACTIVE, draws it, and makes it live with Kurt. A port that
// swapped the two arms, or that dropped the clear, passed every gate.
//
// THE GEOMETRY IS SHIPPED, AND THE SPOT IS THE ORACLE'S OWN. Level 7's DANT_10
// writes two type-1 boxes (golden/L7_dti.txt:45-46, big-endian float bits, min/max
// taken the way :718-719 takes them):
//
//     id=18  x[509,511] y[4968,5077] z[0,7]    id=-1  x[511,514] y[4964,5082] z[0,6]
//
// and DoorArenaTests already names where to stand: `Far = "DANT_10"`, `FarSpot =
// (495, 5005, 24)` (:33-34). The C# gets there with TeleportKurt, which is unported;
// here the FIRST tick's geometric lookup answers the same arena, so the walk starts
// on the oracle's own spot and `currentArena_` is DANT_10 -- the only arena whose
// records these boxes are, because :711 reads CurrentArena.
//
// THE BOXES ARE XY-ONLY, AND THAT IS PINNED BY THE HEIGHT. The show box's z range is
// 0..7 and the clear box's 0..6 (golden/L7_dti.txt:45 and :46, whose third box
// coordinate is 40E00000 and 40C00000), while FarSpot stands at z = 24, so a port that
// read either box as a 3-D volume would never fire it. :713-720 is Vector2 arithmetic
// on the feet and nothing else.
//
// arenas[18] is CDANT_9, the last of the DTI's 19 arenas (golden/L7_dti.txt:86), and
// it is a CORRIDOR. So the show draws it and makes it live, but it is not in the MTO
// and the resident arena stays DANT_10 (:617-620) -- a different consequence from
// batch 139's room preload, pinned separately rather than assumed. And because
// DANT_10 writes exactly ONE connection record, this id is the first shipped trigger
// id that CANNOT be read as an index into the connection list: batch 139 recorded
// that its CDANT_1 ids (0, 1) named the same arena either way, so the shipped data
// could not separate the two readings. Here a connection-list index of 18 is out of
// range, names no arena, and the box would CLEAR instead of showing.
//
// THE TWO BOXES ARE PINNED APART FROM EACH OTHER, AND THEY ARE BOTH SHOW BOXES. The
// show box's y range is [4968,5077] and the clear box's is [4964,5082], so at y = 4965
// the show box is out of reach and only the id = -1 box can fire: the clear is pinned
// ALONE, not as a side effect of the show having fired first. At y = 5005 both boxes
// fire on the step into x = 511, in the DTI's record order, which is the other thing
// this case pins: the arms run in record order, not in some order of the port's
// choosing. Review-7 F1, and the correction matters: BOTH records are `type=1`
// (golden/L7_dti.txt:45-46), so BOTH take the TriggerShow arm (:727-730) and the id =
// -1 box clears by naming no arena to that arm -- ShowArena("") and its own empty test
// at :574-578 -- NOT by the `else if (arena.empty())` LOAD arm at :731-734, which no
// shipped record reaches at all (see the paragraph below, and the batch's M4).
//
// WHAT THIS DOES NOT PIN. The TriggerLoad arm that names no arena (a type-3 record
// with an id of -1) is NOT reachable from shipped data: every type-3 id in all six
// DTI goldens is 0..9 while every level writes 19 arenas, so no load box can name
// nothing. That arm stays compile-verified, and says so here instead of being
// claimed as covered.
void testTriggerShowBoxDrawsAndTheMinusOneBoxClears(LevelData* levelData, const Cmi* cmi,
                                                    audio::SoundBank* bank) {
    const float kZ = 24.0f;     // DoorArenaTests.FarSpot.Z
    const float kY = 5005.0f;   // DoorArenaTests.FarSpot.Y
    ArenaSpace space;
    for (const formats::Arena* arena : levelData->arenas()) {
        if (levelData->isReachable(arena->name())) {
            space.add(arena);
        }
    }
    audio::SoundMixer mixer(bank);
    Player player;
    player.setSpace(&space);
    player.setMixer(&mixer);
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, &player, nullptr);
    runtime.option = 1;  // the oracle's default (ScriptRuntime.cs:214)

    player.feet = Vec3{495.0f, kY, kZ};  // DoorArenaTests.FarSpot
    runtime.update(1.0f / 30.0f);
    CHECK(runtime.currentArena() == "DANT_10");
    CHECK(runtime.residentArena() == "DANT_10");
    CHECK(runtime.drawnArenas().size() == 1);   // the room alone
    // Nothing of the corridor's is in the world yet. An arena's own objects appear only
    // while it is LIVE, and only Kurt's arena and the ACTIVE second are live (:703), so
    // this counts the second slot having never been active.
    const auto corridorObjects = [&runtime]() {
        int32_t n = 0;
        for (const scripts::MdkObject& o : runtime.objects()) {
            if (!o.dead && o.arena == "CDANT_9") ++n;
        }
        return n;
    };
    CHECK(corridorObjects() == 0);

    // x = 496..508 at the oracle's y: the show box starts at x = 509, so nothing has
    // fired, and the walk does not touch the DANT_10 -> CDANT_9 doorway either (that
    // one is at y = 5001 and only MinusY moves cross it).
    for (float x = 496.0f; x <= 508.0f; x += 1.0f) {
        player.feet = Vec3{x, kY, kZ};
        runtime.update(1.0f / 30.0f);
    }
    CHECK(runtime.currentArena() == "DANT_10");
    CHECK(runtime.secondArena().empty());
    CHECK(runtime.drawnArenas().size() == 1);
    CHECK(corridorObjects() == 0);

    // THE SHOW BOX. The step 508 -> 509 crosses x = 509, the id=18 box's near edge,
    // and it is the SHOW arm (:727-730): the corridor takes the second slot ACTIVE --
    // a preload would have left it inactive and undrawn -- and the drawn list is TWO
    // arenas with the corridor second. The corridor is not in the MTO, so the resident
    // arena does NOT move (:617-620).
    player.feet = Vec3{509.0f, kY, kZ};
    runtime.update(1.0f / 30.0f);
    CHECK(runtime.secondArena() == "CDANT_9");  // arenas[18], the DTI's last
    CHECK(runtime.secondActive());
    CHECK(runtime.drawnArenas().size() == 2);
    CHECK(runtime.drawnArenas()[1] == "CDANT_9");
    CHECK(runtime.residentArena() == "DANT_10");
    // And objects of the corridor are in the world. The cause is ShowArena's own
    // `if (!state.Started) { state.Started = true; SpawnDtiAliens(arena); }` (:594-598,
    // the only caller of SpawnDtiAliens in the oracle), which a preload does NOT do
    // (:602-612 has no such call) -- review-7 F3, which corrected an earlier draft that
    // credited liveness. Liveness (IsLiveArena, :703) only gates their per-tick UPDATE,
    // so the discriminating fact is that the spawn is show-only. Mutation M1, which
    // routes the show box through preloadArena, reddens this line along with the three
    // above it -- and the batch that ports the loaded-arena half of UpdateArenas has to
    // keep SpawnDtiAliens out of PreloadArena or this pin stops separating the arms.
    CHECK(corridorObjects() > 0);
    // obj.Visible (UpdateArenas :696-699): an object is drawn exactly while its arena
    // is in the drawn list. Both arenas are, so every live object of both is visible.
    const auto visibleIn = [&runtime](const char* arena, bool wanted) {
        int32_t n = 0;
        for (const scripts::MdkObject& o : runtime.objects()) {
            if (!o.dead && o.arena == arena && o.visible == wanted) ++n;
        }
        return n;
    };
    CHECK(visibleIn("CDANT_9", true) == corridorObjects());
    CHECK(visibleIn("CDANT_9", false) == 0);
    CHECK(visibleIn("DANT_10", false) == 0);

    // Still inside it: the box fires on every step (:720) and changes nothing.
    player.feet = Vec3{510.0f, kY, kZ};
    runtime.update(1.0f / 30.0f);
    CHECK(runtime.secondArena() == "CDANT_9");
    CHECK(runtime.secondActive());
    CHECK(runtime.drawnArenas().size() == 2);

    // DOWN to y = 4965 at x = 510, one unit per tick. Every step inside the show
    // box's y range re-fires the show; below y = 4968 nothing fires, and the second
    // is still the corridor when the next step starts.
    for (float y = 5004.0f; y >= 4965.0f; y -= 1.0f) {
        player.feet = Vec3{510.0f, y, kZ};
        runtime.update(1.0f / 30.0f);
    }
    CHECK(runtime.currentArena() == "DANT_10");
    CHECK(runtime.secondArena() == "CDANT_9");
    CHECK(runtime.secondActive());

    // THE id = -1 BOX, ALONE. At y = 4965 the show box cannot answer: its y starts at
    // 4968 and a move at 4965 touches none of its edges, so the step 510 -> 511 is this
    // box on its own. It is a `type=1` record with id = -1, so it takes the SHOW arm
    // (:726 reads the id as an arena-list index and finds none, :727-730 calls
    // ShowArena("")) and the clear happens inside ShowArena's own empty test, which
    // empties the second AND deactivates it (:574-578). The drawn list is the room alone
    // again, and the resident arena still never moved.
    player.feet = Vec3{511.0f, 4965.0f, kZ};
    runtime.update(1.0f / 30.0f);
    CHECK(runtime.secondArena().empty());
    CHECK(!runtime.secondActive());
    CHECK(runtime.drawnArenas().size() == 1);
    CHECK(runtime.drawnArenas()[0] == "DANT_10");
    CHECK(runtime.residentArena() == "DANT_10");
    // The corridor left the drawn list, so its objects -- still in the world -- are
    // not drawn any more, and the room's still are. Before obj.visible had a writer
    // every one of them was drawn from wherever Kurt stood.
    CHECK(corridorObjects() > 0);
    CHECK(visibleIn("CDANT_9", true) == 0);
    CHECK(visibleIn("CDANT_9", false) == corridorObjects());
    CHECK(visibleIn("DANT_10", false) == 0);

    // AND IT KEEPS FIRING while the box is still crossed (`to.X < max.X`, so up to
    // x = 513 and the edge step onto 514).
    for (float x = 512.0f; x <= 514.0f; x += 1.0f) {
        player.feet = Vec3{x, 4965.0f, kZ};
        runtime.update(1.0f / 30.0f);
        CHECK(runtime.secondArena().empty());
        CHECK(!runtime.secondActive());
    }
}

// TELEPORT_KURT, LEVEL 4 — the oracle's own ArenaSwitchTests cases that needed a teleport,
// run here. tests/Mdk.Game.Tests/ArenaSwitchTests.cs is the source of every expected value
// below, and the two it does not assert are named where they appear.
//
// WHY THIS IS A SEPARATE CASE FROM golden/L4_crossing.txt. The crossing golden drives its
// first state geometrically and says so in its header, because the port had no teleport:
// "the port has no TeleportKurt, so this harness lets the first tick find the arena
// geometrically and the line pins that the two agree on MEAT_7". That golden is the
// oracle's, so it stays exactly as it is; what this case adds is the teleport's OWN
// observables, which no golden has ever run — the arena state a teleport writes, the
// first-tick rule a teleport overrides, and the arena a teleport makes solid.
// The decoded bank's bound (audio.bank-bound): over its budget, decoding
// evicts least-recently-gotten sounds, pinned ones never. The budgets here
// are half of one sound, so every assertion is about WHO survives, never
// about how big a footstep happens to be.
// A mod's model swap in the object pass: the layout draws the swap's
// mesh for the replaced part (fitted to the rest pose here), in one
// batch, and the unswapped model beside it keeps its own vertices.
void testSwappedModelStreamsSwapVertices(LevelData& level) {
    // The fixture mod: models/TEST.glb with a BODY node (as in the
    // formats tests), written by the project's own GLB writer.
    char root[] = "/tmp/mdk-vm-swap-XXXXXX";
    if (!mkdtemp(root)) {
        CHECK(false);
        return;
    }
    const std::string modelsDir = std::string(root) + "/models";
    mkdir(modelsDir.c_str(), 0755);
    Glb::Scene scene;
    Glb::Mesh mesh;
    mesh.name = "BODY";
    Glb::Primitive primitive;
    primitive.positions = {0, 0, 0, 2, 0, 0, 0, 0, -2, 0, 2, 0};
    primitive.uvs = {0, 0, 1, 0, 0, 1, 1, 1};
    primitive.indices = {0, 1, 2, 0, 3, 1};
    primitive.material = 0;
    mesh.primitives.push_back(primitive);
    scene.meshes.push_back(mesh);
    Glb::Material material;
    material.name = "SKIN";
    material.colour[0] = 1.0f;
    material.colour[1] = 0.0f;
    material.colour[2] = 1.0f;
    material.colour[3] = 1.0f;
    scene.materials.push_back(material);
    std::vector<uint8_t> bytes;
    std::string error;
    CHECK(Glb::write(scene, &bytes, &error));
    FILE* file = fopen((modelsDir + "/TEST.glb").c_str(), "wb");
    CHECK(file != nullptr);
    fwrite(bytes.data(), 1, bytes.size(), file);
    fclose(file);

    formats::Model model;
    model.name = "TEST";
    formats::Model::Part part;
    std::snprintf(part.name, sizeof(part.name), "BODY");
    part.vertices = {{0, 0, 0}, {2, 0, 0}, {0, 2, 0}, {0, 0, 2}};
    part.triangleIndices = {0, 1, 2, 0, 1, 3};
    part.triangleMaterials = {0, 0};
    model.partList.push_back(part);

    ModModels models(std::vector<std::string>{root}, 7);
    MaterialResolver resolver(&level.dti(), Shading::Original);
    ObjectView view(resolver);
    view.setSwaps(&models);

    ObjectLook look;
    look.palette = &level.dti().palette();
    if (!level.arenas().empty()) {
        look.archives = level.archivesOf(level.arenas()[0]);
    }

    scripts::MdkObject obj;
    obj.model = &model;
    ObjectStream stream;
    CHECK(view.addObject(obj, look, Mat4::identity(), &stream));
    CHECK(stream.draws.size() == 1);
    CHECK(stream.vertices.size() == 6);
    // The swap's MDK-converted vertices, not the model's own: glTF
    // (0,0,-2) arrived as (0,2,0).
    bool sawConverted = false;
    for (size_t i = 0; i < stream.vertices.size(); i++) {
        const Vec3& p = stream.vertices[i].pos;
        if (p.x == 0.0f && p.y == 2.0f && p.z == 0.0f) {
            sawConverted = true;
        }
    }
    CHECK(sawConverted);
    CHECK(models.found().size() == 1);
}

void testBankBudgetEvictsUnpinned(const MdkData& data) {
    audio::SoundBank bank;
    CHECK(audio::SoundBank::forLevel(&bank, data, 7));
    audio::SoundBank::Entry entry;
    CHECK(bank.get("FOOT1", &entry));
    const size_t oneSound = bank.decodedBytes();
    CHECK(oneSound > 0);
    bank.setDecodedBudgetBytes(oneSound / 2);

    // FOOT1 alone stays (it is the sound just decoded); the next decode
    // evicts it as the least recently gotten unpinned resident.
    CHECK(bank.get("DOOR", &entry));
    CHECK(bank.soundsDecoded() == 1);

    // A pinned sound survives a decode that would otherwise evict it;
    // releasing the pin makes it the next victim again.
    audio::SoundBank pinned;
    CHECK(audio::SoundBank::forLevel(&pinned, data, 7));
    CHECK(pinned.get("FOOT1", &entry));
    pinned.retain("FOOT1");
    CHECK(pinned.pins("FOOT1") == 1);
    pinned.setDecodedBudgetBytes(oneSound / 2);
    CHECK(pinned.get("FOOT2", &entry));
    CHECK(pinned.soundsDecoded() == 2);
    pinned.release("FOOT1");
    CHECK(pinned.pins("FOOT1") == 0);
    CHECK(pinned.get("DOOR", &entry));
    CHECK(pinned.soundsDecoded() == 1);

    // No budget (the shipped default) keeps everything decoded.
    audio::SoundBank unbounded;
    CHECK(audio::SoundBank::forLevel(&unbounded, data, 7));
    CHECK(unbounded.get("FOOT1", &entry));
    CHECK(unbounded.get("DOOR", &entry));
    CHECK(unbounded.soundsDecoded() == 2);
}

void testTeleportKurtLevel4(const MdkData& data) {
    const float kDoorway = 14822.0f;  // ArenaSwitchTests.Doorway (connection 1012's plane)
    const float kFloor = 21.0f;       // ArenaSwitchTests.Floor

    LevelData levelData;
    if (!LevelData::load(&levelData, &data, 4)) {
        printf("  FAIL line %d: level 4 did not load\n", __LINE__);
        failures++;
        return;
    }
    Cmi cmi;
    {
        std::vector<uint8_t> bytes;
        if (!data.read("TRAVERSE/LEVEL4/LEVEL4.CMI", &bytes) ||
            !Cmi::parse(&cmi, bytes.data(), (int32_t)bytes.size())) {
            printf("  FAIL line %d: level 4's CMI did not read\n", __LINE__);
            failures++;
            return;
        }
    }
    audio::SoundBank bank;
    if (!audio::SoundBank::forLevel(&bank, data, 4)) {
        printf("  FAIL line %d: level 4's bank did not load\n", __LINE__);
        failures++;
        return;
    }

    // CreateRuntime (:30-42): the space holds every REACHABLE arena, and the runtime reads
    // its Kurt from a Player. The C#'s fixture builds a Kurt over the same space.
    ArenaSpace space;
    for (const formats::Arena* arena : levelData.arenas()) {
        if (levelData.isReachable(arena->name())) {
            space.add(arena);
        }
    }
    audio::SoundMixer mixer(&bank);
    Player player;
    player.setSpace(&space);
    player.setMixer(&mixer);
    scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, &player, nullptr);
    runtime.option = 1;  // the oracle's default (ScriptRuntime.cs:214)

    // ---- FirstTickHasNoMove, as the contrast case (ArenaSwitchTests.cs:74-82).
    // That test uses Kurt.Teleport — Kurt's OWN teleport, which moves the body and writes
    // no arena state — and asserts KurtVelocity is zero: the first tick has no move to
    // observe, so the jump from wherever Kurt was parked is not a velocity. Player::teleport
    // is the C#'s Kurt.Teleport (Kurt.cs:249), so this is that test and not an imitation of
    // it: note the chute flag and the three speeds, which the teleport zeroes and which is
    // all of "he stops" that this port can show.
    player.teleport(Vec3{0.0f, kDoorway - 100.0f, kFloor}, 90.0f);
    CHECK(player.chuteOpen == false);
    CHECK(player.forwardSpeed == 0.0f && player.strafeSpeed == 0.0f &&
          player.verticalSpeed == 0.0f);
    CHECK(player.teleports == 1);
    runtime.update(1.0f / 30.0f);
    CHECK(runtime.kurtVelocity.x == 0.0f && runtime.kurtVelocity.y == 0.0f &&
          runtime.kurtVelocity.z == 0.0f);  // :81
    CHECK(runtime.currentArena() == "MEAT_7");

    // ---- FirstTickKeepsTeleport (ArenaSwitchTests.cs:89-97), on a FRESH runtime: the C#
    // fixture is fresh and the tick it steps is its tick 0, which is the only tick the rule
    // under test can fire on. Reusing the runtime above would have made the case vacuous —
    // mutation M1 (the flag dropped from the first-tick rule) reddens nothing there, and
    // that negative result was found by running the mutation, not by reading the code.
    // The soak tour put Kurt ON a doorway before the first tick; the first tick then took
    // his new place as the previous one, so he "stayed in the room while in the corridor
    // and fell out when it went". The teleport names the destination as the previous
    // position and raises the flag the first tick now tests (:911
    // `if (_tickCount == 0 && !_teleported)`).
    ArenaSpace soakSpace;
    for (const formats::Arena* arena : levelData.arenas()) {
        if (levelData.isReachable(arena->name())) {
            soakSpace.add(arena);
        }
    }
    audio::SoundMixer soakMixer(&bank);
    Player soakPlayer;
    soakPlayer.setSpace(&soakSpace);
    soakPlayer.setMixer(&soakMixer);
    scripts::ScriptRuntime soak(&levelData, &cmi, 1, &bank, &soakPlayer, nullptr);
    soak.option = 1;
    //
    // Kurt is teleported ONTO the plane (y = 14822 exactly) and then stepped to 14825. The
    // velocity line is the flag itself: with it, the previous position is still the
    // teleport's and the move is 3 units, so 90 u/s; without it the first tick overwrote
    // the previous position with the destination and the velocity is zero — and the arena
    // line goes with it, because a zero move crosses no doorway and Kurt stays in the room
    // while standing in the corridor. Both halves of the bug, in two lines.
    soak.teleportKurt("MEAT_7", Vec3{0.0f, kDoorway, kFloor}, 90.0f);
    CHECK(soak.currentArena() == "MEAT_7");
    // TeleportSecond's FIRST arm (:498-502): into a room the second is EMPTIED, because a
    // room is drawn on its own and a second would be a room behind a door that does not
    // exist. It is emptied and NOT left inactive: ShowArena runs after TeleportSecond
    // (:469 after :468) and its `SecondActive = true` (:592) is unconditional, so the slot
    // is an empty ACTIVE one -- which is what the drawn list reads as the room alone
    // (:661-664 add the second only when it is both active AND named). Nothing in the C#
    // suite asserts the second after a teleport into a room (its two teleport-into-a-corridor
    // cases are the level-7 ones below), so these three lines are the port's own, named as
    // such, and the pair of them is what swapping the two calls reddens (mutation M2).
    CHECK(soak.secondArena().empty());
    CHECK(soak.secondActive());
    // ShowArena loaded it, and the MTO has MEAT_7, so it is the resident arena (:617-620).
    CHECK(soak.residentArena() == "MEAT_7");
    // The unnamed teleport is the one that flashes; a named one does not (:477-480).
    CHECK(soakPlayer.combat().whiteFlash() == 0.0f);
    CHECK(soakPlayer.teleports == 1);  // and it moved KURT too: TeleportKurt ends with Kurt.Teleport

    soakPlayer.teleport(Vec3{0.0f, kDoorway + 3.0f, kFloor}, soakPlayer.yaw);  // StepTo (:96)
    soak.update(1.0f / 30.0f);
    CHECK(soak.kurtVelocity.y == 90.0f);  // 3 units at 30 ticks/s, because the flag held
    CHECK(soak.currentArena() == "CMEAT_7");  // :96
    // And the room he left is the ACTIVE second, which is the crossing running off the
    // teleport's previous position: the drawn list is the corridor and the room.
    CHECK(soak.secondArena() == "MEAT_7");
    CHECK(soak.drawnArenas().size() == 2);

    // ---- TeleportMakesUnreachableArenaSolid (ArenaSwitchTests.cs:102-119). An arena no
    // connection leads to is not in Kurt's space until a script teleports him into it
    // (level.gd enter_arena). The C# builds an EMPTY space here, picks the first arena that
    // is unreachable and has vertices, and takes the centre of its vertex box.
    ArenaSpace bare;  // the PLAYER's space, deliberately left empty: the C#'s fixture is
                      // an empty space, and the runtime builds its own from the reachable
                      // arenas exactly as the C#'s driver line in the fixture does.
    audio::SoundMixer bareMixer(&bank);
    Player barePlayer;
    barePlayer.setSpace(&bare);
    barePlayer.setMixer(&bareMixer);
    scripts::ScriptRuntime bareRuntime(&levelData, &cmi, 1, &bank, &barePlayer, nullptr);
    bareRuntime.option = 1;
    const formats::Arena* target = nullptr;
    for (const formats::Arena* arena : levelData.arenas()) {
        if (!levelData.isReachable(arena->name()) && !arena->vertices().empty()) {
            target = arena;
            break;
        }
    }
    CHECK(target != nullptr);
    if (target != nullptr) {
        const std::vector<formats::Vec3f>& verts = target->vertices();
        formats::Vec3f mn = verts[0];
        formats::Vec3f mx = verts[0];
        for (const formats::Vec3f& v : verts) {
            mn.x = mathMin(mn.x, v.x);
            mn.y = mathMin(mn.y, v.y);
            mn.z = mathMin(mn.z, v.z);
            mx.x = mathMax(mx.x, v.x);
            mx.y = mathMax(mx.y, v.y);
            mx.z = mathMax(mx.z, v.z);
        }
        const Vec3 center{(mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f, (mn.z + mx.z) * 0.5f};
        // Assert.DoesNotContain(space.At(center), ...) (:114) then Assert.Contains (:117),
        // read through the runtime's space, which is the one enterArena() adds to (see the
        // arenaSpace() note: the port has two spaces where the C# has one, so this pins the
        // arena RULE and not yet the collision). ArenaSpace has no C++ twin of the LIST At
        // returns -- arenaAt answers the first box that contains the point -- so this is the
        // weaker pair: not the answer before, and the answer after.
        const Vec at{center.x, center.y, center.z};
        const std::string* before = bareRuntime.arenaSpace().arenaAt(at);
        CHECK(before == nullptr || *before != target->name());
        bareRuntime.teleportKurt(target->name(), center, 0.0f);
        const std::string* after = bareRuntime.arenaSpace().arenaAt(at);
        CHECK(after != nullptr && *after == target->name());  // :117
        CHECK(bareRuntime.currentArena() == target->name());
    }

    // ---- The unnamed teleport: teleport_player with an empty arena name (opcode 112's
    // first operand) moves Kurt INSIDE his current arena and white-flashes him (:477-480,
    // Kurt.WhiteFlash = MathF.Max(Kurt.WhiteFlash, TeleportFlash) with TeleportFlash = 255).
    // No C# assertion covers it — the suite never passes "" — so it is named here as the
    // port's own, and it is the only thing in the tree that reaches that branch.
    bareRuntime.teleportKurt(std::string(), Vec3{0.0f, 10.0f, 0.0f}, 0.0f);
    CHECK(barePlayer.combat().whiteFlash() == 255.0f);
    CHECK(barePlayer.teleports == 2);  // the unnamed teleport moved the body
    CHECK(bareRuntime.currentArena() == (target ? target->name() : std::string()));
}

// TELEPORT_KURT, LEVEL 7 — DoorArenaTests' three teleport cases, which batch 139 could
// only re-state because they reach their state with TeleportKurt. Every expected value is
// that file's; the constants are its own (Tunnel/Room/Corridor, TunnelDoor, TurnedYaw,
// CorridorSpot, FarSpot, TunnelSpot, FarFromDoor, DoorYaw).
void testTeleportKurtLoadsCorridorsAndPullsDoors(LevelData* levelData, const Cmi* cmi,
                                                 audio::SoundBank* bank) {
    const std::string kTunnel = "CDANT_1";
    const std::string kRoom = "DANT_2";
    const std::string kCorridor = "CDANT_2";
    const std::string kFar = "DANT_10";
    const Vec3 kTunnelDoor{0.0f, 673.0f, -3.0f};
    const float kDoorYaw = 90.0f;
    const float kTurnedYaw = 270.0f;
    const Vec3 kFarSpot{495.0f, 5005.0f, 24.0f};
    const Vec3 kTunnelSpot{0.0f, 485.0f, 19.0f};
    const Vec3 kCorridorSpot{100.0f, 990.0f, -6.0f};
    const Vec3 kFarFromDoor{0.0f, 750.0f, -8.0f};

    ArenaSpace space;
    for (const formats::Arena* arena : levelData->arenas()) {
        if (levelData->isReachable(arena->name())) {
            space.add(arena);
        }
    }
    audio::SoundMixer mixer(bank);
    Player player;
    player.setSpace(&space);
    player.setMixer(&mixer);
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, &player, nullptr);
    runtime.option = 1;

    // StandAt (:60-67): put Kurt somewhere and tick, with Kurt's OWN teleport between.
    auto standAt = [&player, &runtime, kDoorYaw](const Vec3& at, int ticks) {
        for (int i = 0; i < ticks; i++) {
            player.teleport(at, kDoorYaw);
            runtime.update(1.0f / 30.0f);
        }
    };
    // DoorAt (:68-69): the first live door within a unit of a place. It takes the runtime,
    // because this case runs three of them and the C#'s DoorAt takes its own.
    auto doorAt = [](scripts::ScriptRuntime& which, const Vec3& at) -> scripts::MdkObject* {
        for (scripts::MdkObject& o : which.objects()) {
            if (o.dead || (o.flags & scripts::MdkObject::FlagDoor) == 0) {
                continue;
            }
            const float dx = o.position.x - at.x;
            const float dy = o.position.y - at.y;
            const float dz = o.position.z - at.z;
            if (sqrtf(dx * dx + dy * dy + dz * dz) < 1.0f) {
                return &o;
            }
        }
        return nullptr;
    };

    // ---- TeleportLoadsCorridorArena (:84-99). "A teleport into a corridor not loaded loads
    // the last arena (DTI order) leading to it, which becomes the ACTIVE second (0x41bce4):
    // from DANT_10 into CDANT_1, DANT_2 behind its door." The first teleport is into a room,
    // so the second slot is emptied and DANT_10 becomes the resident arena; the second is
    // into a corridor the resident arena has no connection to, so TeleportSecond's THIRD arm
    // runs — DANT_2 named, INACTIVE, and loaded ahead — and ShowArena then activates it.
    runtime.teleportKurt(kFar, kFarSpot, 0.0f);
    standAt(kFarSpot, 1);
    CHECK(runtime.currentArena() == kFar);
    CHECK(runtime.residentArena() == kFar);
    runtime.teleportKurt(kTunnel, kTunnelSpot, kDoorYaw);
    standAt(kTunnelSpot, 1);
    CHECK(runtime.secondArena() == kRoom);   // :98
    CHECK(runtime.secondActive());           // ShowArena's doing, not TeleportSecond's
    CHECK(runtime.residentArena() == kRoom);  // the load-ahead put DANT_2 in the MTO
    bool drawnHasRoom = false;
    for (const std::string& name : runtime.drawnArenas()) {
        drawnHasRoom = drawnHasRoom || name == kRoom;
    }
    CHECK(drawnHasRoom);  // :99

    // ---- TeleportKeepsLoadedCorridor (:101-115). "A teleport into a corridor OF THE LOADED
    // arena keeps the second arena (0x41bce4): from DANT_2 into CDANT_2, none." Fresh
    // runtime, because the arm is about what the second ALREADY held — here the first
    // teleport empties it and the second must leave it empty, so the arm's evidence is the
    // absence: a TeleportSecond that fell through to the neighbour scan would name DANT_1 or
    // DANT_2 and the drawn list would be two arenas.
    ArenaSpace space2;
    for (const formats::Arena* arena : levelData->arenas()) {
        if (levelData->isReachable(arena->name())) {
            space2.add(arena);
        }
    }
    audio::SoundMixer mixer2(bank);
    Player walker;
    walker.setSpace(&space2);
    walker.setMixer(&mixer2);
    scripts::ScriptRuntime keep(levelData, cmi, 1, bank, &walker, nullptr);
    keep.option = 1;
    auto keepAt = [&walker, &keep, kDoorYaw](const Vec3& at, int ticks) {
        for (int i = 0; i < ticks; i++) {
            walker.teleport(at, kDoorYaw);
            keep.update(1.0f / 30.0f);
        }
    };
    keep.teleportKurt(kRoom, kFarFromDoor, kDoorYaw);
    keepAt(kFarFromDoor, 1);
    CHECK(keep.currentArena() == kRoom);
    CHECK(keep.residentArena() == kRoom);
    keep.teleportKurt(kCorridor, kCorridorSpot, 0.0f);
    keepAt(kCorridorSpot, 1);
    CHECK(keep.secondArena().empty());   // :113
    CHECK(keep.drawnArenas().size() == 1 && keep.drawnArenas()[0] == kCorridor);  // :114
    // IsCorridorLoaded read the resident arena to reach that answer, which is the first
    // time this port has ever read residentArena_ rather than only written it.
    CHECK(keep.residentArena() == kRoom);  // a corridor is not in the MTO (:617-620)

    // ---- LoadedArenaPullsItsDoors (:64-82), the oracle's own PullDoors assertion, which
    // batch 139 could only reach with built doors. The tunnel's real door — CDANT_1's
    // spawn_connector at (0,673,-3) at yaw 90 — is exempt while CDANT_1 is Kurt's arena, and
    // the preload of DANT_2 with the second cleared is what takes it into the room and turns
    // it to 270. No ticks after the preload, so nothing else can move it.
    ArenaSpace space3;
    for (const formats::Arena* arena : levelData->arenas()) {
        if (levelData->isReachable(arena->name())) {
            space3.add(arena);
        }
    }
    audio::SoundMixer mixer3(bank);
    Player puller;
    puller.setSpace(&space3);
    puller.setMixer(&mixer3);
    scripts::ScriptRuntime pull(levelData, cmi, 1, bank, &puller, nullptr);
    pull.option = 1;
    auto pullAt = [&puller, &pull, kDoorYaw](const Vec3& at, int ticks) {
        for (int i = 0; i < ticks; i++) {
            puller.teleport(at, kDoorYaw);
            pull.update(1.0f / 30.0f);
        }
    };
    pull.teleportKurt(kTunnel, Vec3{0.0f, 655.0f, -3.0f}, kDoorYaw);
    pullAt(Vec3{0.0f, 655.0f, -3.0f}, 1);
    CHECK(pull.currentArena() == kTunnel);
    pull.teleportKurt(kCorridor, kCorridorSpot, 0.0f);
    pullAt(kCorridorSpot, 1);
    CHECK(pull.currentArena() == kCorridor);
    // "DANT_2 stays loaded from the tunnel; a trigger clears it."
    pull.showArena(std::string());
    pull.preloadArena(kRoom);
    scripts::MdkObject* door = doorAt(pull, kTunnelDoor);
    CHECK(door != nullptr);
    if (door != nullptr) {
        CHECK(door->arena == kRoom);      // :80
        CHECK(door->connects == kTunnel);  // :81
        CHECK(door->yaw > kTurnedYaw - 0.01f && door->yaw < kTurnedYaw + 0.01f);  // :82
    }
}

// TELEPORT_KURT, THE DISPATCH — opcodes 112 (teleport_player) and 173
// (teleport_player_keep) through real bytecode. The runtime method takes
// (arena, position, yaw); the BYTES say [pstr, f32, f32, f32, f32] for 112
// (src/Mdk.Formats/Scripts/ScriptOpcodes.g.cs:376) and one op173_target operand for 173
// (:436, and the C++ decoder's readOp173Target is its twin). A dispatch that read the yaw
// from operand 3, or the z from operand 4, or turned before instead of after adding the
// offset, passes every runtime case above and puts Kurt into geometry. The yield is the
// other half the bytes pin: opcode 112 ends the script ONLY when it named an arena
// (ScriptVm.cs:1771-1778) and opcode 173 always does (:1296-1299).
void testTeleportOpcodesDispatch(LevelData* levelData, const Cmi* cmi, audio::SoundBank* bank) {
    ArenaSpace space;
    for (const formats::Arena* arena : levelData->arenas()) {
        if (levelData->isReachable(arena->name())) {
            space.add(arena);
        }
    }
    audio::SoundMixer mixer(bank);
    Player player;
    player.setSpace(&space);
    player.setMixer(&mixer);
    scripts::ScriptRuntime runtime(levelData, cmi, 1, bank, &player, nullptr);
    runtime.option = 1;
    player.feet = Vec3{0.0f, 655.0f, -3.0f};
    player.yaw = 90.0f;
    runtime.update(1.0f / 30.0f);
    CHECK(runtime.currentArena() == "CDANT_1");
    const float kDoorway = 673.0f;   // CDANT_1's connection 1001 plane (golden/L7_dti.txt:52)

    // ---- 112 with a named arena: the arena, the feet, the yaw, and the script ENDS.
    Code named;
    named.u8(112).pstr("DANT_2").f32(0.0f).f32(750.0f).f32(-8.0f).f32(45.0f);
    named.u8(kSetVar).u8(kGlobal).u8(0).f32(1.0f);  // the marker after the teleport
    named.end();
    scripts::MdkObject obj;
    obj.restart = 0;
    runtime.globalVariables[0] = 0.0f;
    runProgram(runtime, named, &obj);
    CHECK(runtime.currentArena() == "DANT_2");
    CHECK(player.feet.x == 0.0f && player.feet.y == 750.0f && player.feet.z == -8.0f);
    CHECK(player.yaw == 45.0f);  // operand 4, not operand 3
    CHECK(runtime.globalVariables[0] == 0.0f);  // the yield: the marker never ran
    CHECK(obj.restart == 0);

    // ---- 112 with an EMPTY arena: Kurt moves inside his own arena, flashes, and the
    // script KEEPS RUNNING (the C#'s `if (name.Length != 0)` around the yield).
    Code unnamed;
    unnamed.u8(112).pstr("").f32(0.0f).f32(700.0f).f32(-3.0f).f32(0.0f);
    unnamed.u8(kSetVar).u8(kGlobal).u8(0).f32(1.0f);
    unnamed.end();
    obj.restart = 0;
    runtime.globalVariables[0] = 0.0f;
    runProgram(runtime, unnamed, &obj);
    CHECK(runtime.currentArena() == "DANT_2");   // no arena named, so none entered
    CHECK(player.feet.y == 700.0f && player.feet.z == -3.0f);
    CHECK(player.combat().whiteFlash() == 255.0f);
    CHECK(runtime.globalVariables[0] == 1.0f);  // and it fell through: the marker ran
    CHECK(runtime.isRemoved(&obj) == false);  // the script ran on to its end, not a yield

    // ---- 173, the keep form: [arena] teleports to Kurt's OWN place and yaw, so the only
    // thing that moves is the arena. From DANT_2 across the doorway he was standing at:
    // the teleport puts him back where he was, in the corridor, and the corridor's
    // TeleportSecond arm runs (CDANT_1 is not in the MTO, and DANT_2 -- the resident arena
    // -- connects to it, so IsCorridorLoaded answers and the second is left alone).
    runtime.teleportKurt("CDANT_1", Vec3{0.0f, kDoorway - 13.0f, -3.0f}, 90.0f);
    runtime.update(1.0f / 30.0f);
    CHECK(runtime.currentArena() == "CDANT_1");
    Code keep;
    keep.u8(173).pstr("CMEAT_6");  // a level-4 name: arenaNamed finds nothing, so the
                                   // arena is still entered as far as the runtime knows
    keep.end();
    obj.restart = 0;
    runProgram(runtime, keep, &obj);
    CHECK(obj.restart == 0);  // opcode 173 yields whatever the slot held
    CHECK(player.feet.y == kDoorway - 13.0f);  // kept: no offset operand, no move

    // ---- 173, the offset form: ["", arena, dx, dy, dz, dyaw]. The C#'s CMEAT_6 lift is
    // 1971 up; here the same shape, from Kurt's snapshot, and the turn ADDED to his yaw.
    Code shifted;
    shifted.u8(173).pstr("").pstr("DANT_2").f32(10.0f).f32(-20.0f).f32(1971.0f).f32(90.0f);
    shifted.end();
    obj.restart = 0;
    runProgram(runtime, shifted, &obj);
    CHECK(runtime.currentArena() == "DANT_2");
    CHECK(player.feet.x == 10.0f);
    CHECK(player.feet.y == kDoorway - 33.0f);
    CHECK(player.feet.z == 1968.0f);
    CHECK(player.yaw == 180.0f);  // 90 kept + 90 turned
    CHECK(obj.restart == 0);
}

// A rebuilt ScriptRuntime owns a NEW SoundMixer, and that mixer owns a NEW
// SoundDevice, so the sink the frontend attached to the old device does NOT
// follow the rebuild. Review-6 F3: on the console the respawn path is not
// reachable by the emulator's idle scene, so nothing executed it there and the
// frontend's own gate is a source read. What CAN be executed here is the
// invariant that gate stands on, and it is the one that fails silently: a level
// after the first death that mixes, keeps a healthy ledger, and plays nothing.
namespace {
int g_sinkCalls = 0;
int g_sinkFrames = 0;
void countingSink(void*, const float*, int32_t frames) {
    g_sinkCalls++;
    g_sinkFrames += frames;
}
}  // namespace

void testARebuiltRuntimeNeedsItsSinkReattached(LevelData* levelData, Cmi* cmi,
                                               audio::SoundBank* bank) {
    std::unique_ptr<scripts::ScriptRuntime> first(
        new scripts::ScriptRuntime(levelData, cmi, 1, bank, nullptr, nullptr));
    audio::SoundMixer* mix = &first->mixer();
    mix->device().setSink(countingSink, nullptr);
    g_sinkCalls = 0;
    g_sinkFrames = 0;
    // The device feeds its sink on any update that produced frames, voices or
    // not, so this does not need a sound to play to be a real observation.
    mix->device().update(1.0f / 60.0f);
    CHECK(g_sinkCalls == 1);
    CHECK(g_sinkFrames > 0);

    // The respawn: destroyed and rebuilt, because that is what the C# does (a
    // fresh Viewer, a fresh ScriptRuntime) and the runtime has no reset.
    first.reset();
    std::unique_ptr<scripts::ScriptRuntime> second(
        new scripts::ScriptRuntime(levelData, cmi, 1, bank, nullptr, nullptr));
    mix = &second->mixer();
    // A rebuilt runtime is a different mixer object, which is WHY the sink has to
    // be re-attached: the C# cannot have this failure, because its Viewer owns
    // the mixer and hands the same one to the runtime (Viewer.cs:194, :207).
    // The mixer ADDRESSES are not compared: the first is freed before the second
    // is allocated, so the allocator hands back the same block and pointer
    // inequality would be an assertion about malloc, not about the runtime. The
    // sink going quiet two lines below is the real proof that this is a new device.

    // WITHOUT the re-attach the new device is muted: the mix runs, the frame
    // counter advances, and the sink never hears about it. This is the silent
    // level after the first death.
    g_sinkCalls = 0;
    mix->device().update(1.0f / 60.0f);
    CHECK(g_sinkCalls == 0);

    // WITH it, the sink is on the device the frame mixes.
    mix->device().setSink(countingSink, nullptr);
    g_sinkCalls = 0;
    mix->device().update(1.0f / 60.0f);
    CHECK(g_sinkCalls == 1);
}

} // namespace

int main() {
    // Player::facing uses the shared trig table; initialize it before the
    // opcode-170 test constructs a Player.
    mdk::mathLutInit();

    // The formats library reaches the filesystem through this seam.
    mdk::formats::setFileSystem(mdk::test::host::fileSystem());

    // The sky inverse's exact .NET reduction order, independent of game data.
    const int numericsFailures = mdk::test::runSystemNumericsTests();
    failures += numericsFailures;
    if (numericsFailures == 0) {
        printf("PASS SystemNumericsMatrixInverse\n");
    }

    // The archive-index guards the missing-TRAVSPRT.BNI refusal rests on. They
    // run BEFORE the data check on purpose: the refusal can only be OBSERVED
    // against a real data tree, so if these needed the tree they would SKIP
    // without one and stop being a gate.
    const int archiveFailures = mdk::test::runArchiveGuardTests();
    failures += archiveFailures;
    if (archiveFailures == 0) {
        printf("PASS ArchiveIndexGuards\n");
    }

    // The Citro3D per-draw matrix and the three PICA conversions. Same ordering
    // argument as the archive guards above: the 3DS cannot be run here, so this
    // suite is the ONLY thing that checks any of it, and it must not be able to
    // skip on a machine with no game data.
    const int picaFailures = mdk::test::runPicaMatrixTests();
    failures += picaFailures;
    if (picaFailures == 0) {
        printf("PASS PicaMatrix\n");
    }

    // The 3DS control mapping: the buttons, the circle pad's look, and who owns
    // the accumulated mouse. Data-independent for the same reason, and it is the
    // only thing in the tree that checks the controls at all -- the console cannot
    // be run here, and the C# oracle reads SDL, not libctru.
    // The 3DS sky dome against the oracle's panorama mapping (sky_dome_tests.h).
    const int skyFailures = mdk::test::runSkyDomeTests();
    failures += skyFailures;
    if (skyFailures == 0) {
        printf("PASS SkyDome\n");
    }

    const int inputFailures = mdk::test::runInput3dsTests();
    failures += inputFailures;
    if (inputFailures == 0) {
        printf("PASS Input3dsMap\n");
    }

    // The 3DS heap split: main capped at its 96 MiB area (Luma crash dump 41, a
    // New 3DS in extended memory). Data-independent.
    const int heapSplitFailures = mdk::test::runHeapSplitTests();
    failures += heapSplitFailures;
    if (heapSplitFailures == 0) {
        printf("PASS HeapSplit\n");
    }

    // Kurt's pack at level load and its stream (kurt_stream_tests.h), against a
    // fake backend: the 3DS cannot be run here and the stream's logic has no
    // other check, so it is data-independent and sits above the data check.
    const int kurtStreamFailures = mdk::test::runKurtStreamTests();
    failures += kurtStreamFailures;
    if (kurtStreamFailures == 0) {
        printf("PASS KurtPackStream\n");
    }

    // The MathF.Min/MathF.Max twins, in the oracle's bits. Data-independent, so it
    // sits above the game-data check for the same reason the archive guards do: a
    // test that can skip is a test that will.
    {
        const int beforeMinMax = failures;
        testMathMinMaxMatchTheOracleOnNaNAndSignedZero();
        if (failures == beforeMinMax) {
            printf("PASS MathMinMaxMatchTheOracleOnNaNAndSignedZero\n");
        }
    }

    // The follow camera's MathF.Min/MathF.Max sites, in the oracle's bits. This is
    // data-independent too, so it sits above the game-data check for the same reason.
    {
        const int beforeFollowCameraMinMax = failures;
        testFollowCameraMathFMinMaxMatchTheOracle();
        if (failures == beforeFollowCameraMinMax) {
            printf("PASS FollowCameraMathFMinMaxMatchTheOracle\n");
        }
    }

    // The shatter's group box, in the oracle's bits: the same helper, at the site the
    // last batch left behind. Data-independent for the same reason as above.
    {
        const int beforeGroupBox = failures;
        testDebrisGroupBoxPropagatesNaNLikeTheOracle();
        if (failures == beforeGroupBox) {
            printf("PASS DebrisGroupBoxPropagatesNaNLikeTheOracle\n");
        }
    }

    MdkData data = locateData();
    if (!data.found()) {
        // The exit code is the ONLY channel the gate reads. `make -C tests/scripts`
        // and ralph-pi.sh both branch on it and ignore prose, so returning 0 here
        // would discard every failure counted above — including the archive guards,
        // which are above the data check precisely so that they cannot skip. The
        // FAIL lines would then print over a green exit, which is the exact
        // pass-generator this ordering exists to avoid, and it would happen on
        // exactly the data-less machine the ordering was moved forward to cover.
        // (Vault: "a warning that exits zero is a pass generator with better
        // manners" — a validator must return in the channel its callers read.)
        printf("SKIP: no MDK data (set MDK_DATA_DIR)\n");
        if (failures != 0) {
            printf("%d check(s) FAILED (before the data check, without game data)\n", failures);
            return 1;
        }
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

    // The synthetic waypoint arena for pick_waypoint8 (opcode 221). Injected here,
    // before ANY runtime is built, so no runtime has cached a record pointer into
    // the arenas vector that setArenas is about to replace. The demo builds a Dti
    // by hand the same way (the C# LevelData.Arenas is a mutable list).
    {
        std::vector<Dti::ArenaEntry> arenas = levelData.dti().arenas();
        Dti::ArenaEntry wp;
        wp.name = "WPTEST";
        // Positions are chosen so one waypoint dominates each mode's weight total
        // (id 1 in mode 0; id 0/id 1 in mode 1), so a uniform-over-qualifying picker
        // cannot reproduce the seed-1 sequence. distance2d is the x-y ground plane
        // (z is height), and Kurt's 2D offset is (0,4); the far waypoints sit on +y
        // (toward Kurt) where toKurt < distance suppresses the +250 bump and their
        // weight falls to the bare 500-distance value. The mode-0 distance window
        // and mode-1 id window keep the membership in the header comment's table.
        const int32_t wpId[6] = {0, 1, 2, 3, 9, 21};
        const float wpPos[6][3] = {
            {30, 0, 0}, {50, 0, 0}, {0, 499, 0}, {0, 498, 0}, {0, 497, 0}, {600, 0, 0}};
        for (int i = 0; i < 6; i++) {
            Dti::Record r;
            r.type = 8;  // DtiWaypoint
            r.id = wpId[i];
            r.position[0] = wpPos[i][0];
            r.position[1] = wpPos[i][1];
            r.position[2] = wpPos[i][2];
            wp.records.push_back(r);
        }
        arenas.push_back(wp);
        // A second arena whose waypoints sit on the x-axis, so the +250 bump moves
        // the cumulative pick boundaries. WPTEST's dominant fixture pins the
        // selection DISTRIBUTION (it rejects a uniform picker) but is invariant to
        // the weight formula; WPTEST2's pinned sequence changes if the +250 bump is
        // dropped (the test comment records which other terms these pins do not
        // distinguish).
        Dti::ArenaEntry wp2;
        wp2.name = "WPTEST2";
        const float wp2Pos[6][3] = {
            {30, 0, 0}, {100, 0, 0}, {300, 0, 0}, {120, 0, 0}, {150, 0, 0}, {600, 0, 0}};
        for (int i = 0; i < 6; i++) {
            Dti::Record r;
            r.type = 8;  // DtiWaypoint
            r.id = wpId[i];
            r.position[0] = wp2Pos[i][0];
            r.position[1] = wp2Pos[i][1];
            r.position[2] = wp2Pos[i][2];
            wp2.records.push_back(r);
        }
        arenas.push_back(wp2);
        levelData.mutableDti().setArenas(arenas);
    }

    scripts::ScriptRuntime runtime(&levelData, &cmi, 1, &bank, nullptr, nullptr);
    runtime.option = 1;  // the oracle's default (ScriptRuntime.cs:214)

    testARebuiltRuntimeNeedsItsSinkReattached(&levelData, &cmi, &bank);
    printf("PASS ARebuiltRuntimeNeedsItsSinkReattached\n");

    scripts::ScriptRuntime attachRuntime(&levelData, &cmi, 1, &bank, nullptr, nullptr);
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
    testIfOptionBranchesOnTheRuntimeOption(&levelData, &cmi, &bank);
    printf("PASS IfOptionBranchesOnTheRuntimeOption\n");
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
    testFrontendTickCadenceCopiesLivePlayer(&levelData, &cmi, &bank);
    printf("PASS FrontendTickCadenceCopiesLivePlayer\n");
    testWrapAnimationFrameIsTheOracleReduction();
    printf("PASS WrapAnimationFrameIsTheOracleReduction\n");
    testDeathUsesTheOraclesCompleteFallOperation(&levelData);
    printf("PASS DeathUsesTheOraclesCompleteFallOperation\n");
    testSpawnAlongPathSpawnsEveryStep(runtime);
    printf("PASS SpawnAlongPathSpawnsEveryStep\n");
    testHeadShotsAddOnlyWhenEnabled(runtime);
    printf("PASS HeadShotsAddOnlyWhenEnabled\n");
    testSetSkyModeWritesSharedLevelState(&levelData, &cmi, &bank);
    printf("PASS SetSkyModeWritesSharedLevelState\n");
    testCameraTrackCalculatesPitchAndExpires(&levelData, &cmi, &bank);
    printf("PASS CameraTrackCalculatesPitchAndExpires\n");
    testPlaceNearKurtChoosesWrapsAndSpaces(&levelData, &cmi, &bank);
    printf("PASS PlaceNearKurtChoosesWrapsAndSpaces\n");
    testGunAimAimsAndFires(&levelData, &cmi, &bank);
    printf("PASS GunAimAimsAndFires\n");
    testPointSeesPlayerNeedsRangeAndAClearLine(runtime);
    printf("PASS PointSeesPlayerNeedsRangeAndAClearLine\n");
    testIfPlayerHealthComparesKurtHealth(&levelData, &cmi, &bank);
    printf("PASS IfPlayerHealthComparesKurtHealth\n");
    testIfIsRiddenBranchesOnRidesRidden(&levelData, &cmi, &bank);
    printf("PASS IfIsRiddenBranchesOnRidesRidden\n");
    testBlastDamageFalloff(&levelData, &cmi, &bank);
    printf("PASS BlastDamageFalloff\n");
    testPartBoundsAndWeakParts(&levelData, &cmi, &bank);
    printf("PASS PartBoundsAndWeakParts\n");
    testBlastWeakParts(&levelData, &cmi, &bank);
    printf("PASS BlastWeakParts\n");
    testIfAmmoReadsSuperChainGunAndSniperAmmo(&levelData, &cmi, &bank);
    printf("PASS IfAmmoReadsSuperChainGunAndSniperAmmo\n");
    testBossBarShowsOnlyWhileKurtFires(&levelData, &cmi, &bank);
    printf("PASS BossBarShowsOnlyWhileKurtFires\n");
    testIfPlayerOnMeLowNeedsThePlatform(&levelData, &cmi, &bank);
    printf("PASS IfPlayerOnMeLowNeedsThePlatform\n");
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
    testKurtFlashes(runtime);
    printf("PASS KurtFlashes\n");
    testUnidentifiedSet27cIsNoOp(runtime);
    printf("PASS UnidentifiedSet27cIsNoOp\n");
    testArenaTextureFrameAnimates(runtime);
    printf("PASS ArenaTextureFrameAnimates\n");
    testLobToKurtThrowsAtKurt(runtime);
    printf("PASS LobToKurtThrowsAtKurt\n");
    testHudMessageQueues(runtime);
    printf("PASS HudMessageQueues\n");
    testFanLifecycle(runtime, levelData);
    printf("PASS FanLifecycle\n");
    testPickWaypoint8(&levelData, &cmi, &bank);
    printf("PASS PickWaypoint8\n");
    testEffectsLifecycle(runtime);
    testEffectsViewGeometry();
    testEffectsRibbonVertices();
    testMessagesQueue();
    printf("PASS EffectsLifecycle\n");
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
    testChaseFliesAtTheRuntimeTarget(&levelData, &cmi, &bank);
    printf("PASS ChaseFliesAtTheRuntimeTarget\n");
    testFormationFollowersKeepTheirSlot(runtime);
    printf("PASS FormationFollowersKeepTheirSlot\n");
    testMoveToTargetOpcodeRunsChase(&levelData, &cmi, &bank);
    printf("PASS MoveToTargetOpcodeRunsChase\n");
    testCommandObjectsFormationArmInstallsTheSlot(&levelData, &cmi, &bank);
    printf("PASS CommandObjectsFormationArmInstallsTheSlot\n");
    testChaseSpeedClampPropagatesNaNLikeTheOracle(&levelData, &cmi, &bank);
    printf("PASS ChaseSpeedClampPropagatesNaNLikeTheOracle\n");
    testMoveForwardClampPropagatesNaNLikeTheOracle(&levelData, &cmi, &bank);
    printf("PASS MoveForwardClampPropagatesNaNLikeTheOracle\n");
    testWalkClampPropagatesNaNLikeTheOracle(&levelData, &cmi, &bank);
    printf("PASS WalkClampPropagatesNaNLikeTheOracle\n");
    testFlyClampPropagatesNaNLikeTheOracle(&levelData, &cmi, &bank);
    printf("PASS FlyClampPropagatesNaNLikeTheOracle\n");
    testAccelerateIsOneImplementationAcrossTheArms(&levelData, &cmi, &bank);
    printf("PASS AccelerateIsOneImplementationAcrossTheArms\n");
    testFrictionClampPropagatesNaNLikeTheOracle(&levelData, &cmi, &bank);
    printf("PASS FrictionClampPropagatesNaNLikeTheOracle\n");
    testSegmentEntrySlabPropagatesNaNLikeTheOracle(&levelData, &cmi, &bank);
    printf("PASS SegmentEntrySlabPropagatesNaNLikeTheOracle\n");
    testConeFloorIsSkippedForNaNBeforeTheClampLikeTheOracle(&levelData, &cmi, &bank);
    printf("PASS ConeFloorIsSkippedForNaNBeforeTheClampLikeTheOracle\n");
    testSweepHalfHeightPropagatesNaNLikeTheOracle(&levelData, &cmi, &bank);
    printf("PASS SweepHalfHeightPropagatesNaNLikeTheOracle\n");
    testArrivalKeepsTheStuckCountTheOracleKeeps(&levelData, &cmi, &bank);
    printf("PASS ArrivalKeepsTheStuckCountTheOracleKeeps\n");
    testCameraRollBanksTheFollowCamera();
    testFollowCameraBlendPlacesTheFrame();
    printf("PASS CameraRollBanksTheFollowCamera\n");
    testFollowCameraBlendInterpolatesThePivotAndRoll();
    printf("PASS FollowCameraBlendInterpolatesThePivotAndRoll\n");
    testFollowCameraArenaPitchUsesTheOraclesEaseRows();
    printf("PASS FollowCameraArenaPitchUsesTheOraclesEaseRows\n");
    testCameraRollMatchesTheOraclesOwnUnitTest();
    printf("PASS CameraRollMatchesTheOraclesOwnUnitTest\n");
    testCameraRollNaNStaysNaNWhereTheOracleThrows();
    printf("PASS CameraRollNaNStaysNaNWhereTheOracleThrows\n");
    testKnockDownSpendsTheCounterAndOpensTheTimer();
    printf("PASS KnockDownSpendsTheCounterAndOpensTheTimer\n");
    testAirborneKnockDownSlamsToTheFloorTheProbeReaches(&levelData);
    printf("PASS AirborneKnockDownSlamsToTheFloorTheProbeReaches\n");
    testFloorProbeAnswersWhatTheTriggerAsks(&levelData);
    printf("PASS FloorProbeAnswersWhatTheTriggerAsks\n");
    testTheSolidArenaPinIsStickyAndReplacesTheArenasAroundTheFeet(&levelData);
    printf("PASS TheSolidArenaPinIsStickyAndReplacesTheArenasAroundTheFeet\n");
    testSolidArenasAreTheDrawnListAndThePinTakesBoth(&levelData, &cmi, &bank);
    printf("PASS SolidArenasAreTheDrawnListAndThePinTakesBoth\n");
    testTriggerBoxesPreloadAndPullDoors(&levelData, &cmi, &bank);
    printf("PASS TriggerBoxesPreloadAndPullDoors\n");
    testTriggerShowBoxDrawsAndTheMinusOneBoxClears(&levelData, &cmi, &bank);
    printf("PASS TriggerShowBoxDrawsAndTheMinusOneBoxClears\n");
    testSwappedModelStreamsSwapVertices(levelData);
    printf("PASS SwappedModelStreamsSwapVertices\n");
    testBankBudgetEvictsUnpinned(data);
    printf("PASS BankBudgetEvictsUnpinned\n");
    // Arena streaming Phase 5: the music archive by its directory, the bank's
    // stepped prefetch, the level's music (audio_stream_tests.h).
    const int audioStreamFailures = mdk::test::runAudioStreamTests(data);
    failures += audioStreamFailures;
    if (audioStreamFailures == 0) {
        printf("PASS AudioStreamAndLevelMusic\n");
    }
    testTeleportKurtLevel4(data);
    printf("PASS TeleportKurtLevel4\n");
    testTeleportKurtLoadsCorridorsAndPullsDoors(&levelData, &cmi, &bank);
    printf("PASS TeleportKurtLoadsCorridorsAndPullsDoors\n");
    testTeleportOpcodesDispatch(&levelData, &cmi, &bank);
    printf("PASS TeleportOpcodesDispatch\n");
    testPushKurtDispatchesDecodedListToPlayer(&levelData, &cmi, &bank);
    printf("PASS PushKurtDispatchesDecodedListToPlayer\n");
    testPushKurtModeZeroFrameAndLiftAreExact(&levelData, &cmi, &bank);
    printf("PASS PushKurtModeZeroFrameAndLiftAreExact\n");

    // The depth layers and the depth pull: the C#'s LayerTests.cs and
    // DepthPullTests.cs, run here.
    const int layerFailures = mdk::test::runDepthLayerTests(data);
    failures += layerFailures;
    if (layerFailures == 0) {
        printf("PASS DepthLayersAndDepthPull\n");
    }

    // The sprite cache's residency policy. The C# has no bound at all, so
    // there is no C# twin for this: it pins the port's own policy, which is
    // what keeps the 3DS's sprite frames inside its memory (sprite_tests.h).
    const int spriteFailures = mdk::test::runSpriteResidencyTests();
    failures += spriteFailures;
    if (spriteFailures == 0) {
        printf("PASS SpriteResidencyPolicy\n");
    }

    // The game-flow state machine: the order of play, what follows a level, and
    // the step the frame runs on (flow_tests.h).
    const int flowFailures = mdk::test::runFlowTests();
    failures += flowFailures;
    if (flowFailures == 0) {
        printf("PASS GameFlowMatchesTheOracle\n");
    }

    // The audio sink seam: how much a frame mixes and where it goes
    // (audio_sink_tests.h). TRANSLITERATED from AudioDevice.Update, not measured:
    // the queue branch needs a sound device, and this host has none.
    const int sinkFailures = mdk::test::runAudioSinkTests();
    failures += sinkFailures;
    if (sinkFailures == 0) {
        printf("PASS AudioSinkSeam\n");
    }

    // The 3DS DSP output's platform-neutral half (voice_binding_tests.h): the
    // lifecycle with the sample plane off, bit for bit against the mix's, and the
    // voice -> channel binding (allocation, eviction that never ends a voice,
    // release on end, the music pool, the looping rebind).
    const int bindingFailures = mdk::test::runVoiceBindingTests();
    failures += bindingFailures;
    if (bindingFailures == 0) {
        printf("PASS VoiceBinding\n");
    }

    // The MVE audio decode: the 8-bit PCM path over the whole byte domain, the
    // stereo pair order, and the 16-bit branch, against the C#'s own expression
    // on a movie built in the test (mve_audio_tests.h).
    const int mveFailures = mdk::test::runMveAudioTests();
    failures += mveFailures;
    if (mveFailures == 0) {
        printf("PASS MveAudioDecode\n");
    }

    if (failures == 0) {
        printf("All VM unit tests match the oracle.\n");
        return 0;
    }
    printf("%d check(s) FAILED\n", failures);
    return 1;
}

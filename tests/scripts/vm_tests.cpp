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
// level. A divergence fails here, not in a golden.

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

    Code& target(const std::string& label) {
        fixups_.push_back(std::make_pair(bytes_.size(), label));
        for (int i = 0; i < 4; i++) {
            bytes_.push_back(0);
        }
        return *this;
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

    if (failures == 0) {
        printf("All VM unit tests match the oracle.\n");
        return 0;
    }
    printf("%d check(s) FAILED\n", failures);
    return 1;
}

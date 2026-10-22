// ============================================================================
// tick1.cpp — the level-3 tick-1 object-table check.
//
// The scripts' first end-to-end milestone: load level 3, place Kurt at the
// level's start, run one tick, and dump the object table. The arena controller
// spawns the level's objects during that tick, and each runs its type's init
// script inside Spawn. The expected table is the C# oracle's, byte for byte.
//
// The oracle is /tmp/kilo/oracle (headless Mdk.Game). Its driver sets Option=0
// and seed=1; this matches it exactly.
// ============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "test_support.h"
#include "formats/case_insensitive_path.h"
#include "formats/cmi.h"
#include "formats/file_system.h"
#include "formats/mdk_data.h"
#include "game/level_data.h"
#include "scripts/script_runtime.h"

using namespace mdk;
using namespace mdk::formats;

namespace {

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

// The oracle's table, verbatim (level 3, one tick, seed 1, Option 0).
const char* kExpected =
    "level=3 ticks=1 seed=1 currentArena=HMO_1 objects=11\n"
    "000 type=SW_BONES arena=HMO_1 pos=(-64.0000,349.0000,350.0000) yaw=0.0000 hp=100 flags=0x002008A6 dead=0 move=0/0\n"
    "001 type=SW_H01 arena=HMO_1 pos=(-59.0000,314.0000,104.0000) yaw=0.0000 hp=100 flags=0x002008A6 dead=0 move=0/0\n"
    "002 type=SW_GATT arena=HMO_1 pos=(58.0000,355.0000,380.0000) yaw=0.0000 hp=100 flags=0x002008A6 dead=0 move=0/0\n"
    "003 type=SW_HBOMB arena=HMO_1 pos=(-57.0000,186.0000,157.0000) yaw=0.0000 hp=100 flags=0x002008A6 dead=0 move=0/0\n"
    "004 type=XF arena=HMO_1 pos=(-5.0000,703.0000,306.0000) yaw=0.0000 hp=10 flags=0x00000000 dead=0 move=0/0\n"
    "005 type=XG arena=HMO_1 pos=(-49.0000,181.0000,127.0000) yaw=0.0000 hp=60 flags=0x00000006 dead=0 move=0/0\n"
    "006 type=XG arena=HMO_1 pos=(19.0000,181.0000,127.0000) yaw=0.0000 hp=60 flags=0x00000006 dead=0 move=0/0\n"
    "007 type=XS arena=HMO_1 pos=(0.0000,615.0000,115.0000) yaw=270.0000 hp=400 flags=0x00000004 dead=0 move=0/0\n"
    "008 type=XH1_DOOR arena=HMO_1 pos=(0.0000,632.0000,104.0000) yaw=270.0000 hp=65000 flags=0x01002000 dead=0 move=0/0\n"
    "009 type=XTUR arena=HMO_1 pos=(13.0000,460.0000,149.0000) yaw=0.0000 hp=100 flags=0x00000000 dead=0 move=0/0\n"
    "010 type=XTUR arena=HMO_1 pos=(-14.0000,460.0000,149.0000) yaw=0.0000 hp=100 flags=0x00000000 dead=0 move=0/0\n";

} // namespace

int main() {
    // The formats library reaches the filesystem through this seam.
    mdk::formats::setFileSystem(mdk::test::host::fileSystem());

    MdkData data = locateData();
    if (!data.found()) {
        printf("SKIP: no MDK data (set MDK_DATA_DIR)\n");
        return 0;
    }

    LevelData level;
    if (!LevelData::load(&level, &data, 3)) {
        printf("FAIL: level 3 did not load\n");
        return 1;
    }

    std::vector<uint8_t> cmiBytes;
    if (!data.read("TRAVERSE/LEVEL3/LEVEL3.CMI", &cmiBytes)) {
        printf("FAIL: LEVEL3.CMI did not read\n");
        return 1;
    }
    Cmi cmi;
    if (!Cmi::parse(&cmi, cmiBytes.data(), (int32_t)cmiBytes.size())) {
        printf("FAIL: LEVEL3.CMI did not parse\n");
        return 1;
    }

    scripts::ScriptRuntime runtime(&level, &cmi, 1);
    runtime.option = 0;  // the oracle's Option
    runtime.update(1.0f / 30.0f);

    // Build the table in the oracle's format.
    std::string out;
    char line[512];
    snprintf(line, sizeof(line), "level=3 ticks=1 seed=1 currentArena=%s objects=%d\n",
             runtime.currentArena().c_str(), (int)runtime.objects().size());
    out += line;
    int index = 0;
    for (std::deque<scripts::MdkObject>::const_iterator it = runtime.objects().begin();
         it != runtime.objects().end(); ++it, ++index) {
        const scripts::MdkObject& o = *it;
        snprintf(line, sizeof(line),
                 "%03d type=%s arena=%s pos=(%.4f,%.4f,%.4f) yaw=%.4f hp=%d flags=0x%08X dead=%d "
                 "move=%d/%d\n",
                 index, o.type_name.c_str(), o.arena.c_str(), o.position.x, o.position.y,
                 o.position.z, o.yaw, o.health, (unsigned)o.flags, o.dead ? 1 : 0, o.move_command,
                 o.move_parameter);
        out += line;
    }

    if (out == std::string(kExpected)) {
        printf("PASS: level-3 tick-1 table matches the oracle (%d objects)\n",
               (int)runtime.objects().size());
        return 0;
    }

    printf("FAIL: level-3 tick-1 table differs from the oracle\n--- got ---\n%s", out.c_str());
    printf("--- expected ---\n%s", kExpected);
    return 1;
}

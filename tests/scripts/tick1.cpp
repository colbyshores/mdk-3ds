// ============================================================================
// tick1.cpp — the level tick-1 golden-state check.
//
// The scripts' first end-to-end milestone: for each level, load it, place Kurt
// at the level's start, run one tick, and dump the object table. The arena
// controller (and, on some levels, the DTI's alien/static records) spawns the
// level's objects during that tick, and each runs its type's init script inside
// Spawn. The expected tables are the C# oracle's, byte for byte, in golden/.
//
// The oracle is /tmp/kilo/oracle (headless Mdk.Game). Its driver sets Option=0
// and seed=1; this matches it exactly.
//
// WHY TICK 1 ONLY. A spawned object runs its init script inside Spawn and gets
// no motion update on the first tick beyond path-following, so a non-path object
// stays at its spawn position. Gravity/footing/movement commands are later
// motion slices; the multi-tick golden states land with them.
//
// PATH-RIDING OBJECTS. Level 8's first object (XBSHIP) rides a path: follow_path
// puts it at the path start and Motion's path-following step (motionPath) advances
// it one frame and turns it along the path. That is the one motion that moves an
// object at tick 1 (gravity objects are footing-held at their spawn), so it is in
// scope here; the rest of ObjectMotion is not.
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

// A whole file's text, read with plain stdio (a test fixture, not game data).
bool readText(const std::string& path, std::string* out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    out->clear();
    char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        out->append(buf, n);
    }
    fclose(f);
    return true;
}

// The object table in the oracle's format.
std::string dumpTable(int level, int ticks, scripts::ScriptRuntime& runtime) {
    std::string out;
    char line[512];
    snprintf(line, sizeof(line), "level=%d ticks=%d seed=1 currentArena=%s objects=%d\n", level,
             ticks, runtime.currentArena().c_str(), runtime.objectCount());
    out += line;
    int index = 0;
    for (std::deque<scripts::MdkObject>::const_iterator it = runtime.objects().begin();
         it != runtime.objects().end(); ++it, ++index) {
        const scripts::MdkObject& o = *it;
        if (runtime.isRemoved(&o)) {
            index--;  // the oracle's list does not contain removed objects
            continue;
        }
        snprintf(line, sizeof(line),
                 "%03d type=%s arena=%s pos=(%.4f,%.4f,%.4f) yaw=%.4f hp=%d flags=0x%08X dead=%d "
                 "move=%d/%d connects=%s path=%d/%.4f/%.4f/%d\n",
                 index, o.type_name.c_str(), o.arena.c_str(), o.position.x, o.position.y,
                 o.position.z, o.yaw, o.health, (unsigned)o.flags, o.dead ? 1 : 0, o.move_command,
                 o.move_parameter, o.connects.c_str(), o.path, o.path_time, o.path_speed,
                 o.path_stop);
        out += line;
    }
    return out;
}

// Comma-join a list of ints, as the oracle's string.Join(",", list) does (empty when
// the list is empty).
std::string joinInts(const std::vector<int32_t>& v) {
    std::string s;
    char n[16];
    for (size_t i = 0; i < v.size(); i++) {
        if (i) s += ",";
        snprintf(n, sizeof(n), "%d", (int)v[i]);
        s += n;
    }
    return s;
}

// The script register file + per-object VM state, in the oracle's "script" dump format.
std::string dumpScript(int level, int ticks, scripts::ScriptRuntime& runtime) {
    std::string out;
    char line[512];
    snprintf(line, sizeof(line), "SCRIPT level=%d ticks=%d seed=1 currentArena=%s objects=%d\n",
             level, ticks, runtime.currentArena().c_str(), runtime.objectCount());
    out += line;
    const std::array<float, 4>& g = runtime.globalVariables;
    snprintf(line, sizeof(line), "GVAR %.4f %.4f %.4f %.4f\n", g[0], g[1], g[2], g[3]);
    out += line;
    int index = 0;
    for (std::deque<scripts::MdkObject>::const_iterator it = runtime.objects().begin();
         it != runtime.objects().end(); ++it, ++index) {
        const scripts::MdkObject& o = *it;
        if (runtime.isRemoved(&o)) {
            index--;  // the oracle's list does not contain removed objects
            continue;
        }
        snprintf(line, sizeof(line),
                 "OBJ %03d type=%s sflags=0x%08X vars=%.4f,%.4f,%.4f,%.4f restart=%d wait=%.4f "
                 "wresume=%d death=%d cmdtgt=%d gret=%s gres=%s timers=%.4f,%.4f,%.4f,%.4f,%.4f\n",
                 index, o.type_name.c_str(), (unsigned)o.script_flags, o.variables[0], o.variables[1],
                 o.variables[2], o.variables[3], o.restart, o.wait_time, o.wait_resume, o.death_script,
                 o.command_target, joinInts(o.gosub_returns).c_str(), joinInts(o.gosub_restarts).c_str(),
                 o.level_timers[0], o.level_timers[1], o.level_timers[2], o.level_timers[3],
                 o.level_timers[4]);
        out += line;
    }
    return out;
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

    int failures = 0;
    // (level, ticks) cases. Tick 1 covers every shipped level; the multi-tick cases
    // exercise the motion slices that move an object after its spawn tick:
    // path-following, gravity free-fall, the BSP footing sweep, and the
    // face_target / turn_to_yaw turning opcodes. A level/tick is added here only
    // once its whole table matches the oracle byte for byte. The deeper ticks still
    // differ and are deferred to their own slices: the walking/chasing enemies'
    // stuck detection and replanning (DetectStuck/HandleStuck), the combat and
    // death paths, and the transient object effects (projectiles, explosions).
    struct Case {
        int level;
        int ticks;
    };
    const Case cases[] = {
        {3, 1}, {4, 1}, {5, 1}, {6, 1}, {7, 1}, {8, 1},
        {3, 2}, {4, 2}, {4, 3}, {4, 4}, {4, 5}, {4, 6}, {5, 2}, {5, 3}, {5, 4}, {5, 5}, {5, 6}, {5, 10}, {6, 2}, {6, 3}, {6, 4}, {6, 5}, {6, 6}, {6, 10}, {7, 2}, {7, 3}, {7, 4}, {7, 5}, {7, 6}, {7, 7}, {7, 10},
        {4, 15}, {5, 15}, {6, 15}, {7, 15}, {4, 20}, {5, 20}, {6, 20}, {7, 20},
        {5, 25}, {6, 25}, {7, 25}, {5, 30}, {6, 30}, {7, 30},
        {5, 40}, {6, 40}, {7, 40}, {5, 60}, {6, 60}, {7, 60},
        // The XGEN spawner's aliens: the countdown (add_var_dt) opens the spawn at
        // ~t119, and the spawned XG runs its spawn script, lands (footing), and walks
        // off with set_move_x (command 88) — the tables at t240/t300 keep matching
        // while it moves.
        {5, 120}, {5, 240}, {5, 300},
        // With .NET's degree/radian conversions spelled left-associatively (the
        // constant-folded forms are 1 ULP off at many inputs) the yaws are
        // bit-exact, so the previously-excluded cases match: L5_t180 and L3_t3
        // (their 4th-decimal yaw shifts), L8_t2+ (the walk planner's detour picked
        // a different waypoint off the ULP), and every level runs green to t300
        // except where the walk's stuck/replan slice begins to matter (L8_t120's
        // table, L3_t300's XS, and past t300).
        {3, 3}, {3, 4}, {3, 5}, {3, 6}, {3, 10}, {3, 15}, {3, 30}, {3, 60}, {3, 120},
        {4, 25}, {4, 30}, {4, 40}, {4, 60}, {4, 120}, {4, 300},
        {5, 180},
        {6, 120}, {6, 300},
        {7, 120}, {7, 300},
        {8, 2}, {8, 3}, {8, 4}, {8, 5}, {8, 6}, {8, 10}, {8, 15}, {8, 20}, {8, 25}, {8, 30},
        {8, 40}, {8, 60},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        int level = cases[c].level;
        int ticks = cases[c].ticks;
        LevelData levelData;
        if (!LevelData::load(&levelData, &data, level)) {
            printf("FAIL L%d: did not load\n", level);
            failures++;
            continue;
        }

        char cmiPath[128];
        snprintf(cmiPath, sizeof(cmiPath), "TRAVERSE/LEVEL%d/LEVEL%d.CMI", level, level);
        std::vector<uint8_t> cmiBytes;
        if (!data.read(cmiPath, &cmiBytes)) {
            printf("FAIL L%d: CMI did not read\n", level);
            failures++;
            continue;
        }
        Cmi cmi;
        if (!Cmi::parse(&cmi, cmiBytes.data(), (int32_t)cmiBytes.size())) {
            printf("FAIL L%d: CMI did not parse\n", level);
            failures++;
            continue;
        }

        scripts::ScriptRuntime runtime(&levelData, &cmi, 1);
        runtime.option = 0;  // the oracle's Option
        for (int t = 0; t < ticks; t++) {
            runtime.update(1.0f / 30.0f);
        }

        std::string got = dumpTable(level, ticks, runtime);

        char goldenPath[128];
        snprintf(goldenPath, sizeof(goldenPath), "golden/L%d_t%d.txt", level, ticks);
        std::string expected;
        if (!readText(goldenPath, &expected)) {
            printf("FAIL L%d_t%d: no golden file %s\n", level, ticks, goldenPath);
            failures++;
            continue;
        }

        if (got == expected) {
            printf("PASS L%d_t%d: table matches the oracle (%d objects)\n", level, ticks,
                   runtime.objectCount());
        } else {
            printf("FAIL L%d_t%d: table differs\n--- got ---\n%s--- expected ---\n%s", level, ticks,
                   got.c_str(), expected.c_str());
            failures++;
        }
    }

    // Script register file + per-object VM state. The object table can match while a
    // script variable or the VM's per-object state (restart/wait/timers) diverges, so
    // this is a second, independent byte-for-byte check over the same cases.
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        int level = cases[c].level;
        int ticks = cases[c].ticks;
        LevelData levelData;
        if (!LevelData::load(&levelData, &data, level)) {
            continue;
        }
        char cmiPath[128];
        snprintf(cmiPath, sizeof(cmiPath), "TRAVERSE/LEVEL%d/LEVEL%d.CMI", level, level);
        std::vector<uint8_t> cmiBytes;
        if (!data.read(cmiPath, &cmiBytes)) {
            continue;
        }
        Cmi cmi;
        if (!Cmi::parse(&cmi, cmiBytes.data(), (int32_t)cmiBytes.size())) {
            continue;
        }
        scripts::ScriptRuntime runtime(&levelData, &cmi, 1);
        runtime.option = 0;
        for (int t = 0; t < ticks; t++) {
            runtime.update(1.0f / 30.0f);
        }
        std::string got = dumpScript(level, ticks, runtime);
        char goldenPath[128];
        snprintf(goldenPath, sizeof(goldenPath), "golden/L%d_t%d_script.txt", level, ticks);
        std::string expected;
        if (!readText(goldenPath, &expected)) {
            // No script golden yet: the script state diverges on a later slice (the
            // door's wait_anim_frame parks restart on an animation frame the advance
            // slice has not landed). Skip, not fail — the object table above still
            // covers this case. A golden is added here once the state matches.
            printf("SKIP L%d_t%d_script: no script golden (later slice)\n", level, ticks);
            continue;
        }
        if (got == expected) {
            printf("PASS L%d_t%d_script: register file + VM state match the oracle\n", level, ticks);
        } else {
            printf("FAIL L%d_t%d_script: script state differs\n--- got ---\n%s--- expected ---\n%s",
                   level, ticks, got.c_str(), expected.c_str());
            failures++;
        }
    }

    // Footing (the BSP sweep that stops a gravity object on a floor). The L3_t3
    // table is now a whole-table golden; the footing probes below pin the clean
    // landings directly: each XG rests on its arena floor exactly as the oracle,
    // with velocity.Z zeroed so it stays put on later ticks.
    {
        LevelData levelData;
        if (LevelData::load(&levelData, &data, 3)) {
            std::vector<uint8_t> cmiBytes;
            if (data.read("TRAVERSE/LEVEL3/LEVEL3.CMI", &cmiBytes)) {
                Cmi cmi;
                if (Cmi::parse(&cmi, cmiBytes.data(), (int32_t)cmiBytes.size())) {
                    scripts::ScriptRuntime runtime(&levelData, &cmi, 1);
                    runtime.option = 0;
                    for (int t = 0; t < 3; t++) {
                        runtime.update(1.0f / 30.0f);
                    }
                    // The two XG spawn at z=127.0 and fall; the oracle lands them at
                    // 126.9500 and 126.9644 (their arena floors), which free-fall alone
                    // would overshoot (126.8933).
                    int checked = 0;
                    const float expectedZ[2] = {126.9500f, 126.9644f};
                    int xi = 0;
                    for (std::deque<scripts::MdkObject>::const_iterator it = runtime.objects().begin();
                         it != runtime.objects().end(); ++it) {
                        if (it->type_name != "XG") {
                            continue;
                        }
                        if (xi < 2) {
                            float z = it->position.z;
                            bool ok = z > expectedZ[xi] - 0.0001f && z < expectedZ[xi] + 0.0001f;
                            printf("%s footing XG[%d] landed z=%.4f (oracle %.4f)\n", ok ? "PASS" : "FAIL",
                                   xi, z, expectedZ[xi]);
                            if (!ok) {
                                failures++;
                            }
                            checked++;
                            xi++;
                        }
                    }
                    if (checked != 2) {
                        printf("FAIL footing: expected 2 XG objects, found %d\n", checked);
                        failures++;
                    }
                }
            }
        }
    }

    // Walk command (move_to opcode 78 -> StartMove -> PlanMove -> Walk). The commanded
    // XGs prove the turn math the oracle agrees with byte for byte: opcode 78 records
    // the command, PlanMove finds a clear line (waypoint = destination), and Walk turns
    // the yaw toward it (90 -> 84.5768 and 90 -> 84.0) via the atan2f/wrap/clamp turn;
    // they have not translated yet (speed still ramping from 0). The first XD proves the
    // FULL chain including translation: its script gates the move behind if_sees_kurt
    // and if_flag_set (both now ported), so it walks to the oracle's exact position and
    // yaw. The second XD is a near-miss (yaw 356.3 vs the oracle's 354.0): its direct
    // line is blocked and the detour waypoint differs, a BSP-raycast/detour fidelity
    // slice separate from this one, so the whole L8_t2 table is not yet green.
    {
        LevelData levelData;
        if (LevelData::load(&levelData, &data, 8)) {
            std::vector<uint8_t> cmiBytes;
            if (data.read("TRAVERSE/LEVEL8/LEVEL8.CMI", &cmiBytes)) {
                Cmi cmi;
                if (Cmi::parse(&cmi, cmiBytes.data(), (int32_t)cmiBytes.size())) {
                    scripts::ScriptRuntime runtime(&levelData, &cmi, 1);
                    runtime.option = 0;
                    for (int t = 0; t < 2; t++) {
                        runtime.update(1.0f / 30.0f);
                    }
                    // The first two XGs (in table order) are commanded to move; the
                    // oracle turns them to these yaws with move command 78 active.
                    const float expectedYaw[2] = {84.5768f, 84.0f};
                    int xi = 0;
                    for (std::deque<scripts::MdkObject>::const_iterator it = runtime.objects().begin();
                         it != runtime.objects().end() && xi < 2; ++it) {
                        if (it->type_name != "XG") {
                            continue;
                        }
                        float yaw = it->yaw;
                        bool yawOk = yaw > expectedYaw[xi] - 0.0001f && yaw < expectedYaw[xi] + 0.0001f;
                        bool cmdOk = it->move_command == 78;
                        printf("%s walk XG[%d] yaw=%.4f move=%d (oracle yaw %.4f move 78)\n",
                               (yawOk && cmdOk) ? "PASS" : "FAIL", xi, yaw, it->move_command,
                               expectedYaw[xi]);
                        if (!yawOk || !cmdOk) {
                            failures++;
                        }
                        xi++;
                    }
                    if (xi != 2) {
                        printf("FAIL walk: expected 2 commanded XG objects, found %d\n", xi);
                        failures++;
                    }
                    // The first XD (x > 0) is commanded via if_sees_kurt + if_flag_set
                    // and walks to the oracle's exact position and yaw (full translation).
                    const float xdPos[3] = {18.0663f, 115.0070f, 11.9600f};
                    const float xdYaw = 6.0f;
                    bool xdFound = false;
                    for (std::deque<scripts::MdkObject>::const_iterator it = runtime.objects().begin();
                         it != runtime.objects().end(); ++it) {
                        if (it->type_name != "XD" || it->position.x <= 0.0f) {
                            continue;
                        }
                        xdFound = true;
                        bool posOk = fabsf(it->position.x - xdPos[0]) < 0.0001f &&
                                     fabsf(it->position.y - xdPos[1]) < 0.0001f &&
                                     fabsf(it->position.z - xdPos[2]) < 0.0001f;
                        bool yawOk = fabsf(it->yaw - xdYaw) < 0.0001f;
                        bool cmdOk = it->move_command == 78;
                        printf("%s walk XD pos=(%.4f,%.4f,%.4f) yaw=%.4f move=%d (oracle "
                               "(18.0663,115.0070,11.9600) yaw 6.0000 move 78)\n",
                               (posOk && yawOk && cmdOk) ? "PASS" : "FAIL", it->position.x,
                               it->position.y, it->position.z, it->yaw, it->move_command);
                        if (!posOk || !yawOk || !cmdOk) {
                            failures++;
                        }
                        break;
                    }
                    if (!xdFound) {
                        printf("FAIL walk: expected a commanded XD object, found none\n");
                        failures++;
                    }
                }
            }
        }
    }

    // Pickup landing (UpdatePickup -> Fall). L7_t3 proves the Runner (SW_H150) lands
    // with hover 0, but that does not exercise the Z += hover arithmetic. SW_H01 is a
    // normal pickup that lands on the floor at tick 3 with hover 1.5 (z 103.95 ->
    // 105.45) and FlagLanded set, so it verifies the non-Runner landing path.
    {
        LevelData levelData;
        if (LevelData::load(&levelData, &data, 3)) {
            std::vector<uint8_t> cmiBytes;
            if (data.read("TRAVERSE/LEVEL3/LEVEL3.CMI", &cmiBytes)) {
                Cmi cmi;
                if (Cmi::parse(&cmi, cmiBytes.data(), (int32_t)cmiBytes.size())) {
                    scripts::ScriptRuntime runtime(&levelData, &cmi, 1);
                    runtime.option = 0;
                    for (int t = 0; t < 3; t++) {
                        runtime.update(1.0f / 30.0f);
                    }
                    bool found = false;
                    for (std::deque<scripts::MdkObject>::const_iterator it = runtime.objects().begin();
                         it != runtime.objects().end(); ++it) {
                        if (it->type_name != "SW_H01") {
                            continue;
                        }
                        found = true;
                        bool zOk = fabsf(it->position.z - 105.4500f) < 0.0001f;
                        bool landed = (it->flags & 0x20000) != 0;  // FlagLanded
                        printf("%s pickup SW_H01 z=%.4f landed=%d (oracle z 105.4500 landed)\n",
                               (zOk && landed) ? "PASS" : "FAIL", it->position.z, landed ? 1 : 0);
                        if (!zOk || !landed) {
                            failures++;
                        }
                        break;
                    }
                    if (!found) {
                        printf("FAIL pickup: expected an SW_H01 object, found none\n");
                        failures++;
                    }
                }
            }
        }
    }

    if (failures == 0) {
        printf("\nAll cases match the oracle.\n");
        return 0;
    }
    printf("\n%d case(s) differ.\n", failures);
    return 1;
}

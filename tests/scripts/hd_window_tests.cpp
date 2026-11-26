// ============================================================================
// hd_window_tests.cpp -- see hd_window_tests.h.
// ============================================================================

#include "hd_window_tests.h"

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "formats/cmi.h"
#include "formats/mdk_data.h"
#include "game/level_data.h"
#include "hd_window_3ds.h"

using namespace mdk;

namespace {

int failures = 0;

void expect(const char* what, long long got, long long want) {
    if (got != want) {
        printf("  FAIL: %s was %lld, expected %lld\n", what, got, want);
        failures++;
    }
}

void expectHas(const char* what, const std::vector<std::string>& list, const std::string& name,
               bool want) {
    bool has = false;
    for (size_t i = 0; i < list.size(); i++) {
        has = has || list[i] == name;
    }
    if (has != want) {
        printf("  FAIL: %s %s %s\n", what, want ? "lacks" : "has", name.c_str());
        failures++;
    }
}

// A file of `kb` KiB at 512, a quarter of it at 256 and a sixteenth at 128 (a
// chain's top level dropped each time, roughly).
void add(HdWindow* w, const char* arena, const char* path, uint32_t kb) {
    const uint32_t bytes[HdWindow::kTiers] = {kb * 1024u, kb * 256u, kb * 64u};
    w->addFile(arena, path, bytes);
}

// The chain A - CA - B - CB - C, a teleport from B to Z, and a file F shared by
// every room (the level's own textures through one palette).
void build(HdWindow* w) {
    w->clear();
    add(w, "A", "a", 100);
    add(w, "CA", "ca", 10);
    add(w, "B", "b", 200);
    add(w, "CB", "cb", 10);
    add(w, "C", "c", 300);
    add(w, "Z", "z", 50);
    add(w, "A", "f", 40);
    add(w, "B", "f", 40);
    add(w, "C", "f", 40);
    const char* doors[][2] = {{"A", "CA"}, {"CA", "B"}, {"B", "CB"}, {"CB", "C"}};
    for (size_t i = 0; i < 4; i++) {
        w->addNeighbour(doors[i][0], doors[i][1]);
        w->addNeighbour(doors[i][1], doors[i][0]);
    }
    w->addNeighbour("B", "Z");   // one way: a teleport
}

void testTheRule() {
    HdWindow w;
    build(&w);
    std::vector<std::string> arenas;
    w.windowArenas("CA", std::vector<std::string>(1, "CA"), &arenas);
    expect("CA's window: itself and both rooms", (long long)arenas.size(), 3);
    expectHas("CA's window", arenas, "A", true);
    expectHas("CA's window", arenas, "B", true);
    expectHas("CA's window (one hop only)", arenas, "CB", false);
    w.windowArenas("B", std::vector<std::string>(1, "B"), &arenas);
    expectHas("B's window (the teleport)", arenas, "Z", true);
    expectHas("B's window (one hop only)", arenas, "A", false);
    w.windowArenas("Z", std::vector<std::string>(1, "Z"), &arenas);
    expectHas("Z's window (a teleport is one way)", arenas, "B", false);
    // A drawn arena is in the window whatever the rule says (a teleport's frame
    // still draws the arena Kurt left).
    std::vector<std::string> drawn;
    drawn.push_back("A");
    w.windowArenas("C", drawn, &arenas);
    expectHas("C's window with A still drawn", arenas, "A", true);

    // The paths: the drawn arenas' first, the shared file once.
    std::vector<const char*> paths;
    std::vector<int32_t> caps;
    int32_t drawnCount = -1;
    drawn.assign(1, "CA");
    drawn.push_back("B");
    w.window("CA", drawn, &paths, &caps, &drawnCount);
    expect("CA's files: ca, b, f, a", (long long)paths.size(), 4);
    expect("the drawn arenas' files", drawnCount, 3);
    int fileF = 0;
    bool aLast = paths.size() == 4 && strcmp(paths[3], "a") == 0;
    for (size_t i = 0; i < paths.size(); i++) {
        fileF += strcmp(paths[i], "f") == 0;
    }
    expect("the shared file once", fileF, 1);
    expect("A (not drawn) after the drawn arenas' files", aLast, 1);
    expect("as built: cap 0", caps.empty() ? -1 : caps[0], 0);
    // An arena the packs know nothing of (no HD rows, no neighbours): an empty
    // window, nothing drawn first -- the frontend still sets it (the HUD's pictures
    // stay pinned, render.h) and waits for nothing.
    w.window("NOWHERE", std::vector<std::string>(1, "NOWHERE"), &paths, &caps, &drawnCount);
    expect("an unknown arena's window is empty", (long long)paths.size(), 0);
    expect("... and nothing is drawn first", drawnCount, 0);
    // A drawn arena outside the rule (a teleport's frame still drawing the arena
    // Kurt left) brings its files, and they come first.
    w.window("Z", std::vector<std::string>(1, "A"), &paths, &caps, &drawnCount);
    expect("Z with A drawn: A's two files first, then z", (long long)paths.size(), 3);
    expect("... A's files are the drawn ones", drawnCount, 2);
    // One arena's bytes: B's window is CA + B + CB + Z = 10+200+10+50 + f 40.
    expect("B's window bytes", w.windowBytes("B"), 310 * 1024);
}

void testTheFit() {
    HdWindow w;
    build(&w);
    // The windows: A 150, CA 350, B 310, CB 550 (b + cb + c + f), C 350 KiB.
    expect("everything fits 550 KiB", w.fit(550 * 1024), 550 * 1024);
    expect("nothing stepped down", (long long)w.steppedDown().size(), 0);
    // 400 KiB: CB's window is over, and in it C saves the most (c 300 -> 75 and f
    // 40 -> 10: 255 KiB, against B's 180 and CB's 7.5). f is ONE file, read at
    // the smaller cap for every arena that holds it, so after the step: A 120,
    // CA 10 + 100 + 200 + 10 = 320, B 280, CB 295, C 95.
    const uint32_t after = w.fit(400 * 1024);
    expect("one arena stepped down", (long long)w.steppedDown().size(), 1);
    expect("... C", w.steppedDown().empty() ? 0 : w.steppedDown()[0].first == "C", 1);
    expect("... to 256", w.steppedDown().empty() ? 0 : w.steppedDown()[0].second, 256);
    expect("the largest window after (CA)", after, 320 * 1024);
    std::vector<const char*> paths;
    std::vector<int32_t> caps;
    int32_t drawnCount = 0;
    w.window("C", std::vector<std::string>(1, "C"), &paths, &caps, &drawnCount);
    int capC = -1, capF = -1;
    for (size_t i = 0; i < paths.size(); i++) {
        if (strcmp(paths[i], "c") == 0) {
            capC = caps[i];
        }
        if (strcmp(paths[i], "f") == 0) {
            capF = caps[i];
        }
    }
    expect("c is read at 256", capC, 256);
    expect("f, shared with a stepped arena, is read at 256", capF, 256);
    // Nothing can fit 1 KiB: everything at 128, and the worst is returned.
    expect("no room: the worst window is answered", w.fit(1024) > 1024, 1);
}

} // namespace

namespace mdk {
namespace test {

int runHdWindowTests() {
    failures = 0;
    testTheRule();
    testTheFit();
    return failures;
}

int runHdWindowLevelTests(const formats::MdkData& data) {
    failures = 0;
    const int levels[] = {3, 6, 7};
    for (size_t l = 0; l < 3; l++) {
        LevelData level;
        if (!LevelData::load(&level, &data, levels[l])) {
            printf("  FAIL: level %d did not load\n", levels[l]);
            failures++;
            continue;
        }
        char path[64];
        snprintf(path, sizeof(path), "TRAVERSE/LEVEL%d/LEVEL%d.CMI", levels[l], levels[l]);
        std::vector<uint8_t> bytes;
        formats::Cmi cmi;
        const bool cmiRead = data.read(path, &bytes) && formats::Cmi::parse(&cmi, std::move(bytes));
        expect("the CMI reads", cmiRead, 1);
        HdWindow w;
        const int32_t named = addLevelNeighbours(level.dti(), cmiRead ? &cmi : nullptr, &w);
        expect("scripts name arenas", named > 0, 1);
        std::vector<std::string> arenas;
        if (levels[l] == 3) {
            w.windowArenas("HMO_3", std::vector<std::string>(), &arenas);
            expectHas("LEVEL3 HMO_3 (doorway)", arenas, "CHMO_2", true);
            expectHas("LEVEL3 HMO_3 (its script's teleport)", arenas, "HMO_4", true);
            expectHas("LEVEL3 HMO_3 (two doorways away)", arenas, "HMO_2", false);
        } else if (levels[l] == 6) {
            w.windowArenas("OLYM_9", std::vector<std::string>(), &arenas);
            expectHas("LEVEL6 OLYM_9 (its show trigger)", arenas, "COLYM_2", true);
        } else {
            w.windowArenas("CDANT_1", std::vector<std::string>(), &arenas);
            expectHas("LEVEL7 CDANT_1 (doorway 1000)", arenas, "DANT_1", true);
            expectHas("LEVEL7 CDANT_1 (doorway 1001)", arenas, "DANT_2", true);
            expectHas("LEVEL7 CDANT_1 (two doorways away)", arenas, "CDANT_2", false);
            w.windowArenas("DANT_1", std::vector<std::string>(), &arenas);
            expectHas("LEVEL7 DANT_1 (doorway, both ways)", arenas, "CDANT_1", true);
        }
    }
    return failures;
}

} // namespace test
} // namespace mdk

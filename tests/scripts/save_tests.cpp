// save_tests.cpp -- package #19's tests without game data: the save record and
// folder against the C# (golden/save_flow.txt, `Probe flow <dir>` of
// tests/scripts/save_probe), the JSON float writer against 20 000 floats the C#
// formatted (golden/json_floats.txt), the save prompt's decisions (Menu/SavePrompt.cs),
// the death's LASTGAME and the menu's Continue through GameFlow, and the write's
// safety (the temporary, the rename, the .bak a read falls back to).

#include "save_tests.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "game/game_flow.h"
#include "game/game_state.h"
#include "game/save_games.h"
#include "game/save_prompt.h"
#include "game/viewer_saves.h"
#include "util/json.h"

using namespace mdk;

namespace {

int failures = 0;

const char kFixedTime[] = "2026-11-20T12:00:00";

void check(const char* what, bool ok) {
    if (!ok) {
        printf("  FAIL: %s\n", what);
        failures++;
    }
}

bool readFile(const char* path, std::string* out) {
    FILE* file = fopen(path, "rb");
    if (!file) {
        return false;
    }
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        out->append(chunk, n);
    }
    fclose(file);
    return true;
}

void writeFile(const std::string& path, const std::string& text) {
    FILE* file = fopen(path.c_str(), "wb");
    if (file) {
        fwrite(text.data(), 1, text.size(), file);
        fclose(file);
    }
}

void removeTree(const std::string& dir) {
    flow::SaveGames saves(dir);
    const std::vector<std::string> names = saves.list();
    for (size_t i = 0; i < names.size(); i++) {
        saves.remove(names[i]);
    }
    const char* const kStray[] = {"BAD.sav", "note.txt", "A.sav.tmp", "A.sav.bak", "LASTGAME.sav"};
    for (size_t i = 0; i < sizeof(kStray) / sizeof(kStray[0]); i++) {
        ::remove((dir + "/" + kStray[i]).c_str());
    }
    rmdir(dir.c_str());
}

// A scratch folder under $TMPDIR (the gates give each run its own), else build/.
std::string scratchDir(const char* leaf) {
    const char* tmp = getenv("TMPDIR");
    std::string dir = (tmp && tmp[0]) ? std::string(tmp) : std::string("build");
    char pid[32];
    snprintf(pid, sizeof(pid), "/%s-%d", leaf, (int)getpid());
    return dir + pid;
}

std::string bools(bool value) {
    return value ? "True" : "False";
}

std::string orNull(bool has, const std::string& text) {
    return has ? text : "null";
}

std::string firstDiff(const std::string& got, const std::string& want) {
    size_t at = 0;
    while (at < got.size() && at < want.size() && got[at] == want[at]) {
        at++;
    }
    size_t line = at;
    while (line > 0 && want[line - 1] != '\n') {
        line--;
    }
    char buf[400];
    snprintf(buf, sizeof(buf), "at byte %lu: oracle \"%.120s\", port \"%.120s\"",
             (unsigned long)at, want.c_str() + line, line < got.size() ? got.c_str() + line : "");
    return buf;
}

} // namespace

namespace mdk {
namespace test {

std::string saveFlowDump(const std::string& dir) {
    // The C++ twin of `Probe flow <dir>` (tests/scripts/save_probe/Program.cs).
    removeTree(dir);
    std::string out;
    flow::SaveGames saves(dir);
    flow::GameState light;
    light.level = 4;
    light.deaths = 2;
    light.strikeUsed = true;
    const flow::SaveKind kinds[] = {flow::SaveKind::LevelStart, flow::SaveKind::BeforeLevel,
                                    flow::SaveKind::Snapshot};
    for (size_t i = 0; i < 3; i++) {
        out += "save " + std::to_string((int)kinds[i]) + " " +
               light.saveAt(kinds[i], kFixedTime).toJson() + "\n";
    }
    flow::SaveGame escaped;
    escaped.level = 3;
    escaped.time = "a<b>&'\"+`\\/\xc3\xa9\n\x01";
    out += "escaped " + escaped.toJson() + "\n";

    static const char* const kTexts[] = {
        "", "{}", "[]", "not json", "{\"level\":5} x", "{\"level\":2}", "{\"level\":9}",
        "{\"level\":5}",
        "{\"level\":5.9,\"type\":6,\"health\":40.5,\"deaths\":-1,\"strike_used\":true,\"time\":\"x\"}",
        "{\"level\":5,\"strike_used\":1,\"time\":7,\"snapshot\":[1],\"health\":null}",
        "{ \"level\" : 5 , \"snapshot\" : {\"a\": [1, 2 ], \"b\":\"\\u0041\"} }",
        "{\"level\":7,\"type\":1003,\"snapshot\":{},\"time\":\"\\u00e9\\ud83d\\ude00\"}",
        "{\"level\":3,\"level\":8}", "{\"level\":8e0,\"type\":3.5}", "{\"level\":-0}",
    };
    for (size_t i = 0; i < sizeof(kTexts) / sizeof(kTexts[0]); i++) {
        flow::SaveGame parsed;
        if (!flow::SaveGame::parse(kTexts[i], &parsed)) {
            out += "parse null\n";
            continue;
        }
        flow::GameState loaded;
        loaded.load(parsed);
        out += "parse kind=" + std::to_string((int)parsed.kind) +
               " level=" + std::to_string((int)parsed.level) +
               " health=" + std::to_string((int)parsed.health) +
               " deaths=" + std::to_string((int)parsed.deaths) +
               " strike=" + bools(parsed.strikeUsed) + " time=" + parsed.time +
               " state=" + orNull(parsed.hasState, parsed.state) +
               " -> load level=" + std::to_string((int)loaded.level) +
               " deaths=" + std::to_string((int)loaded.deaths) +
               " strike=" + bools(loaded.strikeUsed) +
               " snapshot=" + orNull(!loaded.snapshot.empty(), loaded.snapshot) +
               " json=" + parsed.toJson() + "\n";
    }

    const auto join = [](const std::vector<std::string>& names) {
        std::string text;
        for (size_t i = 0; i < names.size(); i++) {
            text += (i ? "," : "") + names[i];
        }
        return text;
    };
    const auto readJson = [&saves](const char* name) {
        flow::SaveGame save;
        return saves.read(name, &save) ? save.toJson() : std::string("null");
    };
    out += "list " + join(saves.list()) + " read " + readJson("A") + "\n";
    const bool wroteB = saves.write("B", light.saveAt(flow::SaveKind::BeforeLevel, kFixedTime));
    const bool wroteA = saves.write("A", light.saveAt(flow::SaveKind::LevelStart, kFixedTime));
    out += "write B " + bools(wroteB) + " write A " + bools(wroteA) + "\n";
    writeFile(dir + "/BAD.sav", "{\"level\":1}");
    writeFile(dir + "/note.txt", "not a save");
    out += "list " + join(saves.list()) + " read A " + readJson("A") + " read BAD " +
           readJson("BAD") + " read C " + readJson("C") + "\n";
    std::string fileA;
    readFile((dir + "/A.sav").c_str(), &fileA);
    out += "file A " + fileA + "\n";
    saves.remove("B");
    saves.remove("C");
    std::string quick;
    saves.quickSlot("A", &quick);
    out += "deleted B: list " + join(saves.list()) + " quick(A) " + quick + "\n";

    // A death: countDeath (Viewer.OnDied), Game.Handle(KurtDied)'s LASTGAME, the menu's
    // Continue.
    flow::GameState died;
    died.level = 6;
    flow::countDeath(&died, 6, false);
    saves.write(flow::SaveGames::kLastGame, died.saveAt(flow::SaveKind::LevelStart, kFixedTime));
    flow::SaveGame last;
    saves.read(flow::SaveGames::kLastGame, &last);
    flow::GameState next;
    next.load(last);
    std::string lastFile;
    readFile((dir + "/LASTGAME.sav").c_str(), &lastFile);
    out += "died LASTGAME " + lastFile + " -> level " + std::to_string((int)next.level) +
           " deaths " + std::to_string((int)next.deaths) + " next " +
           (last.isKind(flow::SaveKind::BeforeLevel) ? "Briefing" : "Play") + "\n";
    removeTree(dir);
    return out;
}

} // namespace test
} // namespace mdk

namespace {

void testSaveFlowGolden() {
    std::string expected;
    if (!readFile("golden/save_flow.txt", &expected)) {
        printf("  FAIL: no golden file golden/save_flow.txt\n");
        failures++;
        return;
    }
    const std::string got = test::saveFlowDump(scratchDir("save_flow"));
    if (got != expected) {
        printf("  FAIL: save_flow differs from the C# probe %s\n", firstDiff(got, expected).c_str());
        failures++;
    }
}

void testJsonFloats() {
    // golden/json_floats.txt: `<bits> [<the C#'s JsonArray(float).ToJsonString()>]`.
    std::string text;
    if (!readFile("golden/json_floats.txt", &text)) {
        printf("  FAIL: no golden file golden/json_floats.txt\n");
        failures++;
        return;
    }
    int lines = 0;
    int bad = 0;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) {
            end = text.size();
        }
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        unsigned bits = 0;
        char want[64];
        if (sscanf(line.c_str(), "%x [%63[^]]]", &bits, want) != 2) {
            continue;
        }
        float value;
        memcpy(&value, &bits, sizeof(value));
        char got[32];
        json::formatFloat(value, got);
        lines++;
        if (strcmp(got, want) != 0) {
            if (bad++ < 5) {
                printf("  FAIL: float %08X: the C# writes %s, the port %s\n", bits, want, got);
            }
        }
    }
    failures += bad;
    check("json_floats has its 20 000 floats", lines == 20000);
}

void testJsonReader() {
    json::Document doc;
    check("a document parses",
          doc.parse("{\"a\":[1,-2.5e3,\"x\\u00e9\\n\"],\"b\":{\"c\":null,\"d\":true},\"a\":7}"));
    check("the last of two names wins", doc.intOr(doc.member(doc.root(), "a"), 0) == 7);
    const int b = doc.member(doc.root(), "b");
    check("raw text is the source's", doc.raw(b) == "{\"c\":null,\"d\":true}");
    check("null is null", doc.isNull(doc.member(b, "c")));
    int32_t n = 0;
    check("1.0 is not an int (GetValue<int>)", doc.parse("1.0") && !doc.asInt(doc.root(), &n));
    check("trailing commas refuse", !doc.parse("[1,]"));
    check("comments refuse", !doc.parse("/*x*/1"));
    std::string deep(65, '[');
    deep += std::string(65, ']');
    check("65 levels refuse (JsonDocument's 64)", !doc.parse(deep));
    std::string nested(64, '[');
    nested += std::string(64, ']');
    check("64 levels parse", doc.parse(nested));
    std::string text;
    json::Writer w(&text);
    w.beginObject();
    w.key("n");
    w.valueFloat(strtof("nan", nullptr));
    w.key("i");
    w.valueFloat(-strtof("inf", nullptr));
    w.key("z");
    w.valueFloat(-0.0f);
    w.key("e");
    w.beginArray();
    w.endArray();
    w.endObject();
    check("non-finite floats are strings (Snapshot.Number)",
          text == "{\"n\":\"NaN\",\"i\":\"-Infinity\",\"z\":-0,\"e\":[]}");
}

void testPrompt() {
    const std::string dir = scratchDir("save_prompt");
    removeTree(dir);
    flow::SaveGames saves(dir);
    flow::GameState state;
    state.level = 8;
    // After LEVEL8 (Game.AfterStream): a light save, "Save Game?", offering "6".
    flow::SavePrompt ask(saves, state.saveAt(flow::SaveKind::LevelStart, kFixedTime), "6");
    check("a light save asks first", ask.stage() == flow::SavePrompt::Stage::Ask);
    check("on Yes", ask.selected() == flow::SavePrompt::kYes);
    flow::SavePrompt::Input input;
    input.down = true;
    ask.update(input, 1.0f / 60.0f);
    check("down moves to No", ask.selected() == 1);
    input = flow::SavePrompt::Input();
    input.accept = true;
    ask.update(input, 1.0f / 60.0f);
    check("No closes without a save", ask.closed() && !ask.saved());

    flow::SavePrompt yes(saves, state.saveAt(flow::SaveKind::LevelStart, kFixedTime), "6");
    input = flow::SavePrompt::Input();
    input.pointerMoved = true;
    input.pointerY = flow::SavePrompt::kRowTop + flow::SavePrompt::kRowHeight * 1.5f;
    yes.update(input, 0.0f);
    check("the pointer over the second row selects No", yes.selected() == 1);
    input.pointerY = flow::SavePrompt::kRowTop + 1.0f;
    yes.update(input, 0.0f);
    check("and over the first, Yes", yes.selected() == 0);
    input = flow::SavePrompt::Input();
    input.accept = true;
    yes.update(input, 0.0f);
    check("Yes asks for the name", yes.stage() == flow::SavePrompt::Stage::Name && !yes.closed());
    // Typing replaces at the cursor; the cursor starts after the offered name.
    input = flow::SavePrompt::Input();
    input.typed = "ab-$_x9z";
    yes.update(input, 0.0f);
    check("typed upper-cased, '-' refused, at most 8", yes.name() == "6AB$_X9Z");
    input = flow::SavePrompt::Input();
    input.home = true;
    yes.update(input, 0.0f);
    input = flow::SavePrompt::Input();
    input.typed = "q";
    yes.update(input, 0.0f);
    check("a character replaces the one at the cursor", yes.name() == "QAB$_X9Z");
    input = flow::SavePrompt::Input();
    input.del = true;
    input.right = true;
    yes.update(input, 0.0f);
    // The C#'s order: the cursor moves (right) before Delete takes the one under it.
    check("right then delete removes the one under the cursor",
          yes.name() == "QA$_X9Z" && yes.cursor() == 2);
    input = flow::SavePrompt::Input();
    input.backspace = true;
    yes.update(input, 0.0f);
    check("backspace removes the one before", yes.name() == "Q$_X9Z" && yes.cursor() == 1);
    input = flow::SavePrompt::Input();
    input.accept = true;
    input.typed = " ";
    yes.update(input, 0.0f);
    check("Space (an Accept key) does not save", !yes.closed());
    input = flow::SavePrompt::Input();
    input.accept = true;
    yes.update(input, 0.0f);
    flow::SaveGame written;
    check("Enter saves under the name", yes.saved() && yes.closed() && saves.read("Q$_X9Z", &written));
    check("the light save it was given", written.level == 8 && written.isKind(flow::SaveKind::LevelStart));

    // F2's full save asks for the name at once; an empty name does not save.
    flow::SaveGame full = state.saveAt(flow::SaveKind::Snapshot, kFixedTime);
    flow::SavePrompt name(saves, full, "123456789");
    check("a full save asks for the name", name.stage() == flow::SavePrompt::Stage::Name);
    check("the offered name is cut to 8", name.name() == "12345678");
    input = flow::SavePrompt::Input();
    input.end = true;
    for (int i = 0; i < 9; i++) {
        input.backspace = true;
        name.update(input, 0.0f);
    }
    input = flow::SavePrompt::Input();
    input.accept = true;
    name.update(input, 0.0f);
    check("an empty name does not save", !name.closed() && !name.error().empty());
    input = flow::SavePrompt::Input();
    input.back = true;
    name.update(input, 0.0f);
    check("Esc gives up", name.closed() && !name.saved());

    // The layout: the rows' baselines and the selected row's growth (5 ticks).
    flow::SavePrompt grow(saves, state.saveAt(flow::SaveKind::LevelStart, kFixedTime), "6");
    input = flow::SavePrompt::Input();
    grow.update(input, 5.0f / 30.0f);
    std::vector<flow::SavePrompt::Run> runs;
    grow.layout([](void*, const std::string&, bool) { return 100.0f; }, nullptr, &runs);
    check("three runs", runs.size() == 3);
    check("SV_ASK at 139, small", runs.size() == 3 && runs[0].y == 139.0f && runs[0].scale == 0.65f);
    check("Yes grown to full size, centred",
          runs.size() == 3 && runs[1].scale == 1.0f && runs[1].x == 250.0f && runs[1].y == 175.0f);
    check("No at 211", runs.size() == 3 && runs[2].y == 211.0f);
    removeTree(dir);
}

void testDeathAndContinue() {
    const std::string dir = scratchDir("save_death");
    removeTree(dir);
    flow::FlowOptions options;
    options.start = flow::Start::Level;
    options.level = 6;
    flow::GameFlow game(options);
    game.setSaves(flow::SaveGames(dir));
    check("the level plays", game.handle(game.firstEvent()) == flow::Screen::Level);
    flow::countDeath(&game.state(), 6, false);
    check("a death goes to the menu", game.handle(flow::Event::KurtDied) == flow::Screen::Menu);
    check("with its splash", game.state().splash);
    flow::SaveGame last;
    check("LASTGAME written", game.saves().read(flow::SaveGames::kLastGame, &last));
    check("the level's start, full health, one death",
          last.level == 6 && last.health == 100 && last.deaths == 1 &&
              last.isKind(flow::SaveKind::LevelStart));
    // The menu is shown, not skipped (package #20a): its Continue is MainMenu.Load
    // through the saves hooks, then Event.Play.
    flow::GameFlow::SkipTrail trail;
    check("the menu is not skipped", game.skipUnported(&trail) == flow::Screen::Menu &&
                                         trail.count == 0);
    flow::SaveGames hookSaves = game.saves();
    int32_t lastLevel = 0;
    check("the menu peeks LASTGAME (its Continue)",
          flow::menuPeekSave(&hookSaves, flow::SaveGames::kLastGame, &lastLevel) &&
              lastLevel == 6);
    bool lastBefore = true;
    check("Continue loads LASTGAME",
          flow::menuLoadSave(&hookSaves, flow::SaveGames::kLastGame, &game.state(), &lastBefore) &&
              !lastBefore);
    check("and plays the level", game.handle(flow::Event::Play) == flow::Screen::Level);
    check("level 6 again, the death kept", game.state().level == 6 && game.state().deaths == 1);

    // Without LASTGAME there is no Continue.
    game.saves().remove(flow::SaveGames::kLastGame);
    check("no LASTGAME: no Continue",
          !flow::menuPeekSave(&hookSaves, flow::SaveGames::kLastGame, &lastLevel));

    // --load: a before-level save starts at its briefing; a missing one quits.
    flow::GameState before;
    before.level = 4;
    game.saves().write("PRE", before.saveAt(flow::SaveKind::BeforeLevel, kFixedTime));
    flow::FlowOptions load = options;
    load.load = "PRE";
    flow::GameFlow loaded(load);
    loaded.setSaves(flow::SaveGames(dir));
    check("--load of a before-level save is its briefing",
          loaded.firstEvent() == flow::Event::Briefing && loaded.state().level == 4);
    load.load = "NONE";
    flow::GameFlow missing(load);
    missing.setSaves(flow::SaveGames(dir));
    check("--load of no save quits", missing.firstEvent() == flow::Event::Quit &&
                                        missing.takeReport() == "No saved game NONE");

    // F9 (Viewer.QuickLoad): the session's slot, else the newest; nothing -> false.
    flow::GameState state;
    flow::Event next = flow::Event::None;
    std::string log;
    check("quick load takes PRE (the only save)",
          flow::quickLoad(flow::SaveGames(dir), &state, &next, &log) &&
              next == flow::Event::Briefing && state.notice == "Game loaded");
    flow::GameState empty;
    check("nothing to load", !flow::quickLoad(flow::SaveGames(dir + "-none"), &empty, &next, &log));

    // The main menu's hooks (#20a's MenuSaves): the list, a peek, a load.
    flow::SaveGames folder(dir);
    std::vector<std::string> names;
    flow::menuListSaves(&folder, &names);
    check("the menu lists PRE", names.size() == 1 && names[0] == "PRE");
    int32_t peeked = 0;
    check("the menu peeks PRE's level", flow::menuPeekSave(&folder, "PRE", &peeked) && peeked == 4);
    check("no peek of a missing save", !flow::menuPeekSave(&folder, "NONE", &peeked));
    flow::GameState menuState;
    bool beforeLevel = false;
    check("the menu loads PRE, a before-level save",
          flow::menuLoadSave(&folder, "PRE", &menuState, &beforeLevel) && beforeLevel &&
              menuState.level == 4);
    removeTree(dir);
}

void testSafeWrite() {
    const std::string dir = scratchDir("save_safe");
    removeTree(dir);
    flow::SaveGames saves(dir);
    flow::GameState state;
    state.level = 7;
    check("a write makes the folder", saves.write("A", state.saveAt(flow::SaveKind::LevelStart, kFixedTime)));
    std::string why;
    struct stat info;
    check("no .tmp file", stat((dir + "/A.sav.tmp").c_str(), &info) != 0);
    // A power cut between the old save stepping aside and the new one landing leaves
    // only the .bak: a read takes it.
    rename((dir + "/A.sav").c_str(), (dir + "/A.sav.bak").c_str());
    flow::SaveGame save;
    check("the .bak reads back", saves.read("A", &save) && save.level == 7);
    check("and counts as there", saves.exists("A"));
    check("the list shows only complete saves", saves.list().empty());
    state.level = 3;
    check("a write over it", saves.write("A", state.saveAt(flow::SaveKind::LevelStart, kFixedTime)));
    check("the new one reads", saves.read("A", &save) && save.level == 3);
    check("the .bak is gone", stat((dir + "/A.sav.bak").c_str(), &info) != 0);
    // A folder that cannot be made: a reason, not a retry.
    writeFile(dir + "/file", "x");
    flow::SaveGames blocked(dir + "/file/saves");
    check("a folder under a file refuses with a reason",
          !blocked.write("A", save, &why) && !why.empty());
    ::remove((dir + "/file").c_str());
    // An invalid save is not a save.
    writeFile(dir + "/BAD.sav", "{\"level\":2}");
    check("an invalid save does not read", !saves.read("BAD", &save, &why));
    removeTree(dir);
}

} // namespace

namespace mdk {
namespace test {

int runSaveTests() {
    failures = 0;
    testSaveFlowGolden();
    testJsonFloats();
    testJsonReader();
    testPrompt();
    testDeathAndContinue();
    testSafeWrite();
    return failures;
}

} // namespace test
} // namespace mdk

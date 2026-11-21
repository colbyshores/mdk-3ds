// cheats_tests.cpp -- see cheats_tests.h.

#include "cheats_tests.h"

#include <stdio.h>

#include <string>

#include "game/cheats.h"
#include "game/cheats_menu.h"
#include "game/game_settings.h"
#include "game/game_state.h"
#include "game/main_menu.h"

using namespace mdk;

namespace {

int failures = 0;

void expect(const char* what, long long got, long long want) {
    if (got != want) {
        printf("  FAIL: %s was %lld, want %lld\n", what, got, want);
        failures++;
    }
}

// The probe's cases, in the order gen_goldens ran them (each golden's case lines
// repeat the arguments, so a table that drifts from the golden fails on its header).
struct TypedCase {
    int gore;
    const char* text;
};
const TypedCase kTypedCases[] = {
    {1, "tooscaryforme"},
    {1, "TOOSCARYFORMETOOSCARYFORME"},
    {0, "seethewholegame"},
    {1, "xxTOOSCARYFORMseeTheWholeGame1toos CARY-FORME!"},
    {1, "SEETHEWHOLEGAMESEETHEWHOLEGAMETOOSCARYFORME"},
    {1, "TOOSCARYFORTOOSCARYFORME"},
    {1, "SEETHEWHOLEGAMTOOSCARYFORME"},
    {0, "ABCDEFGHIJKLMNOPQRSTUVWXYZTOOSCARYFORME"},
    {1, ""},
};

struct MenuCase {
    int debug;
    const char* text;
};
const MenuCase kMenuCases[] = {
    {1, "3"},  {1, "8"},     {1, "9"},    {1, "2"}, {1, "x5"}, {1, "f"}, {1, "S"},
    {1, "d"},  {1, "hello"}, {1, "ab4f"}, {1, "q"}, {0, "5"},  {0, "f"}, {1, ""},
};

// C# Event.ToString() for the events DebugKey leaves.
const char* eventName(flow::Event event) {
    switch (event) {
        case flow::Event::None: return "None";
        case flow::Event::Play: return "Play";
        case flow::Event::Fall: return "Fall";
        case flow::Event::Stream: return "Stream";
        case flow::Event::Statistics: return "Statistics";
        case flow::Event::Quit: return "Quit";
        default: return "?";
    }
}

// The probe's `typed` section for one case: Viewer.TypeCheats over Cheats.Type.
void typedCase(int gore, const char* text, std::string* out) {
    GameSettings settings;
    settings.gore = gore != 0;
    flow::GameState state;
    flow::Cheats cheats(&settings, &state);
    // Viewer.cs:234: the runtime's Option is the setting's when the level starts.
    int option = settings.gore ? 1 : 0;
    *out += std::string("typed gore=") + std::to_string(gore) + " text=" + text + "\n";
    const std::string letters = flow::cheatLetters(text);
    char line[256];
    for (size_t i = 0; i < letters.size(); i++) {
        const flow::Cheats::Cheat cheat = cheats.type(letters[i]);
        // Viewer.TypeCheats' two arms (Viewer.cs:872-880).
        if (cheat == flow::Cheats::Gore) {
            option = settings.gore ? 1 : 0;
            *out += settings.gore ? "Gore on\n" : "Gore off\n";
        } else if (cheat == flow::Cheats::DebugKeys) {
            *out += state.debugKeys ? "Debug keys on\n" : "Debug keys off\n";
        }
        snprintf(line, sizeof(line), "c=%c cheat=%s gore=%d debug=%d option=%d window=%s\n",
                 letters[i], flow::Cheats::name(cheat), settings.gore ? 1 : 0,
                 state.debugKeys ? 1 : 0, option,
                 cheats.typed().empty() ? "-" : cheats.typed().c_str());
        *out += line;
    }
}

// The probe's `menu` section for one case: HandleKeys' typed half on the main page.
void menuCase(int debug, const char* text, std::string* out) {
    flow::GameState state;
    state.debugKeys = debug != 0;
    flow::Event next = flow::Event::None;
    std::string typed = text;
    for (size_t i = 0; i < typed.size(); i++) {
        if (typed[i] >= 'a' && typed[i] <= 'z') {
            typed[i] = (char)(typed[i] - 'a' + 'A');
        }
    }
    const bool handled = state.debugKeys && menu::MainMenu::debugKey(typed, &state, &next);
    const scripts::GameStats& s = state.stats;
    const int counts[] = {s.shots, s.shotHits, s.sniperShots, s.sniperHits,
                          s.headShots, s.enemies, s.kills};
    bool inRange = true;
    for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
        inRange = inRange && counts[i] >= 0 && counts[i] < 100;
    }
    const char* drawn = next == flow::Event::Statistics ? (inRange ? "below100" : "OUT") : "-";
    char line[256];
    snprintf(line, sizeof(line), "menu debug=%d text=%s handled=%d next=%s level=%d counts=%s\n",
             debug, text, handled ? 1 : 0, eventName(next), (int)state.level, drawn);
    *out += line;
}

bool readGolden(const char* path, std::string* text) {
    FILE* file = fopen(path, "rb");
    if (!file) {
        return false;
    }
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        text->append(chunk, n);
    }
    fclose(file);
    return true;
}

void compareGolden(const char* section, const char* path) {
    const std::string got = mdk::test::cheatsDump(section);
    std::string expected;
    if (!readGolden(path, &expected)) {
        printf("  FAIL: no golden file %s\n", path);
        failures++;
        return;
    }
    if (got == expected) {
        return;
    }
    size_t at = 0;
    while (at < got.size() && at < expected.size() && got[at] == expected[at]) {
        at++;
    }
    size_t lineStart = at;
    while (lineStart > 0 && expected[lineStart - 1] != '\n') {
        lineStart--;
    }
    printf("  FAIL: %s differs from the C# probe at byte %lu: oracle \"%.80s\", port \"%.80s\"\n",
           path, (unsigned long)at, expected.c_str() + lineStart,
           lineStart < got.size() ? got.c_str() + lineStart : "");
    failures++;
}

// The 3DS pause menu's CHEATS page: rows, wrap, flips, BACK and close.
void testCheatsMenu() {
    CheatsMenu page;
    CheatsMenu::Switches switches;
    EnhancedMenu::Input none;
    expect("a closed page does nothing", (int)page.handle(none, &switches),
           (int)CheatsMenu::Action::None);
    page.setOpen(true);
    expect("opens on GORE", page.selected(), CheatsMenu::kGoreRow);
    EnhancedMenu::Input confirm;
    confirm.confirm = true;
    expect("A flips gore", (int)page.handle(confirm, &switches), (int)CheatsMenu::Action::Changed);
    expect("gore off", switches.gore, 0);
    EnhancedMenu::Input down;
    down.down = true;
    page.handle(down, &switches);
    EnhancedMenu::Input right;
    right.right = true;
    expect("right flips god", (int)page.handle(right, &switches),
           (int)CheatsMenu::Action::Changed);
    expect("god on", switches.god, 1);
    page.handle(down, &switches);
    page.handle(confirm, &switches);
    expect("noclip on", switches.noclip, 1);
    page.handle(down, &switches);
    expect("BACK is the last row", page.selected(), CheatsMenu::kBackRow);
    expect("left on BACK does nothing", (int)page.handle(EnhancedMenu::Input(), &switches),
           (int)CheatsMenu::Action::None);
    page.handle(down, &switches);
    expect("down from BACK wraps to GORE", page.selected(), CheatsMenu::kGoreRow);
    EnhancedMenu::Input up;
    up.up = true;
    page.handle(up, &switches);
    expect("A on BACK closes", (int)page.handle(confirm, &switches),
           (int)CheatsMenu::Action::Closed);
    expect("closed", page.open(), 0);
    expect("the switches stay", switches.god && switches.noclip && !switches.gore, 1);
    page.setOpen(true);
    EnhancedMenu::Input close;
    close.close = true;
    expect("B closes", (int)page.handle(close, &switches), (int)CheatsMenu::Action::Closed);
    expect("START menu's CHEATS row", EnhancedMenu::kCheatsRow, EnhancedMenu::kOptionsRow - 1);
    EnhancedMenu start;
    start.setOpen(true);
    EnhancedToggles toggles;
    for (int32_t i = 0; i < EnhancedMenu::kCheatsRow; i++) {
        start.handle(down, &toggles);
    }
    expect("A on CHEATS opens the page", (int)start.handle(confirm, &toggles),
           (int)EnhancedMenu::Action::Cheats);
    expect("the START menu stays open under it", start.open(), 1);
}

// The main page's frame() reads Keys::typed (MainMenu.HandleKeys): the debug keys only
// with SEETHEWHOLEGAME on; "Really Quit?" takes Y/J/O/S/T for yes and N for no.
void testMainMenuTyped() {
    flow::GameState state;
    menu::MainMenu page;
    page.open(nullptr, &state, menu::MenuSaves(), false, menu::MenuPage::Main);
    menu::Keys keys;
    keys.typed = "5";
    expect("no debug keys: 5 is nothing", (int)page.frame(keys, 1.0f / 60.0f),
           (int)flow::Event::None);
    state.debugKeys = true;
    expect("debug keys: 5 plays", (int)page.frame(keys, 1.0f / 60.0f), (int)flow::Event::Play);
    expect("level 5", state.level, 5);

    flow::GameState quitState;
    menu::MainMenu quit;
    quit.open(nullptr, &quitState, menu::MenuSaves(), false, menu::MenuPage::Main);
    menu::Keys back;
    back.back = true;
    quit.frame(back, 1.0f / 60.0f);
    expect("Esc asks Really Quit?", std::string(quit.pageName()) == "Quit", 1);
    menu::Keys no;
    no.typed = "n";
    quit.frame(no, 1.0f / 60.0f);
    expect("n is no", std::string(quit.pageName()) == "Main", 1);
    quit.frame(back, 1.0f / 60.0f);
    menu::Keys yes;
    yes.typed = "j";
    expect("j is yes (Ja)", (int)quit.frame(yes, 1.0f / 60.0f), (int)flow::Event::Quit);
}

} // namespace

namespace mdk {
namespace test {

std::string cheatsDump(const char* section) {
    std::string out;
    if (std::string(section) == "typed") {
        for (size_t i = 0; i < sizeof(kTypedCases) / sizeof(kTypedCases[0]); i++) {
            typedCase(kTypedCases[i].gore, kTypedCases[i].text, &out);
        }
    } else if (std::string(section) == "menu") {
        for (size_t i = 0; i < sizeof(kMenuCases) / sizeof(kMenuCases[0]); i++) {
            menuCase(kMenuCases[i].debug, kMenuCases[i].text, &out);
        }
    }
    return out;
}

int runCheatsTests() {
    failures = 0;
    compareGolden("typed", "golden/cheats_typed.txt");
    compareGolden("menu", "golden/cheats_menu.txt");
    testCheatsMenu();
    testMainMenuTyped();
    return failures;
}

} // namespace test
} // namespace mdk

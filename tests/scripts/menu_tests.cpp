// menu_tests.cpp -- see menu_tests.h.

#include "menu_tests.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <vector>

#include "formats/font.h"
#include "formats/fti.h"
#include "formats/mdk_data.h"
#include "game/briefing.h"
#include "game/enhanced_config.h"
#include "game/enhanced_menu.h"
#include "game/game_settings.h"
#include "game/main_menu.h"
#include "game/menu_items.h"
#include "game/pause_menu.h"
#include "game/save_games.h"
#include "game/stats_screen.h"
#include "game/viewer_saves.h"
#include "render/render.h"

// The one render seam the menus reach that no other vm_tests source stubs
// (screen_view.cpp's loadPalettedImage gives a palette back on failure).
void mdk_render_release_palette(MdkRenderer, MdkPaletteId) {}

namespace mdk {
namespace test {
namespace {

int failures = 0;

#define MENU_CHECK(cond)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            failures++;                                                      \
        }                                                                    \
    } while (0)

// The schedules: the C# probe's own strings (menu_probe/Program.cs).
const char kMainSchedule[] =
    "-:10,down:4,accept:4,accept:3,accept:3,accept:3,accept:3,accept:3,"
    "down:3,accept:4,back:3,accept:4,"
    "down:2,down:2,down:2,accept:4,"
    "down:3,accept:4,right:3,down:3,right:2,right:2,left:2,down:3,accept:3,down:2,back:4,"
    "down:2,down:2,accept:4,left:3,right:2,right:2,right:2,right:2,rheld:40,down:2,lheld:8,down:2,"
    "down:2,accept:3,down:3,accept:4,"
    "down:2,down:2,down:2,accept:4,right:2,right:2,left:2,back:4,"
    "back:4,back:4,down:3,accept:4,back:4,back:4,"
    "down:3,accept:4,up:3,accept:6";
const char kPauseSchedule[] =
    "-:3,down:3,accept:4,down:3,accept:4,back:3,back:3,down:2,down:2,accept:3,"
    "new:3,back:3,new:3,up:2,down:2,down:2,down:2,accept:3";
const char kBriefingSchedule[] = "-:150,accept:2,fire:60,-:20,back:3,-:4,accept:40";
// The game's fixed step (C# Viewer.Step), as the probe's `1f / 60f`.
const float kStep = 1.0f / 60.0f;
// The saves on the main menu (package #19; the probe's SavesSchedule/ContinueSchedule).
const char kSavesSchedule[] = "-:4,down:3,down:3,down:3,accept:4,down:3,accept:4";
const char kContinueSchedule[] = "-:2,accept:4";

// The schedule's frames: the key on a step's first frame (a held one on each),
// then nothing.
std::vector<std::string> schedule(const char* text) {
    std::vector<std::string> frames;
    const std::string all(text);
    size_t at = 0;
    while (at <= all.size()) {
        size_t comma = all.find(',', at);
        if (comma == std::string::npos) {
            comma = all.size();
        }
        const std::string step = all.substr(at, comma - at);
        const size_t colon = step.find(':');
        const std::string key = step.substr(0, colon);
        const int count = atoi(step.c_str() + colon + 1);
        const bool held = key == "rheld" || key == "lheld" || key == "fire";
        for (int i = 0; i < count; i++) {
            frames.push_back(i == 0 || held ? key : "-");
        }
        at = comma + 1;
    }
    return frames;
}

menu::Keys keysOf(const std::string& key) {
    menu::Keys keys;
    keys.up = key == "up";
    keys.down = key == "down";
    keys.left = key == "left";
    keys.right = key == "right";
    keys.accept = key == "accept";
    keys.back = key == "back";
    keys.leftHeld = key == "left" || key == "lheld";
    keys.rightHeld = key == "right" || key == "rheld";
    keys.any = keys.up || keys.down || keys.left || keys.right || keys.accept || keys.back;
    return keys;
}

const char* eventName(flow::Event event) {
    switch (event) {
        case flow::Event::None: return "None";
        case flow::Event::Quit: return "Quit";
        case flow::Event::Menu: return "Menu";
        case flow::Event::Play: return "Play";
        case flow::Event::Briefing: return "Briefing";
        case flow::Event::Statistics: return "Statistics";
        case flow::Event::LevelEnded: return "LevelEnded";
        case flow::Event::GameOver: return "GameOver";
        case flow::Event::KurtDied: return "KurtDied";
        case flow::Event::GameFinished: return "GameFinished";
        case flow::Event::Fall: return "Fall";
        case flow::Event::Stream: return "Stream";
        case flow::Event::StreamEnded: return "StreamEnded";
    }
    return "?";
}

std::string f(float value) {
    return formatShortestFloat(value);
}

// The menus' printed lines, collected (the C# probe's captured Console).
void collect(void* user, const char* line) {
    std::string* out = static_cast<std::string*>(user);
    *out += line;
    *out += "\n";
}

void noApply(void*) {}

struct Fonts {
    formats::Fti fti;
    formats::Font big;
    formats::Font small;
    bool ready = false;
};

bool loadFonts(const formats::MdkData& data, Fonts* out) {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> bigBytes;
    std::vector<uint8_t> smallBytes;
    out->ready = data.read("MISC/MDKFONT.FTI", &bytes) &&
                 formats::Fti::parse(&out->fti, bytes.data(), (int32_t)bytes.size()) &&
                 out->fti.getBytes("FONTBIG", &bigBytes) &&
                 out->fti.getBytes("FONTSML", &smallBytes) &&
                 formats::Font::parse(bigBytes.data(), (int32_t)bigBytes.size(), 6, &out->big) &&
                 formats::Font::parse(smallBytes.data(), (int32_t)smallBytes.size(), 4, &out->small);
    return out->ready;
}

void printPage(menu::MenuItems& items, std::string* last, std::string* out) {
    const std::string describe = items.describe();
    items.layout();
    const std::vector<menu::MenuItems::Entry>& entries = items.entries();
    if (describe == *last || entries.empty()) {
        return;
    }
    *last = describe;
    const menu::MenuItems::Entry& e = entries[0];
    *out += "  rows x=" + f(e.x) + " w=" + f(e.width) + " h=" + f(e.height) + " y0=" + f(e.y) +
            " big=" + (menu::MenuItems::usesBigFont(e.height) ? "1" : "0") + "\n";
}

void printGrow(const menu::MenuItems& items, std::string* out) {
    // The probe's `"  grow " + string.Join(" ", ...)`: the space stays on an
    // empty page.
    *out += "  grow ";
    for (size_t i = 0; i < items.entries().size(); i++) {
        *out += (i > 0 ? " " : "") + f(items.entries()[i].grow);
    }
    *out += "\n";
}

void setUp(menu::MenuItems* items, Fonts* fonts, GameSettings* settings, std::string* printed) {
    items->setFonts(&fonts->big, &fonts->small);
    items->setHost(menu::Host::Oracle);
    items->setSettings(settings, noApply, nullptr);
    items->setPrint(collect, printed);
}

void flush(std::string* printed, std::string* out) {
    *out += *printed;
    printed->clear();
}

const char kCrafted[] =
    "master_volume= 55 \nmusic_volume=150\neffects_volume=-3\nmusic_filter=false\r\n"
    "mouse_sensitivity=1.5\ninvert_mouse=TRUE\npad_sensitivity=abc\nfullscreen=True\nwindow_size=800x600\n"
    "exclusive_size=0x5\nrender_scale=150\nvsync=2\nframe_limit=61\ngpu_backend=Vulkan\ngamma=3\n"
    "difficulty=Hard\ngore=False\ngraphics=Enhanced\nantialiasing=X4\nstereo=TabReversed\n"
    "stereo_separation=0.3\nstereo_convergence=12.5\nhead_tracking=On\ntouch_controls=1\n"
    "upscaler_url=http://x/y=z\nbind.Forward=Up\nbind.3=D\nbind.Nope=Q\nbind.Forward=W\npad.Jump=A\n"
    "mod.Zeta=Off\nmod.alpha=On\nmod.ZETA=On\nmod.beta=Maybe\ntextures=Hd\ngarbage line\n=novalue\n";

std::string dumpSettings() {
    std::string out = "== settings: defaults\n";
    out += GameSettings().format();
    out += "== settings: crafted\n";
    out += GameSettings::parse(kCrafted).format();
    out += "== settings: older\n";
    out += GameSettings::parse("fullscreen=False\ntextures=Original\nmod.hd-textures=On\ndifficulty=0\n"
                               "gamma=0.25\nstereo_convergence=99999\n")
               .format();
    return out;
}

std::string dumpTypeset(Fonts* fonts) {
    std::string out;
    char line[512];
    for (int n = 1; n <= 6; n++) {
        char name[16];
        snprintf(name, sizeof(name), "BRIEF%d", n);
        const std::vector<uint8_t> bytes = fonts->fti.getTextBytes(name);
        std::vector<TypesetLine> lines;
        const int32_t count = typesetLayout(bytes.data(), (int32_t)bytes.size(), 32.0f, &lines);
        snprintf(line, sizeof(line), "== typeset %s: %d bytes, %d lines, %d characters\n", name,
                 (int)bytes.size(), (int)lines.size(), (int)count);
        out += line;
        for (size_t i = 0; i < lines.size(); i++) {
            out += "  x=" + f(lines[i].x) + " y=" + f(lines[i].y) +
                   " centred=" + (lines[i].centred ? "1" : "0") + " start=";
            snprintf(line, sizeof(line), "%d", (int)lines[i].start);
            out += line;
            out += " \"" + std::string(lines[i].text.begin(), lines[i].text.end()) + "\"\n";
        }
    }
    return out;
}

std::string dumpMain(Fonts* fonts) {
    std::string out = "== main menu (beta: 0)\n";
    GameSettings settings;
    flow::GameState state;
    state.splash = false;
    std::string printed;
    menu::MainMenu menu;
    setUp(&menu.items(), fonts, &settings, &printed);
    menu.open(&fonts->fti, &state, menu::MenuSaves(), false, menu::MenuPage::Main);
    flush(&printed, &out);
    std::string last;
    const std::vector<std::string> frames = schedule(kMainSchedule);
    char line[256];
    for (size_t i = 0; i < frames.size(); i++) {
        const flow::Event next = menu.frame(keysOf(frames[i]), kStep);
        flush(&printed, &out);
        printPage(menu.items(), &last, &out);
        snprintf(line, sizeof(line), "f%d %s page=%s next=%s level=%d index=%d sel=%d\n", (int)i,
                 frames[i].c_str(), menu.pageName(), eventName(next), (int)state.level,
                 (int)menu.levelIndex(), (int)menu.items().selected());
        out += line;
        printGrow(menu.items(), &out);
    }
    out += "== main menu: settings after\n";
    out += settings.format();
    return out;
}

// The probe's "saves" section: a user folder with LASTGAME, A, B and BAD, read
// through package #19's hooks (flow::menuListSaves/menuPeekSave/menuLoadSave).
std::string dumpSaves(Fonts* fonts) {
    std::string out = "== saves\n";
    const char* tmp = getenv("TMPDIR");
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/menu-saves-%d", (tmp && tmp[0]) ? tmp : "build", (int)getpid());
    flow::SaveGames saves(dir);
    const char kTime[] = "2026-11-20T12:00:00";
    flow::SaveGame save;
    save.time = kTime;
    save.level = 6;
    save.deaths = 1;
    saves.write(flow::SaveGames::kLastGame, save);
    save.level = 3;
    save.deaths = 0;
    saves.write("A", save);
    save.kind = (int32_t)flow::SaveKind::BeforeLevel;
    save.level = 4;
    save.deaths = 2;
    save.strikeUsed = true;
    saves.write("B", save);
    if (FILE* bad = fopen(saves.pathOf("BAD").c_str(), "wb")) {
        fputs("{\"level\":1}", bad);
        fclose(bad);
    }
    menu::MenuSaves hooks;
    hooks.list = flow::menuListSaves;
    hooks.peek = flow::menuPeekSave;
    hooks.load = flow::menuLoadSave;
    hooks.user = &saves;
    const char* const schedules[2] = {kSavesSchedule, kContinueSchedule};
    const char* const labels[2] = {"saved game", "continue"};
    char line[256];
    for (int s = 0; s < 2; s++) {
        out += std::string("== saves: ") + labels[s] + "\n";
        GameSettings settings;
        flow::GameState state;
        state.splash = false;
        std::string printed;
        menu::MainMenu menu;
        setUp(&menu.items(), fonts, &settings, &printed);
        menu.open(&fonts->fti, &state, hooks, false, menu::MenuPage::Main);
        flush(&printed, &out);
        std::string last;
        const std::vector<std::string> frames = schedule(schedules[s]);
        for (size_t i = 0; i < frames.size(); i++) {
            const flow::Event next = menu.frame(keysOf(frames[i]), kStep);
            flush(&printed, &out);
            printPage(menu.items(), &last, &out);
            snprintf(line, sizeof(line), "f%d %s page=%s next=%s level=%d deaths=%d strike=%d sel=%d\n",
                     (int)i, frames[i].c_str(), menu.pageName(), eventName(next), (int)state.level,
                     (int)state.deaths, state.strikeUsed ? 1 : 0, (int)menu.items().selected());
            out += line;
        }
    }
    const char* const names[] = {"LASTGAME", "A", "B", "BAD"};
    for (size_t i = 0; i < 4; i++) {
        saves.remove(names[i]);
    }
    rmdir(dir);
    return out;
}

std::string dumpPause(Fonts* fonts) {
    std::string out = "== pause menu\n";
    GameSettings settings;
    std::string printed;
    menu::PauseMenu pause;
    setUp(&pause.items(), fonts, &settings, &printed);
    pause.show();
    flush(&printed, &out);
    std::string last;
    const std::vector<std::string> frames = schedule(kPauseSchedule);
    char line[256];
    for (size_t i = 0; i < frames.size(); i++) {
        menu::Keys keys = keysOf(frames[i]);
        if (frames[i] == "new") {
            // The probe's new PauseMenu per Viewer: show() starts _next at None.
            pause.show();
            keys = menu::Keys();
        }
        const flow::Event next = pause.open() ? pause.update(keys, kStep) : flow::Event::None;
        flush(&printed, &out);
        printPage(pause.items(), &last, &out);
        snprintf(line, sizeof(line), "f%d %s open=%d next=%s sel=%d\n", (int)i, frames[i].c_str(),
                 pause.open() ? 1 : 0, eventName(next), (int)pause.items().selected());
        out += line;
        printGrow(pause.items(), &out);
    }
    return out;
}

std::string dumpBriefing(Fonts* fonts) {
    std::string out;
    const int32_t levels[] = {7, 5};
    char line[256];
    for (int l = 0; l < 2; l++) {
        snprintf(line, sizeof(line), "== briefing LEVEL%d\n", (int)levels[l]);
        out += line;
        // C# StatsScreen(..., StatsPages.Briefing, null): the briefing alone,
        // which is StatsScreen's Briefing page since package #20b.
        flow::GameState state;
        state.level = levels[l];
        StatsScreen::Setup setup;
        setup.fti = &fonts->fti;
        setup.state = &state;
        setup.pages = StatsPages::Briefing;
        std::unique_ptr<StatsScreen> briefing(new StatsScreen());
        briefing->open(setup);
        const std::vector<std::string> frames = schedule(kBriefingSchedule);
        for (size_t i = 0; i < frames.size(); i++) {
            const menu::Keys keys = keysOf(frames[i]);
            StatsScreen::Keys statsKeys;
            statsKeys.any = keys.any;
            statsKeys.back = keys.back;
            statsKeys.fastHeld = frames[i] == "fire";
            const flow::Event next = briefing->frame(statsKeys, kStep);
            snprintf(line, sizeof(line), "f%d %s in=%s out=%s current=%d count=%s shown=%d ticks=%s next=%s\n",
                     (int)i, frames[i].c_str(), f(briefing->fadeIn()).c_str(),
                     f(briefing->fadeOut()).c_str(), (int)briefing->current(),
                     f(briefing->count()).c_str(), (int)briefing->shown(),
                     f(briefing->ticks()).c_str(), eventName(next));
            out += line;
            if (next != flow::Event::None) {
                break;
            }
        }
    }
    return out;
}

bool readGolden(const char* path, std::string* out) {
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

void checkGolden(const formats::MdkData& data, const char* section) {
    char path[128];
    snprintf(path, sizeof(path), "golden/menu_%s.txt", section);
    std::string expected;
    if (!readGolden(path, &expected)) {
        printf("  FAIL: no golden file %s\n", path);
        failures++;
        return;
    }
    const std::string got = menuDump(data, section);
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

// The frontends' host: the pages they can apply, and nothing else.
void testConsoleHost(Fonts* fonts) {
    GameSettings settings;
    std::string printed;
    menu::MenuItems items;
    items.setFonts(&fonts->big, &fonts->small);
    items.setHost(menu::Host::Console);
    items.setSettings(&settings, noApply, nullptr);
    items.showOptions(7);
    MENU_CHECK(items.describe() == "Graphics | Audio | Game | Back");
    // Graphics, then the page: gore alone.
    menu::Keys accept;
    accept.accept = true;
    MENU_CHECK(items.handle(accept, kStep).kind == menu::Event::None);
    MENU_CHECK(items.describe() == "Gore: On | Back");
    MENU_CHECK(items.handle(accept, kStep).kind == menu::Event::None);
    MENU_CHECK(!settings.gore && items.describe() == "Gore: Off | Back");
    // Esc: the options again, on Graphics; Esc again: the owner's back action.
    menu::Keys back;
    back.back = true;
    items.handle(back, kStep);
    MENU_CHECK(items.describe() == "Graphics | Audio | Game | Back" && items.selected() == 0);
    const menu::Event owner = items.handle(back, kStep);
    MENU_CHECK(owner.kind == menu::Event::Press && owner.action == 7);
}

// The 3DS keeps its game lines in config.ini after the toggles: each parser
// skips the other's keys, and the two round-trip together.
void testConsoleConfig() {
    GameSettings settings;
    settings.masterVolume = 30;
    settings.difficulty = GameSettings::Hard;
    settings.gore = false;
    settings.musicFilter = false;
    EnhancedToggles toggles;
    toggles.enableHaze = false;
    const std::string text = writeEnhancedConfig(toggles) + settings.formatConsole();
    EnhancedToggles readToggles;
    MENU_CHECK(parseEnhancedConfig(text.c_str(), &readToggles));
    MENU_CHECK(!readToggles.enableHaze && readToggles.enableFiltering == toggles.enableFiltering);
    GameSettings read;
    read.parseOver(text.c_str());
    MENU_CHECK(read.masterVolume == 30 && read.difficulty == GameSettings::Hard && !read.gore &&
               !read.musicFilter && read.musicVolume == 50);
    MENU_CHECK(read.formatConsole() == settings.formatConsole());
}

// The 3DS's START menu carries the C# PauseMenu's Options and Main menu rows,
// just above QUIT, which stays the last row (up from RESUME).
void testEnhancedMenuRows() {
    using Action = EnhancedMenu::Action;
    EnhancedMenu menu;
    EnhancedToggles toggles;
    menu.setOpen(true);
    EnhancedMenu::Input up;
    up.up = true;
    EnhancedMenu::Input confirm;
    confirm.confirm = true;
    menu.handle(up, &toggles);
    MENU_CHECK(menu.selected() == EnhancedMenu::kQuitRow);
    menu.handle(up, &toggles);
    MENU_CHECK(strcmp(EnhancedMenu::rowLabel(menu.selected()), "MAIN MENU") == 0);
    MENU_CHECK(menu.handle(confirm, &toggles) == Action::MainMenu);
    MENU_CHECK(menu.open());
    menu.handle(up, &toggles);
    MENU_CHECK(strcmp(EnhancedMenu::rowLabel(menu.selected()), "OPTIONS") == 0);
    MENU_CHECK(menu.handle(confirm, &toggles) == Action::Options);
}

// The menu with the 1996 demo found, and with a LASTGAME save (#19's hooks).
void testSavesAndBeta(Fonts* fonts) {
    // The 1996 demo found: its page is between Saved Game and Options.
    GameSettings settings;
    flow::GameState state;
    std::string printed;
    menu::MainMenu menu;
    setUp(&menu.items(), fonts, &settings, &printed);
    menu.open(&fonts->fti, &state, menu::MenuSaves(), true, menu::MenuPage::Main);
    MENU_CHECK(menu.items().describe() ==
               "New Game | Level: 1 | Saved Game | Beta Levels | Options | Quit");
    // A save hook answering LASTGAME puts Continue first, and NextLevel then
    // selects the third row.
    struct Fake {
        static bool peek(void*, const char* name, int32_t* level) {
            *level = 6;
            return strcmp(name, "LASTGAME") == 0;
        }
    };
    menu::MenuSaves saves;
    saves.peek = Fake::peek;
    menu.open(&fonts->fti, &state, saves, false, menu::MenuPage::Main);
    MENU_CHECK(menu.items().describe() == "Continue | New Game | Level: 1 | Saved Game | Options | Quit");
    menu::Keys down;
    down.down = true;
    menu::Keys accept;
    accept.accept = true;
    menu.frame(down, kStep);
    menu.frame(down, kStep);
    menu.frame(accept, kStep);
    MENU_CHECK(menu.items().selected() == 2 && menu.levelIndex() == 1);
}

} // namespace

std::string menuDump(const formats::MdkData& data, const char* section) {
    Fonts* fonts = new Fonts();
    std::string out;
    if (loadFonts(data, fonts)) {
        if (strcmp(section, "settings") == 0) {
            out = dumpSettings();
        } else if (strcmp(section, "typeset") == 0) {
            out = dumpTypeset(fonts);
        } else if (strcmp(section, "main") == 0) {
            out = dumpMain(fonts);
        } else if (strcmp(section, "pause") == 0) {
            out = dumpPause(fonts);
        } else if (strcmp(section, "briefing") == 0) {
            out = dumpBriefing(fonts);
        } else if (strcmp(section, "saves") == 0) {
            out = dumpSaves(fonts);
        }
    }
    delete fonts;
    return out;
}

int runMenuTests(const formats::MdkData& data) {
    failures = 0;
    const char* sections[] = {"settings", "typeset", "main", "pause", "briefing", "saves"};
    for (size_t i = 0; i < sizeof(sections) / sizeof(sections[0]); i++) {
        checkGolden(data, sections[i]);
    }
    Fonts* fonts = new Fonts();
    if (!loadFonts(data, fonts)) {
        printf("  FAIL: MDKFONT.FTI's fonts did not load\n");
        failures++;
    } else {
        testConsoleHost(fonts);
        testConsoleConfig();
        testEnhancedMenuRows();
        testSavesAndBeta(fonts);
    }
    delete fonts;
    return failures;
}

} // namespace test
} // namespace mdk

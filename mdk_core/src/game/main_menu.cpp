// main_menu.cpp -- see main_menu.h. C# src/Mdk.Game/Menu/MainMenu.cs, method by
// method.

#include "game/main_menu.h"

#include <stdio.h>

#include "formats/beta_demo.h"
#include "formats/fti.h"
#include "game/level_flow.h"

namespace mdk {
namespace menu {
namespace {

// "Really Quit?" (0x403cf8): its title in row 3, Yes and No below.
const int32_t kQuitRow = 3;
// The page of the 1996 demo's levels: its title and the levels' names.
const char kBetaTitle[] = "Beta Levels";

const char* betaLevelName(int32_t level) {
    switch (level) {
        case 1: return "96 Level 1: City";
        case 3: return "96 Level 3: Wheel Boss";
        case 6: return "96 Level 6: Olympus";
    }
    return "?";
}

} // namespace

const char* const MainMenu::kLastGame = "LASTGAME";

std::string MainMenu::text(const char* name, const char* fallback) const {
    return fti_ ? fti_->getText(name, fallback) : std::string(fallback);
}

// The C# constructor (the video, the slideshow, the splash and the song are
// not ported: main_menu.h).
void MainMenu::open(const formats::Fti* fti, flow::GameState* state, const MenuSaves& saves,
                    bool hasBeta, MenuPage first) {
    fti_ = fti;
    state_ = state;
    saves_ = saves;
    hasBeta_ = hasBeta;
    next_ = flow::Event::None;
    state_->splash = false;
    const int32_t index = flow::indexOf(state_->level);
    levelIndex_ = index > 0 ? index : 0;
    showMain();
    if (first == MenuPage::Options) {
        showOptions();
    }
}

const char* MainMenu::pageName() const {
    switch (page_) {
        case Page::Main: return "Main";
        case Page::Quit: return "Quit";
        case Page::Other: return "Other";
    }
    return "?";
}

// C# Frame's input half: HandleKeys, then the items' Update.
flow::Event MainMenu::frame(const Keys& keys, float elapsed) {
    handleKeys(keys);
    const Event event = items_.handle(keys, elapsed);
    act(event);
    items_.grow(elapsed);
    return next_;
}

// C# HandleKeys: Esc on the main page asks "Really Quit?", and there means no.
void MainMenu::handleKeys(const Keys& keys) {
    if (!items_.visible) {
        return;
    }
    if (page_ == Page::Main && keys.back) {
        showQuit();
    } else if (page_ == Page::Quit && keys.back) {
        showMain();
    }
}

void MainMenu::act(const Event& event) {
    if (event.kind == Event::None) {
        return;
    }
    const int32_t action = event.action;
    if (action >= ActBetaLevel && action < ActBetaLevel + 100) {
        playBeta(action - ActBetaLevel);
        return;
    }
    if (action >= ActSave && action < ActBetaLevel) {
        const int32_t i = action - ActSave;
        if (i >= 0 && i < (int32_t)saveNames_.size()) {
            load(saveNames_[i]);
        }
        return;
    }
    switch (action) {
        case ActContinue: load(kLastGame); break;
        case ActNewGame: newGame(); break;
        case ActNextLevel: nextLevel(); break;
        case ActSaves: showSaves(); break;
        case ActBeta: showBetaLevels(); break;
        case ActOptions: showOptions(); break;
        case ActQuit:
        case ActQuitYes: next_ = flow::Event::Quit; break;
        case ActQuitNo:
        case ActBack:
        case ActOptionsBack: showMain(); break;
        default: break;
    }
}

bool MainMenu::hasLastGame() const {
    int32_t level = 0;
    return saves_.peek && saves_.peek(saves_.user, kLastGame, &level);
}

// C# ShowQuit.
void MainMenu::showQuit() {
    items_.clear();
    page_ = Page::Quit;
    items_.firstRow = kQuitRow;
    items_.addTitle(text("ABORT1", "Really Quit?"));
    items_.addItem(text("ABORT2", "Yes"), ActQuitYes);
    items_.addItem(text("ABORT3", "No"), ActQuitNo);
    items_.print();
}

// C# ShowMain.
void MainMenu::showMain() {
    items_.clear();
    page_ = Page::Main;
    if (hasLastGame()) {
        items_.addItem(text("OPT0", "Continue"), ActContinue);
    }
    items_.addItem(text("OPT1", "New Game"), ActNewGame);
    char level[32];
    snprintf(level, sizeof(level), "Level: %d", (int)(levelIndex_ + 1));
    items_.addItem(level, ActNextLevel);
    items_.addItem(text("OPT2", "Saved Game"), ActSaves);
    if (hasBeta_) {
        items_.addItem(kBetaTitle, ActBeta);
    }
    items_.addItem(text("OPT3", "Options"), ActOptions);
    items_.addItem(text("OPT4", "Quit"), ActQuit);
    // The main page is a column at the view's left edge (0x4265c0).
    items_.alignment = MenuItems::Align::Left;
    items_.print();
}

// C# ShowSaves: the saved games (SVOPT1, or SVOPT3 when there are none), with
// their level.
void MainMenu::showSaves() {
    items_.clear();
    page_ = Page::Other;
    saveNames_.clear();
    if (saves_.list) {
        saves_.list(saves_.user, &saveNames_);
    }
    std::string title = text(!saveNames_.empty() ? "SVOPT1" : "SVOPT3", "Saved games");
    const size_t newline = title.find('\n');
    if (newline != std::string::npos) {
        title = title.substr(0, newline);
    }
    items_.addTitle(title);
    for (size_t i = 0; i < saveNames_.size(); i++) {
        int32_t level = 0;
        if (!saves_.peek || !saves_.peek(saves_.user, saveNames_[i].c_str(), &level)) {
            items_.addDisabled(saveNames_[i] + "  (" + text("SVBAD", "Invalid") + ")");
            continue;
        }
        char label[96];
        snprintf(label, sizeof(label), "%s  (Level %d)", saveNames_[i].c_str(),
                 (int)(flow::indexOf(level) + 1));
        items_.addItem(label, ActSave + (int32_t)i);
    }
    items_.addItem("Back", ActBack);
    items_.print();
}

// C# Load: a save made before a level shows its briefing first; the others
// start the level.
void MainMenu::load(const std::string& name) {
    bool beforeLevel = false;
    if (!saves_.load || !saves_.load(saves_.user, name.c_str(), state_, &beforeLevel)) {
        return;
    }
    next_ = beforeLevel ? flow::Event::Briefing : flow::Event::Play;
}

// C# ShowOptions (MAINSONG/OPTSONG are not ported).
void MainMenu::showOptions() {
    page_ = Page::Other;
    items_.showOptions(ActOptionsBack);
}

// C# ShowBetaLevels.
void MainMenu::showBetaLevels() {
    items_.clear();
    page_ = Page::Other;
    items_.addTitle(kBetaTitle);
    for (int32_t i = 0; i < 3; i++) {
        const int32_t level = formats::BetaDemo::kLevels[i];
        items_.addItem(betaLevelName(level), ActBetaLevel + level);
    }
    items_.addItem("Back", ActBack);
    items_.print();
}

// C# PlayBeta: the demo's levels start at once (no briefing, no fall).
void MainMenu::playBeta(int32_t level) {
    state_->newGame(0);
    state_->level = formats::BetaDemo::numberOf(level);
    state_->hasCarry = false;
    state_->carry = flow::FallCarry();
    next_ = flow::Event::Play;
}

// C# NewGame: the briefing, the fall, then the level.
void MainMenu::newGame() {
    state_->newGame(levelIndex_);
    next_ = flow::Event::Briefing;
}

// C# NextLevel.
void MainMenu::nextLevel() {
    levelIndex_ = (levelIndex_ + 1) % flow::kOrderCount;
    showMain();
    items_.select(page_ == Page::Main && hasLastGame() ? 2 : 1);
}

} // namespace menu
} // namespace mdk

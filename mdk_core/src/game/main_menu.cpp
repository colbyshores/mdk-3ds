// main_menu.cpp -- see main_menu.h. C# src/Mdk.Game/Menu/MainMenu.cs, method by
// method.

#include "game/main_menu.h"

#include <stdio.h>

#include <random>

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

void MainMenu::setMedia(const formats::Bni* options, const formats::MdkData* data) {
    options_ = options;
    data_ = data;
}

// The C# constructor: MAINSONG starts (`_songVoice = ui.Play(_song, Bus.Music)`).
// The video's Finished starts the slideshow (C# `_video.Finished += _slideshow.Start`).
void MainMenu::open(const formats::Fti* fti, flow::GameState* state, const MenuSaves& saves,
                    bool hasBeta, MenuPage first) {
    fti_ = fti;
    state_ = state;
    saves_ = saves;
    hasBeta_ = hasBeta;
    next_ = flow::Event::None;
    // Package #20c: the player reads the game's data; the splash when the state
    // asks for it (C# `if (state.Splash) _splash = new IntroSplash(ui, options)`).
    video_.setData(data_);
    video_.setFinished(&MainMenu::videoFinished, this);
    videoStarted_ = false;
    splashFrame_ = false;
    hasSplash_ = state_->splash && options_ != nullptr;
    if (hasSplash_) {
        splash_.open(*options_);
    }
    state_->splash = false;
    mainSong_ = true;
    songStarts_++;
    const int32_t index = flow::indexOf(state_->level);
    levelIndex_ = index > 0 ? index : 0;
    showMain();
    if (first == MenuPage::Options) {
        showOptions();
    }
}

const char* MainMenu::song() const {
    if (items_.optionsSong()) {
        return "OPTSONG";
    }
    return mainSong_ ? "MAINSONG" : nullptr;
}

const char* MainMenu::pageName() const {
    switch (page_) {
        case Page::Main: return "Main";
        case Page::Quit: return "Quit";
        case Page::Other: return "Other";
    }
    return "?";
}

// C# Frame's input half: the splash's frame, or StartVideo, the video's Update,
// HandleKeys, then the items' Update.
flow::Event MainMenu::frame(const Keys& keys, float elapsed) {
    splashFrame_ = hasSplash_ && !splash_.done();
    if (splashFrame_) {
        splash_.update(keys.any, elapsed);
        return flow::Event::None;
    }
    // C# StartVideo: once per menu.
    if (!videoStarted_) {
        videoStarted_ = true;
        video_.play("MISC/FLIC/MDK12.FLC");
    }
    video_.update(elapsed);
    // C# `_slideshow.Update(elapsed)`, whose Next shows the image on the player;
    // any input starts its wait again (Reset); the items show unless the bare
    // first slide does (`_items.Visible = _slideshow.ItemsShown`).
    const int32_t image = slideshow_.image();
    slideshow_.update(elapsed);
    if (slideshow_.image() != image) {
        showSlide();
    }
    if (keys.any) {
        slideshow_.reset();
    }
    items_.visible = slideshow_.itemsShown();
    handleKeys(keys);
    const Event event = items_.handle(keys, elapsed);
    act(event);
    items_.grow(elapsed);
    return next_;
}

// C# HandleKeys (MainMenu.cs:175-200): Esc on the main page asks "Really Quit?", and
// there means no; Y/J/O/S/T typed there mean yes and N no; the debug keys first.
void MainMenu::handleKeys(const Keys& keys) {
    // C# YesKeys and NoKeys.
    static const char kYesKeys[] = "YJOST";
    static const char kNoKeys[] = "N";
    if (!items_.visible) {
        return;
    }
    // `var typed = input.Typed.ToUpperInvariant();` (ASCII: the keys are letters and
    // digits).
    std::string typed = keys.typed;
    for (size_t i = 0; i < typed.size(); i++) {
        if (typed[i] >= 'a' && typed[i] <= 'z') {
            typed[i] = (char)(typed[i] - 'a' + 'A');
        }
    }
    if (page_ == Page::Main && state_ != nullptr && state_->debugKeys &&
        debugKey(typed, state_, &next_)) {
        return;
    }
    if (page_ == Page::Main && keys.back) {
        showQuit();
    } else if (page_ == Page::Quit && typed.find_first_of(kYesKeys) != std::string::npos) {
        next_ = flow::Event::Quit;
    } else if (page_ == Page::Quit &&
               (typed.find_first_of(kNoKeys) != std::string::npos || keys.back)) {
        showMain();
    }
}

// C# DebugKey (MainMenu.cs:202-243), branch for branch.
bool MainMenu::debugKey(const std::string& typed, flow::GameState* state, flow::Event* next) {
    // C# DebugFirstLevel/LastLevel, DebugStatistics, DebugFall (Order[4]), DebugStream
    // (Order[0]) and DebugCountLimit.
    static const char kFirstLevel = '3';
    static const char kLastLevel = '8';
    static const char kStatistics = 'D';
    static const char kFall = 'F';
    static const int32_t kFallIndex = 4;
    static const char kStream = 'S';
    static const int32_t kStreamIndex = 0;
    static const int32_t kCountLimit = 100;
    for (size_t i = 0; i < typed.size(); i++) {
        const char c = typed[i];
        if (c >= kFirstLevel && c <= kLastLevel) {
            state->level = c - '0';
            *next = flow::Event::Play;
            return true;
        }
        if (c == kFall) {
            state->level = flow::kOrder[kFallIndex];
            *next = flow::Event::Fall;
            return true;
        }
        if (c == kStream) {
            state->level = flow::kOrder[kStreamIndex];
            *next = flow::Event::Stream;
            return true;
        }
        if (c != kStatistics) {
            continue;
        }
        // Random.Shared: a host random source, not the game's (the C#'s counts are not
        // reproducible either; the golden checks only their range).
        static std::random_device device;
        static std::mt19937 engine(device());
        std::uniform_int_distribution<int32_t> count(0, kCountLimit - 1);
        scripts::GameStats& stats = state->stats;
        stats.shots = count(engine);
        stats.shotHits = count(engine);
        stats.sniperShots = count(engine);
        stats.sniperHits = count(engine);
        stats.headShots = count(engine);
        stats.enemies = count(engine);
        stats.kills = count(engine);
        *next = flow::Event::Statistics;
        return true;
    }
    return false;
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
        case ActBack: showMain(); break;
        case ActOptionsBack:
            // C# ShowOptions' back: `_songVoice = _ui.Play(_song, Bus.Music)`, then
            // ShowMain (whose Clear stops the options' song).
            mainSong_ = true;
            songStarts_++;
            showMain();
            break;
        default: break;
    }
}

// C# Status (the slideshow's line is #20b's).
std::string MainMenu::status() const {
    if (hasSplash_ && !splash_.done()) {
        return "intro splash";
    }
    if (!slideshow_.itemsShown()) {
        return "main menu: slideshow";
    }
    std::string line = std::string("main menu: ") + pageName();
    return video_.playing() ? line + ", video" : line;
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

// C# ShowOptions: MAINSONG stops while the options play OPTSONG.
void MainMenu::showOptions() {
    page_ = Page::Other;
    mainSong_ = false;
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

// C# Slideshow.Start, through the player's Finished: the video's last frame is
// kept (`_last = video.Still`), the wait starts, the slides load once.
void MainMenu::videoFinished(void* user) {
    MainMenu* menu = static_cast<MainMenu*>(user);
    const VideoPlayer& video = menu->video_;
    menu->hasLast_ = video.hasStill();
    if (menu->hasLast_) {
        menu->lastWidth_ = video.width();
        menu->lastHeight_ = video.height();
        menu->lastIndices_ = video.indices();
        menu->lastPalette_.assign(video.paletteRgb(),
                                  video.paletteRgb() + VideoPlayer::kPaletteBytes);
    }
    if (menu->data_ != nullptr) {
        menu->slideshow_.start(*menu->data_);
    }
}

// C# Slideshow.Next's showing half: image 0 is the video's last frame again, the
// others the slides (VideoPlayer.ShowStill).
void MainMenu::showSlide() {
    const int32_t image = slideshow_.image();
    if (image == 0) {
        if (hasLast_) {
            video_.showStill(lastWidth_, lastHeight_, lastIndices_.data(), lastPalette_.data());
        }
        return;
    }
    const formats::Gif* gif = slideshow_.gif(image);
    if (gif != nullptr) {
        video_.showStill(gif->width(), gif->height(), gif->indices(), gif->palette());
    }
}

} // namespace menu
} // namespace mdk

// menu_items.cpp -- see menu_items.h. C# src/Mdk.Game/Menu/MenuItems.cs, method
// by method; each function names the C# it ports.

#include "game/menu_items.h"

#include <stdio.h>

#include "formats/font.h"
#include "game/font_view.h"
#include "game/game_settings.h"
#include "game/screen_view.h"

namespace mdk {
namespace menu {
namespace {

// C# MenuItems' constants (MenuItems.cs:51-60).
const float kTop = 5.0f;
const float kRow = 36.0f;
// The baseline is 26 below the row's top.
const float kBaseline = 26.0f / 36.0f;
const float kSmallFontRow = 24.0f;
const float kSmall = 0.65f;
// 0.35 x 0.2 per tick.
const float kGrowRate = 0.07f * 30.0f;
const float kDisabledAlpha = 0.5f;
constexpr int32_t kVolumeIncrement = 10;
// The stereo sliders: a held Left/Right waits, then steps again.
const float kRepeatWait = 0.4f;
const float kRepeatRate = 0.06f;

// The options pages (C# ShowOptions' list, MenuItems.cs:411-415).
enum Page { PageDisplay, PageGraphics, PageStereo, PageAudio, PageKeyboard, PageGamepad, PageGame,
            PageMods, PageCount };
const char* const kPageNames[PageCount] = {"Display", "Graphics", "3D Stereo", "Audio",
                                           "Keyboard & mouse", "Gamepad", "Game", "Mods"};

// The options (the C#'s AddOption rows this port has).
enum Option { OptionGraphics, OptionAntiAliasing, OptionGore, OptionMaster, OptionMusic,
              OptionEffects, OptionFilter, OptionDifficulty };
const char* const kDifficultyNames[] = {"Easy", "Normal", "Hard"};
const char* const kAntiAliasingNames[] = {"Off", "2x", "4x"};

// The action ids inside kOptionsBase: open a page (from the options' row),
// back to the options (selecting a row), step an option.
const int32_t kOpenPage = MenuItems::kOptionsBase;          // + page * 16 + row
const int32_t kReturn = MenuItems::kOptionsBase + 200;      // + row
const int32_t kOptionAction = MenuItems::kOptionsBase + 300; // + option
constexpr int32_t kPageStride = 16;

// C# Wrap: ((value % count) + count) % count, for the one-step changes it is
// given (a step is +1 or -1, so one add or subtract lands in range; the loops
// make it total for any value without a runtime divisor, AGENTS.md R11).
int32_t wrap(int32_t value, int32_t count) {
    while (value < 0) {
        value += count;
    }
    while (value >= count) {
        value -= count;
    }
    return value;
}

// C# MoveToward.
float moveToward(float value, float target, float step) {
    if (value < target) {
        const float next = value + step;
        return next < target ? next : target;
    }
    const float next = value - step;
    return next > target ? next : target;
}

const char* onOff(bool value) {
    return value ? "On" : "Off";
}

// The pages a host shows, in the C#'s order.
bool hostShows(Host host, int32_t page) {
    if (host == Host::Oracle) {
        return true;
    }
    return page == PageGraphics || page == PageAudio || page == PageGame;
}

} // namespace

std::string MenuItems::describe() const {
    std::string out;
    for (size_t i = 0; i < entries_.size(); i++) {
        if (i > 0) {
            out += " | ";
        }
        out += entries_[i].text;
    }
    return out;
}

void MenuItems::print() const {
    if (print_) {
        const std::string line = "Menu: " + describe();
        print_(printUser_, line.c_str());
    }
}

// C# ClearPage.
void MenuItems::clearPage() {
    back_ = -1;
    alignment = Align::Centre;
    firstRow = 0;
    entries_.clear();
    selected_ = -1;
    repeat_ = 0.0f;
}

// C# Add.
void MenuItems::add(const Entry& entry) {
    entries_.push_back(entry);
    if (selected_ < 0 && entry.selectable()) {
        selected_ = (int32_t)entries_.size() - 1;
    }
}

void MenuItems::addItem(const std::string& text, int32_t action) {
    Entry entry;
    entry.text = text;
    entry.action = action;
    entry.grow = kSmall;
    add(entry);
}

void MenuItems::addDisabled(const std::string& text) {
    Entry entry;
    entry.text = text;
    entry.disabled = true;
    entry.grow = kSmall;
    add(entry);
}

void MenuItems::addTitle(const std::string& text) {
    Entry entry;
    entry.text = text;
    entry.title = true;
    entry.grow = 1.0f;
    add(entry);
}

// C# AddOption: a click or Right goes on, Left back; its label is made again
// whenever any option changes.
void MenuItems::addOption(int32_t option) {
    Entry entry;
    entry.text = optionLabel(option);
    entry.action = kOptionAction + option;
    entry.option = option;
    entry.grow = kSmall;
    add(entry);
}

void MenuItems::select(int32_t index) {
    for (int32_t i = index < 0 ? 0 : index; i < (int32_t)entries_.size(); i++) {
        if (entries_[i].selectable()) {
            selected_ = i;
            return;
        }
    }
}

// C# Update's head (ShowJob is not ported) and HandleInput.
Event MenuItems::handle(const Keys& keys, float delta) {
    layout();
    Event event;
    if (!visible) {
        return event;
    }
    if (back_ >= 0 && keys.back) {
        event.kind = Event::Press;
        event.action = back_;
    } else {
        if (keys.up) {
            move(-1);
        }
        if (keys.down) {
            move(1);
        }
        if (selected_ < 0 || selected_ >= (int32_t)entries_.size()) {
            return event;
        }
        const Entry& selected = entries_[selected_];
        if (keys.accept) {
            // An option's Press is Step(1); a title or a disabled item has none.
            if (selected.option >= 0) {
                event.kind = Event::Change;
                event.action = selected.action;
                event.step = 1;
            } else if (selected.action >= 0) {
                event.kind = Event::Press;
                event.action = selected.action;
            }
        } else if (selected.option >= 0) {
            int32_t step = 0;
            if (keys.right) {
                step = 1;
                repeat_ = kRepeatWait;
            } else if (keys.left) {
                step = -1;
                repeat_ = kRepeatWait;
            } else {
                // Held, an option steps on.
                const int32_t held = keys.rightHeld ? 1 : (keys.leftHeld ? -1 : 0);
                if (held == 0) {
                    repeat_ = 0.0f;
                } else {
                    repeat_ -= delta;
                    if (repeat_ <= 0.0f) {
                        step = held;
                        repeat_ = kRepeatRate;
                    }
                }
            }
            if (step != 0) {
                event.kind = Event::Change;
                event.action = selected.action;
                event.step = step;
            }
        }
    }
    Event owner;
    if (event.kind != Event::None && !act(event, &owner)) {
        return event;
    }
    return owner;
}

// C# Update's tail: the growth.
void MenuItems::grow(float delta) {
    for (size_t i = 0; i < entries_.size(); i++) {
        Entry& entry = entries_[i];
        const float target = ((int32_t)i == selected_ || entry.title) ? 1.0f : kSmall;
        entry.grow = moveToward(entry.grow, target, kGrowRate * delta);
    }
}

// C# Move: the next selectable item up or down.
void MenuItems::move(int32_t step) {
    for (int32_t i = selected_ + step; i >= 0 && i < (int32_t)entries_.size(); i += step) {
        if (!entries_[i].selectable()) {
            continue;
        }
        selected_ = i;
        return;
    }
}

int32_t MenuItems::textWidth(bool big, const std::string& text) const {
    const formats::Font* font = big ? big_ : small_;
    if (!font) {
        return 0;
    }
    // C# Encoding.ASCII.GetBytes: a character past 0x7F becomes '?'.
    std::string ascii = text;
    for (size_t i = 0; i < ascii.size(); i++) {
        if ((unsigned char)ascii[i] > 0x7F) {
            ascii[i] = '?';
        }
    }
    return font->width((const uint8_t*)ascii.data(), (int32_t)ascii.size());
}

// C# Layout.
void MenuItems::layout() {
    if (entries_.empty()) {
        return;
    }
    const float row = rowFor((int32_t)entries_.size());
    const bool big = usesBigFont(row);
    int32_t width = 0;
    for (size_t i = 0; i < entries_.size(); i++) {
        const int32_t w = textWidth(big, entries_[i].text);
        width = w > width ? w : width;
    }
    const float x = alignment == Align::Left ? 0.0f : (ScreenView::kWidth - (float)width) / 2.0f;
    for (size_t i = 0; i < entries_.size(); i++) {
        entries_[i].x = x;
        entries_[i].y = rowTop(row, (int32_t)i + firstRow);
        entries_[i].width = (float)width;
        entries_[i].height = row;
    }
}

float MenuItems::rowFor(int32_t entries) {
    const float fit = (ScreenView::kHeight - kTop * 2.0f) / (float)(entries > 1 ? entries : 1);
    return kRow < fit ? kRow : fit;
}

bool MenuItems::usesBigFont(float row) {
    return row >= kSmallFontRow;
}

float MenuItems::rowTop(float row, int32_t index) {
    return kTop + row * (float)index;
}

// C# Draw.
void MenuItems::draw(MdkRenderer renderer, const ScreenView& view, const FontView& big,
                     const FontView& small) {
    if (!visible || entries_.empty()) {
        return;
    }
    // A press may have just opened another page: its items have no rows yet.
    layout();
    const float row = entries_[0].height;
    const bool useBig = usesBigFont(row);
    const FontView& font = useBig ? big : small;
    const float baseline = roundEven(row * kBaseline);
    for (size_t i = 0; i < entries_.size(); i++) {
        const Entry& entry = entries_[i];
        const float x =
            entry.x + (entry.width - (float)textWidth(useBig, entry.text) * entry.grow) / 2.0f;
        const float alpha = entry.disabled ? kDisabledAlpha : 1.0f;
        view.text(renderer, font, (const uint8_t*)entry.text.data(), (int32_t)entry.text.size(),
                  roundEven(x), entry.y + baseline, entry.grow, alpha);
    }
}

// --- Options -------------------------------------------------------------------

// C# ShowOptions.
void MenuItems::showOptions(int32_t back) {
    clearPage();
    back_ = back;
    optionsBack_ = back;
    int32_t row = 0;
    for (int32_t page = 0; page < PageCount; page++) {
        if (!hostShows(host_, page)) {
            continue;
        }
        addItem(kPageNames[page], kOpenPage + page * kPageStride + row);
        row++;
    }
    addItem("Back", back);
    // C# `if (_songVoice == 0) _songVoice = _ui.Play(_song, Bus.Music)`.
    if (!optionsSong_) {
        optionsSong_ = true;
        songStarts_++;
    }
    print();
}

// C# EndPage: Esc and the page's last item go back.
void MenuItems::endPage(int32_t back) {
    back_ = back;
    addItem("Back", back);
    print();
}

// C# ShowDisplay/ShowGraphics/ShowStereo/ShowAudio/ShowControls/ShowGamepad/
// ShowGame/ShowMods, as this port has them (the header).
void MenuItems::showPage(int32_t page, int32_t row) {
    clearPage();
    const int32_t back = kReturn + row;
    switch (page) {
        case PageGraphics:
            // The look and its anti-aliasing are the desktop's (the 3DS's look is
            // the pause menu's toggles; the Linux frontend's is --enhanced).
            if (host_ == Host::Oracle) {
                addOption(OptionGraphics);
                addOption(OptionAntiAliasing);
            }
            addOption(OptionGore);
            break;
        case PageAudio:
            addOption(OptionMaster);
            addOption(OptionMusic);
            addOption(OptionEffects);
            addOption(OptionFilter);
            break;
        case PageGame:
            addOption(OptionDifficulty);
            break;
        default:
            break;
    }
    endPage(back);
}

std::string MenuItems::optionLabel(int32_t option) const {
    if (!settings_) {
        return std::string();
    }
    const GameSettings& s = *settings_;
    char buffer[64];
    switch (option) {
        case OptionGraphics:
            snprintf(buffer, sizeof(buffer), "Graphics: %s", GameSettings::graphicsName(s.graphics));
            break;
        case OptionAntiAliasing:
            snprintf(buffer, sizeof(buffer), "Anti-aliasing: %s",
                     kAntiAliasingNames[(uint32_t)s.antiAliasing < 3u ? (uint32_t)s.antiAliasing : 0u]);
            break;
        case OptionGore:
            snprintf(buffer, sizeof(buffer), "Gore: %s", onOff(s.gore));
            break;
        case OptionMaster:
            snprintf(buffer, sizeof(buffer), "Master volume: %d", (int)s.masterVolume);
            break;
        case OptionMusic:
            snprintf(buffer, sizeof(buffer), "Music volume: %d", (int)s.musicVolume);
            break;
        case OptionEffects:
            snprintf(buffer, sizeof(buffer), "Effects volume: %d", (int)s.effectsVolume);
            break;
        case OptionFilter:
            snprintf(buffer, sizeof(buffer), "Music filter: %s", onOff(s.musicFilter));
            break;
        case OptionDifficulty:
            snprintf(buffer, sizeof(buffer), "Difficulty: %s",
                     kDifficultyNames[(uint32_t)s.difficulty < 3u ? (uint32_t)s.difficulty : 1u]);
            break;
        default:
            buffer[0] = '\0';
            break;
    }
    return buffer;
}

// C# AddOption's Step: the change, applied and saved, every option relabelled.
void MenuItems::stepOption(int32_t option, int32_t step) {
    if (!settings_) {
        return;
    }
    GameSettings& s = *settings_;
    switch (option) {
        case OptionGraphics:
            s.graphics = (GameSettings::Graphics)wrap((int32_t)s.graphics + step, 2);
            break;
        case OptionAntiAliasing:
            s.antiAliasing = (GameSettings::AntiAliasing)wrap((int32_t)s.antiAliasing + step, 3);
            break;
        case OptionGore:
            s.gore = !s.gore;
            break;
        case OptionMaster:
            s.masterVolume = volumeStep(s.masterVolume, step);
            break;
        case OptionMusic:
            s.musicVolume = volumeStep(s.musicVolume, step);
            break;
        case OptionEffects:
            s.effectsVolume = volumeStep(s.effectsVolume, step);
            break;
        case OptionFilter:
            s.musicFilter = !s.musicFilter;
            break;
        case OptionDifficulty:
            s.difficulty = (GameSettings::Difficulty)wrap((int32_t)s.difficulty + step, 3);
            break;
        default:
            return;
    }
    if (applied_) {
        applied_(appliedUser_);
    }
    for (size_t i = 0; i < entries_.size(); i++) {
        if (entries_[i].option >= 0) {
            entries_[i].text = optionLabel(entries_[i].option);
        }
    }
}

bool MenuItems::act(const Event& event, Event* forOwner) {
    *forOwner = Event();
    const int32_t action = event.action;
    if (action < kOptionsBase) {
        return false;
    }
    if (action >= kOptionAction) {
        stepOption(action - kOptionAction, event.kind == Event::Change ? event.step : 1);
        return true;
    }
    if (action >= kReturn) {
        // The page's Back: the options again, on the row it was opened from.
        showOptions(optionsBack_);
        select(action - kReturn);
        return true;
    }
    const int32_t local = action - kOpenPage;
    showPage(local / kPageStride, local % kPageStride);
    return true;
}

int32_t MenuItems::volumeStep(int32_t value, int32_t step) {
    // C# Wrap(value + step * 10, MaxVolume + 10): 0-100 by 10, wrapping.
    constexpr int32_t kCount = GameSettings::kMaxVolume + kVolumeIncrement;
    return wrap(value + step * kVolumeIncrement, kCount);
}

} // namespace menu
} // namespace mdk

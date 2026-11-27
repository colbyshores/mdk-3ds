// enhanced_menu.cpp -- see enhanced_menu.h. Cold path: the menu runs only
// while the game is paused, one frame's edges at a time.

#include "game/enhanced_menu.h"

#include <stdio.h>

#include "game/enhanced_config.h"
#include "game/font_view.h"
#include "game/game_settings.h"

namespace mdk {
namespace {

using Item = EnhancedMenu::Item;
using Kind = EnhancedMenu::Kind;

// THE CHOICES' VALUES (render_types.h): the sun shadow map's side (0 is
// automatic) and how often it is redrawn, in frames (0 is automatic). Exactly
// the values the config parser accepts, so a parsed file is always one of them.
const int32_t kMapSizes[] = {0, 128, 256, 512};
const char* const kMapSizeLabels[] = {"AUTO", "128", "256", "512"};
const int32_t kMapRefreshes[] = {0, 1, 2, 3, 4};
const char* const kMapRefreshLabels[] = {"AUTO", "1", "2", "3", "4"};
// The tiny-debris and tiny-enemy culls' sizes in screen pixels (render_types.h),
// 0 = off.
const int32_t kTinyPixels[] = {0, 1, 2, 3};
const char* const kTinyPixelLabels[] = {"OFF", "1 PX", "2 PX", "3 PX"};
// THE STEREO ROWS' VALUES, in HUNDREDTHS of a game unit (Kind::Setting). 3D
// DEPTH is the eyes' separation at full slider: 100% is the 3DS default
// (GameSettings::kConsoleStereoSeparation, 1 unit) and 200% the parser's
// ceiling (2 units). 3D SCREEN DISTANCE is where the eyes' pictures meet, the
// panel's surface: Kurt stands ~8 units from the follow camera, 10 is the
// default, and FAR (0) is infinity -- the parallel pair the 3DS drew before.
const int32_t kStereoDepths[] = {25, 50, 75, 100, 125, 150, 175, 200};
const char* const kStereoDepthLabels[] = {"25%",  "50%",  "75%",  "100%",
                                          "125%", "150%", "175%", "200%"};
const int32_t kStereoDistances[] = {400, 600, 800, 1000, 1200, 1500, 2000, 3000, 5000, 0};
const char* const kStereoDistanceLabels[] = {"4", "6", "8", "10", "12", "15", "20", "30", "50",
                                             "FAR"};

// The setting rows, in order: the ten Enhanced Mode toggles in the config
// file's order (SPEC.md 7.9's flag order), the frame cap, the look dials
// (kLookDials' order), the shadow map's two choices, then the movies' rate,
// the 3DS draw culls and scope rows, and the two stereo rows. Constant-initialised
// (addresses only), so no static-initialisation order is involved.
const Item kItems[EnhancedMenu::kItemCount] = {
    {"SKY SHADOWS", Kind::Toggle, &EnhancedToggles::enableSkyShadows, nullptr, nullptr, nullptr,
     nullptr, 0, nullptr},
    {"FRAGMENT LIGHTS", Kind::Toggle, &EnhancedToggles::enableFragmentLights, nullptr, nullptr,
     nullptr, nullptr, 0, nullptr},
    {"HAZE", Kind::Toggle, &EnhancedToggles::enableHaze, nullptr, nullptr, nullptr, nullptr, 0, nullptr},
    {"MIRRORS", Kind::Toggle, &EnhancedToggles::enableMirrors, nullptr, nullptr, nullptr, nullptr,
     0, nullptr},
    {"GLOW", Kind::Toggle, &EnhancedToggles::enableGlowBillboards, nullptr, nullptr, nullptr,
     nullptr, 0, nullptr},
    {"FILTERING", Kind::Toggle, &EnhancedToggles::enableFiltering, nullptr, nullptr, nullptr,
     nullptr, 0, nullptr},
    {"MIPMAPS", Kind::Toggle, &EnhancedToggles::enableMipmaps, nullptr, nullptr, nullptr, nullptr,
     0, nullptr},
    {"HD TEXTURES", Kind::Toggle, &EnhancedToggles::enableHdTextures, nullptr, nullptr, nullptr,
     nullptr, 0, nullptr},
    {"LIGHTING", Kind::Toggle, &EnhancedToggles::enableLighting, nullptr, nullptr, nullptr,
     nullptr, 0, nullptr},
    {"EDGE SMOOTHING", Kind::Toggle, &EnhancedToggles::enableEdgeSmoothing, nullptr, nullptr,
     nullptr, nullptr, 0, nullptr},
    {"30 FPS CAP", Kind::Toggle, &EnhancedToggles::cap30Fps, nullptr, nullptr, nullptr, nullptr,
     0, nullptr},
    {"HAZE DENSITY", Kind::Slider, nullptr, &kLookDials[0], nullptr, nullptr, nullptr, 0, nullptr},
    {"GLOW STRENGTH", Kind::Slider, nullptr, &kLookDials[1], nullptr, nullptr, nullptr, 0, nullptr},
    {"SHADOW DARKNESS", Kind::Slider, nullptr, &kLookDials[2], nullptr, nullptr, nullptr, 0, nullptr},
    {"EXPOSURE", Kind::Slider, nullptr, &kLookDials[3], nullptr, nullptr, nullptr, 0, nullptr},
    {"TINT SATURATION", Kind::Slider, nullptr, &kLookDials[4], nullptr, nullptr, nullptr, 0, nullptr},
    {"SHADOW MAP", Kind::Choice, nullptr, nullptr, &EnhancedToggles::shadowMapSize, kMapSizes,
     kMapSizeLabels, 4, nullptr},
    {"SHADOW REFRESH", Kind::Choice, nullptr, nullptr, &EnhancedToggles::shadowMapRefresh,
     kMapRefreshes, kMapRefreshLabels, 5, nullptr},
    // Package #20c, last so the rows above keep their numbers.
    {"MOVIES HALF RATE", Kind::Toggle, &EnhancedToggles::movieHalfRate, nullptr, nullptr, nullptr,
     nullptr, 0, nullptr},
    // The 3DS draw culls, after it so the rows above keep their numbers.
    {"TINY DEBRIS", Kind::Choice, nullptr, nullptr, &EnhancedToggles::tinyDebrisPixels,
     kTinyPixels, kTinyPixelLabels, 4, nullptr},
    {"TINY ENEMIES", Kind::Choice, nullptr, nullptr, &EnhancedToggles::tinyEnemyPixels,
     kTinyPixels, kTinyPixelLabels, 4, nullptr},
    {"SCOPE IN 2D", Kind::Toggle, &EnhancedToggles::scopeMono, nullptr, nullptr, nullptr, nullptr,
     0, nullptr},
    {"SCOPE CLIP", Kind::Toggle, &EnhancedToggles::scopeClip, nullptr, nullptr, nullptr, nullptr,
     0, nullptr},
    {"ROUND CAMERAS", Kind::Toggle, &EnhancedToggles::sniperRoundCams, nullptr, nullptr, nullptr,
     nullptr, 0, nullptr},
    // THE STEREO PAIR (owner 2026-11-27: the 3D "doesn't pop"), last so the rows
    // above keep their numbers. The game's settings, not toggles: config.ini's
    // stereo_separation / stereo_convergence (game_settings.h formatConsole).
    {"3D DEPTH", Kind::Setting, nullptr, nullptr, nullptr, kStereoDepths, kStereoDepthLabels, 8,
     &GameSettings::stereoSeparation},
    {"3D SCREEN DISTANCE", Kind::Setting, nullptr, nullptr, nullptr, kStereoDistances,
     kStereoDistanceLabels, 10, &GameSettings::stereoConvergence},
    // SWAP STICKS (owner 2026-11-27), last so the rows above keep their numbers:
    // a New 3DS's C-Stick looks and its circle pad walks. Inert without a C-Stick
    // (EnhancedMenu::setCStick).
    {"SWAP STICKS", Kind::Toggle, &EnhancedToggles::swapSticks, nullptr, nullptr, nullptr, nullptr,
     0, nullptr},
};
static_assert(EnhancedMenu::kSwapSticksRow == EnhancedMenu::kItemCount,
              "SWAP STICKS is the last setting row");

bool isItemRow(int32_t row) {
    return row > EnhancedMenu::kResumeRow && row <= EnhancedMenu::kItemCount;
}

// A Setting row's value in hundredths, rounded (the values are never negative:
// the parser clamps both stereo keys at 0).
int32_t settingHundredths(const Item& item, const GameSettings& settings) {
    const float value = settings.*(item.setting);
    return value > 0.0f ? (int32_t)(value * 100.0f + 0.5f) : 0;
}

// The index of a choice's current value in its list; -1 when it is none of
// them (a struct set by hand: the parser never leaves one; or a hand-edited
// config.ini stereo value off the rows' grid).
int32_t choiceIndex(const Item& item, const EnhancedToggles& toggles,
                    const GameSettings* settings) {
    int32_t value = 0;
    if (item.kind == Kind::Setting) {
        if (!settings) {
            return -1;
        }
        value = settingHundredths(item, *settings);
    } else {
        value = toggles.*(item.value);
    }
    for (int32_t i = 0; i < item.choiceCount; i++) {
        if (item.choices[i] == value) {
            return i;
        }
    }
    return -1;
}

// Step a slider or cycle a choice by `dir` (-1 left, +1 right). True when its
// value changed.
bool adjust(const Item& item, int32_t dir, EnhancedToggles* toggles, GameSettings* settings) {
    if (item.kind == Kind::Slider) {
        int32_t& value = toggles->*(item.dial->field);
        const int32_t next = clampLookDial(*item.dial, value + dir * item.dial->step);
        if (next == value) {
            return false;
        }
        value = next;
        return true;
    }
    if (item.kind == Kind::Setting) {
        // The next grid value in the direction pressed, and STOP at both ends (a
        // slider's behaviour: wrapping from 200% depth to 25%, or from FAR to 4,
        // would be a jolt on a parallax barrier). The grid is ascending; a
        // trailing 0 is FAR (infinity), so it orders last. A hand-edited value
        // off the grid steps to its nearest neighbour that way, never across.
        if (!settings) {
            return false;
        }
        const bool zeroIsFar = item.choiceCount > 0 && item.choices[item.choiceCount - 1] == 0;
        const int32_t current = settingHundredths(item, *settings);
        const int64_t currentKey = (current == 0 && zeroIsFar) ? INT64_MAX : current;
        int32_t next = -1;
        for (int32_t i = 0; i < item.choiceCount; i++) {
            const int32_t at = dir > 0 ? i : item.choiceCount - 1 - i;
            const int64_t key = (item.choices[at] == 0 && zeroIsFar) ? INT64_MAX : item.choices[at];
            if ((dir > 0 && key > currentKey) || (dir < 0 && key < currentKey)) {
                next = at;
                break;
            }
        }
        if (next < 0) {
            return false;
        }
        settings->*(item.setting) = (float)item.choices[next] / 100.0f;
        return true;
    }
    if (item.kind == Kind::Choice) {
        // Wraps both ways; a value off the list goes to the first or the last.
        int32_t index = choiceIndex(item, *toggles, nullptr);
        if (index < 0) {
            index = dir > 0 ? 0 : item.choiceCount - 1;
        } else {
            index += dir;
            if (index < 0) {
                index = item.choiceCount - 1;
            } else if (index >= item.choiceCount) {
                index = 0;
            }
        }
        const int32_t next = item.choices[index];
        if (next == toggles->*(item.value)) {
            return false;
        }
        toggles->*(item.value) = next;
        return true;
    }
    toggles->*(item.flag) = !(toggles->*(item.flag));
    return true;
}

// A flat canvas rectangle (the HudView::fillRect shape).
void fillRect(MdkRenderer renderer, float x, float y, float w, float h, const Colour& colour) {
    CanvasQuad q;
    q.x0 = x; q.y0 = y;
    q.x1 = x + w; q.y1 = y;
    q.x2 = x + w; q.y2 = y + h;
    q.x3 = x; q.y3 = y + h;
    q.u0 = 0.0f; q.v0 = 0.0f;
    q.u1 = 1.0f; q.v1 = 0.0f;
    q.u2 = 1.0f; q.v2 = 1.0f;
    q.u3 = 0.0f; q.v3 = 1.0f;
    q.colour = colour;
    q.texture = -1;
    q.palette = -1;
    mdk_render_canvas_quad(renderer, &q);
}

// A flat triangle `w` wide and `h` tall, its base's left corner at (x, y),
// pointing up (`up`) or down: the scroll markers. A quad whose last corner
// repeats its first.
void fillArrow(MdkRenderer renderer, float x, float y, float w, float h, bool up,
               const Colour& colour) {
    CanvasQuad q;
    q.x0 = x; q.y0 = y;
    q.x1 = x + w; q.y1 = y;
    q.x2 = x + w * 0.5f; q.y2 = up ? y - h : y + h;
    q.x3 = x; q.y3 = y;
    q.colour = colour;
    q.texture = -1;
    q.palette = -1;
    mdk_render_canvas_quad(renderer, &q);
}

} // namespace

const char* const EnhancedMenu::kHint = "A: SELECT   LEFT/RIGHT: ADJUST   B: RESUME";

const EnhancedMenu::Item* EnhancedMenu::rowItem(int32_t row) {
    return isItemRow(row) ? &kItems[row - 1] : nullptr;
}

const char* EnhancedMenu::rowLabel(int32_t row) {
    if (row == kResumeRow) {
        return "RESUME";
    }
    if (row == kQuitRow) {
        return "QUIT";
    }
    if (row == kOptionsRow) {
        return "OPTIONS";
    }
    if (row == kMainMenuRow) {
        return "MAIN MENU";
    }
    if (row == kCheatsRow) {
        return "CHEATS";
    }
    return isItemRow(row) ? kItems[row - 1].label : "";
}

void EnhancedMenu::rowValue(int32_t row, const EnhancedToggles& toggles, char* out,
                            int32_t size, const GameSettings* settings) {
    if (!out || size <= 0) {
        return;
    }
    out[0] = '\0';
    const Item* item = rowItem(row);
    if (!item) {
        return;
    }
    if (item->kind == Kind::Slider) {
        snprintf(out, (size_t)size, "%d%%", (int)(toggles.*(item->dial->field)));
    } else if (item->kind == Kind::Choice) {
        const int32_t index = choiceIndex(*item, toggles, nullptr);
        if (index >= 0) {
            snprintf(out, (size_t)size, "%s", item->choiceLabels[index]);
        } else {
            snprintf(out, (size_t)size, "%d", (int)(toggles.*(item->value)));
        }
    } else if (item->kind == Kind::Setting) {
        if (!settings) {
            return;
        }
        const int32_t index = choiceIndex(*item, toggles, settings);
        if (index >= 0) {
            snprintf(out, (size_t)size, "%s", item->choiceLabels[index]);
        } else {
            snprintf(out, (size_t)size, "%.2f", (double)(settings->*(item->setting)));
        }
    } else {
        snprintf(out, (size_t)size, "%s", toggles.*(item->flag) ? "ON" : "OFF");
    }
}

EnhancedMenu::Action EnhancedMenu::handle(const Input& input, EnhancedToggles* toggles,
                                          GameSettings* settings) {
    if (!open_ || !toggles) {
        return Action::None;
    }
    if (input.close) {
        open_ = false;
        return Action::Closed;
    }
    // Up and down wrap: up from RESUME is QUIT, so QUIT is two presses away
    // from where the menu opens (up, then A) and never one.
    if (input.up) {
        selected_ = selected_ > 0 ? selected_ - 1 : kRowCount - 1;
    }
    if (input.down) {
        selected_ = selected_ < kRowCount - 1 ? selected_ + 1 : 0;
    }
    // The window follows the selection.
    if (selected_ < top_) {
        top_ = selected_;
    } else if (selected_ >= top_ + kVisibleRows) {
        top_ = selected_ - kVisibleRows + 1;
    }
    if (input.confirm && selected_ == kResumeRow) {
        open_ = false;
        return Action::Closed;
    }
    if (input.confirm && selected_ == kQuitRow) {
        return Action::Quit;
    }
    if (input.confirm && selected_ == kOptionsRow) {
        return Action::Options;
    }
    if (input.confirm && selected_ == kMainMenuRow) {
        return Action::MainMenu;
    }
    if (input.confirm && selected_ == kCheatsRow) {
        return Action::Cheats;
    }
    const Item* item = rowItem(selected_);
    if (!item) {
        return Action::None;
    }
    // SWAP STICKS without a C-Stick: shown, never changed.
    if (selected_ == kSwapSticksRow && !cstick_) {
        return Action::None;
    }
    if (item->kind == Kind::Toggle) {
        // Left, right or A: one flip, whichever of them came (as before).
        if (!(input.confirm || input.left || input.right)) {
            return Action::None;
        }
        adjust(*item, 1, toggles, settings);
        return Action::Changed;
    }
    // Left and right together cancel; A is one step forward on a choice and
    // nothing on a slider (t2k's activate).
    int32_t dir = (input.right ? 1 : 0) - (input.left ? 1 : 0);
    if (dir == 0 && input.confirm && item->kind == Kind::Choice) {
        dir = 1;
    }
    if (dir == 0) {
        return Action::None;
    }
    return adjust(*item, dir, toggles, settings) ? Action::Changed : Action::None;
}

void EnhancedMenu::draw(MdkRenderer renderer, FontView& big, FontView& small,
                        float canvasWidth, const EnhancedToggles& toggles,
                        const GameSettings* settings) const {
    if (!open_ || !big.ready() || !small.ready()) {
        return;
    }
    // Sized from the fonts' own metrics: the FONTBIG title is about 341
    // units wide at most, the widest FONTSML row at 1.5x is 248, so a
    // 400-wide panel holds both with the value column clear of the labels.
    // Twelve rows at 22 + the title and the footer fit the canvas's 360
    // units of height (render_types.h kCanvasHeight), which the top screen
    // shows in its 240 lines; the rest scroll (kVisibleRows).
    const float kPanelW = 400.0f;
    const float kPanelH = 344.0f;
    const float px = (canvasWidth - kPanelW) * 0.5f;
    const float py = 8.0f;
    const Colour kPanel{0.0f, 0.0f, 0.0f, 0.72f};
    const Colour kTitle{1.0f, 0.85f, 0.4f, 1.0f};
    const Colour kRow{0.75f, 0.75f, 0.75f, 1.0f};
    const Colour kSelected{1.0f, 1.0f, 1.0f, 1.0f};
    const Colour kOn{0.45f, 1.0f, 0.45f, 1.0f};
    const Colour kOff{1.0f, 0.45f, 0.45f, 1.0f};
    const Colour kLevel{0.55f, 0.8f, 1.0f, 1.0f};
    const Colour kHintColour{0.55f, 0.55f, 0.55f, 1.0f};
    const Colour kMark{1.0f, 0.85f, 0.4f, 1.0f};
    const Colour kQuit{1.0f, 0.55f, 0.45f, 1.0f};

    fillRect(renderer, px, py, kPanelW, kPanelH, kPanel);

    const char* title = "PAUSED";
    const float titleX = px + (kPanelW - (float)big.width(title)) * 0.5f;
    big.draw(renderer, title, titleX, py + 34.0f, 1.0f, kTitle);

    const int32_t end = top_ + kVisibleRows < kRowCount ? top_ + kVisibleRows : kRowCount;
    for (int32_t row = top_; row < end; row++) {
        const float baseline = py + 62.0f + (float)(row - top_) * 22.0f;
        const bool isSelected = row == selected_;
        if (isSelected) {
            small.draw(renderer, ">", px + 14.0f, baseline, 1.5f, kMark);
        }
        const Colour& labelColour =
            isSelected ? kSelected
                       : ((row == kQuitRow || row == kMainMenuRow)
                              ? kQuit
                              : ((row == kSwapSticksRow && !cstick_) ? kHintColour : kRow));
        small.draw(renderer, rowLabel(row), px + 30.0f, baseline, 1.5f, labelColour);
        const Item* item = rowItem(row);
        if (!item) {
            continue;
        }
        char value[16];
        rowValue(row, toggles, value, (int32_t)sizeof(value), settings);
        const bool disabled = row == kSwapSticksRow && !cstick_;
        if (disabled) {
            snprintf(value, sizeof(value), "%s", "NO C-STICK");
        }
        const Colour& valueColour =
            disabled ? kHintColour
                     : (item->kind != Kind::Toggle ? kLevel : (toggles.*(item->flag) ? kOn : kOff));
        small.draw(renderer, value, px + kPanelW - 24.0f - (float)small.width(value) * 1.5f,
                   baseline, 1.5f, valueColour);
    }
    // The scroll markers: rows above the window, rows below it.
    if (top_ > 0) {
        fillArrow(renderer, px + kPanelW - 18.0f, py + 46.0f, 10.0f, 6.0f, true, kMark);
    }
    if (end < kRowCount) {
        fillArrow(renderer, px + kPanelW - 18.0f, py + kPanelH - 28.0f, 10.0f, 6.0f, false,
                  kMark);
    }

    small.draw(renderer, kHint, px + 30.0f, py + kPanelH - 10.0f, 1.0f, kHintColour);
}

} // namespace mdk

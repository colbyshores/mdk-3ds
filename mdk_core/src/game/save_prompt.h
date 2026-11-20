#pragma once

// ============================================================================
// save_prompt.h -- the save prompt's decisions (C# src/Mdk.Game/Menu/SavePrompt.cs,
// package #19), without its drawing.
//
// The original's prompt after a level (0x42b520(1), each frame 0x42b75c) and F2's:
//
//   Ask   SV_ASK "Save Game?"    y 139   up/down or the pointer choose, Enter, Space
//         ABORT2 "Yes"           y 175   or a click answers, Esc says no; the
//         ABORT3 "No"            y 211   selected line grows 65 % -> 100 % in 5 ticks
//   Name  SV_TITLE "Name for Saved Game" (y 31), the name below it one character
//         per 28 px from x 202 (baseline 200): up to 8 of A-Z 0-9 _ $, typed over
//         the character at the cursor; the cursor "_" blinks every 8 ticks. Enter
//         writes the save under the name, Esc gives up.
//
// A full save (F2) starts at Name; the light saves (after LEVEL8, the statistics'
// before-level save) start at Ask. The name offered is the next level's number in
// the order of play (LevelFlow.SaveName).
//
// THE DRAWING IS NOT HERE. The prompt is drawn on black in FONTBIG, which is the
// menus' business (package #20a ports the menu screens in parallel). layout()
// turns the state into the text runs the C# Draw makes -- text, position, scale --
// given the font's width, so a frontend (or #20a) draws them with its own font
// renderer and the positions stay the C#'s. The C#'s mouse rows are in update()'s
// input as the pointer's y on the 600 x 360 view.
//
// No virtuals, no std::function (AGENTS.md §10): the width is a function pointer.
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

#include "game/save_games.h"

namespace mdk {
namespace flow {

class SavePrompt {
public:
    enum class Stage { Ask, Name };

    // The C#'s constants (SavePrompt.cs:20-38).
    static constexpr float kTicksPerSecond = 30.0f;
    static constexpr float kRowY = 175.0f;
    static constexpr float kRowHeight = 36.0f;
    // The rows under the pointer start 26 above the first baseline.
    static constexpr float kRowTop = kRowY - kRowHeight + 10.0f;
    static const int32_t kAnswers = 2;  // ABORT2 "Yes", ABORT3 "No"
    static const int32_t kYes = 0;
    static constexpr float kSmall = 0.65f;
    static constexpr float kGrowTicks = 5.0f;
    static constexpr float kTitleY = 31.0f;
    static constexpr float kNameX = 202.0f;
    static constexpr float kNameY = 200.0f;
    static constexpr float kNameStep = 28.0f;
    static const int32_t kNameLength = 8;
    // The cursor blinks every 8 frames (0x5742e8 & 8).
    static const int32_t kBlinkTicks = 8;
    // ScreenView.Size.X: the view the prompt is laid out on.
    static constexpr float kViewWidth = 600.0f;

    // The frame's input, as the C#'s Input gives it to Update: the edges of the
    // menu keys, the pointer, and the characters typed this frame.
    struct Input {
        bool pointerMoved = false;
        bool clicked = false;     // the left button went down
        float pointerY = -1.0f;   // the pointer on the 600 x 360 view
        bool up = false;
        bool down = false;
        bool accept = false;      // Enter or Space (MenuKey.Accept)
        bool back = false;        // Esc
        bool left = false;
        bool right = false;
        bool home = false;
        bool end = false;
        bool backspace = false;
        bool del = false;
        std::string typed;        // the characters typed (Input.Typed)
    };

    // Asks whether to save `save` into `saves`, offering `name` (C# constructor).
    SavePrompt(const SaveGames& saves, const SaveGame& save, const std::string& name);

    // C# Update: the ticks, the growth, then the stage's keys.
    void update(const Input& input, float delta);

    bool closed() const { return closed_; }
    // Closed after writing the save, under name().
    bool saved() const { return saved_; }
    const std::string& name() const { return name_; }
    Stage stage() const { return stage_; }
    int32_t selected() const { return selected_; }
    int32_t cursor() const { return cursor_; }
    const SaveGame& save() const { return save_; }
    // Why the last Enter did not save (an empty name, or the write's reason).
    const std::string& error() const { return error_; }

    // One text run of the C#'s Draw: the text (an FTI key when `key`, else the
    // characters themselves), where its left edge and baseline go on the 600 x 360
    // view, and its scale.
    struct Run {
        std::string text;
        bool key = false;
        float x = 0.0f;
        float y = 0.0f;
        float scale = 1.0f;
    };
    // The font's width of a run's text at scale 1 (FontView.Width). For an FTI key
    // the frontend measures that key's text.
    typedef float (*WidthFn)(void* context, const std::string& text, bool key);
    // C# Draw, as runs on black (the canvas filled first).
    void layout(WidthFn width, void* context, std::vector<Run>* out) const;

private:
    void ask(const Input& input);
    void select(int32_t row);
    void answer();
    void typeName(const Input& input);
    void type(char c);
    void write();

    SaveGames saves_;
    SaveGame save_;
    Stage stage_ = Stage::Ask;
    int32_t selected_ = kYes;
    float grow_ = 0.0f;
    std::string name_;
    int32_t cursor_ = 0;
    float ticks_ = 0.0f;
    bool closed_ = false;
    bool saved_ = false;
    std::string error_;
};

} // namespace flow
} // namespace mdk

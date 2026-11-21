#pragma once

// ============================================================================
// save_prompt_screen.h -- the save prompt alone, after LEVEL8's Gunter stream
// (C# SavePromptScreen, src/Mdk.Game/Flow/Game.cs:670-694), and the prompt's
// drawing, which the statistics' prompt (game/stats_screen.h) shares.
//
// C# AfterStream, After.LastLevel (Game.cs:527-533): LEVEL5 follows the Gunter
// stream at once, after this prompt -- a light save (SaveKind.LevelStart) of the
// state EnterLastLevel made, offered under LevelFlow.SaveName(5), "6". Its Frame
// lays the view out (Fit.Inside), updates the prompt, draws it on black and
// presents; once the prompt is closed (Esc, "No", or a save written) it returns
// Event.Play, and the flow loads the level.
//
// The decisions are flow::SavePrompt's (game/save_prompt.h, package #19). The
// drawing is SavePrompt.Draw: drawSavePrompt() fills the canvas black and draws
// SavePrompt::layout()'s runs in FONTBIG, an FTI key's text through MDKFONT.FTI
// -- the same function StatsScreen draws its prompt with (package #20b).
//
// The frontend owns the canvas pass (mdk_render_canvas_begin/end) and the keys.
// ============================================================================

#include <memory>
#include <string>

#include "game/game_flow.h"
#include "game/save_prompt.h"
#include "render/render.h"

namespace mdk {

class FontView;
struct ScreenView;

namespace formats {
class Fti;
}

// C# SavePrompt.Draw (SavePrompt.cs:205-232): the canvas filled black, then the
// prompt's runs in `big`; an FTI key's text from `fti` (none when null). A null
// prompt draws the black alone.
void drawSavePrompt(MdkRenderer renderer, const ScreenView& view, const FontView& big,
                    const formats::Fti* fti, const flow::SavePrompt* prompt);

class SavePromptScreen {
public:
    // C# `new SavePromptScreen(_ui, _saves, _state.Save(SaveKind.LevelStart),
    // LevelFlow.SaveName(_state.Level))` (Game.cs:532): the prompt over the state
    // the flow's AfterStream left (EnterLastLevel: LEVEL5, the stream's health).
    void open(const flow::SaveGames& saves, const flow::GameState& state);
    // The prompt is gone (C# Dispose).
    void close();
    bool isOpen() const { return prompt_ != nullptr; }
    // C# Frame's update half: the prompt's Update, then Closed ? Event.Play :
    // Event.None. The prompt stays (closed) for this frame's draw().
    flow::Event frame(const flow::SavePrompt::Input& input, float elapsed);
    // C# Frame's draw half (SavePrompt.Draw), inside the frontend's canvas pass.
    void draw(MdkRenderer renderer, const ScreenView& view, const FontView& big,
              const formats::Fti* fti) const;
    const flow::SavePrompt* prompt() const { return prompt_.get(); }

private:
    std::unique_ptr<flow::SavePrompt> prompt_;
};

} // namespace mdk

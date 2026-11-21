// save_prompt_screen.cpp -- see save_prompt_screen.h. C# SavePromptScreen
// (Flow/Game.cs:670-694) and SavePrompt.Draw (Menu/SavePrompt.cs:205-240).

#include "game/save_prompt_screen.h"

#include <stdio.h>

#include <vector>

#include "formats/fti.h"
#include "game/font_view.h"
#include "game/game_state.h"
#include "game/level_flow.h"
#include "game/screen_view.h"
#include "render/render_types.h"

namespace mdk {

namespace {

// The font's width of a run (FontView.Width): an FTI key's text, or the
// characters themselves.
struct Measure {
    const FontView* font;
    const formats::Fti* fti;
    static float width(void* context, const std::string& text, bool key) {
        const Measure* m = static_cast<const Measure*>(context);
        if (key) {
            const std::vector<uint8_t> bytes =
                m->fti ? m->fti->getTextBytes(text.c_str()) : std::vector<uint8_t>();
            return (float)m->font->width(bytes.data(), (int32_t)bytes.size());
        }
        return (float)m->font->width(text.c_str());
    }
};

} // namespace

// C# SavePrompt.Draw: view.FillCanvas(Ui.Black), then the stage's lines.
void drawSavePrompt(MdkRenderer renderer, const ScreenView& view, const FontView& big,
                    const formats::Fti* fti, const flow::SavePrompt* prompt) {
    view.fillCanvas(renderer, Colour{0.0f, 0.0f, 0.0f, 1.0f});
    if (!prompt) {
        return;
    }
    Measure measure = {&big, fti};
    std::vector<flow::SavePrompt::Run> runs;
    prompt->layout(Measure::width, &measure, &runs);
    for (size_t r = 0; r < runs.size(); r++) {
        const std::vector<uint8_t> bytes =
            runs[r].key ? (fti ? fti->getTextBytes(runs[r].text.c_str()) : std::vector<uint8_t>())
                        : std::vector<uint8_t>(runs[r].text.begin(), runs[r].text.end());
        view.text(renderer, big, bytes.data(), (int32_t)bytes.size(), runs[r].x, runs[r].y,
                  runs[r].scale, 1.0f);
    }
}

// C# AfterStream's `new SavePromptScreen(_ui, _saves, _state.Save(SaveKind.LevelStart),
// LevelFlow.SaveName(_state.Level))` (Game.cs:532).
void SavePromptScreen::open(const flow::SaveGames& saves, const flow::GameState& state) {
    char name[16];
    snprintf(name, sizeof(name), "%d", (int)flow::saveNameNumber(state.level));
    prompt_.reset(new flow::SavePrompt(saves, state.save(flow::SaveKind::LevelStart), name));
}

void SavePromptScreen::close() {
    prompt_.reset();
}

// C# Frame: _prompt.Update(ui.Input, elapsed); ...; _prompt.Closed ? Event.Play :
// Event.None (Game.cs:675-682).
flow::Event SavePromptScreen::frame(const flow::SavePrompt::Input& input, float elapsed) {
    if (!prompt_) {
        return flow::Event::None;
    }
    prompt_->update(input, elapsed);
    return prompt_->closed() ? flow::Event::Play : flow::Event::None;
}

void SavePromptScreen::draw(MdkRenderer renderer, const ScreenView& view, const FontView& big,
                            const formats::Fti* fti) const {
    drawSavePrompt(renderer, view, big, fti, prompt_.get());
}

} // namespace mdk

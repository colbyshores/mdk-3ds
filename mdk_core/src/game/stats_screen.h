#pragma once

// ============================================================================
// stats_screen.h -- the screens between levels (C# src/Mdk.Game/Menu/
// StatsScreen.cs, game state 6: 0x431b00, each frame 0x43200c; stats_screen.gd).
//
// After a level: the intermission (L1_INTRM), the debriefing on the level's map,
// the Score-O-matic, the save prompt, then the briefing of the next level on its
// map. Every page fades in (from white for the intermission, else from black)
// over 0.5 s, waits for a key once it is fully shown and fades out to black. Esc
// skips; holding Fire or Jump runs everything twice as fast (typing 4x). All on
// the 600 x 360 screen.
//
//   intermission -> debriefing -> Score-O-matic -> save prompt -> briefing -> fall
//
// A new game or a save made before a level shows the briefing alone
// (StatsPages::Briefing; package #20a's page, which this class now is).
//
// THE SPLIT WITH THE FRONTENDS. This is all of StatsScreen that is not a
// platform's: the pages, their timing, the typing, the Score-O-matic's rows and
// heads (game/heads_view.h), the save prompt (game/save_prompt.h, package #19)
// and the sounds of STATS.BNI on the screen's own sound device (the C#'s
// _ui.Audio, as the fall's FallScreen does). A frontend owns the frame loop:
//
//   stats.frame(keys, elapsed);                 // C# Frame's input and Update
//   begin_frame; stats.draw(r, view, big, small); end_frame;   // C# Draw
//   stats.device().update(elapsed);              // Game.Run's audio update
//
// and the device's output (its sink); the screen's renderer objects (the page
// images, the heads) live in the frontend's mdk_render_mark scope, and close()
// stops the voices (C# Dispose) when the flow leaves the screen.
//
// THE RANDOM. The C#'s `new Random()` picks only sounds (a ricochet's chance and
// which one, which of the two head sounds); the frontends seed it from the clock,
// the goldens script it (Random hook).
//
// Cold code (between levels). No virtuals, no templates, no exceptions; the
// hooks are function pointers (AGENTS.md §10).
// ============================================================================

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "audio/sound.h"
#include "audio/sound_device.h"
#include "game/game_flow.h"
#include "game/heads_view.h"
#include "game/save_prompt.h"
#include "game/screen_view.h"
#include "render/render.h"

namespace mdk {

class FontView;

namespace formats {
class Bni;
class Fti;
class MdkData;
}

// C# StatsPages: all of them after a level, or the briefing alone.
enum class StatsPages { All, Briefing };

class StatsScreen {
public:
    // C# StatsScreen.Phase (its values are the C#'s, --phase's numbers).
    enum class Phase { None = 0, Score = 1, Intermission = 2, Briefing = 3, Debriefing = 4 };
    static const char* phaseName(Phase phase);

    // The sounds of STATS.BNI the screen plays (C# constructor's list).
    enum Sound { Cgun, Sniper, Rico1, Rico2, Rico3, Aldie, Xghead1, Xghead2, Teletype, kSounds };
    static const char* soundName(int32_t sound);

    // C# _random.Next(max): 0 <= value < max. Null: the screen's own generator.
    struct Random {
        void* context = nullptr;
        int32_t (*next)(void* context, int32_t max) = nullptr;
    };
    // A line of the C#'s Console ("Statistics: Score", "Score-O-matic: ...",
    // "Saved game 4: level 4").
    struct Print {
        void* context = nullptr;
        void (*line)(void* context, const char* text) = nullptr;
    };

    struct Setup {
        // MDKFONT.FTI (the texts); must outlive the screen.
        const formats::Fti* fti = nullptr;
        // MISC/STATS.BNI (the images, the sounds, the heads' model); may be null
        // (nothing drawn but text, no sound). Must outlive the screen.
        const formats::Bni* stats = nullptr;
        // SYS_PAL's 768 bytes (the pages' colours 0-63; the bar's colour).
        const uint8_t* systemRgb = nullptr;
        // The game's state: the level, the counts; the Score-O-matic moves the
        // level on and the save prompt saves it.
        flow::GameState* state = nullptr;
        flow::SaveGames saves;
        StatsPages pages = StatsPages::All;
        // The page to start at (C#'s `first`, --phase); None: the first.
        Phase first = Phase::None;
        // C# _ui.Settings.Gore: without gore the heads' row is left out.
        bool gore = true;
        Random random;
        // The seed of the screen's own generator (when `random` is null).
        uint32_t seed = 1;
        Print print;
    };

    StatsScreen() = default;
    StatsScreen(const StatsScreen&) = delete;
    StatsScreen& operator=(const StatsScreen&) = delete;

    // C# StatsScreen(ui, state, saves, pages, first): the sounds decoded, the
    // first page started. The heads' GPU half is attach().
    void open(const Setup& setup);

    // The renderer's half of the constructor: the heads' model (HeadsView). The
    // page images load as the pages start (in draw()).
    void attach(MdkRenderer renderer, const formats::MdkData& data);

    // The frame's input (C# Frame reads it off Input): `any` is AnyPressed,
    // `back` Esc (MenuKey.Back), `fastHeld` Fire or Jump held; `prompt` is the
    // save prompt's keys when it is open.
    struct Keys {
        bool any = false;
        bool back = false;
        bool fastHeld = false;
        flow::SavePrompt::Input prompt;
    };
    // C# Frame without its drawing: the prompt's Update, or the keys and Update.
    // Returns Event::Fall after the last page, None before.
    flow::Event frame(const Keys& keys, float elapsed);

    // C# Draw (or the prompt's): the heads under the canvas, then the canvas
    // (this opens and closes it: call it between begin_frame and end_frame). The
    // page's image is loaded here when the page changed (the last one released).
    void draw(MdkRenderer renderer, const ScreenView& view, const FontView& big,
              const FontView& small);

    // The screen's sound device: the frontend updates it once a frame after the
    // screen (Game.Run), sets its sink, applies the volumes.
    audio::SoundDevice& device() { return device_; }

    // C# Dispose: every voice stopped; the renderer objects are forgotten (the
    // frontend's scope release frees them).
    void close();

    // C# Status: "statistics: Score", or "save prompt".
    std::string status() const;

    // ---- For the goldens and the frontends' logs ----
    Phase phase() const { return phase_; }
    int32_t index() const { return index_; }
    const flow::SavePrompt* prompt() const { return prompt_.get(); }
    float fadeIn() const { return fadeIn_; }
    float fadeOut() const { return fadeOut_; }
    int32_t current() const { return current_; }
    float count() const { return count_; }
    int32_t shown() const { return shown_; }
    float ticks() const { return ticks_; }
    flow::Event next() const { return next_; }
    // The Score-O-matic.
    static const int32_t kRows = 5;
    int32_t row() const { return row_; }
    int32_t rowStage() const { return rowStage_; }
    float rowTime() const { return rowTime_; }
    float value(int32_t i) const { return values_[i]; }
    int32_t target(int32_t i) const { return targets_[i]; }
    int32_t full(int32_t i) const { return fulls_[i]; }
    float slide(int32_t i) const { return slides_[i]; }
    int32_t heads() const { return heads_; }
    float headTime() const { return headTime_; }
    const HeadsView& headsView() const { return headsView_; }
    // The page's image (an entry of STATS.BNI), empty for none.
    const std::string& imageName() const { return imageName_; }
    // The voice of a sound slot (C# _voices), the loop's and the teletype's.
    int32_t voice(int32_t slot) const { return voices_[slot]; }
    int32_t loopVoice() const { return loopVoice_; }
    // The name of the sound a live voice of the device plays ("?" for none).
    const char* soundOf(int32_t voice) const;

    // C# ValueText: a row's value, "%d/%d" for kills, "%d%%" for accuracies.
    static std::string valueText(int32_t row, int32_t value, int32_t full);

private:
    void update(float delta, bool fastHeld);
    void start(Phase phase);
    void endPhase();
    void nextPhase();
    bool updatePage(float delta, float speed);
    void play(int32_t sound);
    int32_t playVoice(int32_t sound);
    int32_t random(int32_t max);
    void say(const char* text);
    bool updateTyping(float delta);
    void startScore();
    bool updateScore(float delta, float speed);
    void updateRow(float delta, float speed);
    void countRow(float delta, float speed);
    bool updateHeads(float delta, float speed);
    void drawPage(MdkRenderer renderer, const ScreenView& view, const FontView& big,
                  const FontView& small) const;
    void drawTyped(MdkRenderer renderer, const ScreenView& view, const FontView& big,
                   const std::vector<uint8_t>& text, float y, int32_t budget) const;
    void drawScore(MdkRenderer renderer, const ScreenView& view, const FontView& big,
                   const FontView& small, bool shown) const;
    void drawPrompt(MdkRenderer renderer, const ScreenView& view, const FontView& big) const;
    std::vector<uint8_t> text(const char* name) const;

    // The setup's.
    const formats::Fti* fti_ = nullptr;
    const formats::Bni* stats_ = nullptr;
    uint8_t systemRgb_[768] = {};
    flow::GameState* state_ = nullptr;
    flow::SaveGames saves_;
    bool gore_ = true;
    Random random_;
    uint32_t seed_ = 1;
    Print print_;

    // The sounds (C# _sounds) and the device they play on (C# _ui.Audio).
    audio::Sound sounds_[kSounds];
    audio::SoundBank::Entry entries_[kSounds];
    bool hasSound_[kSounds] = {};
    audio::SoundDevice device_;
    // C# Players: the voices a sound may take.
    static const int32_t kPlayers = 4;
    int32_t voices_[kPlayers] = {};
    int32_t loopVoice_ = 0;
    int32_t teletypeVoice_ = 0;

    // The pages still to show (C# _phases, a queue).
    std::vector<Phase> phases_;
    Phase phase_ = Phase::None;
    int32_t index_ = 0;
    std::string imageName_;
    float fadeIn_ = 0.0f;
    float fadeOut_ = -1.0f;
    bool fadeWhite_ = false;
    bool fast_ = false;
    bool skip_ = false;
    bool pressed_ = false;
    float ticks_ = 0.0f;
    std::unique_ptr<flow::SavePrompt> prompt_;
    // The C# Frame draws the prompt when it was open as the frame began, even
    // the frame it closes on (then NextPhase); and the page on the frame EndPhase
    // opens it. drawPrompt_ is that choice, closedPrompt_ the one just closed.
    bool drawPrompt_ = false;
    std::unique_ptr<flow::SavePrompt> closedPrompt_;
    flow::Event next_ = flow::Event::None;

    // Typing: the texts of the page, their baselines, and the characters shown
    // of the current one.
    std::vector<std::vector<uint8_t>> texts_;
    std::vector<float> textY_;
    int32_t current_ = 0;
    float count_ = 1.0f;
    int32_t shown_ = 0;

    // The Score-O-matic.
    int32_t row_ = 0;
    float rowTime_ = 0.0f;
    int32_t rowStage_ = 0;
    float values_[kRows] = {};
    int32_t targets_[kRows] = {};
    int32_t fulls_[kRows] = {};
    float slides_[kRows] = {};
    int32_t heads_ = 0;
    float headTime_ = 0.0f;
    HeadsView headsView_;

    // The page's image on the GPU, and which entry it is.
    PalettedImage image_;
    std::string imageLoaded_;
};

} // namespace mdk

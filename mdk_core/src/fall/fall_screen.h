#pragma once

// ============================================================================
// fall_screen.h — the fall before a level (fall.gd) as a screen of the game.
//
// THE C# ORIGINAL is src/Mdk.Game/Fall/FallScreen.cs: the keys steer Kurt, Esc
// skips it (the port's addition); it ends with the level, which keeps Kurt's
// health and pickups (GameState.Carry), or, when he dies, with the main menu.
//
//   input ──► FallSim ──► FallView (3D), FallHud, palette effects ──► frame
//                └── sounds (FALL3D.SNI): 8 voices, the wind and grind loops
//
// THE SPLIT WITH THE FRONTENDS. This is everything of FallScreen that is not a
// platform's: the assets, the sim, its events (the sounds on at most 8 voices,
// the messages), the wind and grind loops, the HUD's clock, the drawing, and
// Next's carry. A frontend owns the frame loop around it -- the clock, the
// input, mdk_render_begin_frame/end_frame with backdrop() as the clear colour,
// the sound device's update (and the 3DS's DSP frame, audio_dsp_3ds.h) -- and
// calls, per frame:
//
//   fall.frame(FallScreen::steering(input, back), elapsed);    // C# Frame's first half
//   begin_frame(clear = fall.backdrop()); fall.draw(r); end_frame;
//   Event next = fall.next(&flow.state());                     // C# Next
//
// and close() when the flow leaves the screen: the voices stop and everything
// the screen made through the renderer is released (its own mdk_render_mark
// scope), as the C#'s Game.Show does between screens.
//
// THE SEED. The C# builds its sim with an unseeded `new Random()`; the frontend
// passes the seed (the clock; a fixed one under a test's fixed step).
//
// Cold (one screen): AGENTS.md §2. The sounds' voice list is reserved for its 8.
// No virtuals, no templates, no exceptions, no std::function (AGENTS.md §10):
// the print hook is a function pointer.
// ============================================================================

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "audio/sound_bank.h"
#include "audio/sound_mixer.h"
#include "fall/fall_sim.h"
#include "fall/fall_view.h"
#include "formats/bni.h"
#include "formats/texture_archive.h"
#include "game/game_flow.h"
#include "game/input.h"
#include "render/render.h"

namespace mdk {

namespace formats {
class Font;
class Fti;
class MdkData;
}

namespace fall {

class FallScreen {
public:
    // A line of the C#'s Console output ("Fall: index ...", "Fall ended ...").
    struct Print {
        void* context = nullptr;
        void (*line)(void* context, const char* text) = nullptr;
    };

    FallScreen() = default;
    FallScreen(const FallScreen&) = delete;
    FallScreen& operator=(const FallScreen&) = delete;

    // C# FallScreen(ui, state, test): the fall of `level` (its index clamped to
    // 0-4), FALL3D.BNI, FALL3D_n.MTI, the view, FALL3D.SNI, the sim with `seed`,
    // and the wind's loop (silent until the intro's end). `fti` and the fonts are
    // MDKFONT.FTI's (the messages' texts and FONTBIG/FONTSML); they must outlive
    // the screen. False (with `why`, everything made released) when the fall's
    // data does not load.
    bool open(MdkRenderer renderer, const formats::MdkData& data, const formats::Fti& fti,
              const formats::Font& big, const formats::Font& small, int32_t level, Kurt::Difficulty difficulty,
              int32_t seed, const FallViewOptions& view, const Print& print, std::string* why);

    // FallScreen.Steering: x right, y up the screen (the turn or strafe keys,
    // forward and back), each -1, 0 or 1; `back` is Esc (MenuKey.Back): skip.
    static FallSim::Input steering(const Input& input, bool back);
    // A test aid (the fall probe's `chase`, tests/scripts/fall_probe): steer
    // towards the first pickup under its chute, one axis at a time by the sign
    // of the gap when it is a unit or more. Leaves `input` alone otherwise.
    static void chase(const FallSim& sim, FallSim::Input* input);

    // FallScreen.Frame's first half: Esc (skip), the update, the sounds, and out
    // of the intro the messages' and the HUD's clocks.
    void frame(const FallSim::Input& input, float elapsed);

    // The colour behind the frame (FallView.Backdrop), 0xAARRGGBB.
    uint32_t backdrop() const { return view_->backdrop(); }

    // FallScreen.Frame's drawing half, between begin_frame and end_frame.
    void draw(MdkRenderer renderer);

    // FallScreen.Next: Landed carries Kurt's health and pickups into `state` and
    // is Event::Play; GameOver is Event::Menu; otherwise Event::None.
    flow::Event next(flow::GameState* state);

    // FallScreen.Status: the fall's time, Kurt's height above the ground, his
    // health and pickups.
    std::string status() const;

    // The mixer whose device the frontend updates once a frame (and hands to the
    // 3DS's DSP frame). The fall plays on the device itself, as the C# does.
    audio::SoundMixer& mixer() { return *mixer_; }
    // The bank the mixer plays from (the 3DS's DSP path binds its sounds).
    audio::SoundBank* bank() { return bank_.get(); }
    const FallSim& sim() const { return *sim_; }
    bool ready() const { return sim_ != nullptr; }

    // FallScreen.Dispose and Game.Show's half: every voice stopped, the screen's
    // renderer scope released. Safe to call twice.
    void close(MdkRenderer renderer);

private:
    static void soundHook(void* context, const char* name);
    static void messageHook(void* context, const char* name);
    // FallScreen.Play: a sound on one of 8 voices at most; more are dropped.
    void play(const char* name);
    // FallScreen.Loop: a looping sound at gain 0 (0 when the bank has none).
    int32_t loop(const char* name);
    void updateSounds();
    void report(const char* what);
    void say(const char* format, ...) __attribute__((format(printf, 2, 3)));

    MdkScope scope_;
    bool open_ = false;
    Print print_;
    formats::Bni bni_;
    formats::TextureArchive mti_;
    std::unique_ptr<FallView> view_;
    std::unique_ptr<audio::SoundBank> bank_;
    std::unique_ptr<audio::SoundMixer> mixer_;
    std::unique_ptr<FallSim> sim_;
    std::vector<int32_t> voices_;
    int32_t windVoice_ = 0;
    int32_t grindVoice_ = 0;
};

} // namespace fall
} // namespace mdk

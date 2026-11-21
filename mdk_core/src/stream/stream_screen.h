#pragma once

// ============================================================================
// stream_screen.h — the stream after a level (C# src/Mdk.Game/Stream/
// StreamScreen.cs; original game state 5: init 0x433b50, each frame 0x4352ac,
// cleanup 0x435210). Package #18b.
//
// Kurt flies down the generated tube (stream_flight.h's Flight), drawn by
// stream_view.h; the walls hurt. After about 30 seconds (or at 1 health) Bones
// picks him up; after LEVEL8 (the Gunter variant) Kurt follows Gunter carrying
// Bones to a planet and can die.
//
//   level -> stream -+- normal: StreamEnded -> statistics
//                    +- Gunter: StreamEnded -> save prompt -> LEVEL5
//                    +- died (Gunter tube): main menu
//
// WHAT THE FRONTEND DOES, AND WHAT THIS DOES. The screen is shared by both
// frontends; what it cannot own is the platform: the frame (begin/end), the
// input device and the audio output. So, once a frame:
//
//   frontend: poll input
//   screen:   update(elapsed, steeringOf(input) [+ the pad], back)  <- C# Frame:
//             Stop on Back, Flight.Update, PlaySounds, Report, the camera
//   frontend: begin_frame
//   screen:   draw()                                                <- StreamView.Draw
//   frontend: mixer().update(elapsed) [, the 3DS's DSP frame], mixer().device()
//             .update(elapsed) (C# Game.Run: the device after the screen);
//             end_frame; act on the Event update() returned
//
// THE SOUNDS are STREAM.BNI's WAVs (WIND, HITSIDE, HURT1-7, APPLE, RESCUE),
// played through the shared SoundMixer exactly as the level's are, so both
// targets' outputs (the SDL sink, the 3DS's DSP channels) need nothing new: the
// WAVs are laid out as a one-archive SNI in memory (every entry at full volume,
// 0x7FFF, which is SoundMixer::gain 1 -- the C#'s Ui.Play gain) and handed to a
// SoundBank. Flat voices, no position: the C#'s AudioDevice.Play(sound, 1, 1, 0).
//
// MEMORY. The flight's StreamRun is ~25 KB of fixed arrays and the screen's
// assets (STREAM.BNI 1.4 MB, the baked poses) are heap; the frontend holds the
// screen by pointer (the 3DS's main stack is small). Everything made through the
// renderer is the frontend's scope to release (mdk_render_mark before open,
// mdk_render_release after the screen is destroyed), as the C#'s Game.Show
// releases a screen's scope.
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

#include "audio/sound_bank.h"
#include "audio/sound_mixer.h"
#include "formats/bni.h"
#include "formats/mdk_data.h"
#include "formats/palette.h"
#include "formats/texture_archive.h"
#include "game/game_flow.h"
#include "game/input.h"
#include "render/render.h"
#include "stream/stream_flight.h"
#include "stream/stream_view.h"

namespace mdk {
namespace stream {

// C# StreamScreen.SteeringOf: the turn keys or strafe keys steer left and right,
// forward and back up and down, each signed.
Steering steeringOf(const Input& input);

// The 3DS's circle pad added to the keys' steering: a pad pushed past `dead`
// (of its +-156 range) counts as the key of its direction, the sum signed again.
// (Not in the C#: the PC's stick is a key binding there.)
Steering steeringWithPad(Steering keys, int32_t padX, int32_t padY, int32_t dead);

class StreamScreen {
public:
    // Where a line of the screen's report goes (C# Console.WriteLine): the
    // frontend's log. A plain function pointer (AGENTS.md §10).
    typedef void (*LogLine)(void* user, const char* line);

    struct Setup {
        MdkRenderer renderer = nullptr;
        // The data folder (STREAM/STREAM.BNI, STREAM/STREAM.MTI, MISC/MDKFONT.FTI).
        const formats::MdkData* data = nullptr;
        // The level just played (C# _level), Kurt's health, the difficulty and
        // the random's seed (GameFlow::streamSeed).
        int32_t level = 7;
        int32_t health = flow::kFullHealth;
        Kurt::Difficulty difficulty = Kurt::Normal;
        uint32_t seed = flow::kStreamDefaultSeed;
        LogLine log = nullptr;
        void* logUser = nullptr;
    };

    StreamScreen();
    ~StreamScreen();
    StreamScreen(const StreamScreen&) = delete;
    StreamScreen& operator=(const StreamScreen&) = delete;

    // C# StreamScreen's constructor: the assets, the tube, the view, the flight,
    // the WIND loop and the "Stream after LEVELn" line. False (and the reason in
    // the log) when STREAM's data cannot be read; the frontend then goes on as if
    // the stream had ended with the health it was given.
    bool open(const Setup& setup);

    // C# Frame, split where the renderer seam needs it: update() is everything
    // but the drawing -- Stop on `back` (the menu's Back pressed this frame: Esc,
    // the 3DS's START), Flight.Update, the sounds, the report, the camera and the
    // frame's renderer state -- and is called BEFORE begin_frame (the camera must
    // be set before the frame begins: stream_screen.cpp says why). Event::None
    // while the stream goes on, then Event::StreamEnded, or Event::Menu when Kurt
    // died. draw() queues StreamView's frame, between begin_frame and end_frame.
    flow::Event update(float elapsed, Steering steering, bool back);
    void draw();

    // C# Health: Kurt's health in the stream (LEVEL5 starts with it).
    int32_t health() const;
    bool opened() const { return run_ != nullptr; }

    // The mixer the screen plays into; the frontend updates it and its device
    // once a frame and wires the device to its output.
    audio::SoundMixer& mixer() { return mixer_; }
    audio::SoundBank& bank() { return bank_; }

    // C# Status: the flight's segment, time, Kurt's speed and health.
    void status(char* out, int32_t size) const;

    const Flight* flight() const;

private:
    void playSounds();
    void report();
    void say(const char* format, ...);

    Setup setup_;
    formats::Bni bni_;
    formats::Palette palette_;
    formats::TextureArchive archive_;
    audio::SoundBank bank_;
    audio::SoundMixer mixer_;
    StreamView view_;
    StreamRun* run_ = nullptr;
    // C# _voices (at most kPlayers) and _hitVoice, _wallHits.
    static constexpr int32_t kPlayers = 8;
    int32_t voices_[kPlayers];
    int32_t voiceCount_ = 0;
    int32_t hitVoice_ = 0;
    int32_t wallHits_ = 0;
};

} // namespace stream
} // namespace mdk

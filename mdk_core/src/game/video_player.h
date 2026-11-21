#pragma once

// ============================================================================
// video_player.h -- plays an FLC or an MVE (C# src/Mdk.Game/Menu/VideoPlayer.cs;
// video.gd). An FLC plays on the 600 x 360 view: one frame per game frame of
// the original (34 ms; the file's own speed is ignored), the palette per
// frame; the last frame stays. Still images use the same view. An MVE plays at
// its own rate with its sound, centred on the 640 x 480 screen.
//
//   FLC --34 ms--> frame --> image (indices + palette, brightened) --> view
//   MVE --frame time, or sooner when the sound runs low--> frame + sound --> stream voice
//
// THE PLAYER IS THE LOGIC; THE FRONTEND DRAWS. The C#'s PalettedImage (a GPU
// texture and palette) becomes the image's indices and its shown palette
// (brightened), with a version number each that changes when they do, so a
// frontend re-uploads only what changed -- and, on the 3DS, uploads at most
// once a frame whatever the player decoded in between (drop a frame rather
// than stall: the owner's rule). draw() is the C#'s placement: the view the
// image is fitted in and where it stands in it.
//
// THE C#'s EVENTS (FrameShown, Finished) are one listener each, a function and
// its user pointer (AGENTS.md §10: no std::function). The C#'s `+=`/`-=` swaps
// are the owner's state (end_movie.h).
//
// THE MOVIE'S SOUND goes to a stream voice through VideoAudio's hooks (the C#'s
// AudioDevice.PlayStream/Push/Queued/Stop). Streamed voices are not ported on
// either frontend (audio/sound_device.h), so the hooks are null there: the movie
// is then paced by its frame time alone (Queued reads as "enough"), silent. The
// one MVE the game plays (MDKBZK.MVE) is not in the GOG data this port is
// tested on.
//
// MEMORY: the file's bytes are read whole (632 KB for MDK12.FLC) and its frames
// decoded one at a time into one buffer -- an FLC frame is a delta of the one
// before, so they cannot be skipped -- never preloaded.
//
// Cold code (menus). No virtuals, no templates, no exceptions (AGENTS.md §10).
// ============================================================================

#include <stdint.h>

#include <memory>
#include <vector>

#include "formats/mve.h"

namespace mdk {

namespace formats {
class Flc;
class MdkData;
}

namespace menu {

// The C#'s AudioDevice stream voice, for an MVE's sound. Each may be null.
struct VideoAudio {
    // C# PlayStream(rate, 1f): a voice id, 0 for none.
    int32_t (*playStream)(void* user, int32_t sampleRate) = nullptr;
    // C# Push: the samples decoded since the last frame.
    void (*push)(void* user, int32_t voice, const formats::Mve::AudioSample* samples,
                 int32_t count) = nullptr;
    // C# Queued: the seconds still queued on the voice.
    float (*queued)(void* user, int32_t voice) = nullptr;
    void (*stop)(void* user, int32_t voice) = nullptr;
    void* user = nullptr;
};

class VideoPlayer {
public:
    // C# FrameSeconds: the original's game frame is at least 34 ms.
    static constexpr float kFrameSeconds = 0.034f;
    // C# SoundLow: with less queued, the next frame comes at once.
    static constexpr float kSoundLow = 0.25f;
    static constexpr float kMicrosecondsPerSecond = 1e6f;
    static constexpr int32_t kPaletteBytes = 768;

    VideoPlayer();
    ~VideoPlayer();
    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;

    // The game's data (the C#'s ui.Data), where play() reads its files.
    void setData(const formats::MdkData* data) { data_ = data; }
    void setAudio(const VideoAudio& audio) { audio_ = audio; }
    // C# FrameShown: a frame was shown, its number (from 1).
    void setFrameShown(void (*fn)(void* user, int32_t frame), void* user) {
        frameShown_ = fn;
        frameShownUser_ = user;
    }
    // C# Finished.
    void setFinished(void (*fn)(void* user), void* user) {
        finished_ = fn;
        finishedUser_ = user;
    }

    // C# Play: starts an FLC (relative to the game's data), its first frame
    // shown at once. False when it can't be read.
    bool play(const char* path);
    // C# PlayMovie: starts an MVE with its sound. False when it can't be read.
    bool playMovie(const char* path);
    // C# Hold: stays on the current frame for `seconds`.
    void hold(float seconds) { hold_ = seconds; }
    // C# Brighten: raises every colour by `amount` (0-1, the end movie's flash).
    void brighten(float amount);
    // C# ShowStill: an image of palette indices with its palette (768 RGB bytes).
    void showStill(int32_t width, int32_t height, const uint8_t* indices,
                   const uint8_t* paletteRgb);
    // C# Stop (and Dispose): no Finished.
    void stop();
    // C# Update.
    void update(float delta);

    // C# Playing.
    bool playing() const { return flc_ != nullptr || mve_ != nullptr; }
    // C# Still != null: an image is on the view.
    bool hasStill() const { return hasStill_; }
    int32_t width() const { return width_; }
    int32_t height() const { return height_; }
    const std::vector<uint8_t>& indices() const { return indices_; }
    // The palette as given (C# _palette), and as shown (Brightened(_palette)).
    const uint8_t* paletteRgb() const { return palette_; }
    const uint8_t* shownPaletteRgb() const { return shown_; }
    // Change counters for the frontend's uploads: the indices (each frame), the
    // shown palette (a frame, a Brighten).
    uint32_t imageVersion() const { return imageVersion_; }
    uint32_t paletteVersion() const { return paletteVersion_; }

    // C# Draw's placement: the view the image is fitted in (600 x 360, or the
    // movie's 640 x 480; ScreenView.Fit.Inside) and the image's corner in it,
    // (view - size) / 2.
    float viewWidth() const { return viewWidth_; }
    float viewHeight() const { return viewHeight_; }
    float imageX() const { return (viewWidth_ - (float)width_) / 2.0f; }
    float imageY() const { return (viewHeight_ - (float)height_) / 2.0f; }

    // For the goldens and the log: the clock, the hold, the brightening, and the
    // FLC's frame (-1 without one).
    float time() const { return time_; }
    float holdTime() const { return hold_; }
    float brightness() const { return brighten_; }
    int32_t flcFrame() const;

private:
    void showNext();
    void updateMovie(float delta);
    bool nextMovieFrame();
    void pushSound();
    void setShown();
    void finish();
    bool read(const char* path, std::vector<uint8_t>* bytes) const;

    const formats::MdkData* data_ = nullptr;
    VideoAudio audio_;
    void (*frameShown_)(void*, int32_t) = nullptr;
    void* frameShownUser_ = nullptr;
    void (*finished_)(void*) = nullptr;
    void* finishedUser_ = nullptr;

    std::unique_ptr<formats::Flc> flc_;
    std::unique_ptr<formats::Mve> mve_;
    float viewWidth_ = 600.0f;
    float viewHeight_ = 360.0f;
    float time_ = 0.0f;
    float hold_ = 0.0f;
    float brighten_ = 0.0f;
    int32_t soundVoice_ = 0;
    bool hasStill_ = false;
    int32_t width_ = 0;
    int32_t height_ = 0;
    std::vector<uint8_t> indices_;
    uint8_t palette_[kPaletteBytes] = {};
    uint8_t shown_[kPaletteBytes] = {};
    uint32_t imageVersion_ = 0;
    uint32_t paletteVersion_ = 0;
};

} // namespace menu
} // namespace mdk

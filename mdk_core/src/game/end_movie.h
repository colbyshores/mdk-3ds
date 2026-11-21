#pragma once

// ============================================================================
// end_movie.h -- the end of the game (C# src/Mdk.Game/Menu/EndMovie.cs; game
// state 8, 0x47727c; end_movie.gd): MISC/FLIC/MDKEND.FLC with its sounds from
// MISC/FINISH.BNI and a white flash (0x477604), then MISC/FLIC/MDKBZK.MVE
// (0x477870) and the main menu, after the splash. Esc (the 3DS's B, START or
// A: the C#'s Back) gives up.
//
//   frame 1 DOGSHIP (loop) ... 129 DROP, 133 FLYBY, 186 EXPLODE1, 188 DOGSHIP stops, 194 ENDEXP,
//   196 EXPLODE1, 210-232 whiter, 233 white for 1 s, 233-260 back
//
// THE GOG DATA HAS NEITHER MOVIE: the C#'s EndMovie then falls through both at
// construction and its first frame returns Event.Menu (the main menu, with the
// splash). That is what both frontends do on it; the screen is ported whole so a
// copy that has the files plays them (the goldens drive the frame extras over
// pie.flc, which has the 311 frames MDKEND.FLC's numbers need).
//
// The sounds are the frontend's (EndSounds: FINISH.BNI's entries by name; null
// hooks play nothing, which is the C#'s `_sounds == null`). The drawing is the
// player's (video_player.h).
//
// Cold code. No virtuals, no templates, no exceptions (AGENTS.md §10).
// ============================================================================

#include <stdint.h>

#include "game/game_flow.h"
#include "game/video_player.h"

namespace mdk {
namespace menu {

// The C#'s `Ui.Play(Ui.SoundOf(_sounds, name, looping))` and `Audio.Stop`.
struct EndSounds {
    // A voice id (0 for none). `loop`: Looping.Forever (DOGSHIP), else Once.
    int32_t (*play)(void* user, const char* name, bool loop) = nullptr;
    void (*stop)(void* user, int32_t voice) = nullptr;
    void* user = nullptr;
};

class EndMovie {
public:
    static const char* const kVideo;
    static const char* const kMovie;
    static const char* const kSounds;

    EndMovie();
    EndMovie(const EndMovie&) = delete;
    EndMovie& operator=(const EndMovie&) = delete;

    // The C# constructor: the FLC, or the MVE at once when it can't be read.
    // The player's data and audio hooks are set first (player()).
    void open(const EndSounds& sounds);

    // C# Frame's logic: Back gives up; the player's update. The event for the
    // game (C# _next: None, then Menu).
    flow::Event frame(bool back, float elapsed);

    // C# Status.
    const char* status() const { return video_.playing() ? "end movies" : "end movies: done"; }

    // C# Flash: how much whiter a frame is (0-1).
    static float flash(int32_t frame);

    VideoPlayer& player() { return video_; }
    const VideoPlayer& player() const { return video_; }
    flow::Event next() const { return next_; }

    // The goldens' stand-in for MDKEND.FLC (movie_probe's endmovie section):
    // `_next` back to None and the FLC played on the same player, as the probe
    // does through reflection after the data's own fall-through.
    void playForTest(const char* flc);
    // The voices the movie holds, by sound (the goldens read which changed).
    static const int32_t kSoundCount = 5;
    static const char* soundName(int32_t i);
    int32_t voice(int32_t i) const { return voices_[i]; }
    bool hasVoice(int32_t i) const { return hasVoice_[i]; }

    // C# Dispose: the player stopped, every voice stopped.
    void close();

private:
    static void onFrame(void* self, int32_t frame);
    static void onVideoFinished(void* self);
    void playMovie();
    void play(int32_t sound);

    VideoPlayer video_;
    EndSounds sounds_;
    // C# _voices (a Dictionary by name): FINISH.BNI's five, each kept once played.
    int32_t voices_[kSoundCount] = {};
    bool hasVoice_[kSoundCount] = {};
    // Which Finished handler is subscribed: the C#'s `+= PlayMovie`, then
    // `+= () => _next = Event.Menu`.
    bool movieNext_ = false;
    flow::Event next_ = flow::Event::None;
};

} // namespace menu
} // namespace mdk

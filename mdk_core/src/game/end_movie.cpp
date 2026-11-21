// end_movie.cpp -- see end_movie.h. C# src/Mdk.Game/Menu/EndMovie.cs, method by
// method.

#include "game/end_movie.h"

#include <math.h>
#include <string.h>

namespace mdk {
namespace menu {
namespace {

// C# FrameSounds (the frame counter before decoding a frame), Looped, LoopEnd,
// the flash and its hold.
const char* const kSoundNames[EndMovie::kSoundCount] = {"DOGSHIP", "DROP", "FLYBY", "EXPLODE1",
                                                       "ENDEXP"};
const int32_t kDogship = 0;
struct FrameSound {
    int32_t frame;
    int32_t sound;
};
const FrameSound kFrameSounds[] = {{1, 0}, {129, 1}, {133, 2}, {186, 3}, {194, 4}, {196, 3}};
const int32_t kLoopEnd = 188;
const int32_t kFlashStart = 210;
const int32_t kFlashPeak = 233;
const int32_t kFlashEnd = 260;
const float kFlashHold = 1.0f;
const float kMaxColour = 255.0f;

} // namespace

const char* const EndMovie::kVideo = "MISC/FLIC/MDKEND.FLC";
const char* const EndMovie::kMovie = "MISC/FLIC/MDKBZK.MVE";
const char* const EndMovie::kSounds = "MISC/FINISH.BNI";

EndMovie::EndMovie() {}

const char* EndMovie::soundName(int32_t i) {
    return i >= 0 && i < kSoundCount ? kSoundNames[i] : "?";
}

// The C# constructor.
void EndMovie::open(const EndSounds& sounds) {
    sounds_ = sounds;
    next_ = flow::Event::None;
    movieNext_ = false;
    for (int32_t i = 0; i < kSoundCount; i++) {
        voices_[i] = 0;
        hasVoice_[i] = false;
    }
    video_.setFrameShown(onFrame, this);
    video_.setFinished(onVideoFinished, this);
    if (!video_.play(kVideo)) {
        playMovie();
    }
}

// C# Frame: Back gives up; the player's update (its drawing is the frontend's).
flow::Event EndMovie::frame(bool back, float elapsed) {
    if (back) {
        next_ = flow::Event::Menu;
    }
    video_.update(elapsed);
    return next_;
}

// C# OnFrame: a frame was shown, the extras of the next one (0x477604).
void EndMovie::onFrame(void* self, int32_t frame) {
    EndMovie* movie = static_cast<EndMovie*>(self);
    for (size_t i = 0; i < sizeof(kFrameSounds) / sizeof(kFrameSounds[0]); i++) {
        if (kFrameSounds[i].frame == frame) {
            movie->play(kFrameSounds[i].sound);
        }
    }
    if (frame == kLoopEnd && movie->hasVoice_[kDogship] && movie->sounds_.stop) {
        movie->sounds_.stop(movie->sounds_.user, movie->voices_[kDogship]);
    }
    movie->video_.brighten(flash(frame));
    if (frame == kFlashPeak) {
        movie->video_.hold(kFlashHold);
    }
}

// The subscribed Finished handler: PlayMovie, then `_next = Event.Menu`.
void EndMovie::onVideoFinished(void* self) {
    EndMovie* movie = static_cast<EndMovie*>(self);
    if (movie->movieNext_) {
        movie->next_ = flow::Event::Menu;
    } else {
        movie->playMovie();
    }
}

// C# Flash.
float EndMovie::flash(int32_t frame) {
    if (frame < kFlashStart || frame > kFlashEnd) {
        return 0.0f;
    }
    const float t = frame < kFlashPeak
                        ? (float)(frame - kFlashStart) / (float)(kFlashPeak - kFlashStart)
                        : (float)(kFlashEnd - frame) / (float)(kFlashEnd - kFlashPeak);
    return nearbyintf(kMaxColour * t) / kMaxColour;
}

// C# PlayMovie: after the FLC, the MVE, then the menu.
void EndMovie::playMovie() {
    movieNext_ = true;
    for (int32_t i = 0; i < kSoundCount; i++) {
        if (hasVoice_[i] && sounds_.stop) {
            sounds_.stop(sounds_.user, voices_[i]);
        }
    }
    video_.brighten(0.0f);
    if (!video_.playMovie(kMovie)) {
        next_ = flow::Event::Menu;
    }
}

// C# Play: nothing without FINISH.BNI (null hooks).
void EndMovie::play(int32_t sound) {
    if (!sounds_.play) {
        return;
    }
    voices_[sound] = sounds_.play(sounds_.user, kSoundNames[sound], sound == kDogship);
    hasVoice_[sound] = true;
}

void EndMovie::playForTest(const char* flc) {
    next_ = flow::Event::None;
    video_.play(flc);
}

// C# Dispose: `_video.Dispose()` (Stop) and `_ui.Audio.StopAll()` -- the latter is
// the frontend's (it owns the device), so here every voice this movie holds.
void EndMovie::close() {
    video_.stop();
    for (int32_t i = 0; i < kSoundCount; i++) {
        if (hasVoice_[i] && sounds_.stop) {
            sounds_.stop(sounds_.user, voices_[i]);
        }
        hasVoice_[i] = false;
    }
}

} // namespace menu
} // namespace mdk

// video_player.cpp -- see video_player.h. C# src/Mdk.Game/Menu/VideoPlayer.cs,
// method by method.

#include "game/video_player.h"

#include <math.h>
#include <string.h>

#include "formats/flc.h"
#include "formats/mdk_data.h"

namespace mdk {
namespace menu {
namespace {

// C# ScreenView.Size and MovieScreen.
const float kScreenWidth = 600.0f;
const float kScreenHeight = 360.0f;
const float kMovieWidth = 640.0f;
const float kMovieHeight = 480.0f;
// C# MaxColour.
const int32_t kMaxColour = 255;

} // namespace

VideoPlayer::VideoPlayer() {}
VideoPlayer::~VideoPlayer() {}

int32_t VideoPlayer::flcFrame() const {
    return flc_ ? flc_->frame() : -1;
}

// C#'s `File.Exists(full) ? X.Load(full) : null`: the file's bytes, or false.
bool VideoPlayer::read(const char* path, std::vector<uint8_t>* bytes) const {
    return data_ != nullptr && data_->read(path, bytes);
}

// C# Play.
bool VideoPlayer::play(const char* path) {
    std::vector<uint8_t> bytes;
    std::unique_ptr<formats::Flc> flc(new formats::Flc());
    if (!read(path, &bytes) || !formats::Flc::load(bytes.data(), (int32_t)bytes.size(), flc.get())) {
        flc_.reset();
        return false;
    }
    flc_ = std::move(flc);
    viewWidth_ = kScreenWidth;
    viewHeight_ = kScreenHeight;
    time_ = 0.0f;
    showNext();
    return true;
}

// C# PlayMovie.
bool VideoPlayer::playMovie(const char* path) {
    std::vector<uint8_t> bytes;
    std::unique_ptr<formats::Mve> mve(new formats::Mve());
    if (!read(path, &bytes) || !formats::Mve::load(bytes.data(), (int32_t)bytes.size(), mve.get())) {
        mve_.reset();
        return false;
    }
    std::vector<uint8_t>().swap(bytes);
    mve_ = std::move(mve);
    viewWidth_ = kMovieWidth;
    viewHeight_ = kMovieHeight;
    time_ = 0.0f;
    if (!nextMovieFrame()) {
        mve_.reset();
        return false;
    }
    soundVoice_ = audio_.playStream ? audio_.playStream(audio_.user, mve_->sampleRate()) : 0;
    pushSound();
    return true;
}

// C# Brighten: `_image?.SetPalette(Brightened(_palette))`.
void VideoPlayer::brighten(float amount) {
    brighten_ = amount;
    if (hasStill_) {
        setShown();
    }
}

// C# Brightened: every colour raised by round(brighten * 255), at most 255.
void VideoPlayer::setShown() {
    if (brighten_ <= 0.0f) {
        memcpy(shown_, palette_, sizeof(shown_));
    } else {
        const int32_t raise = (int32_t)nearbyintf(brighten_ * (float)kMaxColour);
        for (int32_t i = 0; i < kPaletteBytes; i++) {
            const int32_t c = palette_[i] + raise;
            shown_[i] = (uint8_t)(c < kMaxColour ? c : kMaxColour);
        }
    }
    paletteVersion_++;
}

// C# ShowStill.
void VideoPlayer::showStill(int32_t width, int32_t height, const uint8_t* indices,
                            const uint8_t* paletteRgb) {
    const size_t count = (size_t)width * (size_t)height;
    if (indices_.size() != count) {
        indices_.assign(indices, indices + count);
    } else {
        memcpy(indices_.data(), indices, count);
    }
    width_ = width;
    height_ = height;
    memcpy(palette_, paletteRgb, sizeof(palette_));
    hasStill_ = true;
    imageVersion_++;
    setShown();
}

// C# Stop.
void VideoPlayer::stop() {
    flc_.reset();
    mve_.reset();
    if (audio_.stop) {
        audio_.stop(audio_.user, soundVoice_);
    }
    soundVoice_ = 0;
}

// C# Update.
void VideoPlayer::update(float delta) {
    if (mve_) {
        updateMovie(delta);
        return;
    }
    if (!flc_) {
        return;
    }
    if (hold_ > 0.0f) {
        hold_ -= delta;
        return;
    }
    time_ += delta;
    /* @vfp-exempt T1,T2 — the C#'s own catch-up loop (VideoPlayer.Update), bounded:
     * one frame per 34 ms of a delta the frontends cap at 0.25 s (at most 8). The
     * repeated subtraction is the clock the goldens pin (golden/movie_menu.txt,
     * movie_endmovie.txt print it); a closed form is not bit-identical to it.
     * measured n/a. Verified 2026-11-20. */
    while (flc_ && hold_ <= 0.0f && time_ >= kFrameSeconds) {
        time_ -= kFrameSeconds;
        showNext();
    }
}

// C# ShowNext.
void VideoPlayer::showNext() {
    if (flc_->nextFrame() != formats::Flc::FrameStatus::Decoded) {
        finish();
        return;
    }
    showStill(flc_->width(), flc_->height(), flc_->indices(), flc_->palette());
    if (frameShown_) {
        frameShown_(frameShownUser_, flc_->frame());
    }
}

// C# UpdateMovie. Without a stream voice (null Queued) the sound never runs low.
void VideoPlayer::updateMovie(float delta) {
    time_ += delta * kMicrosecondsPerSecond;
    /* @vfp-exempt T1,T2 — the C#'s UpdateMovie loop, kept as it is: a frame per
     * frame time owed, or sooner while the sound runs low; each pass decodes a
     * frame, so the movie's end bounds it. measured n/a. Verified 2026-11-20. */
    for (;;) {
        if (!mve_) {
            return;
        }
        const bool low = audio_.queued != nullptr &&
                         audio_.queued(audio_.user, soundVoice_) < kSoundLow;
        if (!(time_ >= (float)mve_->frameTime() || low)) {
            return;
        }
        const float left = time_ - (float)mve_->frameTime();
        time_ = left > 0.0f ? left : 0.0f;
        if (!nextMovieFrame()) {
            finish();
            return;
        }
        pushSound();
    }
}

// C# NextMovieFrame. A broken movie ends where the C#'s throws.
bool VideoPlayer::nextMovieFrame() {
    if (mve_->nextFrame() != formats::Mve::FrameStatus::Decoded) {
        return false;
    }
    showStill(mve_->width(), mve_->height(), mve_->indices(), mve_->palette());
    if (frameShown_) {
        frameShown_(frameShownUser_, mve_->frame());
    }
    return true;
}

// C# PushSound.
void VideoPlayer::pushSound() {
    if (!mve_ || mve_->audio().empty() || !audio_.push) {
        return;
    }
    audio_.push(audio_.user, soundVoice_, mve_->audio().data(), (int32_t)mve_->audio().size());
}

// C# Finish.
void VideoPlayer::finish() {
    flc_.reset();
    mve_.reset();
    if (finished_) {
        finished_(finishedUser_);
    }
}

} // namespace menu
} // namespace mdk

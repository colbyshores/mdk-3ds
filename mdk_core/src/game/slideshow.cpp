// slideshow.cpp -- see slideshow.h. C# Menu/Slideshow.cs: Start (:33-42), Update
// (:44-56), Reset (:59-62), Duration (:64-69), Next (:71-87), Load (:89-102).

#include "game/slideshow.h"

#include <stdio.h>

#include "formats/mdk_data.h"

namespace mdk {
namespace {

// C# Slideshow's constants.
const char kImagePath[] = "MISC/MDKS_%03d.GIF";
const float kFirstDelay = 5.0f;
const float kFirstTime = 4.0f;
const float kNextTime = 2.0f;
// The C#'s Load stops at the first missing slide; no file count is near this.
const int32_t kMaxSlides = 999;

} // namespace

void Slideshow::start(const formats::MdkData& data) {
    running_ = true;
    time_ = 0.0f;
    if (!slides_.empty()) {
        return;
    }
    // C# Load: MDKS_001, MDKS_002, ... until one is missing or not a GIF.
    for (int32_t i = 1; i <= kMaxSlides; i++) {
        char path[32];
        snprintf(path, sizeof(path), kImagePath, (int)i);
        std::vector<uint8_t> bytes;
        formats::Gif gif;
        if (!data.read(path, &bytes) ||
            !formats::Gif::load(bytes.data(), (int32_t)bytes.size(), &gif)) {
            return;
        }
        slides_.push_back(gif);
    }
}

void Slideshow::update(float delta) {
    if (!running_ || slides_.empty()) {
        return;
    }
    time_ += delta;
    if (time_ >= duration()) {
        next();
    }
}

void Slideshow::reset() {
    time_ = image_ == kBare ? duration() : 0.0f;
}

float Slideshow::duration() const {
    return image_ == 0 ? kFirstDelay : image_ == kBare ? kFirstTime : kNextTime;
}

void Slideshow::next() {
    time_ = 0.0f;
    // The C#'s (Image + 1) % (count + 1): past the last slide, the video's frame.
    image_ = image_ + 1 > (int32_t)slides_.size() ? 0 : image_ + 1;
}

const formats::Gif* Slideshow::gif(int32_t image) const {
    return image >= 1 && image <= (int32_t)slides_.size() ? &slides_[(size_t)(image - 1)] : nullptr;
}

} // namespace mdk

// slideshow.cpp -- see slideshow.h. C# Menu/Slideshow.cs: Start (:33-42), Update
// (:44-56), Reset (:59-62), Duration (:64-69), Next (:71-87), Load (:89-102).

#include "game/slideshow.h"

#include <stdio.h>
#include <stdlib.h>

#include <string>

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

Slideshow::~Slideshow() {
    // A read still out is dropped, so the reader's slot is not held forever.
    if (loading_ && requested_ && reader_.cancel) {
        reader_.cancel(reader_.user, kTagBase + loadNext_);
    }
}

void Slideshow::start(const formats::MdkData& data) {
    running_ = true;
    time_ = 0.0f;
    if (!slides_.empty() || loading_) {
        return;
    }
    if (reader_.active()) {
        // THE LENT READER: the slides come one at a time off the main thread
        // (pump), in Load's order and with its stop.
        data_ = &data;
        loading_ = true;
        loadNext_ = 1;
        requested_ = false;
        pump();
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

// C# Load, spread over frames and off the main thread: the next slide's file
// asked for, decoded on the reader's thread (Gif::decodePacked), and at most one
// arrived slide taken a frame (a copy). A missing or broken file ends the load,
// as Load's `gif == null` does.
void Slideshow::pump() {
    if (!loading_) {
        return;
    }
    if (requested_) {
        uint8_t* bytes = nullptr;
        uint32_t size = 0;
        bool ok = false;
        if (!reader_.poll(reader_.user, kTagBase + loadNext_, &bytes, &size, &ok)) {
            return;
        }
        requested_ = false;
        formats::Gif gif;
        const bool good = ok && bytes != nullptr && formats::Gif::fromPacked(bytes, size, &gif);
        free(bytes);
        if (!good) {
            loading_ = false;
            return;
        }
        slides_.push_back(gif);
        loadNext_++;
        if (loadNext_ > kMaxSlides) {
            loading_ = false;
            return;
        }
    }
    // The next file is asked for at once, so it is read while this frame goes on;
    // a full reader is asked again next frame.
    char relative[32];
    snprintf(relative, sizeof(relative), kImagePath, (int)loadNext_);
    const std::string path = data_->pathOf(relative);
    requested_ = reader_.request(reader_.user, kTagBase + loadNext_, path.c_str(),
                                 formats::Gif::decodePacked);
}

void Slideshow::update(float delta) {
    pump();
    if (!running_ || (slides_.empty() && !loading_)) {
        return;
    }
    time_ += delta;
    // While the slides load, an image whose successor is not here yet stays up
    // until it is: what is resident shows (the C# has them all at Start).
    const bool nextHere = !loading_ || image_ + 1 <= (int32_t)slides_.size();
    if (time_ >= duration() && nextHere) {
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

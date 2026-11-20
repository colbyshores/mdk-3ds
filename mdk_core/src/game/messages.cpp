// messages.cpp -- see messages.h.

#include "game/messages.h"

#include <math.h>
#include <string.h>

#include "formats/fti.h"
#include "game/font_view.h"

namespace mdk {

namespace {

// Seconds to grow or shrink (0.5 x 2 = full size).
const float kZoomTime = 0.5f;
const float kViewWidth = 600.0f;
const float kBaseline = 120.0f;
const float kLineOffset = 15.0f;

// The first "\n" escape (two characters) of text[start, length), or -1 (the C#'s
// IndexOf over the span, as an index from `start`).
int32_t findLineBreak(const uint8_t* text, int32_t start, int32_t length) {
    for (int32_t i = start; i + 1 < length; i++) {
        if (text[i] == '\\' && text[i + 1] == 'n') {
            return i - start;
        }
    }
    return -1;
}

} // namespace

void Messages::setTexts(const formats::Fti& fti) {
    // Every entry's Fti.GetTextBytes: its bytes up to the first NUL. Cold.
    texts_.clear();
    textBytes_.clear();
    const formats::ArchiveIndex& index = fti.index();
    const std::vector<uint8_t>& bytes = index.bytes();
    const std::vector<std::pair<std::string, formats::ArchiveIndex::Entry> >& entries =
        index.entries();
    for (size_t i = 0; i < entries.size(); i++) {
        const formats::ArchiveIndex::Entry& entry = entries[i].second;
        Text text;
        text.name = entries[i].first;
        text.offset = (int32_t)textBytes_.size();
        if (entry.offset >= 0 && entry.size >= 0 &&
            entry.offset + entry.size <= (int32_t)bytes.size()) {
            const uint8_t* start = bytes.data() + entry.offset;
            int32_t length = 0;
            while (length < entry.size && start[length] != 0) {
                length++;
            }
            textBytes_.insert(textBytes_.end(), start, start + length);
            text.length = length;
        }
        texts_.push_back(text);
    }
}

void Messages::reset() {
    queueCount_ = 0;
    hasCurrent_ = false;
    time_ = 0.0f;
    zoom_ = 0.0f;
}

bool Messages::push(const char* textName, int32_t flags, float seconds) {
    // C# Push: PushText(fti.GetTextBytes(textName)); a missing name is no text.
    for (size_t i = 0; i < texts_.size(); i++) {
        if (texts_[i].name == textName) {
            return pushText(textBytes_.data() + texts_[i].offset, texts_[i].length, flags,
                            seconds);
        }
    }
    return false;
}

// Viewer.PickupMessageSeconds.
const float Messages::kPickupSeconds = 2.0f;

void Messages::pickedUp(void* context, const std::string& name) {
    static_cast<Messages*>(context)->push(name.c_str(), kFlagZoom, kPickupSeconds);
}

bool Messages::pushText(const uint8_t* text, int32_t length, int32_t flags, float seconds) {
    if (length == 0) {
        return false;
    }

    // At most two lines of 35 characters, split at the two-character \n escape.
    Message message;
    int32_t start = 0;
    while (message.lineCount < kMaxLines) {
        const int32_t found = findLineBreak(text, start, length);
        const int32_t end = found < 0 ? length : start + found;
        const int32_t stop = end < start + kMaxLineLength ? end : start + kMaxLineLength;
        const int32_t line = message.lineCount++;
        message.length[line] = stop - start;
        memcpy(message.text[line], text + start, (size_t)(stop - start));
        if (end >= length) {
            break;
        }
        start = end + 2;
    }
    message.flags = flags;
    message.time = seconds;

    if ((flags & kFlagFront) != 0) {
        for (int32_t i = queueCount_; i > 0; i--) {
            queue_[i] = queue_[i - 1];
        }
        queue_[0] = message;
    } else {
        queue_[queueCount_] = message;
    }
    queueCount_++;
    if (queueCount_ > kQueueSize) {
        queueCount_--;
    }
    return true;
}

void Messages::update(float delta) {
    const float step = queueCount_ > 0 ? delta * 2.0f : delta;
    if (time_ == 0.0f) {
        if (zoom_ > 0.0f) {
            const float zoom = zoom_ - delta;
            zoom_ = zoom > 0.0f ? zoom : 0.0f;
            return;
        }

        // The first waiting message, or none (the C#'s `_queue.First?.Value`).
        hasCurrent_ = queueCount_ > 0;
        if (hasCurrent_) {
            current_ = queue_[0];
            for (int32_t i = 1; i < queueCount_; i++) {
                queue_[i - 1] = queue_[i];
            }
            queueCount_--;
            time_ = current_.time;
        }
        return;
    }

    if ((current_.flags & kFlagZoom) == 0 || zoom_ == kZoomTime) {
        const float time = time_ - step;
        time_ = time > 0.0f ? time : 0.0f;
        return;
    }

    const float zoom = zoom_ + delta;
    zoom_ = zoom < kZoomTime ? zoom : kZoomTime;
}

void Messages::draw(MdkRenderer renderer, const FontView& big, const FontView& small,
                    float width) const {
    if (!hasCurrent_) {
        return;
    }

    const bool full = time_ > 0.0f && ((current_.flags & kFlagZoom) == 0 || zoom_ == kZoomTime);
    const float scale = full ? 1.0f : zoom_ * 2.0f;
    if (scale <= 0.0f) {
        return;
    }

    const Colour white{1.0f, 1.0f, 1.0f, 1.0f};
    for (int32_t i = 0; i < current_.lineCount; i++) {
        const uint8_t* line = current_.text[i];
        const int32_t length = current_.length[i];
        float y = kBaseline;
        if (current_.lineCount == kMaxLines) {
            y += (i == 1 ? kLineOffset : -kLineOffset) * scale;
        }

        const FontView& font =
            full && (float)big.width(line, length) >= kViewWidth ? small : big;
        // MathF.Round rounds ties to even; rintf does the same in the default
        // rounding mode, and no code here changes that mode.
        const float x = rintf(width / 2.0f - (float)font.width(line, length) * scale / 2.0f);
        font.draw(renderer, line, length, x, rintf(y), scale, white);
    }
}

} // namespace mdk

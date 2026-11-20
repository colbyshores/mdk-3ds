// briefing.cpp -- see briefing.h. C# Menu/Typesetter.cs Layout, and
// Menu/StatsScreen.cs's Briefing page: the constructor and Start (:115-145,
// :197-243), Frame (:150-174), Update (:176-195), EndPhase/NextPhase
// (:247-277), UpdateTyping (:300-345), Draw and DrawTyped (:347-375, :537-560).

#include "game/briefing.h"

#include <stdio.h>
#include <string.h>

#include <string>

#include "formats/fti.h"
#include "game/font_view.h"
#include "game/level_flow.h"
#include "game/screen_view.h"

namespace mdk {
namespace {

// C# Typesetter's constants.
const float kLineHeight = 36.0f;
const float kCentre = 300.0f;
const uint8_t kEscape = (uint8_t)'\\';

// C# StatsScreen's constants for this page.
const float kFadeSpeed = 2.0f;
const float kFast = 2.0f;
// Typing (0x4335c0): characters per second, and when fast.
const float kTypeRate = 15.0f;
const float kTypeRateFast = 60.0f;
const float kBriefY = 32.0f;
const int32_t kCursorBlink = 31;
const int32_t kCursorOn = 15;
const float kTicksPerSecond = 30.0f;

// C# int.TryParse over the code's digits and minus signs ("12", "-4"; not "-",
// "1-2"): the escape's number.
bool parseNumber(const uint8_t* text, int32_t length, int32_t* out) {
    if (length <= 0) {
        return false;
    }
    int32_t i = 0;
    bool negative = false;
    if (text[0] == '-') {
        negative = true;
        i = 1;
    }
    if (i >= length) {
        return false;
    }
    int64_t value = 0;
    for (; i < length; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
        value = value * 10 + (text[i] - '0');
        if (value > 2147483648LL) {
            return false;
        }
    }
    value = negative ? -value : value;
    if (value > 2147483647LL) {
        return false;
    }
    *out = (int32_t)value;
    return true;
}

} // namespace

int32_t typesetLayout(const uint8_t* text, int32_t length, float y,
                      std::vector<TypesetLine>* lines) {
    lines->clear();
    TypesetLine line;
    float x = 0.0f;
    bool centred = false;
    int32_t start = 0;
    int32_t count = 0;
    bool cost = true;
    int32_t i = 0;
    while (i < length) {
        const uint8_t c = text[i++];
        if (c != kEscape) {
            line.text.push_back(c);
            count += cost ? 1 : 0;
            continue;
        }
        int32_t j = i;
        while (j < length && ((text[j] >= '0' && text[j] <= '9') || text[j] == '-')) {
            j++;
        }
        int32_t number = 0;
        const bool hasNumber = j > i && parseNumber(text + i, j - i, &number);
        if (!hasNumber) {
            number = 0;
        }
        const uint8_t code = j < length ? text[j] : 0;
        i = j + 1;
        switch (code) {
            case 'c':
            case 'n':
            case 'x':
            case 'y':
                line.x = x;
                line.y = y;
                line.centred = centred;
                line.start = start;
                lines->push_back(line);
                start = count;
                line = TypesetLine();
                if (code == 'c') {
                    x = hasNumber ? (float)number : kCentre;
                    centred = true;
                } else if (code == 'x') {
                    x = hasNumber ? (float)number : 0.0f;
                    centred = false;
                } else if (code == 'n') {
                    x = 0.0f;
                    centred = false;
                    y = y + kLineHeight + (float)number;
                } else {
                    x = 0.0f;
                    centred = false;
                    y = y + (hasNumber ? (float)number : kLineHeight);
                }
                break;
            case 'p':
                count += number;
                break;
            case 'd':
                cost = true;
                break;
            case 'i':
                cost = false;
                break;
            default:
                break;
        }
    }
    line.x = x;
    line.y = y;
    line.centred = centred;
    line.start = start;
    lines->push_back(line);
    return count;
}

// The C# constructor with StatsPages.Briefing, and Start(Phase.Briefing).
void Briefing::open(const formats::Fti* fti, int32_t level) {
    const int32_t index = flow::indexOf(level);
    index_ = index > 0 ? index : 0;
    texts_.clear();
    textY_.clear();
    fadeIn_ = 0.0f;
    fadeOut_ = -1.0f;
    fast_ = false;
    skip_ = false;
    pressed_ = false;
    ticks_ = 0.0f;
    current_ = 0;
    count_ = 1.0f;
    shown_ = 0;
    next_ = flow::Event::None;
    // "On entry the inventory is emptied and the health raised to 100 (every
    // level starts with both)": the level does that, not this page.
    char name[24];
    snprintf(name, sizeof(name), "BRIEF%d", (int)(index_ + 1));
    texts_.push_back(fti ? fti->getTextBytes(name) : std::vector<uint8_t>());
    textY_.push_back(kBriefY);
}

// C# Frame: a key pressed this frame; Esc is a skip.
flow::Event Briefing::frame(bool anyPressed, bool backPressed, bool fastHeld, float elapsed) {
    if (anyPressed) {
        skip_ = skip_ || backPressed;
        pressed_ = true;
    }
    update(elapsed, fastHeld);
    return next_;
}

// C# Update.
void Briefing::update(float delta, bool fastHeld) {
    fast_ = skip_ || fastHeld;
    const float speed = fast_ ? kFast : 1.0f;
    ticks_ += delta * kTicksPerSecond;
    if (fadeOut_ >= 0.0f) {
        fadeOut_ += kFadeSpeed * speed * delta;
        if (fadeOut_ >= 1.0f) {
            // EndPhase -> NextPhase: the briefing is the last page, then the fall.
            next_ = flow::Event::Fall;
        }
    } else if (fadeIn_ < 1.0f) {
        const float in = fadeIn_ + kFadeSpeed * speed * delta;
        fadeIn_ = in < 1.0f ? in : 1.0f;
    } else if (updateTyping(delta) && (pressed_ || fast_)) {
        fadeOut_ = 0.0f;
    }
    skip_ = false;
    pressed_ = false;
}

// C# UpdateTyping: the page's texts one after the other; Esc finishes them all.
bool Briefing::updateTyping(float delta) {
    if (current_ >= (int32_t)texts_.size()) {
        return true;
    }
    std::vector<TypesetLine> lines;
    const std::vector<uint8_t>& text = texts_[current_];
    const int32_t total = typesetLayout(text.data(), (int32_t)text.size(), 0.0f, &lines);
    if (skip_) {
        current_ = (int32_t)texts_.size();
        return false;
    }
    count_ += (fast_ ? kTypeRateFast : kTypeRate) * delta;
    const int32_t rounded = (int32_t)roundEven(count_);
    const int32_t shown = rounded < total ? rounded : total;
    if (shown != shown_) {
        // The C# plays TELETYPE here (not ported: briefing.h).
        shown_ = shown;
    }
    if (count_ < (float)total) {
        return false;
    }
    current_++;
    count_ = 0.0f;
    shown_ = 0;
    return current_ >= (int32_t)texts_.size();
}

// C# Draw.
void Briefing::draw(MdkRenderer renderer, const ScreenView& view, const FontView& big,
                    int32_t mapTexture, int32_t mapPalette, int32_t mapWidth,
                    int32_t mapHeight) const {
    view.image(renderer, mapTexture, mapPalette, mapWidth, mapHeight, 0.0f, 0.0f);
    if (fadeIn_ >= 1.0f) {
        const int32_t end =
            current_ + 1 < (int32_t)texts_.size() ? current_ + 1 : (int32_t)texts_.size();
        for (int32_t i = 0; i < end; i++) {
            drawTyped(renderer, view, big, texts_[i], textY_[i],
                      i < current_ ? -1 : (int32_t)roundEven(count_));
        }
    }
    // The fade: the page blended with black (in and out alike on this page).
    const float fade = fadeOut_ < 0.0f ? 1.0f - fadeIn_ : fadeOut_;
    if (fade > 0.0f) {
        const Colour black{0.0f, 0.0f, 0.0f, fade < 1.0f ? fade : 1.0f};
        view.fill(renderer, 0.0f, 0.0f, ScreenView::kWidth, ScreenView::kHeight, black);
    }
}

// C# DrawTyped: a text with a budget of characters (-1: all), a cursor after the
// last one typed.
void Briefing::drawTyped(MdkRenderer renderer, const ScreenView& view, const FontView& big,
                         const std::vector<uint8_t>& text, float y, int32_t budget) const {
    std::vector<TypesetLine> lines;
    const int32_t count = typesetLayout(text.data(), (int32_t)text.size(), y, &lines);
    const int32_t remaining = budget < 0 ? count : budget;
    for (size_t n = 0; n < lines.size(); n++) {
        const TypesetLine& line = lines[n];
        if (line.start > remaining) {
            break;
        }
        const int32_t take = remaining - line.start < (int32_t)line.text.size()
                                 ? remaining - line.start
                                 : (int32_t)line.text.size();
        std::vector<uint8_t> part(line.text.begin(), line.text.begin() + take);
        // The cursor follows the last line being typed.
        const bool last = n + 1 == lines.size() || lines[n + 1].start > remaining;
        if (last && remaining < count) {
            part.push_back(((int32_t)ticks_ & kCursorBlink) <= kCursorOn ? (uint8_t)'_'
                                                                          : (uint8_t)' ');
        }
        // A centred line is placed by its whole width, so it doesn't move while typed.
        const float left =
            line.centred
                ? line.x - (float)(big.width(line.text.data(), (int32_t)line.text.size()) >> 1)
                : line.x;
        view.text(renderer, big, part.data(), (int32_t)part.size(), left, line.y, 1.0f, 1.0f);
    }
}

} // namespace mdk

// save_prompt.cpp -- see save_prompt.h. C# src/Mdk.Game/Menu/SavePrompt.cs.

#include "game/save_prompt.h"

#include <math.h>

namespace mdk {
namespace flow {

namespace {

const char* const kAnswerKeys[SavePrompt::kAnswers] = {"ABORT2", "ABORT3"};
const char kNameExtra[] = "_$";

bool isNameCharacter(char c) {
    // char.IsAsciiLetterOrDigit(c) || "_$".Contains(c), after ToUpperInvariant.
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == kNameExtra[0] || c == kNameExtra[1];
}

char upper(char c) {
    return c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
}

} // namespace

SavePrompt::SavePrompt(const SaveGames& saves, const SaveGame& save, const std::string& name)
    : saves_(saves), save_(save) {
    name_ = name.size() > (size_t)kNameLength ? name.substr(0, (size_t)kNameLength) : name;
    // A full save (F2) asks for the name at once.
    stage_ = save.isKind(SaveKind::Snapshot) ? Stage::Name : Stage::Ask;
    cursor_ = (int32_t)name_.size();
}

void SavePrompt::update(const Input& input, float delta) {
    ticks_ += delta * kTicksPerSecond;
    const float grown = grow_ + delta * kTicksPerSecond;
    grow_ = grown < kGrowTicks ? grown : kGrowTicks;  // MathF.Min
    if (stage_ == Stage::Ask) {
        ask(input);
        return;
    }
    typeName(input);
}

void SavePrompt::ask(const Input& input) {
    // Up and down or the mouse choose, Enter, Space or a click confirms, Esc says no.
    if (input.pointerMoved || input.clicked) {
        // A float divide (both are floats): MathF.Floor((y - RowTop) / RowHeight).
        const float rows = (input.pointerY - kRowTop) / (float)kRowHeight;
        const int32_t row = (int32_t)floorf(rows);
        if (row >= 0 && row < kAnswers) {
            select(row);
        }
    }
    if (input.up || input.down) {
        select(1 - selected_);
    }
    if (input.accept || input.clicked) {
        answer();
    } else if (input.back) {
        closed_ = true;
    }
}

void SavePrompt::select(int32_t row) {
    if (row == selected_) {
        return;
    }
    selected_ = row;
    grow_ = 0.0f;
}

void SavePrompt::answer() {
    if (selected_ != kYes) {
        closed_ = true;
        return;
    }
    stage_ = Stage::Name;
}

void SavePrompt::typeName(const Input& input) {
    if (input.back) {
        closed_ = true;
        return;
    }
    // Enter saves; Space (also an Accept key) doesn't.
    if (input.accept && input.typed.find(' ') == std::string::npos) {
        write();
        return;
    }
    const int32_t length = (int32_t)name_.size();
    if (input.left) {
        cursor_ = cursor_ - 1 > 0 ? cursor_ - 1 : 0;
    }
    if (input.right) {
        cursor_ = cursor_ + 1 < length ? cursor_ + 1 : length;
    }
    if (input.home) {
        cursor_ = 0;
    }
    if (input.end) {
        cursor_ = length;
    }
    if (input.backspace && cursor_ > 0) {
        name_.erase((size_t)cursor_ - 1, 1);
        cursor_--;
    }
    if (input.del && cursor_ < (int32_t)name_.size()) {
        name_.erase((size_t)cursor_, 1);
    }
    for (size_t i = 0; i < input.typed.size(); i++) {
        type(upper(input.typed[i]));
    }
}

void SavePrompt::type(char c) {
    if (cursor_ >= kNameLength || !isNameCharacter(c)) {
        return;
    }
    // _name[.._cursor] + c + (the rest after the replaced character).
    const std::string rest = cursor_ + 1 < (int32_t)name_.size()
                                 ? name_.substr((size_t)cursor_ + 1)
                                 : std::string();
    name_ = name_.substr(0, (size_t)cursor_) + c + rest;
    cursor_++;
}

void SavePrompt::write() {
    if (name_.empty()) {
        error_ = "no name";
        return;
    }
    if (!saves_.write(name_, save_, &error_)) {
        return;  // the prompt stays open, as the C#'s does on a failed write
    }
    error_.clear();
    saved_ = true;
    closed_ = true;
}

void SavePrompt::layout(WidthFn width, void* context, std::vector<Run>* out) const {
    out->clear();
    if (stage_ == Stage::Ask) {
        // DrawLine: centred at row (SV_ASK at row -1, small), scaled about its centre.
        for (int32_t row = -1; row < kAnswers; row++) {
            Run run;
            run.key = true;
            run.text = row < 0 ? "SV_ASK" : kAnswerKeys[row];
            run.scale = row == selected_ ? kSmall + (1.0f - kSmall) * grow_ / kGrowTicks : kSmall;
            run.x = (kViewWidth - width(context, run.text, true) * run.scale) * 0.5f;
            run.y = kRowY + kRowHeight * (float)row;
            out->push_back(run);
        }
        return;
    }
    // The title, the name one character per step, and the blinking cursor.
    Run title;
    title.key = true;
    title.text = "SV_TITLE";
    title.x = (kViewWidth - width(context, title.text, true)) * 0.5f;
    title.y = kTitleY;
    out->push_back(title);
    for (size_t i = 0; i < name_.size(); i++) {
        Run glyph;
        glyph.text = std::string(1, name_[i]);
        glyph.x = kNameX + kNameStep * (float)i - width(context, glyph.text, false) * 0.5f;
        glyph.y = kNameY;
        out->push_back(glyph);
    }
    if (((int32_t)ticks_ & kBlinkTicks) != 0) {
        Run cursor;
        cursor.text = "_";
        cursor.x = kNameX + kNameStep * (float)cursor_ - width(context, cursor.text, false) * 0.5f;
        cursor.y = kNameY;
        out->push_back(cursor);
    }
}

} // namespace flow
} // namespace mdk

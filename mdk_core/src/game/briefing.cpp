// briefing.cpp -- see briefing.h. C# Menu/Typesetter.cs Layout (the briefing
// page itself is StatsScreen's: game/stats_screen.cpp).

#include "game/briefing.h"

#include <stdint.h>

namespace mdk {
namespace {

// C# Typesetter's constants.
const float kLineHeight = 36.0f;
const float kCentre = 300.0f;
const uint8_t kEscape = (uint8_t)'\\';


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

} // namespace mdk

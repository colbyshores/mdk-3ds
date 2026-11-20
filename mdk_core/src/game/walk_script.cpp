// ============================================================================
// walk_script.cpp -- see walk_script.h. Cold code (parsed once at boot) apart
// from walkKeysAt/applyWalk, which scan at most kMaxSegments integers a frame.
// ============================================================================

#include "game/walk_script.h"

#include <stdio.h>
#include <string.h>

namespace mdk {

namespace {

// Indexed by mdk::Key. Escape has no script name (walk_script.h).
const char* const kKeyNames[kKeyCount] = {
    "forward",  "back",  "strafeleft", "straferight", "turnleft",  "turnright",
    "jump",     "turbo", "fire",       "useitem",     "itemnext",  "itemprevious",
    nullptr,    "sniper", "zoomin",     "zoomout",
};
static_assert(kKeyEscape == kKeyItemPrevious + 1, "kKeyNames has Escape's null slot after itemprevious");
static_assert(kKeySniper == kKeyEscape + 1, "kKeyNames has sniper after Escape's null slot");
static_assert(kKeyZoomIn == kKeySniper + 1 && kKeyZoomOut == kKeyZoomIn + 1,
              "kKeyNames has zoomin, zoomout after sniper");
static_assert(kKeyZoomOut == kKeyCount - 1, "kKeyNames ends with zoomout");

void say(char* error, size_t errorSize, const char* format, int32_t segment, const char* text,
         size_t length, const char* detail) {
    if (!error || errorSize == 0) {
        return;
    }
    snprintf(error, errorSize, format, (int)segment, (int)length, text, detail);
}

bool isSpace(char c) {
    return c == ' ' || c == '\t';
}

// One action name in [begin, end): its Key, or -1.
int32_t keyNamed(const char* begin, const char* end) {
    const size_t length = (size_t)(end - begin);
    for (int32_t k = 0; k < kKeyCount; k++) {
        const char* name = kKeyNames[k];
        if (name && strlen(name) == length && strncmp(name, begin, length) == 0) {
            return k;
        }
    }
    return -1;
}

// A decimal frame number in [begin, end), nothing else; false on any other
// character, an empty field or a value past kMaxFrame.
bool frameNumber(const char* begin, const char* end, uint32_t* out) {
    if (begin == end) {
        return false;
    }
    uint32_t value = 0;
    for (const char* p = begin; p < end; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        value = value * 10u + (uint32_t)(*p - '0');
        if (value > WalkScript::kMaxFrame) {
            return false;
        }
    }
    *out = value;
    return true;
}

// Trim spaces from both ends of [*begin, *end).
void trim(const char** begin, const char** end) {
    while (*begin < *end && isSpace(**begin)) (*begin)++;
    while (*end > *begin && isSpace(*(*end - 1))) (*end)--;
}

} // namespace

bool parseWalkScript(const char* text, WalkScript* out, char* error, size_t errorSize) {
    if (error && errorSize > 0) {
        error[0] = '\0';
    }
    if (!out) {
        return false;
    }
    out->count = 0;
    if (!text) {
        text = "";
    }
    WalkScript parsed;
    const char* p = text;
    int32_t index = 0;
    for (;;) {
        const char* segEnd = strchr(p, ';');
        if (!segEnd) {
            segEnd = p + strlen(p);
        }
        const char* begin = p;
        const char* end = segEnd;
        trim(&begin, &end);
        index++;
        const size_t length = (size_t)(end - begin);
        if (begin == end) {
            // An empty segment: a trailing ';' is tolerated, any other is a typo.
            if (*segEnd == '\0' && parsed.count > 0) {
                break;
            }
            say(error, errorSize, "walk segment %d (`%.*s`) is empty%s", index, begin, length, "");
            return false;
        }
        if (parsed.count >= WalkScript::kMaxSegments) {
            say(error, errorSize, "walk segment %d (`%.*s`): more than %s segments", index, begin,
                length, "32");
            return false;
        }
        static_assert(WalkScript::kMaxSegments == 32, "the refusal above names 32");
        const char* at = (const char*)memchr(begin, '@', length);
        if (!at) {
            say(error, errorSize, "walk segment %d (`%.*s`) has no '@': want %s", index, begin,
                length, "keys@first-end");
            return false;
        }
        // The keys: name('+'name)*.
        uint32_t keys = 0;
        const char* k = begin;
        for (;;) {
            const char* plus = (const char*)memchr(k, '+', (size_t)(at - k));
            const char* nameEnd = plus ? plus : at;
            const char* nameBegin = k;
            trim(&nameBegin, &nameEnd);
            const int32_t key = keyNamed(nameBegin, nameEnd);
            if (key < 0) {
                say(error, errorSize, "walk segment %d (`%.*s`): unknown action %s", index, begin,
                    length, "(want forward, back, strafeleft, straferight, turnleft, turnright, "
                            "jump, turbo, fire, useitem, itemnext or itemprevious, '+' between)");
                return false;
            }
            keys |= 1u << (uint32_t)key;
            if (!plus) {
                break;
            }
            k = plus + 1;
        }
        // The range: first-end.
        const char* rangeBegin = at + 1;
        const char* dash = (const char*)memchr(rangeBegin, '-', (size_t)(end - rangeBegin));
        if (!dash) {
            say(error, errorSize, "walk segment %d (`%.*s`) has no '-' in its range: want %s",
                index, begin, length, "keys@first-end");
            return false;
        }
        const char* firstBegin = rangeBegin;
        const char* firstEnd = dash;
        const char* lastBegin = dash + 1;
        const char* lastEnd = end;
        trim(&firstBegin, &firstEnd);
        trim(&lastBegin, &lastEnd);
        WalkSegment segment;
        if (!frameNumber(firstBegin, firstEnd, &segment.first) ||
            !frameNumber(lastBegin, lastEnd, &segment.end)) {
            say(error, errorSize, "walk segment %d (`%.*s`): %s", index, begin, length,
                "frames are decimal numbers from 0 to 16777216");
            return false;
        }
        static_assert(WalkScript::kMaxFrame == 16777216u, "the refusal above names kMaxFrame");
        if (segment.end <= segment.first) {
            say(error, errorSize, "walk segment %d (`%.*s`): %s", index, begin, length,
                "the end frame must be after the first (the end is exclusive)");
            return false;
        }
        segment.keys = keys;
        parsed.segments[parsed.count++] = segment;
        if (*segEnd == '\0') {
            break;
        }
        p = segEnd + 1;
    }
    *out = parsed;
    return true;
}

uint32_t walkKeysAt(const WalkScript& script, uint32_t frame) {
    uint32_t keys = 0;
    for (int32_t i = 0; i < script.count; i++) {
        const WalkSegment& s = script.segments[i];
        if (frame >= s.first && frame < s.end) {
            keys |= s.keys;
        }
    }
    return keys;
}

int32_t walkSegmentStartingAt(const WalkScript& script, uint32_t frame) {
    for (int32_t i = 0; i < script.count; i++) {
        if (script.segments[i].first == frame) {
            return i;
        }
    }
    return -1;
}

void applyWalk(const WalkScript& script, uint32_t frame, Input* input) {
    if (!input) {
        return;
    }
    const uint32_t keys = walkKeysAt(script, frame);
    for (int32_t k = 0; k < kKeyCount; k++) {
        input->down[k] = (keys & (1u << (uint32_t)k)) != 0;
    }
    input->mouseX = 0.0f;
    input->mouseY = 0.0f;
}

const char* walkKeyName(Key key) {
    if ((uint32_t)key >= (uint32_t)kKeyCount || !kKeyNames[key]) {
        return "?";
    }
    return kKeyNames[key];
}

int32_t formatWalkKeys(uint32_t keys, char* buffer, size_t size) {
    if (!buffer || size == 0) {
        return 0;
    }
    buffer[0] = '\0';
    size_t used = 0;
    for (int32_t k = 0; k < kKeyCount; k++) {
        if ((keys & (1u << (uint32_t)k)) == 0) {
            continue;
        }
        const int written = snprintf(buffer + used, size - used, "%s%s", used ? "+" : "",
                                     walkKeyName((Key)k));
        if (written < 0) {
            break;
        }
        used += (size_t)written;
        if (used >= size) {
            used = size - 1;
            break;
        }
    }
    return (int32_t)used;
}

} // namespace mdk

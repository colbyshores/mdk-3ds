// ============================================================================
// controls_3ds.cpp -- see controls_3ds.h. Cold path: parsed once at boot and
// written at most once; compiled by the 3DS build and by tests/scripts.
// ============================================================================

#include "platform/controls_3ds.h"

#include <stdio.h>
#include <string.h>

#include "platform/input_3ds.h"

namespace mdk {
namespace {

// The file's actions, in the file's order.
struct Action {
    int key;
    const char* name;
};
const Action kActions[] = {
    {kKeyForward, "forward"},
    {kKeyBack, "back"},
    {kKeyStrafeLeft, "strafe_left"},
    {kKeyStrafeRight, "strafe_right"},
    {kKeyJump, "jump"},
    {kKeyFire, "fire"},
    {kKeyItemPrevious, "item_previous"},
    {kKeyItemNext, "item_next"},
    {kKeySniper, "sniper"},
    {kKeyZoomIn, "zoom_in"},
    {kKeyZoomOut, "zoom_out"},
    {kKeyUseItem, "use_item"},
    {kKeyTurbo, "turbo"},
    {kKeyDown, "down"},
    {kControlRunToggle, "run_toggle"},
    {kControlRunHold, "run_hold"},
};
const size_t kActionCount = sizeof(kActions) / sizeof(kActions[0]);

// The button names, in the order a mask is spelled.
struct Button {
    uint32_t bit;
    const char* name;
};
const Button kButtons[] = {
    {kHidA, "A"},         {kHidB, "B"},          {kHidX, "X"},         {kHidY, "Y"},
    {kHidL, "L"},         {kHidR, "R"},          {kHidZL, "ZL"},       {kHidZR, "ZR"},
    {kHidStart, "START"}, {kHidSelect, "SELECT"}, {kHidDUp, "DUP"},    {kHidDDown, "DDOWN"},
    {kHidDLeft, "DLEFT"}, {kHidDRight, "DRIGHT"},
};
const size_t kButtonCount = sizeof(kButtons) / sizeof(kButtons[0]);

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

void trim(const char** begin, const char** end) {
    while (*begin < *end && isSpace(**begin)) {
        (*begin)++;
    }
    while (*end > *begin && isSpace((*end)[-1])) {
        (*end)--;
    }
}

// Case-insensitive: does [begin, end) spell `word`?
bool spells(const char* begin, const char* end, const char* word) {
    const size_t length = (size_t)(end - begin);
    if (strlen(word) != length) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        char c = begin[i];
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        char w = word[i];
        if (w >= 'a' && w <= 'z') {
            w = (char)(w - 'a' + 'A');
        }
        if (c != w) {
            return false;
        }
    }
    return true;
}

void note(ControlsReport* report, int32_t line, const char* begin, const char* end) {
    if (report->firstProblem[0] != '\0') {
        return;
    }
    snprintf(report->firstProblem, sizeof(report->firstProblem), "line %d: %.*s", (int)line,
             (int)(end - begin > 60 ? 60 : end - begin), begin);
}

} // namespace

ControlBindings defaultControlBindings() {
    ControlBindings bindings;
    bindings.buttons[kKeyForward] = kHidX;
    bindings.buttons[kKeyBack] = kHidB;
    bindings.buttons[kKeyStrafeLeft] = kHidY;
    bindings.buttons[kKeyStrafeRight] = kHidA;
    bindings.buttons[kKeyJump] = kHidL;
    // Fire is R alone since the run controls (owner 2026-11-27): ZR is the run
    // button.
    bindings.buttons[kKeyFire] = kHidR;
    bindings.buttons[kKeyItemPrevious] = kHidDLeft;
    bindings.buttons[kKeyItemNext] = kHidDRight;
    bindings.buttons[kKeySniper] = kHidDUp;
    bindings.buttons[kKeyUseItem] = kHidDDown;
    // THE RUN (owner 2026-11-27): ZL and SELECT toggle it (an original 3DS has
    // no ZL, so SELECT is its toggle), ZR held inverts it (input3dsRun). Turbo's
    // own hold is unbound: the toggle and the run button cover it.
    bindings.buttons[kKeyTurbo] = 0;
    bindings.buttons[kControlRunToggle] = kHidZL | kHidSelect;
    bindings.buttons[kControlRunHold] = kHidZR;
    // The scope's zoom (owner 2026-11-21): X in, B out -- the circle pad already
    // moves the scope's view, so forward/back have nothing to do in sniper mode,
    // and input3dsScopeButtons gives their shared buttons to the zoom there.
    bindings.buttons[kKeyZoomIn] = kHidX;
    bindings.buttons[kKeyZoomOut] = kHidB;
    // Noclip's descent (the C#'s Key.Down, gap package #21) is unbound too: the cheat
    // is the pause menu's, and the owner picks its button.
    bindings.buttons[kKeyDown] = 0;
    return bindings;
}

const char* controlActionName(int key) {
    for (size_t i = 0; i < kActionCount; i++) {
        if (kActions[i].key == key) {
            return kActions[i].name;
        }
    }
    return nullptr;
}

uint32_t controlButtonBit(const char* name, size_t length) {
    if (!name) {
        return 0;
    }
    for (size_t i = 0; i < kButtonCount; i++) {
        if (spells(name, name + length, kButtons[i].name)) {
            return kButtons[i].bit;
        }
    }
    return 0;
}

std::string formatControlButtons(uint32_t mask) {
    std::string out;
    for (size_t i = 0; i < kButtonCount; i++) {
        if ((mask & kButtons[i].bit) == 0) {
            continue;
        }
        if (!out.empty()) {
            out += '+';
        }
        out += kButtons[i].name;
    }
    return out.empty() ? std::string("none") : out;
}

bool parseControlButtons(const char* begin, const char* end, uint32_t* mask) {
    if (!begin || !end || !mask) {
        return false;
    }
    trim(&begin, &end);
    if (begin >= end) {
        return false;
    }
    if (spells(begin, end, "none")) {
        *mask = 0;
        return true;
    }
    uint32_t bits = 0;
    const char* part = begin;
    for (;;) {
        const char* plus = part;
        while (plus < end && *plus != '+') {
            plus++;
        }
        const char* a = part;
        const char* b = plus;
        trim(&a, &b);
        const uint32_t bit = controlButtonBit(a, (size_t)(b - a));
        if (bit == 0) {
            return false;   // an empty part ("R+") or a name that is no button
        }
        bits |= bit;
        if (plus >= end) {
            break;
        }
        part = plus + 1;
    }
    *mask = bits;
    return true;
}

void parseControls(const char* text, size_t size, ControlBindings* out, ControlsReport* report) {
    ControlsReport local;
    ControlsReport* r = report ? report : &local;
    if (!text || !out) {
        return;
    }
    const char* at = text;
    const char* const stop = text + size;
    int32_t line = 0;
    while (at < stop) {
        const char* lineEnd = at;
        while (lineEnd < stop && *lineEnd != '\n' && *lineEnd != '\0') {
            lineEnd++;
        }
        line++;
        const char* begin = at;
        const char* end = lineEnd;
        at = lineEnd < stop ? lineEnd + 1 : stop;
        if (lineEnd < stop && *lineEnd == '\0') {
            at = stop;   // a NUL ends the text
        }
        trim(&begin, &end);
        if (begin >= end || *begin == '#' || *begin == ';') {
            continue;
        }
        const char* equals = begin;
        while (equals < end && *equals != '=') {
            equals++;
        }
        const char* keyEnd = equals;
        const char* keyBegin = begin;
        trim(&keyBegin, &keyEnd);
        // The version line: a whole number, read before the actions.
        if (equals < end && spells(keyBegin, keyEnd, "version")) {
            const char* v = equals + 1;
            const char* vEnd = end;
            trim(&v, &vEnd);
            int32_t number = 0;
            bool digits = v < vEnd && vEnd - v <= 4;
            for (const char* c = v; digits && c < vEnd; c++) {
                digits = *c >= '0' && *c <= '9';
                number = number * 10 + (*c - '0');
            }
            if (digits && number >= 1) {
                r->version = number;
            } else {
                r->bad++;
                note(r, line, begin, end);
            }
            continue;
        }
        int key = -1;
        for (size_t i = 0; i < kActionCount && equals < end; i++) {
            if (spells(keyBegin, keyEnd, kActions[i].name)) {
                key = kActions[i].key;
                break;
            }
        }
        if (key < 0) {
            r->unknown++;
            note(r, line, begin, end);
            continue;
        }
        uint32_t mask = 0;
        if (!parseControlButtons(equals + 1, end, &mask)) {
            r->bad++;
            note(r, line, begin, end);
            continue;
        }
        out->buttons[key] = mask;
        r->applied++;
    }
}

namespace {

// One migrated key: when the file's value is exactly `was`, it becomes `now`.
struct Migration {
    int key;
    uint32_t was;
    uint32_t now;
};

void appendChange(std::string* out, int key, uint32_t was, uint32_t now) {
    if (!out->empty()) {
        *out += ' ';
    }
    *out += controlActionName(key);
    *out += ' ';
    *out += formatControlButtons(was);
    *out += "->";
    *out += formatControlButtons(now);
}

} // namespace

int32_t migrateControls(int32_t fromVersion, ControlBindings* bindings, std::string* changes,
                        std::string* conflicts) {
    if (changes) {
        changes->clear();
    }
    if (conflicts) {
        conflicts->clear();
    }
    if (!bindings) {
        return 0;
    }
    int32_t changed = 0;
    if (fromVersion < 2) {
        // Version 1's defaults that today's replace (controls_3ds.h). A value
        // that is not exactly one of them is the owner's and stays.
        const Migration kToVersion2[] = {
            {kKeyFire, kHidR | kHidZR, kHidR},
            {kKeyTurbo, kHidZL, 0},
            {kKeyZoomIn, 0, kHidX},
            {kKeyZoomOut, 0, kHidB},
        };
        for (const Migration& m : kToVersion2) {
            if (bindings->buttons[m.key] != m.was) {
                continue;
            }
            bindings->buttons[m.key] = m.now;
            changed++;
            if (changes) {
                appendChange(changes, m.key, m.was, m.now);
            }
        }
    }
    // The run's buttons shared with a game action: kept, and named.
    if (conflicts) {
        const int runs[2] = {kControlRunToggle, kControlRunHold};
        for (int r = 0; r < 2; r++) {
            const uint32_t run = bindings->buttons[runs[r]];
            for (int key = 0; key < kKeyCount; key++) {
                const uint32_t shared = bindings->buttons[key] & run;
                if (shared == 0) {
                    continue;
                }
                if (!conflicts->empty()) {
                    *conflicts += ", ";
                }
                *conflicts += controlActionName(key);
                *conflicts += '=';
                *conflicts += formatControlButtons(bindings->buttons[key]);
                *conflicts += " shares ";
                *conflicts += formatControlButtons(shared);
                *conflicts += " with ";
                *conflicts += controlActionName(runs[r]);
            }
        }
    }
    return changed;
}

std::string writeControls(const ControlBindings& bindings) {
    std::string out =
        "# MDK 3DS controls. One line per action: action=BUTTONS, the buttons\n"
        "# joined with '+' (any of them held holds the action), or none.\n"
        "# Buttons: A B X Y L R ZL ZR START SELECT DUP DDOWN DLEFT DRIGHT.\n"
        "# ZL and ZR exist on the New 3DS only.\n"
        "# run_toggle: each press flips Kurt's run (ZL, and SELECT for the\n"
        "# original 3DS). run_hold: held, it inverts the toggle -- runs when the\n"
        "# toggle is off, jogs when it is on. turbo: a plain hold-to-run (none).\n"
        "# The circle pad looks; a New 3DS's C-Stick walks (the pause menu's SWAP\n"
        "# STICKS swaps them). START opens the pause menu, whatever is bound.\n"
        "# sniper enters and leaves sniper mode (the circle pad turns the scope);\n"
        "# zoom_in and zoom_out zoom it (X and B, which do not walk there).\n"
        "# down sinks Kurt while the pause menu's NOCLIP cheat is on (unbound).\n"
        "# Delete this file to get the defaults back.\n";
    char version[32];
    snprintf(version, sizeof(version), "version=%d\n", (int)kControlsVersion);
    out += version;
    for (size_t i = 0; i < kActionCount; i++) {
        out += kActions[i].name;
        out += '=';
        out += formatControlButtons(bindings.buttons[kActions[i].key]);
        out += '\n';
    }
    return out;
}

std::string describeControls(const ControlBindings& bindings) {
    std::string out;
    for (size_t i = 0; i < kActionCount; i++) {
        if (!out.empty()) {
            out += ' ';
        }
        out += kActions[i].name;
        out += '=';
        out += formatControlButtons(bindings.buttons[kActions[i].key]);
    }
    return out;
}

} // namespace mdk

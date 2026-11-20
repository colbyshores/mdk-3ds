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
    bindings.buttons[kKeyFire] = kHidR | kHidZR;
    bindings.buttons[kKeyItemPrevious] = kHidDLeft;
    bindings.buttons[kKeyItemNext] = kHidDRight;
    bindings.buttons[kKeySniper] = kHidDUp;
    bindings.buttons[kKeyUseItem] = kHidDDown;
    bindings.buttons[kKeyTurbo] = kHidZL;
    // The scope's zoom keys are unbound until the owner picks their buttons (gap
    // package #5): zoom_in and zoom_out are written as `none`.
    bindings.buttons[kKeyZoomIn] = 0;
    bindings.buttons[kKeyZoomOut] = 0;
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

std::string writeControls(const ControlBindings& bindings) {
    std::string out =
        "# MDK 3DS controls. One line per action: action=BUTTONS, the buttons\n"
        "# joined with '+' (any of them held holds the action), or none.\n"
        "# Buttons: A B X Y L R ZL ZR START SELECT DUP DDOWN DLEFT DRIGHT.\n"
        "# ZL and ZR exist on the New 3DS only: turbo is on ZL (the owner left it\n"
        "# unassigned), so an original 3DS has no turbo unless you bind it here.\n"
        "# The circle pad looks. START opens the pause menu and SELECT the\n"
        "# effects menu, whatever is bound.\n"
        "# sniper enters and leaves sniper mode; the scope's view and overlay are\n"
        "# not drawn yet. zoom_in and zoom_out zoom the scope (unbound by default).\n"
        "# Delete this file to get the defaults back.\n";
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

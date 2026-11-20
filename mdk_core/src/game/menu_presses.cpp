// menu_presses.cpp -- see menu_presses.h. C# Flow/TestPresses.cs Parse and Apply.

#include "game/menu_presses.h"

#include <stdlib.h>

namespace mdk {
namespace {

const char* const kKeys[] = {"Menu.Up", "Menu.Down", "Menu.Left", "Menu.Right", "Menu.Accept",
                             "Menu.Back"};

} // namespace

bool MenuPresses::parse(const char* text, std::string* error) {
    presses.clear();
    const std::string all(text ? text : "");
    size_t at = 0;
    while (at < all.size()) {
        size_t comma = all.find(',', at);
        if (comma == std::string::npos) {
            comma = all.size();
        }
        const std::string item = all.substr(at, comma - at);
        at = comma + 1;
        if (item.empty()) {
            continue;
        }
        const size_t sign = item.find('@');
        const std::string name = item.substr(0, sign);
        Press press;
        press.key = -1;
        for (int32_t k = 0; k < 6; k++) {
            if (name == kKeys[k]) {
                press.key = k;
            }
        }
        if (press.key < 0) {
            *error = "unknown key " + name + " (Menu.Up/Down/Left/Right/Accept/Back)";
            return false;
        }
        // C# `p.Length > 1 ? float.Parse(p[1]) : 0f`.
        if (sign != std::string::npos) {
            const char* begin = item.c_str() + sign + 1;
            char* end = nullptr;
            press.time = strtof(begin, &end);
            if (end == begin || *end != '\0' || press.time < 0.0f) {
                *error = "bad time in " + item;
                return false;
            }
        }
        presses.push_back(press);
    }
    return true;
}

void MenuPresses::apply(float time, menu::Keys* keys) {
    for (size_t i = 0; i < presses.size(); i++) {
        Press& press = presses[i];
        if (press.done || press.time > time) {
            continue;
        }
        press.done = true;
        keys->any = true;
        switch (press.key) {
            case 0: keys->up = true; break;
            case 1: keys->down = true; break;
            case 2: keys->left = true; break;
            case 3: keys->right = true; break;
            case 4: keys->accept = true; break;
            default: keys->back = true; break;
        }
    }
}

} // namespace mdk

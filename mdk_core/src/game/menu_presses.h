#pragma once

// ============================================================================
// menu_presses.h -- menu keys pressed at given game times, for tests (the C#'s
// src/Mdk.Game/Flow/TestPresses.cs, its menu half): "Menu.<Key>@<seconds>",
// comma separated, each pressed once on the first frame whose game time
// reaches it.
//
//   Menu.Down@0.5,Menu.Accept@1
//
// The Linux frontend takes it as --press=..., the 3DS harness as
// `make AUTOQUIT=.. MENU=...` (no command line on a console). The keys are the
// C#'s MenuKey names: Up, Down, Left, Right, Accept, Back. The C#'s game keys
// (no "Menu." prefix) are not taken: no test of this port presses one.
//
// Cold code (test runs only). No virtuals, no templates, no exceptions.
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

#include "game/menu_items.h"

namespace mdk {

struct MenuPresses {
    struct Press {
        int32_t key = 0;   // 0 Up, 1 Down, 2 Left, 3 Right, 4 Accept, 5 Back
        float time = 0.0f;
        bool done = false;
    };
    std::vector<Press> presses;

    // False (with `error` set) for a key that is not a menu key or a bad time;
    // an empty text is no presses.
    bool parse(const char* text, std::string* error);
    // The presses due at `time` added to `keys` (each once; `any` too).
    void apply(float time, menu::Keys* keys);
};

} // namespace mdk

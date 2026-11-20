#pragma once

// ============================================================================
// enhanced_config.h -- the Enhanced Mode toggles as a config file
// (SPEC.md 7.9): `config.ini` on the SD card (sdmc:/3ds/mdk/config.ini).
//
// The format is deliberately tiny: one `key=value` per line, the field
// names from mdk::EnhancedToggles; switches 1/0 (true/false, on/off are also
// read), the shadow map's two numbers and the look dials whole numbers (the
// dials clamped into kLookDials' ranges). Unknown keys and malformed lines are ignored, and a
// missing key keeps the value already in the struct -- so a partial or
// hand-edited file can never turn a feature off by accident, and the
// file the game writes always carries every key in a fixed order.
// The frontends own the file I/O; these two functions own the text.
// ============================================================================

#include <string>

#include "render/render_types.h"

namespace mdk {

// Apply the config text over `toggles`. False only when `text` is null.
bool parseEnhancedConfig(const char* text, EnhancedToggles* toggles);

// The full file text for `toggles` (every key, fixed order).
std::string writeEnhancedConfig(const EnhancedToggles& toggles);

// THE LOOK DIALS (render_types.h): one row per dial, its config key, its
// field, and the range and step the pause menu's slider moves it through, in
// percent of today's value. The parser clamps a hand-edited value into the
// same range, so the file and the menu can never disagree. 100 is the shipped
// look and lies on every dial's step grid.
struct LookDial {
    const char* key;
    int32_t EnhancedToggles::*field;
    int32_t lo;
    int32_t hi;
    int32_t step;
};
const int32_t kLookDialCount = 5;
extern const LookDial kLookDials[kLookDialCount];

// A dial's value clamped into its range.
int32_t clampLookDial(const LookDial& dial, int32_t value);

} // namespace mdk

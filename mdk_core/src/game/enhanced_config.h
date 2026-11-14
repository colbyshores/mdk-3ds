#pragma once

// ============================================================================
// enhanced_config.h -- the Enhanced Mode toggles as a config file
// (SPEC.md 7.9): `config.ini` on the SD card (sdmc:/3ds/mdk/config.ini).
//
// The format is deliberately tiny: one `key=value` per line, the five
// toggle names from mdk::EnhancedToggles, values 1/0 (true/false, on/off
// are also read). Unknown keys and malformed lines are ignored, and a
// missing key keeps the value already in the struct -- so a partial or
// hand-edited file can never turn a feature off by accident, and the
// file the game writes always carries all five keys in a fixed order.
// The frontends own the file I/O; these two functions own the text.
// ============================================================================

#include <string>

#include "render/render_types.h"

namespace mdk {

// Apply the config text over `toggles`. False only when `text` is null.
bool parseEnhancedConfig(const char* text, EnhancedToggles* toggles);

// The full file text for `toggles` (all five keys, fixed order).
std::string writeEnhancedConfig(const EnhancedToggles& toggles);

} // namespace mdk

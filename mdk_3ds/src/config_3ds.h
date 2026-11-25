#pragma once

// ============================================================================
// config_3ds.h -- sdmc:/3ds/mdk/config.ini (the Enhanced Mode toggles and the
// game's settings, read at boot and written whenever either changes) and
// sdmc:/3ds/mdk/controls.ini (read at boot).
// ============================================================================

#include "game/enhanced_config.h"
#include "game/game_settings.h"
#include "platform/controls_3ds.h"

namespace mdk {

// The game's settings (the volumes, the music filter, the difficulty, gore).
extern GameSettings g_gameSettings;
// The toggles config.ini is written with (main's own), for saveConfigNow.
extern const EnhancedToggles* g_toggles;

// Write config.ini: `toggles`, then g_gameSettings. False when it could not be written.
bool saveEnhancedConfig(const EnhancedToggles& toggles);
// config.ini with the current toggles and settings (the options' save).
bool saveConfigNow();
// Read config.ini into `toggles` and g_gameSettings at boot, or write the
// defaults when there is none (or it does not parse). Logged.
void loadConfig3ds(EnhancedToggles* toggles);
// Read controls.ini into `controls` at boot, or write the defaults. Logged.
void loadControls3ds(ControlBindings* controls);
// A pause-menu toggle change: config.ini written, the toggles logged.
void saveToggles3ds(const EnhancedToggles& toggles);
// The options pages' hooks: `user` is the frame's mixer pointer (SoundMixer**).
void optionsChanged3ds(void* user);
void printLine3ds(void* user, const char* line);

} // namespace mdk

// ============================================================================
// config_3ds.cpp -- see config_3ds.h.
// ============================================================================

#include "config_3ds.h"

#include <stdio.h>

#include <string>

#include "audio/sound_mixer.h"
#include "platform/controls_3ds.h"
#include "platform/log_3ds.h"

namespace mdk {

// The Enhanced Mode toggles' persistence (SPEC.md 7.9): the whole
// config.ini, rewritten whenever a toggle changes. The directory is
// the log's (log_3ds.cpp makes sdmc:/3ds/mdk before this can run).
// THE GAME'S SETTINGS (C# Flow/Settings.cs, package #20a): the volumes, the music
// filter, the difficulty and gore, kept in the same config.ini after the toggles
// (game/game_settings.h's formatConsole; each parser skips the other's keys).
// File-scope because the menus' option changes save the file from the screens
// (menu_screens_3ds.cpp) as well as from the level's START menu.
mdk::GameSettings g_gameSettings;
// The toggles the file is written with (main's own), for saveConfigNow.
const mdk::EnhancedToggles* g_toggles = nullptr;

bool saveEnhancedConfig(const mdk::EnhancedToggles& toggles) {
    FILE* config = fopen("sdmc:/3ds/mdk/config.ini", "wb");
    if (!config) {
        return false;
    }
    const std::string text = mdk::writeEnhancedConfig(toggles) + g_gameSettings.formatConsole();
    const bool written = fwrite(text.c_str(), 1, text.size(), config) == text.size();
    fclose(config);
    return written;
}

// config.ini with the current toggles and settings (the options' save, C#
// Ui.ApplySettings).
bool saveConfigNow() {
    return g_toggles != nullptr && saveEnhancedConfig(*g_toggles);
}

// THE CONFIG AT BOOT: a missing file is written with the defaults (all on) so
// the knobs are discoverable; a file that does not parse keeps the defaults
// too, and says so in the boot log.
void loadConfig3ds(EnhancedToggles* toggles) {
    // Static, and 2 KB: the toggles' lines and the game's (package #20a).
    static char configText[2048];
    bool read = false;
    if (FILE* config = fopen("sdmc:/3ds/mdk/config.ini", "rb")) {
        const size_t got = fread(configText, 1, sizeof(configText) - 1, config);
        configText[got] = '\0';
        fclose(config);
        read = true;
    }
    if (read) {
        mdk::g_gameSettings.parseOver(configText);
    }
    if (read && mdk::parseEnhancedConfig(configText, toggles)) {
        mdk::log3dsPrintf("config: sdmc:/3ds/mdk/config.ini loaded (game: master=%d music=%d "
                          "effects=%d filter=%d difficulty=%s gore=%d)",
                          (int)mdk::g_gameSettings.masterVolume, (int)mdk::g_gameSettings.musicVolume,
                          (int)mdk::g_gameSettings.effectsVolume, (int)mdk::g_gameSettings.musicFilter,
                          mdk::GameSettings::difficultyName(mdk::g_gameSettings.difficulty),
                          (int)mdk::g_gameSettings.gore);
    } else {
        if (read) {
            mdk::log3dsPrintf("config: config.ini did not parse; defaults kept");
        }
        if (mdk::saveEnhancedConfig(*toggles)) {
            mdk::log3dsPrintf("config: wrote default sdmc:/3ds/mdk/config.ini");
        } else {
            mdk::log3dsPrintf("config: could not write sdmc:/3ds/mdk/config.ini");
        }
    }
}

// THE CONTROLS AT BOOT (sdmc:/3ds/mdk/controls.ini; platform/controls_3ds.h has
// the format and the defaults): a missing file is the defaults, written out once
// so the owner can edit it; a bad line keeps that action's default and an
// unknown key is ignored, both logged; the resolved table is one line.
void loadControls3ds(ControlBindings* controls) {
    static char controlsText[4096];
    size_t got = 0;
    bool read = false;
    if (FILE* file = fopen("sdmc:/3ds/mdk/controls.ini", "rb")) {
        got = fread(controlsText, 1, sizeof(controlsText) - 1, file);
        controlsText[got] = '\0';
        fclose(file);
        read = true;
    }
    if (read) {
        mdk::ControlsReport report;
        mdk::parseControls(controlsText, got, controls, &report);
        mdk::log3dsPrintf("controls: sdmc:/3ds/mdk/controls.ini loaded: %d actions set, %d "
                          "bad (default kept), %d unknown (ignored)%s%s",
                          (int)report.applied, (int)report.bad, (int)report.unknown,
                          report.firstProblem[0] ? "; first: " : "", report.firstProblem);
    } else {
        const std::string text = mdk::writeControls(*controls);
        bool written = false;
        if (FILE* file = fopen("sdmc:/3ds/mdk/controls.ini", "wb")) {
            written = fwrite(text.c_str(), 1, text.size(), file) == text.size();
            written = fclose(file) == 0 && written;
        }
        mdk::log3dsPrintf(written ? "controls: wrote default sdmc:/3ds/mdk/controls.ini"
                                  : "controls: could not write sdmc:/3ds/mdk/controls.ini; "
                                    "defaults in use");
    }
    mdk::log3dsPrintf("controls: %s", mdk::describeControls(*controls).c_str());
    if (controls->buttons[mdk::kKeyZoomIn] == 0 || controls->buttons[mdk::kKeyZoomOut] == 0) {
        mdk::log3dsPrintf("controls: the scope's zoom_in/zoom_out are unbound; sniper mode "
                          "stays at its entry zoom");
    }
}

// A pause-menu change: config.ini written at once, and the toggles in one line.
void saveToggles3ds(const EnhancedToggles& toggles) {
    const bool saved = mdk::saveEnhancedConfig(toggles);
    mdk::log3dsPrintf("menu: shadows=%d lights=%d haze=%d mirrors=%d "
                      "glow=%d filtering=%d mipmaps=%d hd=%d lighting=%d smoothing=%d "
                      "cap30=%d hazeDensity=%d glowStrength=%d shadowDarkness=%d "
                      "exposure=%d tintSaturation=%d shadowMapSize=%d "
                      "shadowMapRefresh=%d (saved=%d)",
                      (int)toggles.enableSkyShadows,
                      (int)toggles.enableFragmentLights,
                      (int)toggles.enableHaze,
                      (int)toggles.enableMirrors,
                      (int)toggles.enableGlowBillboards,
                      (int)toggles.enableFiltering,
                      (int)toggles.enableMipmaps,
                      (int)toggles.enableHdTextures,
                      (int)toggles.enableLighting,
                      (int)toggles.enableEdgeSmoothing,
                      (int)toggles.cap30Fps,
                      (int)toggles.hazeDensity,
                      (int)toggles.glowStrength,
                      (int)toggles.shadowDarkness,
                      (int)toggles.exposure,
                      (int)toggles.tintSaturation,
                      (int)toggles.shadowMapSize,
                      (int)toggles.shadowMapRefresh, (int)saved);
}

// The pause menu's options pages (pause_menu.h's MenuItems hooks): a change
// applies the volumes to the live mixer (`user` is main's `mix`, a SoundMixer**)
// and writes config.ini; the pages' own lines go to the log.
void optionsChanged3ds(void* user) {
    mdk::audio::SoundMixer* const* live =
        static_cast<mdk::audio::SoundMixer* const*>(user);
    if (*live) {
        mdk::g_gameSettings.applyAudio(&(*live)->device());
    }
    const bool saved = mdk::saveConfigNow();
    mdk::log3dsPrintf("menu: options changed: master=%d music=%d "
                      "effects=%d filter=%d difficulty=%s gore=%d "
                      "(saved=%d)",
                      (int)mdk::g_gameSettings.masterVolume,
                      (int)mdk::g_gameSettings.musicVolume,
                      (int)mdk::g_gameSettings.effectsVolume,
                      (int)mdk::g_gameSettings.musicFilter,
                      mdk::GameSettings::difficultyName(
                          mdk::g_gameSettings.difficulty),
                      (int)mdk::g_gameSettings.gore, (int)saved);
}

void printLine3ds(void*, const char* line) {
    mdk::log3dsPrintf("%s", line);
}

} // namespace mdk

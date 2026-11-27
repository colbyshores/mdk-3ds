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
// It starts from GameSettings::console(): the C# defaults with the 3DS's own
// stereo separation (the parallax barrier's; the PC keeps the C#'s 0.25).
mdk::GameSettings g_gameSettings = mdk::GameSettings::console();
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
        // A hand-edited convergence nearer than 1 unit (the C#'s MinConvergence;
        // the camera's near plane is 0.5) would slide each eye's picture by up to
        // hundreds of pixels: 1 is the nearest the pair converges. The parser
        // (the C#'s, shared) only clamps it to [0, 4000]; 0 stays FAR.
        float& convergence = mdk::g_gameSettings.stereoConvergence;
        if (convergence > 0.0f && convergence < 1.0f) {
            convergence = 1.0f;
        }
    }
    if (read && mdk::parseEnhancedConfig(configText, toggles)) {
        mdk::log3dsPrintf("config: sdmc:/3ds/mdk/config.ini loaded (game: master=%d music=%d "
                          "effects=%d filter=%d difficulty=%s gore=%d stereo=%.2f/%.1f)",
                          (int)mdk::g_gameSettings.masterVolume, (int)mdk::g_gameSettings.musicVolume,
                          (int)mdk::g_gameSettings.effectsVolume, (int)mdk::g_gameSettings.musicFilter,
                          mdk::GameSettings::difficultyName(mdk::g_gameSettings.difficulty),
                          (int)mdk::g_gameSettings.gore,
                          (double)mdk::g_gameSettings.stereoSeparation,
                          (double)mdk::g_gameSettings.stereoConvergence);
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
//
// AN OLDER FILE (no `version=2` line: written before the run controls) is
// migrated (controls_3ds.h THE MIGRATION): each old default becomes today's and
// every change is logged. The migrated table is written to controls.ini.new
// first; only when that is whole does the old file move, by rename and with its
// bytes untouched, to controls.v1.ini, and the new one take its place -- the
// save games' order (game/save_games.cpp: the 3DS FS service will not rename
// onto an existing file). Any failure leaves the owner's file where it was (or
// puts it back) and the migration is applied in memory only, and logged again,
// on the next boot. A file too long for the buffer, or one that did not read,
// is never rewritten.
namespace {

const char* const kControlsPath = "sdmc:/3ds/mdk/controls.ini";
const char* const kControlsNew = "sdmc:/3ds/mdk/controls.ini.new";
const char* const kControlsOld = "sdmc:/3ds/mdk/controls.v1.ini";

bool writeWhole(const char* path, const char* text, size_t size) {
    FILE* file = fopen(path, "wb");
    if (!file) {
        return false;
    }
    bool written = fwrite(text, 1, size, file) == size;
    written = fclose(file) == 0 && written;
    return written;
}

// The migrated table into place, the old file kept as controls.v1.ini.
bool replaceControls(const std::string& text) {
    if (!writeWhole(kControlsNew, text.c_str(), text.size())) {
        ::remove(kControlsNew);
        return false;
    }
    ::remove(kControlsOld);   // an earlier migration's copy: this file is newer
    if (rename(kControlsPath, kControlsOld) != 0) {
        ::remove(kControlsNew);
        return false;
    }
    if (rename(kControlsNew, kControlsPath) != 0) {
        rename(kControlsOld, kControlsPath);   // the owner's file back where it was
        ::remove(kControlsNew);
        return false;
    }
    return true;
}

} // namespace

void loadControls3ds(ControlBindings* controls) {
    static char controlsText[4096];
    size_t got = 0;
    bool read = false;
    bool readFailed = false;
    if (FILE* file = fopen(kControlsPath, "rb")) {
        got = fread(controlsText, 1, sizeof(controlsText) - 1, file);
        controlsText[got] = '\0';
        readFailed = ferror(file) != 0;
        fclose(file);
        read = true;
    }
    // Filled to the last byte: there may be more of it than was read.
    const bool whole = read && !readFailed && got < sizeof(controlsText) - 1;
    if (read && readFailed) {
        // Never overwritten on a failed read: the defaults, and the file alone.
        mdk::log3dsPrintf("controls: sdmc:/3ds/mdk/controls.ini did not read; defaults in use, "
                          "the file left alone");
    } else if (read) {
        mdk::ControlsReport report;
        mdk::parseControls(controlsText, got, controls, &report);
        mdk::log3dsPrintf("controls: sdmc:/3ds/mdk/controls.ini loaded (version %d): %d actions "
                          "set, %d bad (default kept), %d unknown (ignored)%s%s",
                          (int)report.version, (int)report.applied, (int)report.bad,
                          (int)report.unknown, report.firstProblem[0] ? "; first: " : "",
                          report.firstProblem);
        if (report.version < mdk::kControlsVersion) {
            std::string changes;
            const int32_t changed = mdk::migrateControls(report.version, controls, &changes, nullptr);
            mdk::log3dsPrintf("controls: migrated version %d -> %d: %s", (int)report.version,
                              (int)mdk::kControlsVersion,
                              changed > 0 ? changes.c_str() : "no old default to replace");
            const bool replaced = whole && replaceControls(mdk::writeControls(*controls));
            if (replaced) {
                mdk::log3dsPrintf("controls: the old file kept as controls.v1.ini; controls.ini "
                                  "rewritten as version 2");
            } else {
                mdk::log3dsPrintf("controls: controls.ini left as it was (%s); migrated in "
                                  "memory only",
                                  whole ? "could not write the new one" : "4 KB or longer");
            }
        } else if (report.version > mdk::kControlsVersion) {
            mdk::log3dsPrintf("controls: the file says version %d, newer than this build's %d; "
                              "read as it is", (int)report.version, (int)mdk::kControlsVersion);
        }
    } else {
        const std::string text = mdk::writeControls(*controls);
        const bool written = writeWhole(kControlsPath, text.c_str(), text.size());
        mdk::log3dsPrintf(written ? "controls: wrote default sdmc:/3ds/mdk/controls.ini"
                                  : "controls: could not write sdmc:/3ds/mdk/controls.ini; "
                                    "defaults in use");
    }
    mdk::log3dsPrintf("controls: %s", mdk::describeControls(*controls).c_str());
    // The run's buttons shared with a game action (a customised file): kept, and said.
    std::string conflicts;
    mdk::migrateControls(mdk::kControlsVersion, controls, nullptr, &conflicts);
    if (!conflicts.empty()) {
        mdk::log3dsPrintf("controls: kept as written, but %s: both act on that button",
                          conflicts.c_str());
    }
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
                      "shadowMapRefresh=%d stereo=%.2f/%.1f (saved=%d)",
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
                      (int)toggles.shadowMapRefresh,
                      (double)mdk::g_gameSettings.stereoSeparation,
                      (double)mdk::g_gameSettings.stereoConvergence, (int)saved);
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

#pragma once

// ============================================================================
// menu_screens.h -- the Linux frontend's screens between levels (package #20a):
// the main menu (game/main_menu.h) and the briefing (game/briefing.h), and the
// settings file they share with the pause menu.
//
// The game starts here unless --level names a level (the C#'s `--level`: the
// level at once; with no start option the C#'s Start.Menu). The flow decides
// what follows each screen (game/game_flow.h); the screens this port still
// lacks after a briefing (the fall, #17b) are skipped the C#'s way
// (GameFlow::skipUnported), and the function returns when the flow names a
// level, or when the run ends.
//
// THE SETTINGS FILE is the C#'s: settings.cfg in the user folder
// ($MDK_USER_DIR, the C#'s override; ./ otherwise, where this frontend's
// config.ini and mods/ already live), read at start, written whenever an option
// changes (C# Ui.ApplySettings: Apply, then Save).
//
// Tests drive it headless with --press (the C#'s TestPresses spelling,
// `--press=Menu.Down@0.5,Menu.Accept@1`: menu keys at game times) and capture
// a frame with --capture-frame, as a level's.
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

#include "game/game_flow.h"
#include "game/game_settings.h"
#include "game/menu_presses.h"
#include "platform/window_sdl.h"
#include "render/render.h"

namespace mdk {

// The settings file's path (see the header) and the settings themselves.
std::string settingsPath();
GameSettings loadSettings(const std::string& path);
// C# Settings.Save: the file written; false (and a line on stderr) when not.
bool saveSettings(const GameSettings& settings, const std::string& path);

// What the screens share with the level loop.
struct FrontScreens {
    MdkRenderer renderer = nullptr;
    Window* window = nullptr;
    bool headless = false;
    bool fixedStep = false;
    flow::GameFlow* flow = nullptr;
    // The saves the main menu lists, peeks and loads (package #19's
    // flow::menuListSaves/menuPeekSave/menuLoadSave); null: none, as the C#'s
    // menu on an empty user folder.
    const flow::SaveGames* saves = nullptr;
    GameSettings* settings = nullptr;
    std::string settingsFile;
    MenuPresses* presses = nullptr;
    // The run's game time (the presses' clock) and frame count, and the
    // capture/frame budget, shared with the level loop.
    float* time = nullptr;
    uint32_t* frame = nullptr;
    int maxFrames = -1;
    int captureFrame = 0;
    const char* capturePath = nullptr;
    // Set when the run ends here: why.
    const char* exitReason = nullptr;
};

// Runs the Menu and the Briefing (whichever the flow shows) until the flow
// names a Level (returned) or the run ends (Screen::None: Quit, the window
// closed, the frame budget, the capture taken).
flow::Screen runFrontScreens(FrontScreens* front);

} // namespace mdk

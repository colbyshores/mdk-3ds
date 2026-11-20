#pragma once

// ============================================================================
// menu_screens_3ds.h -- the 3DS frontend's screens between levels (package
// #20a): the main menu (game/main_menu.h) and the briefing (game/briefing.h).
//
// THE ENTRY SCREEN. A release build boots into the main menu, as the C#'s game
// does; a harness build with LEVEL=<n> still boots straight into that level
// (every existing hardware and emulator run names one), and a harness build
// without LEVEL= boots into the menu, which `MENU=` (game/menu_presses.h's
// spelling) can drive: `make AUTOQUIT=90 MENU=Menu.Accept@2,Menu.Back@4,
// Menu.Accept@5` is New Game, the briefing skipped, the level.
//
// THE CONTROLS are the ones the START menu already uses: the D-pad moves (and
// steps an option left/right), A or START chooses, B goes back (the C#'s Esc:
// "Really Quit?" on the main page, up a page in the options). The briefing
// takes any button, B finishes its typing, and Fire or Jump held runs it fast.
//
// DRAWN through the canvas and MDKFONT.FTI's FONTBIG/FONTSML -- the fonts the
// START menu and the HUD already use -- over MDKOPT (the C#'s background when
// MDK12.FLC is missing; the video is #20c's). On the 400 x 240 top screen the
// canvas is exactly the original's 600 x 360, so the layout is the C#'s at
// scale 1 (game/screen_view.h).
//
// The lifecycle rules are the level loop's: aptMainLoop every frame, the frame
// clock rebased on wake (AGENTS.md P1), one HID scan a frame, every frame a
// begin/end pair. What the screens load goes into one renderer scope, released
// before the level loads (the level's own teardown rule).
// ============================================================================

#include <stdint.h>

#include "game/game_flow.h"
#include "game/game_settings.h"
#include "render/render.h"

namespace mdk {

struct ControlBindings;

struct FrontScreens3ds {
    MdkRenderer renderer = nullptr;
    flow::GameFlow* flow = nullptr;
    // The saves the main menu lists, peeks and loads (package #19's
    // flow::menuListSaves/menuPeekSave/menuLoadSave); null: none, as the C#'s
    // menu on an empty user folder.
    const flow::SaveGames* saves = nullptr;
    GameSettings* settings = nullptr;
    // Writes config.ini (the toggles and the game's lines); the options call it
    // on every change (C# Ui.ApplySettings).
    bool (*save)() = nullptr;
    const ControlBindings* controls = nullptr;
    // The frame clock (main_3ds.cpp's nowSeconds) and its wake flag (P1).
    double (*now)() = nullptr;
    volatile bool* resumePending = nullptr;
    // The run's frame count, shared with the level loop (CAPTURE=, the budget).
    uint32_t* frame = nullptr;
    uint32_t frameBudget = 0;
    // Set when the run ends here: why.
    const char* exitReason = nullptr;
};

// Runs the Menu and the Briefing (whichever the flow shows) until the flow
// names a Level (returned) or the run ends (Screen::None: Quit, aptMainLoop,
// the harness deadline, the frame budget).
flow::Screen runFrontScreens3ds(FrontScreens3ds* front);

} // namespace mdk

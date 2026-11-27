#pragma once

// ============================================================================
// menu_screens_3ds.h -- the 3DS frontend's screens between levels (package
// #20a): the main menu (game/main_menu.h), the briefing and (package #20b) the
// statistics after a level (game/stats_screen.h: the intermission, the
// debriefing, the Score-O-matic and its 3D heads, the save prompt, the next
// briefing, with STATS.BNI's sounds). The statistics take any button to move
// on, B to skip, Fire or Jump held to run fast; the save prompt the D-pad, A
// and B.
//
// THE ENTRY SCREEN. The game boots into the main menu, as the C#'s game does.
//
// THE CONTROLS are the ones the START menu already uses: the D-pad moves (and
// steps an option left/right), A or START chooses, B goes back (the C#'s Esc:
// "Really Quit?" on the main page, up a page in the options). The briefing
// takes any button, B finishes its typing, and Fire or Jump held runs it fast.
//
// THE FALL (package #17b, fall/fall_screen.h) is shown here too, after a
// briefing and after a level's statistics: the circle
// pad, the D-pad and the movement actions of controls.ini (forward/back,
// strafe/turn left/right) steer, START skips it (the C#'s Esc). It is drawn the lean way (fall_view.h), its sounds
// on the DSP path (audio_dsp_3ds.h, opened for its bank and closed before the
// level opens its own), and everything it made is released when it ends.
//
// DRAWN through the canvas and MDKFONT.FTI's FONTBIG/FONTSML -- the fonts the
// START menu and the HUD already use -- over MDK12.FLC (package #20c: decoded on
// the main thread, ~11 us a frame on the host, and uploaded as a streamed RGB565
// picture at a frame boundary, render.h), MDKOPT before its first frame; the
// INTRO1A splash first at boot, after a death and after the end movies (any
// button skips it). The end movies skip on B, START or A. On the 400 x 240 top screen the
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
struct EnhancedToggles;
struct LoadingScreen;

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
    // The Enhanced Mode toggles (package #20c reads movieHalfRate); null: off.
    const EnhancedToggles* toggles = nullptr;
    // The frame clock (main_3ds.cpp's nowSeconds) and its wake flag (P1).
    double (*now)() = nullptr;
    volatile bool* resumePending = nullptr;
    // The run's frame count, shared with the level loop (and so with the run's
    // own end, platform/run_end_3ds.h).
    uint32_t* frame = nullptr;
    // Set when the run ends here: why.
    const char* exitReason = nullptr;
};

// Runs the Menu, the Briefing, the Statistics, the Fall, the EndMovie and the
// SavePrompt after LEVEL8 (whichever the flow shows) until the flow names a Level (returned) or the run
// ends (Screen::None: Quit, aptMainLoop, the frame budget).
flow::Screen runFrontScreens3ds(FrontScreens3ds* front);

// C# Play's LoadingScreen.Show (package #20c, game/loading_screen.h): LEVEL<level>'s
// loading screen loaded into `loading` and drawn once, bar empty, before the level
// loads. The caller keeps it through the load, draws it again as the load
// advances (drawLoadingScreen3ds) and frees it before play (LoadingScreen::release).
// Not game frames (the run's frame count does not move).
void showLoadingScreen3ds(MdkRenderer renderer, int32_t level, uint32_t frameIndex,
                          LoadingScreen* loading);
// One frame of the loading screen with the bar at `progress` (0-1).
void drawLoadingScreen3ds(MdkRenderer renderer, const LoadingScreen& loading, float progress,
                          uint32_t frameIndex);

} // namespace mdk

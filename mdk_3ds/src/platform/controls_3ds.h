#pragma once

// ============================================================================
// controls_3ds.h -- the 3DS button bindings, held in sdmc:/3ds/mdk/controls.ini.
//
// WHY A TABLE AND A FILE. The owner set new default controls (2026-11-19) and
// asked for them to live in a file, so a button-mapping screen can be built on
// it later: the game's actions (game/input.h Key) each name the 3DS buttons that
// hold them, the mapping (input_3ds.cpp, input3dsApply) reads THIS table, and
// the table is read from, and its defaults written to, controls.ini.
//
// THE DEFAULTS (owner, 2026-11-19):
//
//   forward        X              back           B
//   strafe_left    Y              strafe_right   A
//   jump           L              fire           R + ZR (ZR: New 3DS only)
//   item_previous  D-pad left     item_next      D-pad right
//   sniper         D-pad up       use_item       D-pad down
//   turbo          ZL (New 3DS only: the owner did not assign it, and an
//                  original 3DS has no ZL, so there turbo is unbound unless
//                  the file binds it to something else)
//   look           the circle pad (not a button; not in the file)
//   START          the pause overlay; SELECT the effects menu (the frontend's
//                  own buttons: main_3ds.cpp reads them before the game does,
//                  and while a menu is open the game is paused and these
//                  bindings are not consulted)
//
//   zoom_in, zoom_out  unbound (the scope's zoom; the owner picks the buttons)
//
// SNIPER enters and leaves sniper mode (Player's sniper step, gap package #5):
// Kurt stands, looks and zooms through the scope and fires its clip; the scope's
// view and overlay are drawn (package #7, game/sniper_view.h). The C# pad zooms
// on the d-pad's up and down (GamepadMap.cs), which this layout gives to sniper
// and use_item, so the zoom stays on zoom_in/zoom_out until the owner binds them
// (the frontend logs once while they are unbound).
//
// THE FILE. key=value lines, as config.ini (game/enhanced_config.cpp): '#' or
// ';' starts a comment, blank lines pass, spaces around the key and value are
// ignored. One line per action; the value is a '+'-separated list of button
// names -- A B X Y L R ZL ZR START SELECT DUP DDOWN DLEFT DRIGHT, any case --
// or `none` for an unbound action, e.g. `fire=R+ZR`. Any of the buttons held
// holds the action. A missing action keeps its default; a line whose value does
// not parse keeps that action's default and is reported; an unknown key is
// ignored and reported. A missing file is the defaults, and the frontend writes
// them out once (cold, at boot) so the owner can edit the file.
//
// NO LIBCTRU HERE: the bits are input_3ds.h's mirror (static_asserted against
// <3ds.h> in input_3ds_hid.cpp), so tests/scripts compiles and pins this file.
// Cold path: read once at boot, written at most once.
// ============================================================================

#include <stddef.h>
#include <stdint.h>

#include <string>

#include "game/input.h"

namespace mdk {

// One HID mask per game action: the action is held when any of its buttons is.
// 0: unbound. The turn axes and Escape are not buttons on this target (the
// circle pad turns; START is the frontend's pause) and stay 0.
struct ControlBindings {
    uint32_t buttons[kKeyCount] = {0};
};

// The owner's defaults (above).
ControlBindings defaultControlBindings();

// The file's name for an action ("forward", "strafe_left", ...), or null for an
// action the file does not bind (the turn axes, escape).
const char* controlActionName(int key);

// One button name's bit ("ZR" -> kHidZR), any case; 0 when it is not one.
uint32_t controlButtonBit(const char* name, size_t length);

// "R+ZR" for a mask (the file's spelling, in the fixed button order), "none"
// for 0.
std::string formatControlButtons(uint32_t mask);

// A value ("R+ZR", " x ", "none") into a mask. False -- and `mask` untouched --
// when any part is not a button name, a part is empty, or the value is empty.
bool parseControlButtons(const char* begin, const char* end, uint32_t* mask);

// What a parse found, for the log (one line at boot).
struct ControlsReport {
    int32_t applied = 0;    // action lines taken
    int32_t bad = 0;        // action lines whose value did not parse (default kept)
    int32_t unknown = 0;    // lines whose key is no action (ignored)
    // The first problem, as "line 4: fire=Q" (empty when none).
    char firstProblem[96] = {0};
};

// Reads controls.ini's text over `out`, which the caller starts from the
// defaults (a missing or bad line keeps what is there). `text` need not be
// NUL-terminated.
void parseControls(const char* text, size_t size, ControlBindings* out, ControlsReport* report);

// The file's text for `bindings`: a comment header, then one line per action.
std::string writeControls(const ControlBindings& bindings);

// One line for the boot log: "forward=X back=B ... turbo=ZL".
std::string describeControls(const ControlBindings& bindings);

} // namespace mdk

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
// THE DEFAULTS (owner, 2026-11-19; the run controls 2026-11-27):
//
//   forward        X              back           B
//   strafe_left    Y              strafe_right   A
//   jump           L              fire           R
//   item_previous  D-pad left     item_next      D-pad right
//   sniper         D-pad up       use_item       D-pad down
//   zoom_in        X              zoom_out       B   (the scope's; owner 2026-11-21:
//                  input3dsScopeButtons gives X and B to the zoom in sniper mode)
//   run_toggle     ZL + SELECT    each press flips the run (ZL: New 3DS only, so
//                                 SELECT is the Old 3DS's)
//   run_hold       ZR             held, inverts the toggle (New 3DS only)
//   turbo          none           a plain hold-to-run, for a file that wants one
//   down           none           noclip's descent; the owner picks it
//   look / walk    the circle pad looks; a New 3DS's C-Stick walks (not
//                  buttons, not in the file; config.ini swapSticks swaps them)
//   START          the pause menu (the frontend's own button: main_3ds.cpp reads
//                  it before the game does, and while a menu is open the game is
//                  paused and these bindings are not consulted)
//
// SNIPER enters and leaves sniper mode (Player's sniper step, gap package #5):
// Kurt stands, looks and zooms through the scope and fires its clip; the scope's
// view and overlay are drawn (package #7, game/sniper_view.h).
//
// THE FILE. key=value lines, as config.ini (game/enhanced_config.cpp): '#' or
// ';' starts a comment, blank lines pass, spaces around the key and value are
// ignored. One line per action; the value is a '+'-separated list of button
// names -- A B X Y L R ZL ZR START SELECT DUP DDOWN DLEFT DRIGHT, any case --
// or `none` for an unbound action, e.g. `run_toggle=ZL+SELECT`. Any of the
// buttons held holds the action. A missing action keeps its default; a line
// whose value does not parse keeps that action's default and is reported; an
// unknown key is ignored and reported. A missing file is the defaults, and the
// frontend writes them out once (cold, at boot) so the owner can edit the file.
//
// THE VERSION, AND THE MIGRATION (2026-11-27). The file starts `version=2`. A
// file with no version line (every file written before the run controls) is
// version 1, and its lines are read as usual and then MIGRATED: a key whose
// value is EXACTLY a default an older build wrote takes today's default --
//
//   fire=R+ZR    -> R      (ZR is the run button now)
//   turbo=ZL     -> none   (ZL is the run toggle now)
//   zoom_in=none -> X, zoom_out=none -> B   (the defaults before 2026-11-21,
//                           which the owner had to fix by hand once)
//
// -- and any other value is the owner's own and is KEPT, even where it now
// shares a button with the run (that is logged, not changed). run_toggle and
// run_hold are missing from a version-1 file, so they are their defaults. The
// frontend logs each change, keeps the old file as controls.v1.ini and writes
// the migrated table as version 2, once.
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

// The file's two actions that are not the game's keys: the frontend's run
// (input3dsRun turns them into the game's kKeyTurbo). Numbered after the game's
// keys so `buttons[key]` for a game key is unchanged.
enum ControlAction : int {
    kControlRunToggle = kKeyCount,
    kControlRunHold,
    kControlCount
};

// The file's version today (see THE VERSION above); a file without the line is 1.
constexpr int32_t kControlsVersion = 2;

// One HID mask per action: the action is held when any of its buttons is.
// 0: unbound. The turn axes and Escape are not buttons on this target (the
// circle pad turns; START is the frontend's pause) and stay 0.
struct ControlBindings {
    uint32_t buttons[kControlCount] = {0};
};

// The owner's defaults (above).
ControlBindings defaultControlBindings();

// The file's name for an action ("forward", "strafe_left", "run_toggle", ...),
// or null for an action the file does not bind (the turn axes, escape).
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
    int32_t version = 1;    // the file's `version=` line; 1 when it has none
    // The first problem, as "line 4: fire=Q" (empty when none).
    char firstProblem[96] = {0};
};

// Reads controls.ini's text over `out`, which the caller starts from the
// defaults (a missing or bad line keeps what is there). `text` need not be
// NUL-terminated.
void parseControls(const char* text, size_t size, ControlBindings* out, ControlsReport* report);

// THE MIGRATION (see THE VERSION above): a file of `fromVersion` older than
// kControlsVersion has each old default replaced by today's. Returns how many
// actions changed and describes them in `changes` ("fire R+ZR->R turbo ZL->none",
// empty when none). Separately, `conflicts` names every action that shares a
// button with run_toggle or run_hold (kept as they are: "jump=L+ZR shares ZR
// with run_hold"), empty when none. Either out may be null.
int32_t migrateControls(int32_t fromVersion, ControlBindings* bindings, std::string* changes,
                        std::string* conflicts);

// The file's text for `bindings`: a comment header, the version line, then one
// line per action.
std::string writeControls(const ControlBindings& bindings);

// One line for the boot log: "forward=X back=B ... run_hold=ZR".
std::string describeControls(const ControlBindings& bindings);

} // namespace mdk

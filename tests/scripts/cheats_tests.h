#pragma once

// ============================================================================
// cheats_tests.h -- the typed cheats and the main menu's debug keys against the
// C# oracle (gap package #21).
//
//   golden/cheats_typed.txt   tests/scripts/cheats_probe `Probe typed <gore> <text>`
//                             for each case of kTypedCases: C# Cheats.Type behind
//                             Viewer.TypeCheats' filter, letter by letter
//   golden/cheats_menu.txt    `Probe menu <debug> <text>` for each of kMenuCases:
//                             MainMenu.HandleKeys' typed half, DebugKey
//
// Plus what has no oracle run: the 3DS pause menu's CHEATS page (CheatsMenu) and
// the main menu's frame() reading Keys::typed ("Really Quit?"'s Y/N and the debug
// keys through the real page), both pinned to the C#'s rules by hand.
//
// Kurt's half (god mode, noclip) needs the game data and is tick1's
// (`tick1 dump cheats`, golden/cheats_L*.txt).
// ============================================================================

#include <string>

namespace mdk {
namespace test {

// `vm_tests dump-cheats typed|menu`: the C++ side of the probe's two sections, every
// case concatenated as the goldens hold them.
std::string cheatsDump(const char* section);

// The goldens and the hand pins; the number of failed checks.
int runCheatsTests();

} // namespace test
} // namespace mdk

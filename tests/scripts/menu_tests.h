#pragma once

// ============================================================================
// menu_tests.h -- the menus, the settings file and the briefing (package #20a)
// against the C# oracle.
//
// THE ORACLE IS tests/scripts/menu_probe: the C#'s own MainMenu, MenuItems,
// PauseMenu, Settings, Typesetter and StatsScreen (StatsPages.Briefing), driven
// by key schedules at the game's 1/60 s step through a real (headless) Ui. Its
// output is golden/menu_<section>.txt; menuDump(section) drives the C++ port by
// the SAME schedules and must print the same bytes (`vm_tests dump-menu
// <section>`). The sections: settings, typeset, main, pause, briefing.
//
// It needs the game data (MDKFONT.FTI's fonts and texts), so it runs after the
// data check. The function returns the number of failed checks.
// ============================================================================

#include <string>

namespace mdk {
namespace formats {
class MdkData;
}

namespace test {

// The C++ side of one menu_probe section, or empty for an unknown one.
std::string menuDump(const formats::MdkData& data, const char* section);

// Every section against its golden, plus the checks that the goldens bite.
int runMenuTests(const formats::MdkData& data);

} // namespace test
} // namespace mdk

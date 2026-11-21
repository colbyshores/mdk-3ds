#pragma once

// ============================================================================
// stats_tests.h -- the statistics screen (package #20b: StatsScreen's pages,
// the Score-O-matic's heads, the save prompt between them) against the C#
// oracle.
//
// THE ORACLE IS tests/scripts/menu_probe's `stats-*` sections: the C#'s own
// StatsScreen through a real (headless) Ui, driven by key schedules at 1/60 s
// with given counts, the audio device updated after each frame as Game.Run does,
// the canvas captured in the renderer's Overlay hook. Its output is
// golden/stats_<section>.txt; statsDump(section) drives the C++ port by the SAME
// schedules and counts and must print the same bytes (`vm_tests dump-stats
// <section>`): each frame's page state, the sounds started/stopped/ended, and the
// canvas's quads (count and hash, and the quads on a page's first frame).
//
// It needs the game data (MDKFONT.FTI, MISC/STATS.BNI), so it runs after the data
// check. The function returns the number of failed checks.
// ============================================================================

#include <string>

namespace mdk {
namespace formats {
class MdkData;
}

namespace test {

// The C++ side of one menu_probe stats section, or empty for an unknown one.
std::string statsDump(const formats::MdkData& data, const char* section);

// Every section against its golden, plus the unit checks.
int runStatsTests(const formats::MdkData& data);

} // namespace test
} // namespace mdk

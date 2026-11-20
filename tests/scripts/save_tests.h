#pragma once

// ============================================================================
// save_tests.h -- package #19's data-free tests (save_tests.cpp): the save record
// and folder against the C# (golden/save_flow.txt), the JSON float writer
// (golden/json_floats.txt), the save prompt, a death's LASTGAME and the menu's
// Continue, and the write's safety. The level-state goldens need the game data and
// are tick1's (save_dump.h).
// ============================================================================

#include <string>

namespace mdk {
namespace test {

// The C++ twin of `Probe flow <dir>` (tests/scripts/save_probe).
std::string saveFlowDump(const std::string& dir);

int runSaveTests();

} // namespace test
} // namespace mdk

#pragma once

// ============================================================================
// flow_tests.h — the game-flow state machine, checked against the C# oracle.
//
// Two kinds of pin live here, and they are labelled apart because they are not
// the same kind of evidence:
//
//   MEASURED. The level-order tables (order, index, what follows a level, what
//   comes next, the save prompt's number) and what NewGame and EnterLastLevel
//   leave behind. Every row came out of the read-only C# classes themselves,
//   printed by the scratch probe's `flow` mode; the command and its output are
//   recorded in docs/PROGRESS_LOG.md.
//
//   TRANSLITERATED. The Handle transition table, ApplyTestCounts and the step
//   selection. These are the C#'s own switch and arithmetic, and the C# cannot
//   run them without a window, a renderer and an audio device (Game's
//   constructor builds all three), so the pin is the switch, branch for branch,
//   with the C# line cited. Where a row is a trap the comment says which.
//
// The function returns the number of failed checks, so the caller adds it to
// its own counter.
// ============================================================================

#include <string>

namespace mdk {
namespace test {

// The next-level chain with every screen this port lacks skipped (package 2):
// the C++ twin of tests/scripts/flow_probe, whose output is
// golden/flow_next_level.txt. `vm_tests dump-flow` prints it.
std::string flowNextLevelDump();

// The level order, the flow's transitions, and the tick's step.
int runFlowTests();

} // namespace test
} // namespace mdk

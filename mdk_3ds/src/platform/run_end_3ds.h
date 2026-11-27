// ============================================================================
// run_end_3ds.h -- the run's own end, asked by EVERY frame loop.
//
// A 3DS run is three kinds of frame loop, one after another and in any order:
// the level loop (main_3ds.cpp), the front screens -- the main menu, the
// briefing and statistics, the fall, the end movies, the save prompt
// (menu_screens_3ds.cpp) -- and the Gunter stream (stream_3ds.cpp). Beside the
// player's own ways out (Quit, HOME, aptMainLoop false), a run can have an end
// of its own: the --frames budget, and on a harness branch its deadline
// (AUTOQUIT). That end has to hold in every loop the run can be in.
//
// WHY ONE FUNCTION. The end used to be a field per loop (FrontScreens3ds and
// Stream3dsFrontend each carried a frameBudget copy) and the harness's
// deadline was a call in the level loop only. A harness ported without the
// front screens' copy (the 7d86f347 port that the 2026-11-26 throwaways
// carry) left any run that died into the main menu sitting there for good:
// measured on old3ds-lan167, 2026-11-26 08:17, LEVEL=5 -- Kurt died at frame
// 2279, the menu came up, its slides loaded at frame 2698, and then nothing:
// no autoquit, no session end, hw_cycle's 600 s poll timed out with the
// console's HTTP answering throughout (the game was still in the menu, not
// hung). With one function
// there is one place for an end to go, and tests/scripts/run_end_test.sh
// (the host suite, `make -C tests/scripts test`) fails any frame loop that
// does not ask it.
//
// No 3DS header: the budget's arithmetic needs none.
// ============================================================================
#pragma once

#include <stdint.h>

namespace mdk {

// The --frames budget (0 = none), set once at boot from the command line.
void runEndSetFrameBudget(uint32_t frames);

// True when the run is over by its own end: the frame budget reached (`frame`
// is the run's frame count, which every loop advances) -- or, on a harness
// branch, its deadline. Then `*reason` names it for the exit line. False
// otherwise, and `*reason` is left alone. Every frame loop calls it once per
// frame and leaves through its ordinary teardown when it says so.
bool runEnded3ds(uint32_t frame, const char** reason);

} // namespace mdk

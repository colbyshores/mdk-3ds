#pragma once

// ============================================================================
// hd_window_tests.h -- the 3DS HD window (mdk_3ds/src/hd_window_3ds.h), checked
// on the host.
//
// Where the expected values came from:
//   * the window rule (the arena, its doorway neighbours, the arenas its triggers
//     and scripts name; the drawn arenas' files first) is the owner's,
//     2026-11-26, relayed with the design report (perf/3ds-hd-bubble);
//   * the level neighbours are read off the game data by hand for the report:
//     LEVEL3's HMO_3 script teleports Kurt to HMO_4 (teleport_player, opcode
//     112), an arena no doorway of HMO_3's half of the level reaches; LEVEL6's
//     OLYM_9 has a show trigger for COLYM_2 and no doorway; LEVEL7 is the chain
//     DANT_1 - CDANT_1 - DANT_2 (doorway ids 1000 and 1001);
//   * the fit's numbers are small made-up files, worked by hand below.
// Nothing here was read off hd_window_3ds.cpp's output.
// ============================================================================

namespace mdk {
namespace formats {
class MdkData;
}
namespace test {

// Data-independent: the rule and the fit.
int runHdWindowTests();
// The neighbours read from levels 3, 6 and 7.
int runHdWindowLevelTests(const formats::MdkData& data);

} // namespace test
} // namespace mdk

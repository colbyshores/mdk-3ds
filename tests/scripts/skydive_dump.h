#pragma once

// The fall before a level (fall/fall_sim.h) stepped under a scripted steering:
// the C++ twin of tests/scripts/fall_probe (the C# FallSim). `tick1 dump skydive
// <level> <difficulty> <seed> <steps> <delta> <schedule> [full]` prints it, and
// golden/L*_skydive_*.txt pins it byte for byte. See skydive_dump.cpp.

#include <string>

#include "formats/mdk_data.h"

namespace mdk {
namespace test {

bool skydiveDump(int level, int difficulty, int seed, int steps, const std::string& delta,
                 const std::string& schedule, bool full, const formats::MdkData& data, std::string* out,
                 std::string* why);

// The same fall in the probe's `view` mode (package #17b): each step what the C#'s
// FallView makes of the sim (fall_palette.h, fall_ground.h, fall_layout.h), against
// golden/L*_fallview_*.txt.
bool fallViewDump(int level, int difficulty, int seed, int steps, const std::string& delta,
                  const std::string& schedule, const formats::MdkData& data, std::string* out,
                  std::string* why);

}  // namespace test
}  // namespace mdk

#pragma once

// ============================================================================
// stream_dump.h — the stream simulation's goldens (C++ twin of
// tests/scripts/stream_probe, which runs the C# Flight/StreamTube/WatcomRandom).
//
//   tick1 dump stream <level> <difficulty> <seed> <health> <steps> <dt> [schedule]
//   tick1 dump streamrand <seeds,comma,separated> <count>
//
// print what `Probe stream ...` / `Probe rand ...` print, byte for byte; the
// goldens are golden/stream_*.txt and tick1's own case list runs them. See the
// probe's Program.cs for the line grammar and the schedule.
// ============================================================================

#include <string>

#include "formats/mdk_data.h"

namespace mdk {
namespace test {

// One stream run. False with `why` when the data or the arguments are bad.
bool streamDump(const formats::MdkData& data, int level, int difficulty, unsigned seed, int health, int maxSteps,
                const std::string& dt, const std::string& schedule, std::string* out, std::string* why);

// Watcom rand() sequences: `count` values for each seed.
bool streamRandDump(const std::string& seeds, int count, std::string* out, std::string* why);

} // namespace test
} // namespace mdk

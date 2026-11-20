#pragma once

// ============================================================================
// save_dump.h -- the full save's goldens (package #19), the C++ twin of
// tests/scripts/save_probe (the C# ScriptRuntime.Capture / SaveGame.ToJson /
// SaveGame.Parse / GameState.Load / ScriptRuntime.Load).
//
//   tick1 dump save <level> <steps> <after> <schedule>
//   tick1 dump save-load <file> <level> <after>
//
// print what `Probe <level> <steps> <after> <schedule>` and `Probe load <file>
// <level> <after>` print, byte for byte; the goldens are golden/L*_save_*.txt.
// The first plays to a point, takes the full save (printed whole, with its hash),
// writes the save file's text, parses it back into a FRESH world and runs on; the
// second does the last half from a save file on disk (the cross-load: the C#'s
// file into the C++, and the C++'s into the C#).
// ============================================================================

#include <string>

#include "formats/bni.h"
#include "formats/mdk_data.h"

namespace mdk {
namespace test {

// One save run. False with `why` when the data or the schedule are bad.
bool saveDump(const formats::MdkData& data, const formats::Bni* sprites, int level, int steps,
              int after, const std::string& schedule, std::string* out, std::string* why);

// The load half from a save file's text (the file's bytes, as read).
bool saveLoadDump(const formats::MdkData& data, const formats::Bni* sprites,
                  const std::string& fileText, int level, int after, std::string* out,
                  std::string* why);

// The C++ side's save file for a run (what `saveDump` printed as the file), for the
// cross-load check's other direction.
bool saveFileText(const formats::MdkData& data, const formats::Bni* sprites, int level,
                  int steps, const std::string& schedule, std::string* out, std::string* why);

} // namespace test
} // namespace mdk

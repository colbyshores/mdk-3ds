#pragma once

// ============================================================================
// local_paths.h — the mdk_paths.cfg config file.
//
// A 1:1 port of src/Mdk.Formats/LocalPaths.cs.
//
// mdk_paths.cfg is a file of your own next to the program, naming the game's
// folder and the 1996 demo's, so that no environment variable is needed:
//
//     [paths]
//     mdk="C:/GOG Games/MDK"
//     beta="C:/Games/MDK (1996-08-06) (beta demo)"
//
// The format is Godot's ConfigFile: a section header in square brackets,
// quoted strings, and ';' or '#' comments.
// ============================================================================

#include <string>

#include "formats/file_system.h"

namespace mdk {
namespace formats {

// The config file's name.
extern const char* const kLocalPathsFileName;

// The keys the game looks up.
extern const char* const kLocalPathsGame;   // "mdk"
extern const char* const kLocalPathsBeta;   // "beta"

// The value of `key` in the config file next to the program, or "".
std::string localPathsGet(const FileSystem* system, const std::string& key);

// The value of `key` in the [paths] section of a config text, or "".
std::string localPathsParse(const std::string& text, const std::string& key);

} // namespace formats
} // namespace mdk

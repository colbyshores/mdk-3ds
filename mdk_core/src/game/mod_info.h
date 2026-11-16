#pragma once

// ============================================================================
// mod_info.h -- a mod's mod.txt, ported 1:1 from src/Mdk.Game/Mods/ModInfo.cs.
//
// A mod is a folder in the user folder's mods/ (game/mod_catalog.h). Its
// mod.txt is name=value lines ('#' comments); every field is optional. The
// name defaults to the folder's; a higher priority wins over lower ones
// (default 0) when two mods replace the same thing.
//
// The file reaches here through the formats FileSystem seam, like every
// other file the shared code reads, so the parsing is testable without a
// disk and identical on both targets (the 3DS never scans mods: the mod
// system is Linux-only, SPEC.md line 36).
// ============================================================================

#include <stdint.h>

#include <string>

namespace mdk {

// One mod folder's description.
struct ModInfo {
    std::string folder;       // the folder's own name (its identity)
    std::string name;         // display name; the folder's when mod.txt omits it
    std::string author;
    std::string version;
    std::string description;
    int32_t priority = 0;

    // The file every mod folder may carry.
    static const char* fileName() { return "mod.txt"; }

    // The mod of a folder: its mod.txt read through the installed
    // FileSystem, or the defaults when there is none or it cannot be read.
    static ModInfo load(const std::string& modsRoot, const std::string& folder);

    // Parse mod.txt text for a folder (the C#'s Parse).
    static ModInfo parse(const std::string& folder, const std::string& text);

    // The mod.txt text of this info (a generated mod's).
    std::string format() const;
};

// Join two path parts with the platform separator (a trailing separator on
// `left` is not doubled). Shared by the mod modules.
std::string modPathJoin(const std::string& left, const std::string& right);

// A lower-cased copy, for the mods' case-insensitive name matching.
std::string modLower(const std::string& text);

} // namespace mdk

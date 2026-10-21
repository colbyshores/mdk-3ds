#pragma once

// ============================================================================
// case_insensitive_path.h — finding data files whatever their case.
//
// A 1:1 port of src/Mdk.Formats/CaseInsensitivePath.cs.
//
// WHY IT EXISTS. The game asks for MISC/MDKFONT.FTI. An installation may
// have MISC/mdkfont.fti. A case-sensitive filesystem misses the second, so
// the resolver walks the path part by part and matches each part's name
// case-insensitively.
//
// WHY IT IS A SEAM AND NOT A FILESYSTEM CALL. On the 3DS the data is in a
// ROMFS tree, which is read-only and has its own directory model; the
// resolver must work against THAT tree too (AGENTS.md P5). So the lookup
// goes through the FileSystem table, and the 3DS fills it in with a RomFS
// implementation.
//
// THE RESOLVER NEVER FAILS. A part that is not found keeps its name, so
// the result is always a usable path: the caller finds out that the file
// does not exist when it tries to read it, and the error names the path the
// game asked for.
// ============================================================================

#include <string>

#include "formats/file_system.h"

namespace mdk {
namespace formats {

// The path of `relative` under `dir`, with each part named as on disk.
// Parts that are not found keep their name.
std::string caseInsensitiveResolve(const FileSystem* system, const std::string& dir,
                                   const std::string& relative);

} // namespace formats
} // namespace mdk

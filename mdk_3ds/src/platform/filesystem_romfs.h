#pragma once

// ============================================================================
// filesystem_romfs.h — the 3DS platform's file system.
//
// THE PROBLEM IT SOLVES. libctru's RomFS mount can be READ, but it cannot be
// LISTED: there is no readdir for "romfs:/". And the formats library's
// case-insensitive path resolution (formats/case_insensitive_path.h) needs
// to LIST a directory to find the entry that matches the game's spelling.
//
// AGENTS.md P5 requires the resolution to work against the RomFS tree, not
// only a case-sensitive filesystem: the game asks for MISC/MDKFONT.FTI and
// the staged tree holds misc/mdkfont.fti.
//
// THE ANSWER. The staging script writes a MANIFEST of the tree into the
// RomFS (mdk_3ds/tools/stage_romfs.sh). At startup the 3DS reads the
// manifest once and builds a directory index from it: for every file, each
// of its parent directories gains an entry. The index answers the listings,
// and the files themselves are read through the ordinary RomFS path.
//
// The manifest is the STAGED names, and the staged tree is entirely lower
// case, so the index's names are the on-disk names. The case-insensitive
// resolver matches the game's spelling against them and produces the path
// that actually opens.
// ============================================================================

#include "formats/file_system.h"

namespace mdk {

// Mount the RomFS and install the file system. Returns false when the
// RomFS could not be mounted; the caller decides whether that is fatal.
bool romfsFileSystemInit();

// Release the file system. The RomFS stays mounted: unmounting it is the
// caller's business.
void romfsFileSystemExit();

// The file system, ready to install into the formats library.
const formats::FileSystem* romfsFileSystem();

} // namespace mdk

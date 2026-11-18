#pragma once

// ============================================================================
// file_system.h — the platform's file access, as a seam.
//
// WHY THIS EXISTS. The formats library reads the game's data files by
// path. On the desktop that is an ordinary case-sensitive filesystem. On the
// 3DS it is a ROMFS tree, mounted read-only, with DIFFERENT case rules and
// no write access at all.
//
// AGENTS.md P5 requires the case-insensitive path resolution to work against
// a RomFS tree, not only a case-sensitive filesystem: the game asks for
// MISC/MDKFONT.FTI and the GOG tree ships MISC/mdkfont.fti.
//
// So the formats library does not touch the filesystem directly. It asks
// through this table, and each platform fills it in:
//
//   * mdk_linux  -- the real filesystem (src/platform/filesystem_sdl.cpp)
//   * mdk_3ds    -- the RomFS tree, with its own case-insensitive lookup
//
// This is a function-pointer table rather than a virtual interface on
// purpose: AGENTS.md §10 bans virtual dispatch, and a table of four
// function pointers is a struct, not a class hierarchy.
//
// THE TABLE IS FILLED IN ONCE, at startup, and read-only thereafter. The
// formats library never changes it.
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

namespace mdk {
namespace formats {

// The platform's file access.
struct FileSystem {
    // The names of the entries in a directory, files AND subdirectories. The
    // names are the entries' OWN names, not paths. Returns false when the
    // directory does not exist or cannot be read.
    bool (*listDirectory)(void* user, const char* path, std::vector<std::string>* outNames) = nullptr;

    // Read a whole file. Returns false when it cannot be read.
    bool (*readFile)(void* user, const char* path, std::vector<uint8_t>* outBytes) = nullptr;

    // Read exactly `length` bytes at `offset` of a file; false on a short read.
    // Optional: MdkData::readRange falls back to readFile and a slice when a
    // platform leaves it null.
    bool (*readRange)(void* user, const char* path, int32_t offset, int32_t length,
                      std::vector<uint8_t>* outBytes) = nullptr;

    // Does a file exist and can it be read?
    bool (*fileExists)(void* user, const char* path) = nullptr;

    // The directory the program is running from, with a trailing separator.
    // The data finder walks UP from here, because the project folder is
    // often placed inside the MDK folder.
    const char* (*programDirectory)(void* user) = nullptr;

    // The value of an environment variable, or null. The data finder reads
    // MDK_DATA_DIR through this.
    const char* (*getEnvironment)(void* user, const char* name) = nullptr;

    // The platform's own state, passed back to every call.
    void* user = nullptr;
};

// Install the platform's file system. Called once, at startup, BEFORE any
// format is read. Passing null disables file access; the formats that need
// it then fail cleanly rather than crashing.
void setFileSystem(const FileSystem* system);

// The installed file system, or null.
const FileSystem* fileSystem();

} // namespace formats
} // namespace mdk

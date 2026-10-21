// ============================================================================
// file_system.cpp — the installed file system.
//
// The table is written once at startup and read thereafter. It is a plain
// pointer rather than a copy, so the platform's state stays with the
// platform: the formats library never owns it.
// ============================================================================

#include "formats/file_system.h"

namespace mdk {
namespace formats {

namespace {
const FileSystem* g_system = nullptr;
} // namespace

void setFileSystem(const FileSystem* system) {
    g_system = system;
}

const FileSystem* fileSystem() {
    return g_system;
}

} // namespace formats
} // namespace mdk

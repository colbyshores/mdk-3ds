#pragma once

// ============================================================================
// filesystem_posix.h — the Linux platform's file system.
// ============================================================================

#include "formats/file_system.h"

namespace mdk {

// The platform's file system, ready to install into the formats library.
// It is a function, not a global, so that the table's lifetime is the
// caller's business.
const formats::FileSystem* platformFileSystem();

} // namespace mdk

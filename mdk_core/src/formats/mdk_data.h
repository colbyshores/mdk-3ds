#pragma once

// ============================================================================
// mdk_data.h — locating the original MDK installation and reading its files.
//
// A 1:1 port of src/Mdk.Formats/MdkData.cs.
//
// THE CANDIDATES, in order:
//
//   1. MDK_DATA_DIR (the environment variable)
//   2. the "mdk" entry of mdk_paths.cfg next to the program
//   3. the folders ABOVE the program -- the project folder is often placed
//      inside the MDK folder
//   4. the GOG and Steam folders
//
// The first candidate that has TRAVERSE/TRAVSPRT.BNI (a file every
// installation has) is the installation.
//
// THE FILESYSTEM IS A SEAM. The 3DS has no environment variable and no
// folders above the program; it has a ROMFS tree. The 3DS's FileSystem
// implementation answers the marker check against the mounted tree, and the
// data root is the tree's root.
// ============================================================================

#include <string>
#include <vector>

#include "formats/file_system.h"

namespace mdk {
namespace formats {

// A file every installation has.
extern const char* const kMdkDataMarker;

// The environment variable that names the data folder.
extern const char* const kMdkDataEnvironmentVariable;

// The MDK installation: where it is and how to read it.
class MdkData {
public:
    MdkData() = default;
    MdkData(const FileSystem* system, const std::string& dir) : system_(system), dir_(dir) {}

    // The installation, or an empty one when it was not found.
    static MdkData find(const FileSystem* system);

    // The installation's folder, or "" when it was not found.
    const std::string& dir() const { return dir_; }
    bool found() const { return system_ != nullptr && !dir_.empty(); }

    // The absolute path of a data file, e.g. TRAVERSE/LEVEL3/LEVEL3O.MTO,
    // named as on disk (MISC/MDKFONT.FTI may be MISC/mdkfont.fti).
    std::string pathOf(const std::string& relative) const;

    // Read a data file. Returns false when it cannot be read.
    bool read(const std::string& relative, std::vector<uint8_t>* outBytes) const;

    // Read exactly `length` bytes at `offset` of a data file; false on a short
    // read. Through the platform's readRange, or readFile and a slice when it
    // has none.
    bool readRange(const std::string& relative, int32_t offset, int32_t length,
                   std::vector<uint8_t>* outBytes) const;

    // The same read of a path pathOf() ALREADY resolved. Resolving walks the
    // directory listings (a vector of names copied per path component on the
    // 3DS), which is level-load work: a reader that comes back every frame
    // (the arena stream's 32 KiB steps) resolves once and reads with this.
    bool readRangeAt(const std::string& resolved, int32_t offset, int32_t length,
                     std::vector<uint8_t>* outBytes) const;

private:
    const FileSystem* system_ = nullptr;
    std::string dir_;
};

} // namespace formats
} // namespace mdk

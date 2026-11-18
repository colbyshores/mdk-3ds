// ============================================================================
// mdk_data.cpp — see mdk_data.h.
// ============================================================================

#include "formats/mdk_data.h"

#include "formats/case_insensitive_path.h"
#include "formats/local_paths.h"

namespace mdk {
namespace formats {

const char* const kMdkDataMarker = "TRAVERSE/TRAVSPRT.BNI";
const char* const kMdkDataEnvironmentVariable = "MDK_DATA_DIR";

namespace {

// The folders above the program, which is where a project folder placed
// inside the MDK folder finds it.
void addParentDirectories(const FileSystem* system, std::vector<std::string>* candidates) {
    if (!system || !system->programDirectory) {
        return;
    }
    const std::string base = system->programDirectory(system->user);
    if (base.empty()) {
        return;
    }
    std::string dir = base;
    // Walk up until the root. The C# walks the DirectoryInfo's parents; the
    // same thing, done on the path.
    for (int depth = 0; depth < 16; depth++) {
        candidates->push_back(dir);
        size_t slash = dir.find_last_of("/\\");
        if (slash == std::string::npos) {
            break;
        }
        if (slash == 0) {
            candidates->push_back("/");
            break;
        }
        dir = dir.substr(0, slash);
    }
}

} // namespace

MdkData MdkData::find(const FileSystem* system) {
    if (!system) {
        return MdkData();
    }

    std::vector<std::string> candidates;

    if (system->getEnvironment) {
        const char* env = system->getEnvironment(system->user, kMdkDataEnvironmentVariable);
        if (env && env[0] != '\0') {
            candidates.push_back(env);
        }
    }

    const std::string local = localPathsGet(system, kLocalPathsGame);
    if (!local.empty()) {
        candidates.push_back(local);
    }

    addParentDirectories(system, &candidates);

    candidates.push_back("C:/GOG Games/MDK");
    candidates.push_back("C:/Program Files (x86)/GOG Galaxy/Games/MDK");
    candidates.push_back("C:/Program Files (x86)/Steam/steamapps/common/MDK");

    for (size_t i = 0; i < candidates.size(); i++) {
        const std::string marker = caseInsensitiveResolve(system, candidates[i], kMdkDataMarker);
        if (system->fileExists && system->fileExists(system->user, marker.c_str())) {
            return MdkData(system, candidates[i]);
        }
    }
    return MdkData();
}

std::string MdkData::pathOf(const std::string& relative) const {
    if (!system_) {
        return relative;
    }
    return caseInsensitiveResolve(system_, dir_, relative);
}

bool MdkData::read(const std::string& relative, std::vector<uint8_t>* outBytes) const {
    if (!system_ || !system_->readFile) {
        return false;
    }
    const std::string path = pathOf(relative);
    return system_->readFile(system_->user, path.c_str(), outBytes);
}

bool MdkData::readRange(const std::string& relative, int32_t offset, int32_t length,
                        std::vector<uint8_t>* outBytes) const {
    return readRangeAt(pathOf(relative), offset, length, outBytes);
}

bool MdkData::readRangeAt(const std::string& path, int32_t offset, int32_t length,
                          std::vector<uint8_t>* outBytes) const {
    if (!system_ || offset < 0 || length < 0) {
        return false;
    }
    if (system_->readRange) {
        return system_->readRange(system_->user, path.c_str(), offset, length, outBytes);
    }
    std::vector<uint8_t> whole;
    if (!system_->readFile || !system_->readFile(system_->user, path.c_str(), &whole) ||
        (int64_t)offset + length > (int64_t)whole.size()) {
        return false;
    }
    outBytes->assign(whole.begin() + offset, whole.begin() + offset + length);
    return true;
}

} // namespace formats
} // namespace mdk

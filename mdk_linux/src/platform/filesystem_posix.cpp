// ============================================================================
// filesystem_posix.cpp — the Linux platform's file system.
//
// It fills the FileSystem seam that the formats library reads through, so
// the case-insensitive path resolution works against a real, case-sensitive
// filesystem.
//
// This is the desktop implementation. The 3DS's is
// mdk_3ds/src/platform/filesystem_romfs.cpp, and the formats library does
// not know which one it has.
// ============================================================================

#include "platform/filesystem_posix.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mdk {

namespace {

bool listDirectoryPosix(void* user, const char* path, std::vector<std::string>* outNames) {
    (void)user;
    outNames->clear();
    if (!path || !path[0]) {
        return false;
    }
    DIR* dir = opendir(path);
    if (!dir) {
        return false;
    }
    while (struct dirent* entry = readdir(dir)) {
        const char* name = entry->d_name;
        // The dot entries are not part of the tree the game sees.
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
            continue;
        }
        outNames->push_back(name);
    }
    closedir(dir);
    return true;
}

bool readFilePosix(void* user, const char* path, std::vector<uint8_t>* outBytes) {
    (void)user;
    FILE* file = fopen(path, "rb");
    if (!file) {
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    const long size = ftell(file);
    if (size < 0) {
        fclose(file);
        return false;
    }
    rewind(file);
    outBytes->resize((size_t)size);
    const size_t read = (size > 0) ? fread(outBytes->data(), 1, (size_t)size, file) : 0;
    fclose(file);
    outBytes->resize(read);
    return true;
}

// Read `length` bytes at `offset` of a file: exactly that many, or false (a
// short read is a failure, AGENTS.md P5). The arena stream reads one arena's
// block this way, so a level's MTO is never resident whole.
bool readRangePosix(void* user, const char* path, int32_t offset, int32_t length,
                 std::vector<uint8_t>* outBytes) {
    (void)user;
    if (offset < 0 || length < 0) {
        return false;
    }
    FILE* file = fopen(path, "rb");
    if (!file) {
        return false;
    }
    if (fseek(file, (long)offset, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    outBytes->resize((size_t)length);
    const size_t read = length > 0 ? fread(outBytes->data(), 1, (size_t)length, file) : 0;
    fclose(file);
    if (read != (size_t)length) {
        outBytes->clear();
        return false;
    }
    return true;
}

bool fileExistsPosix(void* user, const char* path) {
    (void)user;
    struct stat info;
    if (stat(path, &info) != 0) {
        return false;
    }
    return S_ISREG(info.st_mode) != 0;
}

const char* programDirectoryPosix(void* user) {
    (void)user;
    // The program's own folder, with a trailing separator, so that a config
    // file beside it can be found.
    static char buffer[4096];
    const ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length <= 0) {
        return ".";
    }
    buffer[length] = '\0';
    char* slash = strrchr(buffer, '/');
    if (slash) {
        slash[1] = '\0';
    }
    return buffer;
}

const char* getEnvironmentPosix(void* user, const char* name) {
    (void)user;
    return getenv(name);
}

} // namespace

const formats::FileSystem* platformFileSystem() {
    static formats::FileSystem instance;
    instance.listDirectory = listDirectoryPosix;
    instance.readFile = readFilePosix;
    instance.readRange = readRangePosix;
    instance.fileExists = fileExistsPosix;
    instance.programDirectory = programDirectoryPosix;
    instance.getEnvironment = getEnvironmentPosix;
    instance.user = nullptr;
    return &instance;
}

} // namespace mdk

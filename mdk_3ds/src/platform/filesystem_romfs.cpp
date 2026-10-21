// ============================================================================
// filesystem_romfs.cpp — see filesystem_romfs.h.
// ============================================================================

#include "platform/filesystem_romfs.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

namespace mdk {

namespace {

// The manifest the staging script writes. Its lines are "<size> <path>",
// with the paths relative to the tree's root.
const char* const kManifestPath = "romfs:/manifest.txt";

// The directory index: a directory's path to the names of its entries.
std::map<std::string, std::vector<std::string>> g_index;

// Split a path into its parts, dropping the empty ones.
std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start < path.size()) {
        while (start < path.size() && path[start] == '/') {
            start++;
        }
        if (start >= path.size()) {
            break;
        }
        size_t end = path.find('/', start);
        if (end == std::string::npos) {
            end = path.size();
        }
        parts.push_back(path.substr(start, end - start));
        start = end;
    }
    return parts;
}

// Add a file's path to the index.
//
// EVERY COMPONENT IS ADDED to the directory that holds it, INCLUDING THE
// LAST. The last component is the file's own name, and a directory listing
// has to include it -- otherwise the directory that holds the file lists
// only its subdirectories, and the case-insensitive resolution cannot match
// the file's name.
//
// The loop therefore runs over every component, not over every component
// but the last.
void indexPath(const std::string& path) {
    const std::vector<std::string> parts = splitPath(path);
    std::string directory = "romfs:/";
    for (size_t i = 0; i < parts.size(); i++) {
        std::vector<std::string>& entries = g_index[directory];
        bool found = false;
        for (size_t e = 0; e < entries.size(); e++) {
            if (entries[e] == parts[i]) {
                found = true;
                break;
            }
        }
        if (!found) {
            entries.push_back(parts[i]);
        }
        if (directory.size() > 1 && directory[directory.size() - 1] != '/') {
            directory += '/';
        }
        directory += parts[i];
    }
}

// Read the manifest and build the index.
bool buildIndex() {
    FILE* file = fopen(kManifestPath, "rb");
    if (!file) {
        return false;
    }
    char line[1024];
    while (fgets(line, sizeof(line), file)) {
        // "<size> <path>": the size is not needed, only the path.
        char* space = strchr(line, ' ');
        if (!space) {
            continue;
        }
        char* path = space + 1;
        // Trim the newline.
        size_t length = strlen(path);
        while (length > 0 && (path[length - 1] == '\n' || path[length - 1] == '\r')) {
            path[--length] = '\0';
        }
        // The staging script writes the paths as "./a/b/c".
        if (path[0] == '.' && path[1] == '/') {
            path += 2;
        }
        if (length == 0) {
            continue;
        }
        indexPath(path);
    }
    fclose(file);
    return true;
}

bool listDirectoryRomfs(void* user, const char* path, std::vector<std::string>* outNames) {
    (void)user;
    outNames->clear();
    if (!path) {
        return false;
    }

    // The path is tried AS GIVEN first, and only then with its trailing
    // slashes removed.
    //
    // THE ORDER MATTERS, and getting it wrong loses the ROOT. The index's
    // root key is "romfs:/" -- with its slash -- because that is the mount
    // point. Stripping the trailing slash from "romfs:/" leaves "romfs:",
    // which is not a key in the index, and then the root lists NOTHING and
    // every case-insensitive resolution fails at the first component. The
    // symptom is a game that has its data and cannot find it.
    std::map<std::string, std::vector<std::string>>::const_iterator found = g_index.find(path);
    if (found == g_index.end()) {
        std::string key = path;
        while (key.size() > 1 && key[key.size() - 1] == '/') {
            key.erase(key.size() - 1);
        }
        found = g_index.find(key);
    }
    if (found == g_index.end()) {
        return false;
    }
    *outNames = found->second;
    return true;
}

bool readFileRomfs(void* user, const char* path, std::vector<uint8_t>* outBytes) {
    (void)user;
    FILE* file = fopen(path, "rb");
    if (!file) {
        return false;
    }
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size < 0) {
        fclose(file);
        return false;
    }
    outBytes->resize((size_t)size);
    const size_t read = (size > 0) ? fread(outBytes->data(), 1, (size_t)size, file) : 0;
    fclose(file);
    outBytes->resize(read);
    return true;
}

bool fileExistsRomfs(void* user, const char* path) {
    (void)user;
    FILE* file = fopen(path, "rb");
    if (!file) {
        return false;
    }
    fclose(file);
    return true;
}

const char* programDirectoryRomfs(void* user) {
    (void)user;
    // The program's data is the RomFS root. The game's data finder walks up
    // from here, and the RomFS root is where the marker file lives.
    return "romfs:/";
}

const char* getEnvironmentRomfs(void* user, const char* name) {
    // The 3DS has no environment variables.
    (void)user;
    (void)name;
    return nullptr;
}

} // namespace

const formats::FileSystem* romfsFileSystem() {
    static formats::FileSystem instance;
    instance.listDirectory = listDirectoryRomfs;
    instance.readFile = readFileRomfs;
    instance.fileExists = fileExistsRomfs;
    instance.programDirectory = programDirectoryRomfs;
    instance.getEnvironment = getEnvironmentRomfs;
    instance.user = nullptr;
    return &instance;
}

bool romfsFileSystemInit() {
    const Result result = romfsInit();
    if (R_FAILED(result)) {
        return false;
    }
    // The index is a nicety: without it the case-insensitive resolution
    // cannot list a directory, but the plain paths still open. So a missing
    // manifest is not fatal.
    buildIndex();
    formats::setFileSystem(romfsFileSystem());
    return true;
}

void romfsFileSystemExit() {
    formats::setFileSystem(nullptr);
}

} // namespace mdk

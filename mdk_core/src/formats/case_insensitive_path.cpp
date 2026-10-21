// ============================================================================
// case_insensitive_path.cpp — see case_insensitive_path.h.
// ============================================================================

#include "formats/case_insensitive_path.h"

#include <vector>

namespace mdk {
namespace formats {

namespace {

bool isSeparator(char c) {
    return c == '/' || c == '\\';
}

// The entry of a folder named `name`: the exact name first, then any case.
// Returns `name` unchanged when the folder cannot be listed.
std::string match(const FileSystem* system, const std::string& dir, const std::string& name) {
    if (!system || !system->listDirectory) {
        return name;
    }
    std::vector<std::string> entries;
    if (!system->listDirectory(system->user, dir.c_str(), &entries)) {
        return name;
    }

    // The exact name wins, whatever the order the listing came in. A second
    // pass finds the first case-insensitive match, and the first match in
    // the listing is kept -- the C# does the same, and a different choice
    // would make the resolution depend on the filesystem's ordering.
    std::string other;
    bool haveOther = false;
    for (size_t i = 0; i < entries.size(); i++) {
        const std::string& entryName = entries[i];
        if (entryName == name) {
            return name;
        }
        if (!haveOther) {
            bool same = entryName.size() == name.size();
            for (size_t c = 0; same && c < entryName.size(); c++) {
                char a = entryName[c];
                char b = name[c];
                if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
                if (a != b) same = false;
            }
            if (same) {
                other = entryName;
                haveOther = true;
            }
        }
    }
    return haveOther ? other : name;
}

} // namespace

std::string caseInsensitiveResolve(const FileSystem* system, const std::string& dir,
                                   const std::string& relative) {
    std::string path = dir;
    size_t i = 0;
    while (i < relative.size()) {
        // Skip the separators. The C# splits on both '/' and '\\' and drops
        // the empty parts, so a doubled separator is not an empty part.
        while (i < relative.size() && isSeparator(relative[i])) {
            i++;
        }
        if (i >= relative.size()) {
            break;
        }
        size_t start = i;
        while (i < relative.size() && !isSeparator(relative[i])) {
            i++;
        }
        const std::string part = relative.substr(start, i - start);

        const std::string resolved = match(system, path, part);
        if (!path.empty() && path[path.size() - 1] != '/' && path[path.size() - 1] != '\\') {
            path += '/';
        }
        path += resolved;
    }
    return path;
}

} // namespace formats
} // namespace mdk

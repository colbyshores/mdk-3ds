// ============================================================================
// mod_info.cpp -- see mod_info.h.
// ============================================================================

#include "game/mod_info.h"

#include <stdlib.h>

#include <map>

#include "formats/file_system.h"

namespace mdk {

namespace {

// Trim spaces, tabs and carriage returns from both ends (the C#'s Trim on
// the line and on each side of the '=').
std::string trim(const std::string& text) {
    size_t first = 0;
    size_t last = text.size();
    while (first < last && (text[first] == ' ' || text[first] == '\t' || text[first] == '\r')) {
        first++;
    }
    while (last > first &&
           (text[last - 1] == ' ' || text[last - 1] == '\t' || text[last - 1] == '\r')) {
        last--;
    }
    return text.substr(first, last - first);
}

// int.TryParse with the invariant culture: an optional sign and digits.
bool parsePriority(const std::string& text, int32_t* out) {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    const long value = strtol(text.c_str(), &end, 10);
    if (end != text.c_str() + text.size() || value < INT32_MIN || value > INT32_MAX) {
        return false;
    }
    *out = (int32_t)value;
    return true;
}

} // namespace

std::string modPathJoin(const std::string& left, const std::string& right) {
    if (left.empty()) {
        return right;
    }
    if (left[left.size() - 1] == '/') {
        return left + right;
    }
    return left + "/" + right;
}

std::string modLower(const std::string& text) {
    std::string lowered = text;
    for (size_t i = 0; i < lowered.size(); i++) {
        if (lowered[i] >= 'A' && lowered[i] <= 'Z') {
            lowered[i] = (char)(lowered[i] - 'A' + 'a');
        }
    }
    return lowered;
}

ModInfo ModInfo::parse(const std::string& folder, const std::string& text) {
    std::map<std::string, std::string> values;
    size_t at = 0;
    while (at <= text.size()) {
        const size_t newline = text.find('\n', at);
        const std::string line =
            trim(text.substr(at, newline == std::string::npos ? std::string::npos : newline - at));
        const size_t equals = line.find('=');
        if (equals != std::string::npos && (line.empty() || line[0] != '#')) {
            values[modLower(trim(line.substr(0, equals)))] = trim(line.substr(equals + 1));
        }
        if (newline == std::string::npos) {
            break;
        }
        at = newline + 1;
    }

    ModInfo info;
    info.folder = folder;
    const std::string name = values.count("name") ? values["name"] : "";
    info.name = name.empty() ? folder : name;
    info.author = values.count("author") ? values["author"] : "";
    info.version = values.count("version") ? values["version"] : "";
    info.description = values.count("description") ? values["description"] : "";
    int32_t priority = 0;
    if (values.count("priority") && parsePriority(values["priority"], &priority)) {
        info.priority = priority;
    }
    return info;
}

ModInfo ModInfo::load(const std::string& modsRoot, const std::string& folder) {
    const formats::FileSystem* system = formats::fileSystem();
    std::string text;
    if (system && system->readFile) {
        std::vector<uint8_t> bytes;
        const std::string path = modPathJoin(modPathJoin(modsRoot, folder), fileName());
        if (system->readFile(system->user, path.c_str(), &bytes)) {
            text.assign(bytes.begin(), bytes.end());
        }
    }
    return parse(folder, text);
}

std::string ModInfo::format() const {
    return "name=" + name + "\nauthor=" + author + "\nversion=" + version +
           "\ndescription=" + description + "\npriority=" + std::to_string(priority) + "\n";
}

} // namespace mdk

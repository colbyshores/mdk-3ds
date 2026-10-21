// ============================================================================
// local_paths.cpp — see local_paths.h.
// ============================================================================

#include "formats/local_paths.h"

#include <vector>

namespace mdk {
namespace formats {

const char* const kLocalPathsFileName = "mdk_paths.cfg";
const char* const kLocalPathsGame = "mdk";
const char* const kLocalPathsBeta = "beta";

namespace {

const char* const kSection = "paths";
const char kQuote = '"';

// "C:/Games/MDK" -> C:/Games/MDK; an escaped quote and an escaped
// backslash each become one.
//
// THE ORDER MATTERS, and it is the C#'s order: the escaped-quote
// replacement runs FIRST, and the escaped-backslash replacement SECOND. A
// single left-to-right pass gives a DIFFERENT answer, and the difference is
// observable:
//
//     the file's text:  escaped="C:\\Games\\\"MDK\""
//     the C#'s answer:  C:\Games\"MDK"
//     a single pass:   C:\Games"MDK"
//
// The single pass consumes the backslash of a `\\` as an escape and so
// never sees the `\"` that follows it. The two passes do not: the first
// turns `\"` into `"` and leaves `\\` alone, and the second turns the
// surviving `\\` into `\`.
std::string unquote(const std::string& value) {
    if (value.size() < 2 || value[0] != kQuote || value[value.size() - 1] != kQuote) {
        return value;
    }
    std::string inner = value.substr(1, value.size() - 2);

    // Pass one: an escaped quote.
    std::string pass1;
    for (size_t i = 0; i < inner.size(); i++) {
        if (inner[i] == '\\' && i + 1 < inner.size() && inner[i + 1] == '"') {
            pass1 += '"';
            i++;
            continue;
        }
        pass1 += inner[i];
    }

    // Pass two: an escaped backslash.
    std::string pass2;
    for (size_t i = 0; i < pass1.size(); i++) {
        if (pass1[i] == '\\' && i + 1 < pass1.size() && pass1[i + 1] == '\\') {
            pass2 += '\\';
            i++;
            continue;
        }
        pass2 += pass1[i];
    }
    return pass2;
}

std::string trim(const std::string& text) {
    size_t start = 0;
    size_t end = text.size();
    while (start < end && (text[start] == ' ' || text[start] == '\t' || text[start] == '\r')) {
        start++;
    }
    while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) {
        end--;
    }
    return text.substr(start, end - start);
}

} // namespace

std::string localPathsGet(const FileSystem* system, const std::string& key) {
    if (!system || !system->readFile || !system->programDirectory) {
        return "";
    }
    const std::string base = system->programDirectory(system->user);
    if (base.empty()) {
        return "";
    }
    std::string path = base;
    if (path[path.size() - 1] != '/' && path[path.size() - 1] != '\\') {
        path += '/';
    }
    path += kLocalPathsFileName;

    std::vector<uint8_t> bytes;
    if (!system->readFile(system->user, path.c_str(), &bytes)) {
        return "";
    }
    return localPathsParse(std::string((const char*)bytes.data(), bytes.size()), key);
}

std::string localPathsParse(const std::string& text, const std::string& key) {
    std::string section;
    size_t lineStart = 0;
    while (lineStart <= text.size()) {
        size_t lineEnd = text.find('\n', lineStart);
        if (lineEnd == std::string::npos) {
            lineEnd = text.size();
        }
        const std::string line = trim(text.substr(lineStart, lineEnd - lineStart));
        lineStart = lineEnd + 1;

        if (line.empty() || line[0] == ';' || line[0] == '#') {
            continue;
        }

        if (line.size() >= 2 && line[0] == '[' && line[line.size() - 1] == ']') {
            section = trim(line.substr(1, line.size() - 2));
            continue;
        }

        const size_t equals = line.find('=');
        if (section != kSection || equals == std::string::npos) {
            continue;
        }
        if (trim(line.substr(0, equals)) != key) {
            continue;
        }
        return unquote(trim(line.substr(equals + 1)));
    }
    return "";
}

} // namespace formats
} // namespace mdk

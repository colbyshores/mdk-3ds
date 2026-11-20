// save_games.cpp -- see save_games.h. C# src/Mdk.Game/Flow/SaveGames.cs.

#include "game/save_games.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>

#include "util/json.h"

namespace mdk {
namespace flow {

const char* const SaveGames::kLastGame = "LASTGAME";
const char* const SaveGames::kSavesFolder = "saves";

namespace {

const char kExtension[] = ".sav";
const char kTemporary[] = ".tmp";
const char kBackup[] = ".bak";

bool fileExists(const std::string& path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

void say(std::string* why, const std::string& what) {
    if (why != nullptr) {
        *why = what;
    }
}

std::string errnoText(const char* what, const std::string& path) {
    return std::string(what) + " " + path + ": " + strerror(errno);
}

// C# SaveGame.Number: the member as an int when it is a number (truncated, as
// `(int)value.GetDouble()`), else the fallback.
int32_t numberOr(const json::Document& doc, int node, int32_t fallback) {
    double value = 0.0;
    if (!doc.asDouble(node, &value)) {
        return fallback;
    }
    if (!(value > -2147483649.0 && value < 2147483648.0)) {
        // Out of int's range: the C#'s cast is unspecified there (x64 gives
        // int.MinValue); no shipped save has one.
        return INT32_MIN;
    }
    return (int32_t)value;
}

} // namespace

// ---------------------------------------------------------------------------
// SaveGame

std::string SaveGame::toJson() const {
    // C# ToJson: type, level, health, deaths, strike_used, time, then the snapshot
    // written raw (Utf8JsonWriter.WriteRawValue).
    std::string out;
    json::Writer json(&out);
    json.beginObject();
    json.key("type");
    json.valueInt(kind);
    json.key("level");
    json.valueInt(level);
    json.key("health");
    json.valueInt(health);
    json.key("deaths");
    json.valueInt(deaths);
    json.key("strike_used");
    json.valueBool(strikeUsed);
    json.key("time");
    json.valueString(time);
    if (hasState) {
        json.key("snapshot");
        json.valueRaw(state);
    }
    json.endObject();
    return out;
}

bool SaveGame::parse(const std::string& text, SaveGame* out) {
    json::Document doc;
    if (!doc.parse(text)) {
        return false;  // the C#'s JsonException -> null
    }
    const int root = doc.root();
    if (doc.type(root) != json::Document::Object) {
        return false;
    }
    const int levelNode = doc.member(root, "level");
    double levelValue = 0.0;
    if (levelNode == json::Document::kNone || !doc.asDouble(levelNode, &levelValue)) {
        // A "level" that is not a number throws InvalidOperationException in the C#
        // (GetDouble), which its catch does not take; here it is just invalid.
        return false;
    }
    const int32_t level = numberOr(doc, levelNode, 0);
    if (level < kFirstLevel || level > kLastLevel) {
        return false;
    }
    SaveGame save;
    save.kind = numberOr(doc, doc.member(root, "type"), (int32_t)SaveKind::LevelStart);
    save.level = level;
    save.health = numberOr(doc, doc.member(root, "health"), kFullHealth);
    save.deaths = numberOr(doc, doc.member(root, "deaths"), 0);
    save.strikeUsed = doc.type(doc.member(root, "strike_used")) == json::Document::True;
    const int time = doc.member(root, "time");
    save.time = doc.type(time) == json::Document::String ? doc.stringOr(time, "") : "";
    const int snapshot = doc.member(root, "snapshot");
    if (doc.type(snapshot) == json::Document::Object) {
        save.hasState = true;
        save.state = doc.raw(snapshot);  // GetRawText: the original bytes
    }
    *out = save;
    return true;
}

// ---------------------------------------------------------------------------
// Files

bool makeDirectories(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    struct stat info;
    if (stat(path.c_str(), &info) == 0) {
        return S_ISDIR(info.st_mode);
    }
    // The parents first (Directory.CreateDirectory makes the whole chain). A
    // device prefix such as "sdmc:/" is skipped: its root always exists.
    size_t start = path.find(":/");
    start = start == std::string::npos ? 1 : start + 2;
    for (size_t slash = path.find('/', start); slash != std::string::npos;
         slash = path.find('/', slash + 1)) {
        const std::string parent = path.substr(0, slash);
        if (parent.empty() || stat(parent.c_str(), &info) == 0) {
            continue;
        }
        if (mkdir(parent.c_str(), 0777) != 0 && errno != EEXIST) {
            return false;
        }
    }
    if (mkdir(path.c_str(), 0777) != 0 && errno != EEXIST) {
        return false;
    }
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool readWholeFile(const std::string& path, std::string* out, std::string* why) {
    struct stat info;
    if (stat(path.c_str(), &info) != 0) {
        say(why, errnoText("cannot stat", path));
        return false;
    }
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        say(why, errnoText("cannot open", path));
        return false;
    }
    const size_t size = (size_t)info.st_size;
    std::string text(size, '\0');
    const size_t got = size == 0 ? 0 : fread(&text[0], 1, size, file);
    const bool readError = ferror(file) != 0;
    const bool closed = fclose(file) == 0;
    if (readError || got != size) {
        char buf[96];
        snprintf(buf, sizeof(buf), "short read: %lu of %lu bytes from ", (unsigned long)got,
                 (unsigned long)size);
        say(why, buf + path);
        return false;
    }
    if (!closed) {
        say(why, errnoText("cannot close", path));
        return false;
    }
    *out = text;
    return true;
}

bool writeFileSafely(const std::string& path, const std::string& text, std::string* why) {
    const std::string temporary = path + kTemporary;
    FILE* file = fopen(temporary.c_str(), "wb");
    if (file == nullptr) {
        say(why, errnoText("cannot create", temporary));
        return false;
    }
    const size_t wrote = text.empty() ? 0 : fwrite(text.data(), 1, text.size(), file);
    const bool flushed = fflush(file) == 0;
    const bool writeError = ferror(file) != 0;
    const bool closed = fclose(file) == 0;
    if (wrote != text.size() || !flushed || writeError || !closed) {
        // A full card lands here: the temporary goes, the last good save stays.
        say(why, errnoText(wrote != text.size() ? "short write to" : "cannot flush", temporary));
        ::remove(temporary.c_str());
        return false;
    }
    if (rename(temporary.c_str(), path.c_str()) == 0) {
        ::remove((path + kBackup).c_str());
        return true;
    }
    // rename would not replace the save (the 3DS FS service refuses an existing
    // target): the old one steps aside to .bak, the new one takes its name, then
    // the .bak goes. A power cut between the two leaves the .bak, which read takes.
    const std::string backup = path + kBackup;
    ::remove(backup.c_str());
    if (fileExists(path) && rename(path.c_str(), backup.c_str()) != 0) {
        say(why, errnoText("cannot move aside", path));
        ::remove(temporary.c_str());
        return false;
    }
    if (rename(temporary.c_str(), path.c_str()) != 0) {
        say(why, errnoText("cannot rename into place", temporary));
        // Put the old save back where it was.
        rename(backup.c_str(), path.c_str());
        ::remove(temporary.c_str());
        return false;
    }
    ::remove(backup.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// SaveGames

SaveGames SaveGames::in(const std::string& userFolder) {
    std::string folder = userFolder;
    if (!folder.empty() && folder[folder.size() - 1] != '/') {
        folder += '/';
    }
    return SaveGames(folder + kSavesFolder);
}

std::string SaveGames::pathOf(const std::string& name) const {
    std::string path = directory_;
    if (!path.empty() && path[path.size() - 1] != '/') {
        path += '/';
    }
    return path + name + kExtension;
}

bool SaveGames::write(const std::string& name, const SaveGame& save, std::string* why) const {
    if (!valid()) {
        say(why, "no save folder");
        return false;
    }
    if (!makeDirectories(directory_)) {
        say(why, errnoText("cannot make", directory_));
        return false;
    }
    return writeFileSafely(pathOf(name), save.toJson(), why);
}

bool SaveGames::read(const std::string& name, SaveGame* out, std::string* why) const {
    if (!valid()) {
        say(why, "no save folder");
        return false;
    }
    std::string path = pathOf(name);
    if (!fileExists(path)) {
        path += kBackup;
        if (!fileExists(path)) {
            say(why, "no save " + name);
            return false;
        }
    }
    std::string text;
    if (!readWholeFile(path, &text, why)) {
        return false;
    }
    if (!SaveGame::parse(text, out)) {
        say(why, "invalid save " + path);
        return false;
    }
    return true;
}

bool SaveGames::exists(const std::string& name) const {
    return valid() && (fileExists(pathOf(name)) || fileExists(pathOf(name) + kBackup));
}

void SaveGames::remove(const std::string& name) const {
    if (!valid()) {
        return;
    }
    ::remove(pathOf(name).c_str());
    ::remove((pathOf(name) + kBackup).c_str());
}

std::vector<std::string> SaveGames::list() const {
    std::vector<std::string> names;
    if (!valid()) {
        return names;
    }
    DIR* dir = opendir(directory_.c_str());
    if (dir == nullptr) {
        return names;
    }
    const size_t extension = sizeof(kExtension) - 1;
    for (struct dirent* entry = readdir(dir); entry != nullptr; entry = readdir(dir)) {
        const std::string file = entry->d_name;
        if (file.size() <= extension ||
            file.compare(file.size() - extension, extension, kExtension) != 0) {
            continue;
        }
        const std::string name = file.substr(0, file.size() - extension);
        if (fileExists(pathOf(name))) {
            names.push_back(name);
        }
    }
    closedir(dir);
    // Order(StringComparer.Ordinal): byte order.
    std::sort(names.begin(), names.end());
    return names;
}

bool SaveGames::quickSlot(const std::string& session, std::string* out) const {
    if (!session.empty() && fileExists(pathOf(session))) {
        *out = session;
        return true;
    }
    // List().OrderByDescending(write time).FirstOrDefault(): the newest, the first
    // in name order among equals (OrderBy is stable).
    const std::vector<std::string> names = list();
    // Seconds are the resolution both platforms' stat gives (the 3DS's FS keeps
    // no finer), so two saves in one second fall back to name order.
    bool found = false;
    time_t newest = 0;
    for (size_t i = 0; i < names.size(); i++) {
        struct stat info;
        if (stat(pathOf(names[i]).c_str(), &info) != 0) {
            continue;
        }
        if (!found || info.st_mtime > newest) {
            newest = info.st_mtime;
            *out = names[i];
            found = true;
        }
    }
    return found;
}

} // namespace flow
} // namespace mdk

#pragma once

// ============================================================================
// save_games.h -- the saved games (C# src/Mdk.Game/Flow/SaveGames.cs, package
// #19): the save record and the folder of `<NAME>.sav` files that holds them.
//
// THE FORMAT IS THE C#'s, BYTE FOR BYTE. A save is one line of compact JSON,
// written exactly as SaveGame.ToJson writes it (System.Text.Json's
// Utf8JsonWriter, see util/json.h):
//
//   {"type":3,"level":7,"health":100,"deaths":0,"strike_used":false,
//    "time":"2026-11-20T12:00:00"}                       a light save
//   {...,"time":"...","snapshot":{"kurt":{...},"level":{...}}}   a full save (F2)
//
// so a save the C# wrote loads here and one written here loads in the C#
// (tests/scripts/golden/L*_save_*.txt and `tick1 dump save`, the cross-load
// check). The kinds are the original's GAME type: 3 at a level's start, 6
// before a level (its briefing first), 1003 a full snapshot.
//
// WHERE THE FILES GO is the frontend's to say (the C#'s `SaveGames.In(userFolder)`,
// `<user folder>/saves`): the Linux frontend uses --save-dir, else
// $MDK_USER_DIR/saves, else the C#'s own Linux default, ~/.local/share/mdk-sdl/saves;
// the 3DS uses sdmc:/3ds/mdk/saves (AGENTS.md P5: an explicit path).
//
// WHAT THIS PORT ADDS TO THE C#'s WRITE, AND WHY. File.WriteAllText truncates the
// save and writes it in place, so a power cut mid-write loses both the new save
// and the old one. Here a save is written whole to `<NAME>.sav.tmp`, flushed and
// closed with every result checked, and only then renamed over `<NAME>.sav`. Where
// rename will not replace an existing file (the 3DS's FS service refuses), the old
// save is first renamed to `<NAME>.sav.bak`, and a read that finds no `.sav` but a
// `.bak` takes the `.bak` -- so at every instant one complete save is on the card.
// None of this changes a byte of the file. A short read (fewer bytes than the
// file's size) is an error, never a truncated save; a missing or full card is a
// `false` and a reason, never a retry.
//
// Plain data and plain functions (AGENTS.md §10): no exceptions -- the C#'s
// `catch (IOException)` paths are the `false` returns here.
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

namespace mdk {
namespace flow {

// C# SaveKind (the original's GAME type).
enum class SaveKind : int32_t { LevelStart = 3, BeforeLevel = 6, Snapshot = 1003 };

// C# SaveGame: the kind, the level (LEVELn), health, deaths, the air strike and
// when; a full snapshot also holds the level's state (`hasState`, the C#'s
// `State != null`), the JSON text of {"kurt": ..., "level": ...}.
struct SaveGame {
    // C# SaveGame.FullHealth.
    static const int32_t kFullHealth = 100;
    static const int32_t kFirstLevel = 3;
    static const int32_t kLastLevel = 8;

    // The C# casts any integer to SaveKind, so the raw value is kept.
    int32_t kind = (int32_t)SaveKind::LevelStart;
    int32_t level = kFirstLevel;
    int32_t health = kFullHealth;
    int32_t deaths = 0;
    bool strikeUsed = false;
    std::string time;
    bool hasState = false;
    std::string state;

    bool isKind(SaveKind k) const { return kind == (int32_t)k; }

    // C# ToJson.
    std::string toJson() const;
    // C# Parse: false when the text is not JSON, has no "level" or a level outside
    // 3-8. Missing fields take the C#'s fallbacks (type 3, health 100, deaths 0,
    // no strike, an empty time, no state).
    static bool parse(const std::string& text, SaveGame* out);
};

// C# SaveGames: the saves in a directory.
class SaveGames {
public:
    // C# SaveGames.LastGame: dying writes it; it only lasts for the session.
    static const char* const kLastGame;
    // C# SaveGames.SavesFolder.
    static const char* const kSavesFolder;

    SaveGames() = default;
    explicit SaveGames(const std::string& directory) : directory_(directory) {}

    // C# SaveGames.In(userFolder): `<userFolder>/saves`.
    static SaveGames in(const std::string& userFolder);

    const std::string& directory() const { return directory_; }
    bool valid() const { return !directory_.empty(); }

    // C# Write: false when it can't (`why` says what failed, when given). The
    // directory is made first (with its parents, as Directory.CreateDirectory).
    bool write(const std::string& name, const SaveGame& save, std::string* why = nullptr) const;
    // C# Read: false when the save is missing, unreadable or invalid.
    bool read(const std::string& name, SaveGame* out, std::string* why = nullptr) const;
    // Whether a save of that name is on disk (the C#'s File.Exists), its `.bak`
    // included (see the header).
    bool exists(const std::string& name) const;
    // C# Delete.
    void remove(const std::string& name) const;
    // C# List: the saves' names, sorted (ordinal).
    std::vector<std::string> list() const;
    // C# QuickSlot: `session` (this session's last full save) while it exists,
    // else the newest file; false when there is none.
    bool quickSlot(const std::string& session, std::string* out) const;

    // The file of a save (`<directory>/<NAME>.sav`).
    std::string pathOf(const std::string& name) const;

private:
    std::string directory_;
};

// The text of a whole file, checked: false when it cannot be opened, its size
// cannot be told, or fewer bytes than that size arrive (a short read).
bool readWholeFile(const std::string& path, std::string* out, std::string* why);
// Writes `text` to `path` through `<path>.tmp` and a rename (see the header).
bool writeFileSafely(const std::string& path, const std::string& text, std::string* why);
// Makes a directory and its parents; true when it exists afterwards.
bool makeDirectories(const std::string& path);

} // namespace flow
} // namespace mdk

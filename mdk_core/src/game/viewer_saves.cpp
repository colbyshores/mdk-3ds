// viewer_saves.cpp -- see viewer_saves.h. C# src/Mdk.Game/Viewer.cs.

#include "game/viewer_saves.h"

#include <stdio.h>

#include "game/level_flow.h"
#include "scripts/script_runtime.h"

namespace mdk {
namespace flow {

void countDeath(GameState* state, int32_t levelNumber, bool strikeUsed) {
    // C# OnDied: Level = _level.Number; Deaths++; StrikeUsed = AirStrike.UsedUp.
    state->level = levelNumber;
    state->deaths++;
    state->strikeUsed = strikeUsed;
}

std::string snapshotName(int32_t levelNumber) {
    char text[16];
    snprintf(text, sizeof(text), "%d", (int)saveNameNumber(levelNumber));
    return text;
}

SaveGame snapshotSave(GameState* state, int32_t levelNumber, int32_t kurtHealth,
                      const std::string& capture) {
    // `_state.Level = _level.Number; return _state.Save(Snapshot) with { Health, State }`.
    state->level = levelNumber;
    SaveGame save = state->save(SaveKind::Snapshot);
    save.health = kurtHealth;
    save.hasState = true;
    save.state = capture;
    return save;
}

bool quickLoad(const SaveGames& saves, GameState* state, Event* next, std::string* log) {
    std::string name;
    SaveGame save;
    if (!saves.quickSlot(state->quickSlot, &name) || !saves.read(name, &save)) {
        return false;
    }
    state->load(save);
    state->notice = "Game loaded";
    char line[96];
    snprintf(line, sizeof(line), "Loaded game %s: level %d", name.c_str(), (int)save.level);
    *log = line;
    *next = save.isKind(SaveKind::BeforeLevel) ? Event::Briefing : Event::Play;
    return true;
}

bool restoreSnapshot(GameState* state, scripts::ScriptRuntime* runtime, bool* restored,
                     std::string* log) {
    *restored = false;
    if (state->snapshot.empty()) {
        return true;
    }
    std::string json;
    json.swap(state->snapshot);  // state.Snapshot = null
    if (runtime == nullptr) {
        *log = "a full save, but no scripts runtime to restore it into";
        return false;
    }
    std::string why;
    if (!runtime->load(json, &why)) {
        *log = "the full save did not restore: " + why;
        return false;
    }
    const std::string again = runtime->capture();
    char line[128];
    snprintf(line, sizeof(line), "restored hash %s, objects %d",
             scripts::ScriptRuntime::snapshotHash(again).c_str(), (int)runtime->objectCount());
    *log = line;
    *restored = true;
    return true;
}

void menuListSaves(void* user, std::vector<std::string>* names) {
    const SaveGames* saves = static_cast<const SaveGames*>(user);
    *names = saves != nullptr ? saves->list() : std::vector<std::string>();
}

bool menuPeekSave(void* user, const char* name, int32_t* level) {
    const SaveGames* saves = static_cast<const SaveGames*>(user);
    SaveGame save;
    if (saves == nullptr || !saves->read(name, &save)) {
        return false;
    }
    *level = save.level;
    return true;
}

bool menuLoadSave(void* user, const char* name, GameState* state, bool* beforeLevel) {
    const SaveGames* saves = static_cast<const SaveGames*>(user);
    SaveGame save;
    if (saves == nullptr || !saves->read(name, &save)) {
        return false;
    }
    state->load(save);
    *beforeLevel = save.isKind(SaveKind::BeforeLevel);
    return true;
}

std::string takeNotice(GameState* state) {
    std::string notice;
    notice.swap(state->notice);
    return notice;
}

} // namespace flow
} // namespace mdk

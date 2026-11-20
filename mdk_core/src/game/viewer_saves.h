#pragma once

// ============================================================================
// viewer_saves.h -- the level's half of saving and loading, and of dying (C#
// src/Mdk.Game/Viewer.cs, package #19): what the C# Viewer does on F2 (OpenSnapshot,
// SnapshotSave), on F9 (QuickLoad), when a level starts from a full save
// (RestoreSnapshot, ShowNotice) and when Kurt dies (OnDied). The frontends are the
// Viewer here, so these are the pieces of it both share; the window, the prompt's
// drawing and the HUD stay theirs.
//
// WHAT A DEATH DOES (Viewer.OnDied, Game.Handle(KurtDied), MainMenu): the death is
// counted and the level kept (state.level, deaths + 1, the air strike's use), the
// game writes LASTGAME -- a light save of the level's START (kind 3, health 100) --
// and goes to the main menu with its splash, whose first item is then "Continue",
// which loads LASTGAME and plays the level again from its start. So a death costs
// the level, not the last full save; a full save is loaded only by F9 or the menu's
// "Saved Game" page. The frontends show that menu (package #20a's main_menu.h), its
// saves read through menuListSaves/menuPeekSave/menuLoadSave below.
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

#include "game/game_flow.h"
#include "game/game_state.h"
#include "game/save_games.h"

namespace mdk {
namespace scripts {
class ScriptRuntime;
} // namespace scripts

namespace flow {

// C# Viewer.NoticeSeconds: the port's own HUD messages ("Game saved") stay 2 s.
constexpr float kNoticeSeconds = 2.0f;

// C# Viewer.OnDied (Viewer.cs:886-893): the level, the death and the strike's use
// are kept; the caller then hands GameFlow Event::KurtDied.
void countDeath(GameState* state, int32_t levelNumber, bool strikeUsed);

// The name F2 offers: the level's number in the order of play (Viewer.OpenSnapshot,
// `(GameState.IndexOf(level) + 1).ToString()`).
std::string snapshotName(int32_t levelNumber);

// C# Viewer.SnapshotSave (:845-849): the level made the state's, then GameState.Save
// (Snapshot) with Kurt's health and the level's state (ScriptRuntime::capture).
SaveGame snapshotSave(GameState* state, int32_t levelNumber, int32_t kurtHealth,
                      const std::string& capture);

// C# Viewer.QuickLoad (:803-816): this session's last full save (state.quickSlot),
// else the newest save. False -- "Nothing to load" -- when there is none or it does
// not read back; otherwise the state is loaded, the notice "Game loaded" left for
// the next level, `next` is what to hand GameFlow::handle (Briefing for a
// before-level save, else Play) and `log` is the C#'s "Loaded game NAME: level N".
bool quickLoad(const SaveGames& saves, GameState* state, Event* next, std::string* log);

// C# Viewer.RestoreSnapshot (:750-760): a full save being loaded, into the fresh
// level's runtime, before its first tick; the state's snapshot is taken (cleared).
// True when there was none (nothing to do) or it restored, `log` then the C#'s
// "restored hash H, objects N"; false, with the reason, when it did not -- the level
// is then not the save's, and the frontend reloads it plain. A frontend also adds
// runtime->enteredArenas() to Kurt's ArenaSpace (the C#'s one space).
bool restoreSnapshot(GameState* state, scripts::ScriptRuntime* runtime, bool* restored,
                     std::string* log);

// The main menu's view of the saves (#20a's menu::MenuSaves hooks; C#
// SaveGames.List/Read and MainMenu.Load). `user` is the frontend's SaveGames.
// C# SaveGames.List(): the names, sorted.
void menuListSaves(void* user, std::vector<std::string>* names);
// C# `SaveGames.Read(name) != null`, and the save's level (LEVELn).
bool menuPeekSave(void* user, const char* name, int32_t* level);
// C# MainMenu.Load: the save read and GameState.Load-ed into `state`; false when it
// is missing or invalid. `beforeLevel`: a before-level save (its briefing first).
bool menuLoadSave(void* user, const char* name, GameState* state, bool* beforeLevel);

// C# Viewer.ShowNotice (:820-829): the message the last level left ("Game loaded"),
// once. Empty when there is none.
std::string takeNotice(GameState* state);

} // namespace flow
} // namespace mdk

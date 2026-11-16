#pragma once

// ============================================================================
// mod_catalog.h -- the mods in the user folder's mods/, ported 1:1 from
// src/Mdk.Game/Mods/ModCatalog.cs.
//
// Each subfolder of mods/ is one mod (hidden ones, starting with '.', are
// not), ordered by priority (highest first), then by folder name ignoring
// case. The first enabled mod with a file for something wins. New mods are
// on; the settings switch them off (mod.<folder>=Off), and a run can name
// only some (--mod; the frontend's business, fed in through only()).
//
//   mods/
//     hd-textures/   mod.txt  textures/...           (made by "Make HD textures")
//     clean-walls/   mod.txt  textures/LEVEL3/...  images/...  models/...
// ============================================================================

#include <string>
#include <vector>

#include "game/mod_info.h"

namespace mdk {

// A mod switched on or off in the settings.
enum class ModState {
    On,
    Off,
};

// The settings file's mod states (`mod.<folder>=On|Off` lines of the
// user folder's settings.cfg; Settings.cs's ModPrefix): the states a
// catalog's enabledFolders() consumes. Other lines are not this
// function's business and are skipped, as are states the enum does not
// name (the oracle's Enum.TryParse gate).
std::vector<std::pair<std::string, ModState>> modStatesFromSettingsText(const std::string& text);

// The mods found under one mods/ root.
class ModCatalog {
public:
    ModCatalog() = default;

    // The mods folder inside a user folder.
    static std::string folderIn(const std::string& userFolder) {
        return modPathJoin(userFolder, "mods");
    }

    // Every mod under `modsRoot`, in order. A missing root is an empty
    // catalog, not an error (a user with no mods folder has no mods).
    static ModCatalog scan(const std::string& modsRoot);

    const std::string& root() const { return root_; }
    const std::vector<ModInfo>& mods() const { return mods_; }

    // Whether a folder names a mod in this catalog (ignoring case).
    bool has(const std::string& folder) const;

    // The full path of a mod's folder.
    std::string pathOf(const ModInfo& mod) const { return modPathJoin(root_, mod.folder); }

    // The enabled mods' folders, in order. `states` maps folder names to
    // their settings state; a mod the map does not name is on.
    std::vector<std::string> enabledFolders(const std::vector<std::pair<std::string, ModState>>& states) const;

    // Only these mods on (--mod; tests), the others off.
    std::vector<std::string> onlyFolders(const std::vector<std::string>& folders) const;

private:
    std::string root_;
    std::vector<ModInfo> mods_;
};

} // namespace mdk

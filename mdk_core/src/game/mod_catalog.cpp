// ============================================================================
// mod_catalog.cpp -- see mod_catalog.h.
// ============================================================================

#include "game/mod_catalog.h"

#include <algorithm>

#include "formats/file_system.h"

namespace mdk {

ModCatalog ModCatalog::scan(const std::string& modsRoot) {
    ModCatalog catalog;
    catalog.root_ = modsRoot;

    const formats::FileSystem* system = formats::fileSystem();
    if (!system || !system->listDirectory) {
        return catalog;
    }
    std::vector<std::string> entries;
    if (!system->listDirectory(system->user, modsRoot.c_str(), &entries)) {
        return catalog;
    }

    for (size_t i = 0; i < entries.size(); i++) {
        const std::string& folder = entries[i];
        if (folder.empty() || folder[0] == '.') {
            continue;  // hidden folders are not mods (the dot entries neither)
        }
        // A mod is a DIRECTORY. The seam has no stat; a directory lists
        // (possibly empty) and a file does not.
        const std::string path = modPathJoin(modsRoot, folder);
        std::vector<std::string> probe;
        if (!system->listDirectory(system->user, path.c_str(), &probe)) {
            continue;
        }
        catalog.mods_.push_back(ModInfo::load(modsRoot, folder));
    }

    // Priority highest first, then folder name ignoring case (then the
    // name itself, so the order is total and the same on every machine).
    std::sort(catalog.mods_.begin(), catalog.mods_.end(), [](const ModInfo& a, const ModInfo& b) {
        if (a.priority != b.priority) {
            return a.priority > b.priority;
        }
        const std::string la = modLower(a.folder);
        const std::string lb = modLower(b.folder);
        if (la != lb) {
            return la < lb;
        }
        return a.folder < b.folder;
    });
    return catalog;
}

bool ModCatalog::has(const std::string& folder) const {
    const std::string wanted = modLower(folder);
    for (size_t i = 0; i < mods_.size(); i++) {
        if (modLower(mods_[i].folder) == wanted) {
            return true;
        }
    }
    return false;
}

std::vector<std::string>
ModCatalog::enabledFolders(const std::vector<std::pair<std::string, ModState>>& states) const {
    std::vector<std::string> folders;
    for (size_t i = 0; i < mods_.size(); i++) {
        ModState state = ModState::On;
        const std::string wanted = modLower(mods_[i].folder);
        for (size_t s = 0; s < states.size(); s++) {
            if (modLower(states[s].first) == wanted) {
                state = states[s].second;
            }
        }
        if (state == ModState::On) {
            folders.push_back(pathOf(mods_[i]));
        }
    }
    return folders;
}

std::vector<std::pair<std::string, ModState>> modStatesFromSettingsText(const std::string& text) {
    std::vector<std::pair<std::string, ModState>> states;
    size_t at = 0;
    while (at <= text.size()) {
        const size_t newline = text.find('\n', at);
        std::string line =
            text.substr(at, newline == std::string::npos ? std::string::npos : newline - at);
        if (!line.empty() && line[line.size() - 1] == '\r') {
            line.erase(line.size() - 1);
        }
        const size_t equals = line.find('=');
        if (equals != std::string::npos) {
            const std::string name = line.substr(0, equals);
            const std::string value = line.substr(equals + 1);
            if (name.compare(0, 4, "mod.") == 0 && name.size() > 4) {
                const std::string word = modLower(value);
                if (word == "on" || word == "off") {
                    states.push_back(std::pair<std::string, ModState>(
                        name.substr(4), word == "on" ? ModState::On : ModState::Off));
                }
            }
        }
        if (newline == std::string::npos) {
            break;
        }
        at = newline + 1;
    }
    return states;
}

std::vector<std::string> ModCatalog::onlyFolders(const std::vector<std::string>& folders) const {
    std::vector<std::pair<std::string, ModState>> states;
    for (size_t i = 0; i < mods_.size(); i++) {
        bool on = false;
        const std::string mine = modLower(mods_[i].folder);
        for (size_t f = 0; f < folders.size(); f++) {
            if (modLower(folders[f]) == mine) {
                on = true;
            }
        }
        states.push_back(std::pair<std::string, ModState>(mods_[i].folder,
                                                          on ? ModState::On : ModState::Off));
    }
    return enabledFolders(states);
}

} // namespace mdk

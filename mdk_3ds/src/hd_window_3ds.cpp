// ============================================================================
// hd_window_3ds.cpp -- see hd_window_3ds.h.
// ============================================================================

#include "hd_window_3ds.h"

#include <set>

#include "formats/cmi.h"
#include "formats/dti.h"
#include "formats/script_decoder.h"
#include "formats/script_opcodes.h"

namespace mdk {

namespace {

// The DTI record types (script_runtime.cpp's kRecordConnection, kRecordTriggerShow,
// kRecordTriggerLoad).
const uint32_t kDoorway = 6;
const uint32_t kTriggerShow = 1;
const uint32_t kTriggerLoad = 3;

// The script opcodes that name an arena (script_vm.cpp).
const int32_t kArenaShow = 100;
const int32_t kTeleport = 112;
const int32_t kTeleportKeep = 173;
const int32_t kArenaSetNeighbour = 223;

void strings(const formats::ScriptValue& value, std::vector<std::string>* out) {
    if (value.kind == formats::ScriptValueKind::String && !value.stringValue.empty()) {
        out->push_back(value.stringValue);
    }
    for (size_t i = 0; i < value.items.size(); i++) {
        strings(value.items[i], out);
    }
}

} // namespace

void HdWindow::clear() {
    files_.clear();
    fileByPath_.clear();
    arenas_.clear();
    stepped_.clear();
}

int32_t HdWindow::find(const std::string& name) const {
    for (size_t i = 0; i < arenas_.size(); i++) {
        if (arenas_[i].name == name) {
            return (int32_t)i;
        }
    }
    return -1;
}

HdWindow::Arena& HdWindow::named(const std::string& name) {
    const int32_t at = find(name);
    if (at >= 0) {
        return arenas_[(size_t)at];
    }
    Arena arena;
    arena.name = name;
    arenas_.push_back(arena);
    return arenas_.back();
}

void HdWindow::addFile(const std::string& arena, const std::string& path,
                       const uint32_t bytes[kTiers]) {
    int32_t index = -1;
    const std::map<std::string, int32_t>::const_iterator known = fileByPath_.find(path);
    if (known != fileByPath_.end()) {
        index = known->second;
    } else {
        index = (int32_t)files_.size();
        File file;
        file.path = path;
        for (int32_t t = 0; t < kTiers; t++) {
            file.bytes[t] = bytes[t];
        }
        files_.push_back(file);
        fileByPath_[path] = index;
    }
    Arena& owner = named(arena);
    for (size_t i = 0; i < owner.files.size(); i++) {
        if (owner.files[i] == index) {
            return;
        }
    }
    owner.files.push_back(index);
}

void HdWindow::addNeighbour(const std::string& arena, const std::string& other) {
    if (arena.empty() || other.empty() || arena == other || other == "NONE") {
        return;
    }
    Arena& owner = named(arena);
    for (size_t i = 0; i < owner.neighbours.size(); i++) {
        if (owner.neighbours[i] == other) {
            return;
        }
    }
    owner.neighbours.push_back(other);
}

void HdWindow::windowArenas(const std::string& current, const std::vector<std::string>& drawn,
                            std::vector<std::string>* out) const {
    out->clear();
    // The drawn arenas first (Kurt's own is drawn[0] once the scripts have ticked).
    for (size_t i = 0; i < drawn.size(); i++) {
        if (!drawn[i].empty()) {
            out->push_back(drawn[i]);
        }
    }
    std::vector<std::string> rest;
    rest.push_back(current);
    const int32_t at = find(current);
    if (at >= 0) {
        rest.insert(rest.end(), arenas_[(size_t)at].neighbours.begin(),
                    arenas_[(size_t)at].neighbours.end());
    }
    for (size_t i = 0; i < rest.size(); i++) {
        bool seen = rest[i].empty();
        for (size_t k = 0; !seen && k < out->size(); k++) {
            seen = (*out)[k] == rest[i];
        }
        if (!seen) {
            out->push_back(rest[i]);
        }
    }
}

void HdWindow::filesOf(const std::vector<std::string>& arenas, std::vector<int32_t>* out) const {
    out->clear();
    std::vector<bool> taken(files_.size(), false);
    for (size_t a = 0; a < arenas.size(); a++) {
        const int32_t at = find(arenas[a]);
        if (at < 0) {
            continue;
        }
        const std::vector<int32_t>& files = arenas_[(size_t)at].files;
        for (size_t f = 0; f < files.size(); f++) {
            if (!taken[(size_t)files[f]]) {
                taken[(size_t)files[f]] = true;
                out->push_back(files[f]);
            }
        }
    }
}

uint32_t HdWindow::windowBytes(const std::string& arena) const {
    std::vector<std::string> arenas;
    windowArenas(arena, std::vector<std::string>(), &arenas);
    std::vector<int32_t> files;
    filesOf(arenas, &files);
    uint32_t bytes = 0;
    for (size_t i = 0; i < files.size(); i++) {
        const File& file = files_[(size_t)files[i]];
        bytes += file.bytes[file.tier];
    }
    return bytes;
}

// A file's tier is the largest of the arenas that hold it: a file shared with an
// arena that stepped down is read once, at the smaller cap.
void HdWindow::retier() {
    for (size_t f = 0; f < files_.size(); f++) {
        files_[f].tier = 0;
    }
    for (size_t a = 0; a < arenas_.size(); a++) {
        for (size_t f = 0; f < arenas_[a].files.size(); f++) {
            File& file = files_[(size_t)arenas_[a].files[f]];
            if (arenas_[a].tier > file.tier) {
                file.tier = arenas_[a].tier;
            }
        }
    }
}

uint32_t HdWindow::fit(uint32_t room) {
    stepped_.clear();
    for (size_t a = 0; a < arenas_.size(); a++) {
        arenas_[a].tier = 0;
    }
    retier();
    for (;;) {
        // The largest window.
        uint32_t worst = 0;
        size_t worstAt = 0;
        for (size_t a = 0; a < arenas_.size(); a++) {
            const uint32_t bytes = windowBytes(arenas_[a].name);
            if (bytes > worst) {
                worst = bytes;
                worstAt = a;
            }
        }
        if (worst <= room) {
            return worst;
        }
        // In it, the arena whose next tier saves the most.
        std::vector<std::string> arenas;
        windowArenas(arenas_[worstAt].name, std::vector<std::string>(), &arenas);
        int32_t pick = -1;
        uint32_t best = 0;
        for (size_t i = 0; i < arenas.size(); i++) {
            const int32_t at = find(arenas[i]);
            if (at < 0 || arenas_[(size_t)at].tier + 1 >= kTiers) {
                continue;
            }
            const Arena& arena = arenas_[(size_t)at];
            uint32_t saves = 0;
            for (size_t f = 0; f < arena.files.size(); f++) {
                const File& file = files_[(size_t)arena.files[f]];
                if (file.tier <= arena.tier) {
                    saves += file.bytes[file.tier] - file.bytes[arena.tier + 1];
                }
            }
            if (saves > best) {
                best = saves;
                pick = at;
            }
        }
        if (pick < 0) {
            return worst;   // nothing left to step down: the caller logs it
        }
        Arena& arena = arenas_[(size_t)pick];
        arena.tier++;
        stepped_.push_back(std::make_pair(arena.name, arena.tier == 1 ? 256 : 128));
        retier();
    }
}

void HdWindow::window(const std::string& current, const std::vector<std::string>& drawn,
                      std::vector<const char*>* paths, std::vector<int32_t>* caps,
                      int32_t* drawnCount) const {
    paths->clear();
    caps->clear();
    // The drawn arenas' files first, then the rest of the window's.
    std::vector<std::string> arenas;
    windowArenas(current, drawn, &arenas);
    std::vector<std::string> shown;
    for (size_t i = 0; i < drawn.size(); i++) {
        if (!drawn[i].empty()) {
            shown.push_back(drawn[i]);
        }
    }
    if (shown.empty()) {
        shown.push_back(current);
    }
    std::vector<int32_t> first;
    filesOf(shown, &first);
    if (drawnCount) {
        *drawnCount = (int32_t)first.size();
    }
    std::vector<int32_t> all;
    filesOf(arenas, &all);
    std::vector<bool> taken(files_.size(), false);
    for (size_t i = 0; i < first.size(); i++) {
        taken[(size_t)first[i]] = true;
    }
    for (size_t i = 0; i < all.size(); i++) {
        if (!taken[(size_t)all[i]]) {
            first.push_back(all[i]);
        }
    }
    for (size_t i = 0; i < first.size(); i++) {
        const File& file = files_[(size_t)first[i]];
        paths->push_back(file.path.c_str());
        caps->push_back(file.tier == 0 ? 0 : (file.tier == 1 ? 256 : 128));
    }
}

int32_t addLevelNeighbours(const formats::Dti& dti, const formats::Cmi* cmi, HdWindow* out) {
    const std::vector<formats::Dti::ArenaEntry>& arenas = dti.arenas();
    // Doorways: both ways.
    std::map<int32_t, std::vector<std::string>> byId;
    for (size_t a = 0; a < arenas.size(); a++) {
        for (size_t r = 0; r < arenas[a].records.size(); r++) {
            const formats::Dti::Record& record = arenas[a].records[r];
            if (record.type == kDoorway) {
                byId[record.id].push_back(arenas[a].name);
            } else if ((record.type == kTriggerShow || record.type == kTriggerLoad) &&
                       record.id >= 0 && (size_t)record.id < arenas.size()) {
                out->addNeighbour(arenas[a].name, arenas[(size_t)record.id].name);
            }
        }
    }
    for (std::map<int32_t, std::vector<std::string>>::const_iterator it = byId.begin();
         it != byId.end(); ++it) {
        for (size_t i = 0; i < it->second.size(); i++) {
            for (size_t k = 0; k < it->second.size(); k++) {
                out->addNeighbour(it->second[i], it->second[k]);
            }
        }
    }
    if (cmi == nullptr || cmi->dialect() != formats::ScriptDialect::Retail) {
        return 0;
    }
    // The scripts: every instruction an arena's or object's script can reach
    // (the decoder's own walk, ScriptDecoder::preload), per script, so the arena
    // that runs it is known.
    std::set<std::string> known;
    for (size_t a = 0; a < arenas.size(); a++) {
        known.insert(arenas[a].name);
    }
    formats::ScriptDecoder decoder(cmi->bytes().data(), (int32_t)cmi->bytes().size());
    int32_t added = 0;
    for (int32_t list = 0; list < 2; list++) {
        const std::vector<std::pair<std::string, int32_t>>& scripts =
            list == 0 ? cmi->arenaScripts() : cmi->objectScripts();
        for (size_t s = 0; s < scripts.size(); s++) {
            const std::string owner = scripts[s].first.substr(0, scripts[s].first.find('$'));
            if (scripts[s].second == 0 || known.count(owner) == 0) {
                continue;
            }
            std::set<int32_t> seen;
            std::vector<int32_t> work(1, scripts[s].second);
            while (!work.empty()) {
                int32_t pc = work.back();
                work.pop_back();
                while (seen.insert(pc).second) {
                    const formats::Instruction* in = decoder.find(pc);
                    if (in == nullptr || in->opcode == formats::ScriptDecoder::kEnd) {
                        break;
                    }
                    if (in->opcode == kArenaShow || in->opcode == kTeleport ||
                        in->opcode == kTeleportKeep || in->opcode == kArenaSetNeighbour) {
                        std::vector<std::string> names;
                        for (size_t o = 0; o < in->operands.size(); o++) {
                            strings(in->operands[o], &names);
                        }
                        for (size_t n = 0; n < names.size(); n++) {
                            if (known.count(names[n]) != 0 && names[n] != owner) {
                                out->addNeighbour(owner, names[n]);
                                added++;
                            }
                        }
                    }
                    std::vector<int32_t> targets;
                    formats::ScriptDecoder::targets(*in, &targets);
                    work.insert(work.end(), targets.begin(), targets.end());
                    const formats::ScriptOpcodeInfo* info = formats::scriptOpcodeInfo(in->opcode);
                    if (info && info->noFallthrough) {
                        break;
                    }
                    pc = in->next;
                }
            }
        }
    }
    return added;
}

} // namespace mdk

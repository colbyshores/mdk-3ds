// ============================================================================
// level_data.cpp — see level_data.h.
// ============================================================================

#include "game/level_data.h"

#include "formats/fti.h"

namespace mdk {

namespace {

// The file with the system colours.
const char* const kSystemFile = "MISC/MDKFONT.FTI";

// The entry in it that holds the system colours.
const char* const kSystemPalette = "SYS_PAL";

// The DTI's record type of a link to another arena: an arena without one
// starts hidden.
const uint32_t kConnection = 6;

} // namespace

bool LevelData::load(LevelData* out, const formats::MdkData* data, int32_t number) {
    if (!out || !data) {
        return false;
    }
    out->number_ = number;

    char base[128];
    snprintf(base, sizeof(base), "TRAVERSE/LEVEL%d/LEVEL%d", (int)number, (int)number);

    // The settings and the sky.
    std::vector<uint8_t> bytes;
    std::string path = std::string(base) + ".DTI";
    if (!data->read(path, &bytes)) {
        return false;
    }
    out->dti_ = formats::Dti::parse(bytes.data(), (int32_t)bytes.size());

    // The system colours. They are applied to the DTI's palette ONCE, and
    // they replace only the 1-63 range: LEVEL8's DTI has magenta and purple
    // at 10-12, and those are the SYSTEM's colours, not the level's.
    std::vector<uint8_t> ftiBytes;
    if (data->read(kSystemFile, &ftiBytes)) {
        formats::Fti fti;
        if (formats::Fti::parse(&fti, ftiBytes.data(), (int32_t)ftiBytes.size())) {
            std::vector<uint8_t> system;
            if (fti.getBytes(kSystemPalette, &system) && !system.empty()) {
                out->dti_.mutablePalette() = out->dti_.palette().withSystemColors(system.data(),
                                                                                   (int32_t)system.size());
            }
        }
    }

    // The arenas.
    bytes.clear();
    path = std::string(base) + "O.MTO";
    if (!data->read(path, &bytes)) {
        return false;
    }
    out->mto_ = formats::Mto::parse(bytes.data(), (int32_t)bytes.size());
    for (size_t i = 0; i < out->mto_.arenaNames().size(); i++) {
        const formats::Arena* arena = out->mto_.getArena(out->mto_.arenaNames()[i]);
        if (arena) {
            out->arenas_.push_back(arena);
        }
    }

    // The level's own textures, shared by every arena.
    bytes.clear();
    path = std::string(base) + "S.MTI";
    if (data->read(path, &bytes)) {
        out->levelTextures_ = formats::TextureArchive::parse(bytes.data(), (int32_t)bytes.size(),
                                                             formats::TextureArchive::kFileBase);
    }

    // The corridors, from the SNI. The SNI's entries are the corridors and
    // the sounds, and the sounds are skipped.
    bytes.clear();
    path = std::string(base) + "O.SNI";
    if (data->read(path, &bytes)) {
        formats::Sni sni;
        if (formats::Sni::parse(&sni, bytes.data(), (int32_t)bytes.size())) {
            const std::vector<formats::Sni::Entry>& entries = sni.entries();
            for (size_t i = 0; i < entries.size(); i++) {
                if (sni.isSound(entries[i])) {
                    continue;
                }
                formats::Arena* corridor = formats::Arena::parseWorld(entries[i].name, bytes.data(),
                                                                       (int32_t)bytes.size(),
                                                                       entries[i].offset);
                if (!corridor) {
                    continue;
                }
                // An empty corridor is a placeholder quad of the NONE
                // material, and it is not a place.
                if (corridor->materials().size() == 1 && corridor->materials()[0] == "NONE") {
                    delete corridor;
                    continue;
                }
                out->corridors_.push_back(corridor);
                out->arenas_.push_back(corridor);
            }
        }
    }

    return true;
}

LevelData::~LevelData() {
    // The corridors are ours; the arenas belong to the MTO.
    for (size_t i = 0; i < corridors_.size(); i++) {
        delete corridors_[i];
    }
}

const formats::Arena* LevelData::arenaNamed(const std::string& name) const {
    for (size_t i = 0; i < arenas_.size(); i++) {
        if (arenas_[i]->name() == name) {
            return arenas_[i];
        }
    }
    return nullptr;
}

const formats::Dti::ArenaEntry* LevelData::entryNamed(const std::string& name) const {
    const std::vector<formats::Dti::ArenaEntry>& entries = dti_.arenas();
    for (size_t i = 0; i < entries.size(); i++) {
        if (entries[i].name == name) {
            return &entries[i];
        }
    }
    return nullptr;
}

bool LevelData::isReachable(const std::string& arenaName) const {
    // The C# LevelData.IsReachable: the starting arena, or one with a
    // connection record (type 6). A name not in the DTI (a corridor) is
    // reachable. Arenas without a connection start hidden and must not be in
    // the collision world, or one whose bounds overlap the playable column can
    // steal the smallest-arena selection and give Kurt the wrong BSP.
    const uint32_t kConnection = 6;
    const std::vector<formats::Dti::ArenaEntry>& arenas = dti_.arenas();
    int32_t index = -1;
    for (size_t i = 0; i < arenas.size(); i++) {
        if (arenas[i].name == arenaName) {
            index = (int32_t)i;
            break;
        }
    }
    if (index < 0 || index == dti_.startArena()) {
        return true;
    }
    for (size_t i = 0; i < arenas[index].records.size(); i++) {
        if (arenas[index].records[i].type == kConnection) {
            return true;
        }
    }
    return false;
}

float LevelData::arenaPitch(const std::string& arenaName) const {
    // C# Viewer.ArenaPitch: the DTI entry's pitch, else the default (4).
    const std::vector<formats::Dti::ArenaEntry>& arenas = dti_.arenas();
    for (size_t i = 0; i < arenas.size(); i++) {
        if (arenas[i].name == arenaName) {
            return arenas[i].pitch;
        }
    }
    return 4.0f;
}

formats::Palette& LevelData::paletteOf(const formats::Arena* arena) {
    std::map<const formats::Arena*, formats::Palette>::iterator found = palettes_.find(arena);
    if (found != palettes_.end()) {
        return found->second;
    }

    // A corridor has no colours of its own: it borrows its arena's.
    // CHMO_1 borrows HMO_1's, by dropping the corridor's first character.
    const formats::Arena* source = arena;
    if (source->paletteRgbBytes() == 0) {
        const std::string& name = source->name();
        const std::string owner = name.size() > 1 ? name.substr(1) : std::string();
        const formats::Arena* borrowed = owner.empty() ? nullptr : mto_.getArena(owner);
        source = borrowed ? borrowed : (arenas_.empty() ? arena : arenas_[0]);
    }

    formats::Palette made = dti_.palette().withArenaColors(source->paletteRgb(),
                                                            source->paletteRgbBytes());
    palettes_[arena] = made;
    return palettes_[arena];
}

std::vector<const formats::TextureArchive*> LevelData::archivesOf(const formats::Arena* arena) const {
    std::vector<const formats::TextureArchive*> archives;
    archives.push_back(&arena->textures());
    archives.push_back(&levelTextures_);
    for (size_t i = 0; i < arenas_.size(); i++) {
        archives.push_back(&arenas_[i]->textures());
    }
    return archives;
}

} // namespace mdk

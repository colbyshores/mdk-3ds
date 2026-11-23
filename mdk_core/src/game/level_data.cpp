// ============================================================================
// level_data.cpp — see level_data.h.
// ============================================================================

#include "game/level_data.h"

#include "formats/fti.h"
#include "scripts/pose_cache.h"

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
    // The pose caches are keyed by Model/ModelAnimation ADDRESS (the C#'s static
    // RestPoses/Baked), and the models being made here can land on addresses the
    // last level's models had. The C# drops the caches once per level, in the
    // Viewer's constructor (Viewer.cs:185) between building the LevelData and
    // laying anything out; doing it here is the same moment and covers every
    // consumer, including a harness with no viewer. Dropping them is never wrong:
    // they hold derived data, so the next pose is rebuilt from the model.
    scripts::forgetPoseCaches();

    // Loading into a LevelData that already holds one STARTS OVER, and a load
    // that FAILS leaves an EMPTY level, never a mixture of two levels. Every
    // field is released here, BEFORE the first read, because four of the six
    // reads below are individually fallible and a reset that missed a field
    // would leave the PREVIOUS level's DTI, MTO bytes or textures behind in an
    // object that claims to hold another one: archivesOf() searches the level
    // archive for every arena's materials, so the new level's arenas would
    // resolve their surfaces against the old level's textures. On a fresh
    // object every line here is a no-op.
    //
    // The MTO releases the arenas it parsed (mto.h), the corridors are ours to
    // delete, and the palette cache is keyed by arena pointer so it must not
    // outlive either. Without those two lines a second load would append a
    // second helping to arenas_ and leak the previous level's corridors.
    out->number_ = 0;
    out->dti_ = formats::Dti();
    out->palettes_.clear();
    for (size_t i = 0; i < out->corridors_.size(); i++) {
        delete out->corridors_[i];
    }
    out->corridors_.clear();
    out->corridorReadFailures_ = 0;
    out->arenas_.clear();
    // The stream first: it points into the arenas the next line frees.
    out->mto_ = formats::Mto();
    out->levelTextures_ = formats::TextureArchive();

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

    // The MTO is OPENED, not read whole: a level's archive is 4.5-9.8 MB and
    // the main heap cannot hold it next to the level's other data (measured
    // 2026-11-22: holding it whole OOMs the 40 MiB heap at level 7 load). The
    // streamed form keeps the directory and reads a block when asked -- and
    // almost nothing asks: the orig pack ships the arena textures as ETC1
    // files (game/hd_key.cpp), and only a texture absent from the pack falls
    // back to reading its arena's pixel prefix here, once, off the door frame
    // (ArenaViews::pumpSurfaces). The old arena stream is gone: it paced 32
    // KiB of SD reads INSIDE gameplay frames, which was the transition
    // stutter; a whole-arena prefix read, made while Kurt is still in the
    // corridor, replaces the pacing.
    path = std::string(base) + "O.MTO";
    if (!formats::Mto::open(&out->mto_, data, path)) {
        return false;
    }
    for (size_t i = 0; i < out->mto_.arenaNames().size(); i++) {
        formats::Arena* arena = out->mto_.getArena(out->mto_.arenaNames()[i]);
        if (arena) {
            // The pixels go as each arena is parsed, so the load never holds
            // more than one arena's: the stream brings them back (Phase 2,
            // attach() below points the textures at it).
            // (Only an arena the stream can read back: one whose block is not
            // the retail layout has no prefix and keeps what it parsed.)
            if (arena->texturePrefixBytes() > 0) {
                arena->dropTexturePixels();
            }
            out->arenas_.push_back(arena);
        }
    }
    // A block that could not be READ fails the load, as the whole-file read
    // used to (a block that reads but does not parse is skipped, as before).
    if (out->mto_.readFailures() > 0) {
        out->arenas_.clear();
        out->mto_ = formats::Mto();
        return false;
    }

    // The level's own textures, shared by every arena.
    bytes.clear();
    path = std::string(base) + "S.MTI";
    if (data->read(path, &bytes)) {
        out->levelTextures_ = formats::TextureArchive::parse(bytes.data(), (int32_t)bytes.size(),
                                                             formats::TextureArchive::kFileBase);
    }

    // The corridors, from the SNI. The SNI's entries are the corridors and
    // the sounds, and the sounds are skipped -- NOT READ: the archive is opened
    // by its directory and each corridor's entry read on its own (the
    // original's 0x431914 seeks to the entry and reads it). The file is 3.3-4.5
    // MB of music around 0.2-0.5 MB of corridors, and reading it whole was the
    // largest single read of a level's load (docs/ARENA_STREAMING_SPEC.md
    // Phase 5). A corridor's world section is read front to back from its
    // entry's first byte, so the entry's bytes parse as the whole file's did
    // (tests/formats: every retail corridor both ways).
    bytes.clear();
    path = std::string(base) + "O.SNI";
    {
        formats::Sni sni;
        if (!formats::Sni::open(&sni, *data, path)) {
            out->corridorReadFailures_++;
        } else {
            const std::vector<formats::Sni::Entry>& entries = sni.entries();
            for (size_t i = 0; i < entries.size(); i++) {
                if (sni.isSound(entries[i])) {
                    continue;
                }
                std::vector<uint8_t> entryBytes;
                if (!sni.getBytes(entries[i], &entryBytes)) {
                    // A corridor that cannot be read is a hole in the level; it
                    // is counted so the frontend can say so (it was all the
                    // corridors or none while the file was read whole).
                    out->corridorReadFailures_++;
                    continue;
                }
                formats::Arena* corridor = formats::Arena::parseWorld(
                    entries[i].name, entryBytes.data(), (int32_t)entryBytes.size(), 0);
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

    // THE PIXELS' FALLBACK SOURCE. The arenas' pixel prefixes were dropped at
    // parse (the loop above); a texture whose pixels are asked for -- one
    // absent from the orig pack, or an oversized strip the pack refused -- has
    // its arena's prefix read once (a whole-arena read, not the old stream's
    // 32 KiB a frame) and re-parsed here. The orig pack covers nearly every
    // texture, so on a shipped build this path is rare; the pump makes the
    // textures before the arena is drawn, so the read never lands on a door
    // frame either.
    for (size_t i = 0; i < out->arenas_.size(); i++) {
        if (out->arenas_[i]->texturePrefixBytes() > 0) {
            const_cast<formats::Arena*>(out->arenas_[i])->setPixelSource(
                &LevelData::loadPixelsFromMto, out);
        }
    }

    // The number is set only on success. A level that failed to load is an
    // empty object, and it must not claim a number it has no files for.
    out->number_ = number;
    return true;
}

// Read one arena's texture prefix back from the MTO and adopt the pixels of
// the texture the archive asked for -- the same parse the level's load ran over
// the same bytes, so the pixels are the ones that were dropped. `user` is the
// LevelData that owns the MTO; the borrow cannot dangle (LevelData is neither
// copyable nor movable). The read is the arena's whole prefix, once; the old
// arena stream's 32 KiB stepping is gone.
void LevelData::loadPixelsFromMto(void* user, const formats::Texture* texture) {
    LevelData* self = static_cast<LevelData*>(user);
    const std::vector<std::string>& names = self->mto_.arenaNames();
    for (size_t i = 0; i < names.size(); i++) {
        formats::Arena* arena = self->mto_.getArena(names[i]);
        if (arena == nullptr || !arena->ownsTexture(texture)) {
            continue;
        }
        const int32_t prefix = arena->texturePrefixBytes();
        if (prefix <= 0) {
            return;
        }
        std::vector<uint8_t> bytes;
        if (!self->mto_.readBlock(names[i], 0, prefix, &bytes)) {
            return;
        }
        formats::TextureArchive fresh = formats::TextureArchive::parse(bytes.data(), prefix, 4 + 0x10);
        arena->adoptTexturePixels(&fresh);
        return;
    }
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

formats::Arena* LevelData::arenaNamedMutable(const std::string& name) {
    for (size_t i = 0; i < arenas_.size(); i++) {
        if (arenas_[i]->name() == name) {
            return const_cast<formats::Arena*>(arenas_[i]);
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
        if (borrowed) {
            // Share the owner's palette OBJECT, not a copy of its bytes. The
            // surface cache keys a texture by (texture, palette), so a copied
            // palette made the corridor re-expand every strip its arena already
            // had under its own key: measured 134 ms of duplicate expansion two
            // frames after the arena dropped (Old 3DS, 2026-11-23, frame 827).
            // The bytes are identical either way -- same construction below --
            // so no pixel can tell the difference, and the golden proves it.
            return paletteOf(borrowed);
        }
        source = arenas_.empty() ? arena : arenas_[0];
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

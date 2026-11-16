#pragma once

// ============================================================================
// level_data.h — the files of one level.
//
// A 1:1 port of src/Mdk.Game/Level/LevelData.cs.
//
// A LEVEL IS FOUR FILES:
//
//   LEVELn.DTI   the settings: the start, the sky, the base palette
//   LEVELnO.MTO  the arenas
//   LEVELnS.MTI  the level's own textures, shared by every arena
//   LEVELnO.SNI  the corridors between the arenas, and the sounds
//
// THE PALETTE IS ASSEMBLED IN THREE PIECES, and the order matters:
//
//   indices 0        black, and TRANSPARENT for a sprite
//   indices 1-63     the SYSTEM colours, from SYS_PAL in MISC/MDKFONT.FTI
//   indices 64-175   the ARENA's own 112 colours
//   indices 176-255  the level's base colours, from the DTI
//
// The system colours are applied to the DTI's palette ONCE, at load, and
// the arena's colours are applied PER ARENA, because an arena's palette is
// what the arena's textures are drawn through.
//
// THE SYSTEM COLOURS STAY THE SYSTEM'S. The DTI's palette is copied with
// only its 64-255 range: LEVEL8's DTI has magenta and purple at 10-12, for
// its aliens' glow, its shots and the grenade's icon, and those are the
// SYSTEM's colours, not the level's.
// ============================================================================

#include <stdint.h>

#include <map>
#include <string>
#include <vector>

#include "formats/arena.h"
#include "formats/dti.h"
#include "formats/mdk_data.h"
#include "formats/mto.h"
#include "formats/palette.h"
#include "formats/sni.h"
#include "formats/texture_archive.h"

namespace mdk {

// The files of one level.
class LevelData {
public:
    LevelData() = default;

    // Load a level. Returns false when its files cannot be read.
    static bool load(LevelData* out, const formats::MdkData* data, int32_t number);

    int32_t number() const { return number_; }

    const formats::Dti& dti() const { return dti_; }
    // The DTI, writable. The 1996 demo builds a Dti by hand rather than parsing
    // one (the C# LevelData.Arenas is a mutable list), so the record tables have
    // to be settable from outside; the synthetic DTI fixtures in tests/scripts
    // append their own arena records through this, exactly as the demo does.
    // A ScriptRuntime copies the records of each arena it looks up, so replacing
    // the arenas here does not dangle a live runtime's cache -- but a runtime
    // built before the change keeps its earlier snapshot, so mutate before you
    // construct (the fixtures inject their arena before any runtime is built).
    formats::Dti& mutableDti() { return dti_; }
    formats::Mto& mto() { return mto_; }
    const formats::TextureArchive& levelTextures() const { return levelTextures_; }

    // The arenas, then the corridors (a corridor CHMO_1 follows the arena
    // HMO_1).
    const std::vector<const formats::Arena*>& arenas() const { return arenas_; }

    // The arena with that name, or null.
    const formats::Arena* arenaNamed(const std::string& name) const;

    // The arena with that name, writable. The C# LevelData.Arenas is a mutable
    // list, and the group machinery (the C# TriangleGroups) converts flagged
    // triangles and stores per-group state ON the arenas, so the play setup and
    // the scripts need write access. The arenas are owned by the MTO and the
    // level's corridor list, neither of which makes them const: the MTO's
    // destructor frees the ones it parsed (mto.h) and ~LevelData frees the
    // corridors.
    formats::Arena* arenaNamedMutable(const std::string& name);

    // The DTI entry of the arena of that name, or null. Case-sensitive, like
    // the C#'s `entry.Name == name`. (C# LevelData.EntryNamed.)
    const formats::Dti::ArenaEntry* entryNamed(const std::string& name) const;

    // Whether an arena can be entered from the start: the starting one, or one
    // with a connection (a DTI record of type 6). A corridor, which is not
    // listed in the DTI, is always reachable. (The C# LevelData.IsReachable.)
    bool isReachable(const std::string& arenaName) const;

    // The DTI camera pitch (degrees, positive looks down) for the arena with
    // that name, or the default (4) when it is not listed. (C# Viewer.ArenaPitch.)
    float arenaPitch(const std::string& arenaName) const;

    // THE PALETTE AN ARENA DRAWS WITH: the level's, with the arena's own
    // 112 colours. A corridor borrows its arena's: CHMO_1 from HMO_1.
    // NOT const: the palette is made once per arena and cached, and making
    // it needs the MTO's arena cache, which is not const.
    formats::Palette& paletteOf(const formats::Arena* arena);

    // THE ARCHIVES SEARCHED FOR AN ARENA'S MATERIALS: its own, the level's,
    // then every other arena's. Some materials are borrowed -- the O3_*
    // ones in level 6 are in another arena's archive.
    std::vector<const formats::TextureArchive*> archivesOf(const formats::Arena* arena) const;

private:
    int32_t number_ = 0;
    formats::Dti dti_;
    // The MTO file bytes, OWNED here: the MTO borrows them, and the arenas it
    // parses on demand (and their animations' track data) borrow them too.
    // Declared before mto_ so it outlives it (members destroy in reverse order).
    std::vector<uint8_t> mtoBytes_;
    formats::Mto mto_;
    formats::TextureArchive levelTextures_;
    std::vector<const formats::Arena*> arenas_;

    // The corridors, which the level owns. The arenas belong to the MTO.
    std::vector<formats::Arena*> corridors_;

    // The palettes, by arena. They are made once each, because the GPU
    // copy of a palette is made once too.
    mutable std::map<const formats::Arena*, formats::Palette> palettes_;

public:
    ~LevelData();
};

} // namespace mdk

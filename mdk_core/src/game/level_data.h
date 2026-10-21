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
    formats::Mto& mto() { return mto_; }
    const formats::TextureArchive& levelTextures() const { return levelTextures_; }

    // The arenas, then the corridors (a corridor CHMO_1 follows the arena
    // HMO_1).
    const std::vector<const formats::Arena*>& arenas() const { return arenas_; }

    // The arena with that name, or null.
    const formats::Arena* arenaNamed(const std::string& name) const;

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

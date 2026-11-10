#pragma once

// ============================================================================
// beta_demo.h — the beta demo of 6 August 1996 (MDKDEMO.EXE).
//
// THE C# ORIGINAL is src/Mdk.Formats/BetaDemo.cs. This is a 1:1 port.
//
// THE DEMO is the game in an earlier state: three levels (1, 3 and 6) in
// loose files and in earlier versions of the retail formats. They are read
// into the retail loaders' objects -- Dti, Mto, Cmi -- so that the game runs
// them like its own levels, numbered 961, 963 and 966.
//
//   LEVELn.SET, LEVELn.CON, ARENAS/*.HOT ──► Dti
//   ARENAS/*.BSP, LEVELnO.MTO ───────────► Mto
//   LEVELnS.MTI, *.LBA ──────────────────► TextureArchive
//   LEVELn.CMI ──────────────────────────► Cmi (the demo's scripts)
//   *.SNI ───────────────────────────────► Sni
//   SPRITES/*.ABB, *.LBB ────────────────► TRAVSPRT.BNI entries
//
// THE SETTINGS ARE TEXT. A level's .SET file names the starting arena and
// Kurt's place in it, the sky's colours, and the four glass colours; the
// .CON file names the arenas and corridors; an arena's .HOT file holds its
// object records. The demo's records are the retail DTI's, so they are
// parsed into Dti.Record.
//
// THE FILES ARE READ THROUGH THE FileSystem SEAM, like every other format,
// so the 3DS reads them from its ROMFS tree (AGENTS.md P5).
//
// THE ARENAS BELONG TO THE CALLER. The C#'s Mto owns the arenas it is given;
// the C++'s Mto only points at the ones add() hands it, so the caller keeps
// them alive and deletes them when the Mto is done with. (The arenas an Mto
// parses out of its own bytes are its own — see mto.h.) So a caller of
// loadMto gets the arenas twice over, once borrowed inside the Mto and once
// in the vector it must free.
//
// THE OUT-VECTOR IS REPLACED, NOT APPENDED. It owns what it holds, so whatever
// it held when it was handed in is deleted by the call. A caller that passes a
// vector holding arenas from somewhere else is handing them over to be freed.
// ============================================================================

#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

#include "formats/arena.h"
#include "formats/bni.h"
#include "formats/cmi.h"
#include "formats/dti.h"
#include "formats/file_system.h"
#include "formats/mdk_data.h"
#include "formats/mto.h"
#include "formats/palette.h"
#include "formats/sni.h"
#include "formats/texture_archive.h"

namespace mdk {
namespace formats {

// The 1996 beta demo.
class BetaDemo {
public:
    // The demo's TRAVERSE/LEVELn folders.
    static constexpr int32_t kLevels[3] = {1, 3, 6};

    // Added to a demo level's number to tell it from the retail levels.
    static const int32_t kNumberBase = 960;

    // A triangle flag of the .BSP files: simple shapes around the detailed
    // ones, solid but not drawn, and left out of the triangle tests of rays.
    static const uint32_t kNotDrawn = 2;

    BetaDemo() = default;

    // The demo, or an empty one when it was not found.
    static BetaDemo find(const FileSystem* system, const MdkData* data);

    bool found() const { return found_; }

    // The folder with TRAVERSE and MDKDEMO.EXE.
    const std::string& dir() const { return dir_; }

    // Is this a demo level's number?
    static bool isBeta(int32_t number) { return number > kNumberBase; }

    // The demo's level (1, 3, 6) of a port's number (961, 963, 966).
    static int32_t levelOf(int32_t number) { return number - kNumberBase; }

    // The port's number of a demo level.
    static int32_t numberOf(int32_t level) { return kNumberBase + level; }

    // The level's loading screen (DEMO/SCREENn.LBB, in the demo's order).
    bool loadingScreen(int32_t level, std::vector<uint8_t>* outBytes) const;

    // The level's settings as the retail LEVELn.DTI has them.
    bool loadDti(int32_t level, Dti* outDti) const;

    // The level's arenas. The caller owns the arenas.
    bool loadMto(int32_t level, Mto* outMto, std::vector<Arena*>* outArenas) const;

    // The level's corridors. The caller owns them.
    std::vector<Arena*> loadCorridors(int32_t level) const;

    // The level's textures.
    bool loadTextures(int32_t level, TextureArchive* outArchive) const;

    // The level's sound archives, later ones winning.
    std::vector<Sni> loadSounds(int32_t level) const;

    // The level's scripts.
    bool loadCmi(int32_t level, Cmi* outCmi) const;

    // The demo's teleports, as an arena name and a place.
    std::vector<std::pair<std::string, Vec3f>> loadTeleports() const;

    // Puts the demo's versions of Kurt's sprites and the HUD's images over
    // the retail ones.
    void addSprites(Bni* sprites) const;

    // The level's arenas and corridors, in the order of LEVELn.CON, in upper
    // case.
    std::vector<std::string> arenaNames(int32_t level) const;

    // LEVELn.SET: the start arena's index and Kurt's position (and angle),
    // the sky's fill colours (top, bottom) and horizon row, then the four
    // glass colours "r g b opacity".
    static bool parseSettings(const std::string& text, Dti* outDti);

    // LEVELn.CON: the number of arenas, then per arena its name, a number of
    // portals and the portals ("C<arena index> side z plane from to", not
    // used).
    static bool parseArenaNames(const std::string& text, std::vector<std::string>* outNames);

    // An arena's .HOT file as DTI records, one per line:
    //
    //   ASHOW arena x1 y1 x2 y2          walking into the rectangle shows that
    //                                   arena (NONE: none)
    //   ALIEN type id n x y z            an alien, run by the script
    //                                   ARENA$TYPE_id
    //   PICKUP type x y z                a pickup
    //   HIDEPT id x y z                  a cover spot
    //   CONNECT id x1 x2 y1 y2 z1 z2     a doorway between an arena and a
    //                                   corridor, in pairs
    //   MSWAP six numbers                a material swap (in no file)
    static bool parseRecords(const std::string& text, const std::vector<std::string>& arenaNames,
                             std::vector<Dti::Record>* outRecords);

    // The demo's teleports, TRAVERSE/TELEPORT.TXT: a line "arena x y z" per
    // digit (the first is 0; 0x440c4).
    static std::vector<std::pair<std::string, Vec3f>> parseTeleports(const std::string& text);

    // The demo's walls and models use palette index 0 as black, where the
    // retail game keeps it for the see-through parts of effects: it becomes
    // 16, except in animated textures and in the see-through ones.
    static void makeOpaque(TextureArchive* archive);

    // Palette indices drawn with `from` as the nearest colours of `to`.
    static std::vector<uint8_t> remap(const std::vector<uint8_t>& indices, const Palette& from,
                                       const Palette& to);

    // The nodes whose listed triangles do not lie on their plane: none when
    // the demo's node layout is read right.
    //
    // A triangle that names a vertex the arena does not have counts as a bad
    // node: the C# throws there, and the answer it wants -- that the node is
    // wrong -- is the one given here.
    static int32_t checkNodes(const Arena& arena);

private:
    // A file's bytes, or none when it is missing.
    bool read(const std::string& relative, std::vector<uint8_t>* outBytes) const;

    // A file's text.
    std::string text(const std::string& relative) const;

    // An arena's geometry, ARENAS/name.BSP.
    Arena* loadWorld(int32_t level, const std::string& name) const;

    // The path of a file of the demo.
    std::string pathOf(const std::string& relative) const;

    const FileSystem* system_ = nullptr;
    std::string dir_;
    bool found_ = false;
};

} // namespace formats
} // namespace mdk

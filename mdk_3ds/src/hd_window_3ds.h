#pragma once

// ============================================================================
// hd_window_3ds.h -- THE HD WINDOW (docs/3DS_ARCHITECTURE.md 3 and 4): which
// arenas' HD copies are resident while Kurt is in an arena, and the size cap of
// every HD file, chosen once at level load.
//
// THE RULE (owner 2026-11-26). The window of an arena is the arena itself, its
// doorway neighbours, and the arenas its trigger boxes and scripts name (a
// teleport, an arena_show, a preload). The arenas the frame draws are always in
// it. Nothing else of the level is resident: the level's whole HD set is 6-23
// MiB at 512, one window at most 8.8 MiB (level 8, CGUNT_9).
//
// THE FIT. Every arena's window must fit the room the linear heap has for HD at
// level load. When one does not, the arena in it whose files save the most
// steps down a tier (512 -> 256 -> 128, a file's levels over the cap dropped at
// the read), and the check runs again -- never the whole level (3.3). Measured
// for the shipped levels: every window fits at 512 in a 30 MiB linear heap.
//
// Plain bookkeeping: no libctru, no renderer. The neighbours come from the
// level's data (addLevelNeighbours); the files and their bytes from the packs
// (packs_3ds.cpp). Host-tested: tests/scripts/hd_window_tests.cpp.
// ============================================================================

#include <stdint.h>

#include <map>
#include <string>
#include <vector>

namespace mdk {

namespace formats {
class Dti;
class Cmi;
}

class HdWindow {
public:
    // The tiers: a file as built (512), at most 256, at most 128 texels a side.
    static const int32_t kTiers = 3;

    void clear();

    // LEVEL LOAD. An arena's HD file, with its imported bytes at each tier
    // (hdChainBytes at 0, 256, 128). A file named by several arenas is one file.
    void addFile(const std::string& arena, const std::string& path, const uint32_t bytes[kTiers]);
    // `other` is in `arena`'s window (one way; a doorway is added both ways).
    void addNeighbour(const std::string& arena, const std::string& other);

    // Steps arenas down until every window is at most `room` bytes, or nothing
    // can step further. Returns the largest window's bytes after.
    uint32_t fit(uint32_t room);

    // THE WINDOW for Kurt in `current` with `drawn` on screen: its files and their
    // caps (0, 256, 128), the drawn arenas' files first -- `*drawnCount` of them,
    // what the frame cannot be drawn without. The paths point into this object.
    void window(const std::string& current, const std::vector<std::string>& drawn,
                std::vector<const char*>* paths, std::vector<int32_t>* caps,
                int32_t* drawnCount) const;
    // One arena's window in bytes, at the caps fit() chose.
    uint32_t windowBytes(const std::string& arena) const;

    int32_t arenaCount() const { return (int32_t)arenas_.size(); }
    int32_t fileCount() const { return (int32_t)files_.size(); }
    // The arenas fit() stepped down, and to which cap, for the log.
    const std::vector<std::pair<std::string, int32_t>>& steppedDown() const { return stepped_; }
    // The window's arenas, for the log and the tests.
    void windowArenas(const std::string& current, const std::vector<std::string>& drawn,
                      std::vector<std::string>* out) const;

private:
    struct File {
        std::string path;
        uint32_t bytes[kTiers] = {0, 0, 0};
        int32_t tier = 0;   // the largest tier of the arenas that hold it
    };
    struct Arena {
        std::string name;
        std::vector<int32_t> files;
        std::vector<std::string> neighbours;
        int32_t tier = 0;
    };

    int32_t find(const std::string& name) const;
    Arena& named(const std::string& name);
    void filesOf(const std::vector<std::string>& arenas, std::vector<int32_t>* out) const;
    void retier();

    std::vector<File> files_;
    std::map<std::string, int32_t> fileByPath_;
    std::vector<Arena> arenas_;
    std::vector<std::pair<std::string, int32_t>> stepped_;
};

// THE NEIGHBOURS THE LEVEL NAMES: the DTI's doorways (a type-6 record in each of
// two arenas, sharing an id: both ways), its show and load trigger boxes (types
// 1 and 3, the id an index into the arena list: from the box's arena), and the
// arenas the CMI's arena and object scripts name with arena_show (100),
// teleport_player (112), teleport_player_keep (173) and arena_set_neighbour
// (223): from the script's arena (an object script's name is ARENA$OBJECT).
// `cmi` may be null. Returns how many script-named neighbours were added.
int32_t addLevelNeighbours(const formats::Dti& dti, const formats::Cmi* cmi, HdWindow* out);

} // namespace mdk

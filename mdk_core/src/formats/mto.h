#pragma once

// ============================================================================
// mto.h — LEVELnO.MTO: a level's arenas.
//
// A 1:1 port of src/Mdk.Formats/Mto.cs.
//
// THE FILE: a directory at 0x14, of u32 count and then per entry a char[8]
// name and a u32 offset to the arena's block, in file order.
//
// THE ARENAS ARE PARSED ON DEMAND. A level's MTO is 8 MB and holds ten
// arenas, and the game only ever draws the current one and the one behind
// an open door, so parsing them all up front would be both slow and memory
// hungry. The offsets are cheap; the parsing is not.
//
// The C# caches the parsed arenas in a dictionary. This does the same, and
// the cache is what keeps a corridor that is re-entered from being parsed
// twice.
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

namespace mdk {
namespace formats {

class Arena;

// A level's arena file.
class Mto {
public:
    // An empty MTO: one whose arenas come from elsewhere, like the 1996
    // demo's loose files.
    Mto() = default;

    // Parse the directory of an MTO. The arenas themselves are not parsed
    // yet.
    static Mto parse(const uint8_t* bytes, int32_t byteCount);

    // Add an arena that was read from elsewhere.
    void add(const Arena* arena);

    // The arena names, in file order.
    const std::vector<std::string>& arenaNames() const { return names_; }

    // The file offset of an arena's block, in file order.
    const std::vector<int32_t>& arenaOffsets() const { return offsets_; }

    bool has(const std::string& name) const;

    // The arena with this name, parsed on demand and cached. Returns null
    // when there is no such arena.
    const Arena* getArena(const std::string& name);

private:
    // The buffer the arenas are parsed from. It is BORROWED: the caller
    // owns it, and the Mto does not outlive it.
    const uint8_t* bytes_ = nullptr;
    int32_t byteCount_ = 0;

    std::vector<std::string> names_;
    std::vector<int32_t> offsets_;

    // The arenas parsed so far, by name. The values are the arenas the
    // caller owns.
    std::vector<std::pair<std::string, Arena*>> arenas_;
};

} // namespace formats
} // namespace mdk

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
//
// WHO OWNS THE ARENAS. Two ways an arena gets into an Mto, and they are not
// the same:
//
//   getArena()  parses one out of bytes_ and caches it. The Mto MADE it, so
//               the Mto frees it, in ~Mto. The C#'s Dictionary<string, Arena>
//               owns them the same way (the GC is the destructor).
//   add()       is handed one. The caller keeps it and deletes it; the Mto
//               only points at it. That is the 1996 demo's contract, see
//               beta_demo.h's "THE ARENAS BELONG TO THE CALLER".
//
// Before this split was written down, nothing freed the getArena() ones at
// all: a level's arenas leaked on every level load, and a script that asked
// for an arena late in a level (ScriptRuntime's FindModel path) leaked one
// per ask. ASan on tests/formats measured 47394202 bytes in 15442
// allocations, every one of them under Mto::getArena.
class Mto {
public:
    // An empty MTO: one whose arenas come from elsewhere, like the 1996
    // demo's loose files.
    Mto() = default;

    // Frees the arenas this Mto parsed for itself. It does NOT touch the
    // arenas it was given with add().
    ~Mto();

    // Movable, never copyable. A copy would put the same owned Arena* in two
    // Mtos and both would delete it, so the copy operations are gone rather
    // than quietly wrong. Declaring ~Mto() suppresses the implicit moves, so
    // both are spelled out here: Mto::parse returns by value and LevelData::load
    // move-assigns the result, and both need them. Neither is defaulted: the
    // assignment has to release what the target already owned, and both have to
    // leave the source without a buffer it can no longer index. See mto.cpp.
    Mto(Mto&&) noexcept;
    Mto& operator=(Mto&&) noexcept;
    Mto(const Mto&) = delete;
    Mto& operator=(const Mto&) = delete;

    // Parse the directory of an MTO. The arenas themselves are not parsed
    // yet.
    static Mto parse(const uint8_t* bytes, int32_t byteCount);

    // Add an arena that was read from elsewhere. The Mto does not own it and
    // will not delete it; the caller must keep it alive for the Mto's life.
    void add(const Arena* arena);

    // The arena names, in file order.
    const std::vector<std::string>& arenaNames() const { return names_; }

    // The file offset of an arena's block, in file order.
    const std::vector<int32_t>& arenaOffsets() const { return offsets_; }

    bool has(const std::string& name) const;

    // The arena with this name, parsed on demand and cached. Returns null
    // when there is no such arena.
    Arena* getArena(const std::string& name);

private:
    // The buffer the arenas are parsed from. It is BORROWED: the caller
    // owns it, and the Mto does not outlive it.
    const uint8_t* bytes_ = nullptr;
    int32_t byteCount_ = 0;

    std::vector<std::string> names_;
    std::vector<int32_t> offsets_;

    // The arenas parsed so far, by name: the ones this Mto made AND the ones
    // add() handed in. The cache does not say which is which, so owned_ does.
    std::vector<std::pair<std::string, Arena*>> arenas_;

    // The arenas getArena() parsed out of bytes_. ~Mto deletes exactly these.
    // add()ed arenas are deliberately absent: the caller owns those.
    std::vector<Arena*> owned_;
};

} // namespace formats
} // namespace mdk

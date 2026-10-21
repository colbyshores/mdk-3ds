#pragma once

// ============================================================================
// arena_space.h — the arenas Kurt can be in, and his moves through their BSPs.
//
// A 1:1 port of the movement half of src/Mdk.Game/Collision/ArenaSpace.cs
// (damp_collide_move 0x465e34): his arena with sliding, then, if that ended
// free, the arena he moves into without. The arena around a point is the
// smallest whose bounds (grown by 2) hold it.
//
//   feet ──► box (0.6,0.6,2.5) at feet+0.5 (walk) │ (0.4,0.4,2.5) at feet+0.01 (fall)
//        ──► Kurt's arena: 4 slides ──free?──► the other arena: no slide ──► new feet
//
// The solid-object pass and the ledge/camera probes (ArenaSpace.Probes.cs)
// land with the entities and camera ports; this is the BSP move Kurt needs to
// walk and fall.
// ============================================================================

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "formats/arena.h"
#include "game/bsp.h"
#include "game/vec_util.h"

namespace mdk {

class ArenaSpace {
public:
    enum Motion { kMotionWalk, kMotionFall };

    struct Result {
        Vec feet;
        int triangle;
        Bsp* bsp;
        int node;
        bool hit() const { return triangle != Bsp::kNone; }
        // The plane's normal of the contact, or up without one.
        Vec normal() const {
            if (bsp == nullptr || node < 0) {
                return Vec{0.0f, 0.0f, 1.0f};
            }
            const formats::BspNode& n = bsp->arena()->nodes()[node];
            return Vec{n.normal[0], n.normal[1], n.normal[2]};
        }
    };

    // Adds an arena once (a teleport may add one no connection leads to).
    void add(const formats::Arena* arena);

    // Moves Kurt's feet by delta, sliding along faces met obliquely enough
    // ((D·n)^2 <= slideK·|D|^2).
    Result move(Vec feet, Vec delta, Motion motion, float slideK);

    // The lowest point of Kurt's arena (or of an arena around the feet), or
    // false when there is none.
    bool bottom(Vec feet, float* out);

    // The first face a box of half extents half meets from from to to (centres),
    // in Kurt's arena then the second (C# ArenaSpace.Sight, camera_clearance):
    // the box's centre there and the face's plane. False when nothing is met.
    bool sight(Vec from, Vec to, Vec half, Vec* outPoint, const formats::BspNode** outPlane);

    // Whether the segment crosses a face of Kurt's arena (C# ArenaSpace.Crosses).
    bool crosses(Vec from, Vec to);

    // The name of the arena around a point, or null.
    const std::string* arenaAt(Vec point) const;

private:
    struct Entry {
        Bsp bsp;
        const formats::Arena* arena;
        Vec min;
        Vec max;
        float volume;
        Entry(const formats::Arena* a, Vec mn, Vec mx, float vol)
            : bsp(a), arena(a), min(mn), max(mx), volume(vol) {}
    };

    static bool contains(Vec min, Vec max, Vec p);
    const Entry* firstEntry(Vec point) const;
    void addAt(Vec point, std::vector<Bsp*>& out);
    Bsp* kurt(Vec point);
    // Kurt's arenas for a probe: the solid ones, else those around the point
    // (C# ArenaSpace.Kurts). Returns a pointer to the scratch or _solid.
    const std::vector<Bsp*>* kurts(Vec point);

    // THE ENTRIES ARE HEAP-ALLOCATED (unique_ptr), not stored by value in the
    // vector. The C# holds Bsp by reference (List<Bsp>), so a Bsp's address
    // never moves when the list grows; a Result or a Kurt contact may hold a
    // Bsp* across a later add() (a teleport). Storing Entry by value would let
    // a vector insert relocate an Entry and silently alias a held Bsp* to the
    // wrong arena. unique_ptr keeps each Entry at a stable address.
    std::vector<std::unique_ptr<Entry>> _arenas;
    std::vector<Bsp*> _solid;   // empty: those around Kurt by bounds are used
    std::vector<Bsp*> _around;  // scratch for the move's arena list
    std::vector<Bsp*> _kurts;   // scratch for kurts()/sight()
};

} // namespace mdk

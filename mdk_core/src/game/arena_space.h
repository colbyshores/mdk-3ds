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

    // An up-facing surface crossed by a segment: where it was crossed and
    // the plane's normal (C# ArenaSpace.Surface).
    struct Floor {
        Vec point;
        Vec normal;
    };

    // Adds an arena once (a teleport may add one no connection leads to).
    void add(const formats::Arena* arena);

    // Pins Kurt's arena to the one with that name (C# ArenaSpace.SetSolid). While
    // set, the move, the sight/crosses probes and bottom() treat it as Kurt's
    // arena even when his feet leave its bounds -- the sticky arena the original
    // keeps (arena+0x44e) and the scripts refresh on a crossing. Because it is
    // sticky, a frontend WITHOUT the scripts has to refresh it itself as Kurt moves
    // (C# Viewer.cs:533 refreshes it every step). WHAT it refreshes to is not the
    // same on every frontend, and the difference is measured, not stylistic: the
    // oracle's value is the SCRIPTS' drawn arenas (ScriptRuntime.cs:265), which
    // move only when a move crosses a doorway connection (CrossedArena, :791) or a
    // teleport puts him elsewhere -- they do NOT follow the geometric arena, and in
    // particular they do not move while he falls out of the world (measured: the
    // oracle's own pin rows, golden/L7_spawn_fall.txt's account). BOTH frontends
    // here use the geometric arena under his feet as the stand-in, IN BOTH OF THEIR
    // BRANCHES -- with a script runtime and without one -- and they can do nothing
    // else while the connections are unported: ScriptRuntime::currentArena_ is
    // written once, on the first tick, so a frontend that pinned the runtime's
    // current arena would freeze the collision world at the spawn arena for the
    // whole level. That is not a hypothetical: review-6 F1 found it on the 3DS
    // frontend and review-7 F1 found the same frozen pin on the Linux one, which is
    // the frontend that takes the pinned frame. Both now refresh from the feet,
    // and tests/scripts/frontend_solid_pin_test.sh reads both frontends for it.
    // Connections are not
    // ported, so neither stand-in is the oracle's rule; both are better than a pin
    // left at the spawn. And every frontend must pin at the spawn, because a
    // Kurt standing outside every arena otherwise leaves bottom() with nothing to
    // resolve and the fall-out death never fires.
    // The oracle passes a LIST (the current arena plus the active second) and this
    // takes one name, so the drawn list is not even expressible here until a
    // two-name form lands; `_solid` is already a vector and kurt() reads element 0.
    // Clears the pin if no arena has that name (then the arenas around the feet are
    // used, as before).
    void setSolid(const std::string& name);

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

    // The first up-facing face a segment crosses in Kurt's arena, then the
    // second (C# ArenaSpace.FloorCrossing). Writes the crossing point and
    // the face's normal when one is found.
    bool floorCrossing(Vec from, Vec to, Floor* out);

    // The NEAREST up-facing face a segment crosses (C# ArenaSpace.Floor, 0x421708),
    // written to *out. This is a DIFFERENT probe from floorCrossing and not a
    // variant of it: it searches the arenas around BOTH ends (AddAt(from) and
    // AddAt(to)) rather than Kurt's arenas around `from`, it ignores the pinned
    // solid arena that floorCrossing's `kurts()` uses, and it keeps the hit
    // CLOSEST to `from` by squared distance rather than the first one found in
    // arena order. Kurt's knock-down uses this one (Kurt.Combat.cs:217 probes 13
    // units straight down from his feet); the chute sprite's uses the other.
    // Writing them as one function is the mistake to avoid: they agree on a flat
    // floor under Kurt and disagree across an arena boundary, which is exactly
    // where a mid-air knock-down lives.
    bool floor(Vec from, Vec to, Vec* out);

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
    std::vector<Bsp*> _solid;   // Kurt's pinned arena (setSolid); empty = those around him by bounds
    std::vector<Bsp*> _around;  // scratch for the move's arena list
    std::vector<Bsp*> _kurts;   // scratch for kurts()/sight()
};

} // namespace mdk

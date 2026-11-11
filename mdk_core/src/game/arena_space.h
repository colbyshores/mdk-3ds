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

    // Pins Kurt's collision arenas to the named ones (C# ArenaSpace.SetSolid,
    // ArenaSpace.cs:45-55). The list is cleared and then EVERY name is resolved in
    // order: C# Named() answers the first arena with that name and null for a name
    // no arena has, so an unknown name -- including the "" the scripts use for "no
    // arena" -- contributes nothing, and an empty list leaves nothing pinned. While
    // the pin is non-empty, the move, the sight/crosses probes and bottom() use it
    // and ignore the arenas around the feet -- the sticky arena the original keeps
    // (arena+0x44e) -- and `move()` collides with element 0 first and the rest as
    // the free pass, exactly as the C# does. Because it is sticky it has to be
    // refreshed every step, which is what the oracle does
    // (`_space.SetSolid(_scripts.SolidArenas)`, Viewer.cs:533).
    //
    // WHAT THE FRONTENDS PASS. With a script runtime, `ScriptRuntime::solidArenas()`:
    // the oracle's own value, the scripts' DRAWN list (the current arena plus the
    // active second, ScriptRuntime.cs:265-278 and UpdateArenas :660-666). It moves
    // when a move crosses a doorway connection (CrossedArena, :791) or a teleport
    // puts Kurt elsewhere, and it does NOT move while he falls out of the world
    // (measured: the oracle's own pin rows, golden/L7_spawn_fall.txt's account).
    // Both frontends pass that now; they used to pass the geometric arena under
    // Kurt's feet in both of their branches, because `currentArena_` was written
    // once on the first tick while the connections were unported and pinning it
    // froze the collision world at the spawn arena for a whole level (review-6 F1
    // on the 3DS frontend, review-7 F1 on the Linux one, which takes the pinned
    // frame). Batch 134 ported the crossing and this batch ported the list, so the
    // frozen-pin argument and the arity argument that replaced it are both gone.
    //
    // The branch with NO runtime -- the asset gate, where a level missing its CMI,
    // bank or sprites archive runs without scripts -- has no drawn list to read, so
    // it keeps the geometric arena as a stand-in. It is a stand-in, not the oracle's
    // rule: it follows the feet, so it can re-pin during a fall-out, and it carries
    // one name, so a door walk leaves the room behind un-solid. It still beats no
    // pin at all, and tests/scripts/frontend_solid_pin_test.sh reads both frontends
    // for exactly this two-branch shape.
    //
    // Every frontend also pins at the spawn, because a Kurt standing outside every
    // arena otherwise leaves bottom() with nothing to resolve and the fall-out death
    // never fires -- and the runtime's list resolves to nothing until its first tick
    // has set the current arena.
    void setSolid(const std::vector<std::string>& names);

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
    std::vector<Bsp*> _solid;   // Kurt's pinned arenas (setSolid); empty = those around him by bounds
    std::vector<Bsp*> _around;  // scratch for the move's arena list
    std::vector<Bsp*> _kurts;   // scratch for kurts()/sight()
};

} // namespace mdk

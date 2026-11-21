// ============================================================================
// arena_space.cpp — the BSP move. See arena_space.h. A 1:1 port of the move
// half of ArenaSpace.cs.
// ============================================================================

#include "game/arena_space.h"

#include <cfloat>

namespace mdk {

namespace {

const Vec kWalkBox = {0.6f, 0.6f, 2.5f};
const Vec kFallBox = {0.4f, 0.4f, 2.5f};
// The box's bottom above the feet: anything lower is walked over.
const float kWalkLift = 0.5f;
const float kFallLift = 0.01f;
const int kSlides = 4;
const float kBoundsMargin = 2.0f;

} // namespace

bool ArenaSpace::contains(Vec min, Vec max, Vec p) {
    return p.x >= min.x - kBoundsMargin && p.y >= min.y - kBoundsMargin &&
           p.z >= min.z - kBoundsMargin && p.x <= max.x + kBoundsMargin &&
           p.y <= max.y + kBoundsMargin && p.z <= max.z + kBoundsMargin;
}

void ArenaSpace::add(const formats::Arena* arena) {
    if (arena->vertices().empty()) {
        return;
    }
    for (const auto& up : _arenas) {
        if (up->arena == arena) {
            return;
        }
    }

    const auto& V = arena->vertices();
    Vec mn = V[0];
    Vec mx = V[0];
    for (const Vec& v : V) {
        if (v.x < mn.x) mn.x = v.x;
        if (v.y < mn.y) mn.y = v.y;
        if (v.z < mn.z) mn.z = v.z;
        if (v.x > mx.x) mx.x = v.x;
        if (v.y > mx.y) mx.y = v.y;
        if (v.z > mx.z) mx.z = v.z;
    }
    Vec size = vSub(mx, mn);
    float volume = size.x * size.y * size.z;

    size_t at = _arenas.size();
    while (at > 0 && _arenas[at - 1]->volume > volume) {
        at--;
    }
    _arenas.insert(_arenas.begin() + at, std::make_unique<Entry>(arena, mn, mx, volume));
}

void ArenaSpace::setSolid(const std::vector<std::string>& names) {
    // C# ArenaSpace.SetSolid (ArenaSpace.cs:45-55): clear, then add the arena each
    // name resolves to, in order -- Kurt's own arena first, then the active second.
    // C# Named() returns the FIRST arena with that name (so a repeated name adds it
    // twice, as the C# does) and null for one no arena has, which is how the "" the
    // scripts use for "no arena" contributes nothing instead of clearing the pin.
    _solid.clear();
    for (const std::string& name : names) {
        for (const auto& up : _arenas) {
            if (up->arena->name() == name) {
                _solid.push_back(&up->bsp);
                break;
            }
        }
    }
}

const ArenaSpace::Entry* ArenaSpace::firstEntry(Vec point) const {
    for (const auto& up : _arenas) {
        if (contains(up->min, up->max, point)) {
            return up.get();
        }
    }
    return nullptr;
}

void ArenaSpace::addAt(Vec point, std::vector<Bsp*>& out) {
    for (const auto& up : _arenas) {
        if (contains(up->min, up->max, point)) {
            bool seen = false;
            for (Bsp* b : out) {
                if (b == &up->bsp) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                out.push_back(&up->bsp);
            }
        }
    }
}

Bsp* ArenaSpace::kurt(Vec point) {
    if (!_solid.empty()) {
        return _solid[0];
    }
    const Entry* e = firstEntry(point);
    return e ? const_cast<Bsp*>(&e->bsp) : nullptr;
}

ArenaSpace::Result ArenaSpace::move(Vec feet, Vec delta, Motion motion, float slideK) {
    Vec box;
    float lift;
    if (motion == kMotionWalk) {
        box = kWalkBox;
        lift = kWalkLift;
    } else {
        box = kFallBox;
        lift = kFallLift;
    }
    Vec a = vAdd(feet, Vec{0.0f, 0.0f, box.z + lift});

    std::vector<Bsp*>* arenas = &_solid;
    if (_solid.empty()) {
        _around.clear();
        addAt(feet, _around);
        addAt(vAdd(feet, delta), _around);
        arenas = &_around;
    }

    Result r;
    r.triangle = Bsp::kNone;
    r.bsp = nullptr;
    r.node = Bsp::kNone;
    if (arenas->empty()) {
        r.feet = vAdd(feet, delta);
        return r;
    }

    Vec end;
    int node = Bsp::kNone;
    Bsp* own = (*arenas)[0];
    int triangle = own->sweepBox(a, vAdd(a, delta), box, kSlides, slideK, &end, &node);
    Bsp* hit = own;
    for (size_t i = 1; i < arenas->size(); i++) {
        if (triangle != Bsp::kNone) {
            break;
        }
        triangle = (*arenas)[i]->sweepBox(a, end, box, 0, slideK, &end, &node);
        hit = (*arenas)[i];
    }

    r.feet = vAdd(feet, vSub(end, a));
    r.triangle = triangle;
    r.bsp = triangle == Bsp::kNone ? nullptr : hit;
    r.node = node;
    return r;
}

bool ArenaSpace::bottom(Vec feet, float* out) {
    Bsp* own = kurt(feet);
    bool have = false;
    float bottom = FLT_MAX;
    for (const auto& up : _arenas) {
        if (&up->bsp == own || contains(up->min, up->max, feet)) {
            if (!have || up->min.z < bottom) {
                bottom = up->min.z;
                have = true;
            }
        }
    }
    if (!have) {
        return false;
    }
    *out = bottom;
    return true;
}

const std::vector<Bsp*>* ArenaSpace::kurts(Vec point) {
    if (!_solid.empty()) {
        return &_solid;
    }
    _kurts.clear();
    addAt(point, _kurts);
    return &_kurts;
}

bool ArenaSpace::sight(Vec from, Vec to, Vec half, Vec* outPoint, const formats::BspNode** outPlane) {
    for (Bsp* bsp : *kurts(from)) {
        Vec end;
        int node = Bsp::kNone;
        const int triangle = bsp->sweepBox(from, to, half, 0, 0.0f, &end, &node);
        if (triangle != Bsp::kNone && node >= 0) {
            if (outPoint != nullptr) {
                *outPoint = end;
            }
            if (outPlane != nullptr) {
                *outPlane = &bsp->arena()->nodes()[node];
            }
            return true;
        }
    }
    return false;
}

bool ArenaSpace::crosses(Vec from, Vec to) {
    Bsp* own = kurt(from);
    if (own == nullptr) {
        return false;
    }
    Vec point;
    return own->segment(from, to, Bsp::kSegmentAny, &point) != Bsp::kNone;
}

bool ArenaSpace::floorCrossing(Vec from, Vec to, Floor* out) {
    if (!out) {
        return false;
    }
    for (Bsp* bsp : *kurts(from)) {
        Vec point;
        const int triangle = bsp->segment(from, to, Bsp::kSegmentFloor, &point);
        if (triangle == Bsp::kNone) {
            continue;
        }
        const formats::BspNode& plane = bsp->planeOf(triangle);
        out->point = point;
        out->normal = Vec{plane.normal[0], plane.normal[1], plane.normal[2]};
        out->bsp = bsp;
        out->triangle = triangle;
        return true;
    }
    return false;
}

// ---- The ledge probes (package #12) -----------------------------------------

bool ArenaSpace::edge(const Floor& surface, Vec back, Vec* outPoint, Vec* outDirection) {
    // C# ArenaSpace.Edge (ArenaSpace.Probes.cs:35-36): the surface's arena's Bsp.Edge.
    return surface.bsp != nullptr &&
           surface.bsp->edge(surface.triangle, surface.point, back, outPoint, outDirection);
}

bool ArenaSpace::free(Vec from, Vec to, Vec half) {
    // C# ArenaSpace.Free (ArenaSpace.Probes.cs:40-44): Kurt's arena only (the first
    // solid one, else the smallest around `from`), one pass, no slide.
    Bsp* own = kurt(from);
    if (own == nullptr) {
        return true;
    }
    Vec end;
    int node = Bsp::kNone;
    return own->sweepBox(from, to, half, 0, 0.0f, &end, &node) == Bsp::kNone;
}

// ---- end of the ledge probes ------------------------------------------------

bool ArenaSpace::floor(Vec from, Vec to, Vec* out) {
    // C# ArenaSpace.Floor (ArenaSpace.cs:191), line for line: the arenas around BOTH
    // ends, the nearest crossing by Vector3.DistanceSquared(from, hit), and the
    // segment's own end as the value `point` starts at. The scratch is the same
    // _around the move uses, exactly as the C# shares its one _around list; the
    // cleared-and-refilled list is why the query allocates nothing per call.
    //
    // It is NOT floorCrossing with a different arena list. That one is the C#'s
    // FloorCrossing (ArenaSpace.Probes.cs:19): Kurt's arenas (the pinned solid ones
    // when there are any) around `from` only, and the FIRST hit in arena order. Two
    // arenas around the segment whose floors are at different heights, or a pinned
    // arena that does not hold the ends, and the two probes answer differently.
    Vec point = to;
    float best = FLT_MAX;
    _around.clear();
    addAt(from, _around);
    addAt(to, _around);
    for (Bsp* bsp : _around) {
        Vec hit;
        if (bsp->segment(from, to, Bsp::kSegmentFloor, &hit) == Bsp::kNone) {
            continue;
        }
        const float distance = vDistSq(from, hit);
        if (distance < best) {
            best = distance;
            point = hit;
        }
    }
    if (out != nullptr) {
        *out = point;
    }
    return best < FLT_MAX;
}

// ---- The solid-object pass (package #3) ----------------------------------

ArenaSpace::Result ArenaSpace::move(Vec feet, Vec delta, Motion motion, float slideK,
                                    const std::vector<solids::Solid>& solids) {
    // C# ArenaSpace.Move with solids (ArenaSpace.cs:172-184), line for line: the
    // BSP move, then Solids.Walk from the box centre over the move the BSP allowed;
    // a hit re-sweeps the BSP horizontally to the walk's end and keeps the first
    // move's Z.
    const Result result = move(feet, delta, motion, slideK);
    const Vec box = motion == kMotionWalk ? kWalkBox : kFallBox;
    const float lift = motion == kMotionWalk ? kWalkLift : kFallLift;
    const Vec a = vAdd(feet, Vec{0.0f, 0.0f, box.z + lift});
    Vec end;
    if (solids.empty() ||
        !solids::walk(a, vAdd(a, vSub(result.feet, feet)), box, solids, &end)) {
        return result;
    }

    Result again = move(feet, Vec{end.x - a.x, end.y - a.y, 0.0f}, motion, slideK);
    again.feet = Vec{again.feet.x, again.feet.y, result.feet.z};
    return again;
}

void ArenaSpace::touching(Vec feet, const std::vector<solids::Solid>& solids,
                          std::vector<scripts::MdkObject*>* owners) {
    // C# ArenaSpace.Touching (ArenaSpace.cs:186-188): the walking box's centre.
    solids::touching(vAdd(feet, Vec{0.0f, 0.0f, kWalkBox.z + kWalkLift}), kWalkBox, solids,
                     owners);
}

// ---- end of the solid-object pass ------------------------------------------

const std::string* ArenaSpace::arenaAt(Vec point) const {
    const Entry* e = firstEntry(point);
    return e ? &e->arena->name() : nullptr;
}

} // namespace mdk

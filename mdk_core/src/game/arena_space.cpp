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

const std::string* ArenaSpace::arenaAt(Vec point) const {
    const Entry* e = firstEntry(point);
    return e ? &e->arena->name() : nullptr;
}

} // namespace mdk

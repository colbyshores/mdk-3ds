// ============================================================================
// solids.cpp — Kurt against the solid objects. See solids.h. A 1:1 port of
// src/Mdk.Game/Collision/Solids.cs.
// ============================================================================

#include "game/solids.h"

namespace mdk {
namespace solids {

namespace {

// C# Solids.Gap: Kurt stops this far before a face, so the next move doesn't
// start inside the box.
const float kGap = 0.01f;

float minf(float a, float b) { return a < b ? a : b; }
float maxf(float a, float b) { return a > b ? a : b; }
float signf(float v) { return (float)((v > 0.0f) - (v < 0.0f)); }

// C# Solids.Inside: strictly inside the box in XY.
bool inside(const formats::Box& box, Vec p) {
    return p.x > box.min.x && p.x < box.max.x && p.y > box.min.y && p.y < box.max.y;
}

// C# Solids.Entry: where the segment enters the box in XY, the fraction and the
// axis (0 X, 1 Y) of the face. Two slabs: a fixed iteration count, not a search.
bool entry(const formats::Box& box, Vec a, Vec b, float* t, int* axisOut) {
    float enter = 0.0f;
    float leave = 1.0f;
    int axis = -1;
    for (int i = 0; i < 2; i++) {
        const float ai = vComp(a, i);
        const float lo = vComp(box.min, i);
        const float hi = vComp(box.max, i);
        const float step = vComp(b, i) - ai;
        if (step == 0.0f) {
            if (ai <= lo || ai >= hi) {
                return false;
            }
            continue;
        }

        const float t0 = (lo - ai) / step;
        const float t1 = (hi - ai) / step;
        // On the face (t = 0) counts as outside: entered at once (0x45f588: A <= min;
        // upstream e96fb32e). A BSP wall in the plane of a part's face left Kurt exactly on
        // the grown face, the walk entered at t = 0 and was ignored, and once the wall let
        // go he slid into the box (LEVEL4's MEAT_7: through the closed X4DOOR).
        const float near = minf(t0, t1);
        if (near > enter || (near == enter && axis < 0)) {
            enter = near;
            axis = i;
        }
        leave = minf(leave, maxf(t0, t1));
        if (enter >= leave) {
            return false;
        }
    }
    if (axis < 0) {
        return false;
    }
    *t = enter;
    *axisOut = axis;
    return true;
}

} // namespace

bool walk(Vec a, Vec b, Vec half, const std::vector<Solid>& solids, Vec* endOut) {
    // C# Solids.Walk (Solids.cs:33-63), in its order: the first box hit slides the rest
    // of the walk along its face; every later hit only shortens the walk to its entry.
    bool have = false;
    Vec end;
    for (size_t i = 0; i < solids.size(); i++) {
        const Solid& solid = solids[i];
        formats::Box box;
        box.min = vSub(solid.box.min, half);
        box.max = vAdd(solid.box.max, half);
        if (solid.footing == kFloor || a.z < box.min.z || a.z > box.max.z || inside(box, a)) {
            continue;
        }

        const Vec target = have ? end : b;
        float t = 0.0f;
        int axis = -1;
        if (!entry(box, a, target, &t, &axis)) {
            continue;
        }

        Vec hit = vAdd(a, vScale(vSub(target, a), t));
        vSetComp(hit, axis,
                 vComp(hit, axis) - signf(vComp(target, axis) - vComp(a, axis)) * kGap);
        if (have) {
            end = hit;
            continue;
        }

        // The first hit slides along the face.
        Vec slid = vSub(b, hit);
        vSetComp(slid, axis, 0.0f);
        end = vAdd(hit, slid);
        have = true;
    }
    if (have) {
        *endOut = end;
    }
    return have;
}

bool platform(Vec feet, float bottom, const std::vector<Solid>& solids, float* top,
              scripts::MdkObject** owner) {
    // C# Solids.Platform (Solids.cs:67-85): the highest box top inside the ray, ties
    // to the first (`b.Top >= box.Max.Z` skips an equal one).
    bool have = false;
    float bestTop = 0.0f;
    scripts::MdkObject* bestOwner = nullptr;
    const float from = feet.z + kPlatformReach;
    const float to = minf(bottom, feet.z - kPlatformReach);
    for (size_t i = 0; i < solids.size(); i++) {
        const Solid& solid = solids[i];
        const formats::Box& box = solid.box;
        if (solid.footing == kWall || feet.x < box.min.x || feet.x > box.max.x ||
            feet.y < box.min.y || feet.y > box.max.y || box.max.z > from || box.max.z < to ||
            (have && bestTop >= box.max.z)) {
            continue;
        }
        have = true;
        bestTop = box.max.z;
        bestOwner = solid.owner;
    }
    if (have) {
        *top = bestTop;
        *owner = bestOwner;
    }
    return have;
}

void touching(Vec a, Vec half, const std::vector<Solid>& solids,
              std::vector<scripts::MdkObject*>* owners) {
    // C# Solids.Touching (Solids.cs:97-110).
    const Vec reach = vAdd(half, Vec{kGap * 2.0f, kGap * 2.0f, kGap * 2.0f});
    for (size_t i = 0; i < solids.size(); i++) {
        const Solid& s = solids[i];
        const Vec mn = vSub(s.box.min, reach);
        const Vec mx = vAdd(s.box.max, reach);
        if (!(a.x >= mn.x && a.x <= mx.x && a.y >= mn.y && a.y <= mx.y && a.z >= mn.z &&
              a.z <= mx.z)) {
            continue;
        }
        bool seen = false;
        for (size_t j = 0; j < owners->size(); j++) {
            if ((*owners)[j] == s.owner) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            owners->push_back(s.owner);
        }
    }
}

} // namespace solids
} // namespace mdk

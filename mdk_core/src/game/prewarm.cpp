// ============================================================================
// prewarm.cpp -- see prewarm.h.
// ============================================================================

#include "game/prewarm.h"

#include "scripts/pose_cache.h"

namespace mdk {

bool prewarmModel(ObjectView* view, const formats::Model* model, const ObjectLook& look,
                  SurfaceCache* surfaces, PrewarmBudget* budget) {
    if (view == nullptr || surfaces == nullptr || budget == nullptr) {
        return false;
    }
    // The layout first: it is what names the surfaces, and it is first-use work
    // of its own (made once per (model, palette) and kept by the view).
    const ObjectLayout* layout = view->layoutOf(model, look);
    if (layout == nullptr) {
        return true;   // nothing to draw, so nothing to make
    }
    bool complete = true;
    for (size_t i = 0; i < layout->batches.size(); i++) {
        const ObjectBatch& batch = layout->batches[i];
        if (batch.image != nullptr) {
            continue;
        }
        if (prewarmStops(surfaces->prewarm(batch.surface, look.palette, budget))) {
            complete = false;
            if (budget->spent()) {
                return false;
            }
        }
    }
    return complete;
}

int32_t ObjectPrewarm::step(ObjectView* view, const std::deque<scripts::MdkObject>& objects,
                            PrewarmLook look, PrewarmRemoved removed, void* user,
                            SurfaceCache* surfaces, PrewarmBudget* budget, int32_t count,
                            bool poses) {
    if (view == nullptr || look == nullptr || surfaces == nullptr || budget == nullptr ||
        objects.empty()) {
        return 0;
    }
    int32_t visited = 0;
    while (visited < count && !budget->spent()) {
        // The deque only grows during play (removed objects stay in it), so the
        // cursor wraps at its CURRENT size: an object appended since the last
        // step is reached on this lap.
        if (cursor_ >= objects.size()) {
            cursor_ = 0;
        }
        const scripts::MdkObject& obj = objects[cursor_];
        cursor_++;
        visited++;
        if (obj.model == nullptr || obj.dead || (removed != nullptr && removed(user, &obj))) {
            continue;
        }
        const ObjectLook* objectLook = look(user, obj.arena);
        if (objectLook == nullptr) {
            continue;
        }
        (void)prewarmModel(view, obj.model, *objectLook, surfaces, budget);
        if (poses) {
            // The pose the object's next draw asks for: a cache read when it is
            // made, the rest pose or its animation's whole bake when it is not.
            const int before = scripts::poseCacheBuilds();
            (void)obj.pose_parts();
            stats_.poses += scripts::poseCacheBuilds() - before;
        }
    }
    stats_.visited += visited;
    return visited;
}

} // namespace mdk

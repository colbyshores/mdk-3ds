// ============================================================================
// pose_cache.cpp — the shared object-pose caches, and the drop of them.
//
// WHY THIS IS ITS OWN TRANSLATION UNIT. `scripts/pose_cache.h` exists because
// the game layer has to invalidate the pose caches without including the
// scripts layer (see that header). A header alone does not finish that job:
// the definition sat in `scripts/object.cpp`, so every consumer of the free
// function still had to LINK the whole script VM. `game/level_data.cpp` calls
// `forgetPoseCaches()`, so any target that links the level loader also links
// object.cpp — and object.cpp needs `game/frame_blend.cpp` and
// `ScriptRuntime::kTick`, and the runtime needs the BSP, the mixer, Kurt and
// the player. The handle was dependency-free at INCLUDE time and carried the
// entire game at LINK time. That is what broke `make -C tests/formats`: the
// format tests link the level loader, so they inherited a dependency on the
// script VM, and the target did not link for 25 batches.
//
// Living here, the caches need exactly two things: the two formats classes they
// are keyed by, and the MdkObject fields `pose_parts()` reads. A level loader
// now pulls in two parsed-format translation units instead of a game, which is
// the seam the header has always claimed.
//
// The C#'s static MdkObject.RestPoses and MdkObject.Baked, ported as the same
// two maps. The C# drops them in MdkObject.ForgetPoses(); pose_cache.h states
// the moment the drop happens and scripts/object.h states the fields.
// ============================================================================

#include "scripts/pose_cache.h"

#include <map>
#include <utility>
#include <vector>

#include "formats/model.h"
#include "formats/model_animation.h"
#include "scripts/object.h"

namespace {

typedef std::vector<std::vector<mdk::formats::Vec3f> > ObjectPose;

// The C#'s static RestPoses and Baked, shared by every object because a level
// has a handful of models drawn by many of them. std::map, not std::vector:
// pose_parts() hands out a reference into these, and a map's elements do not
// move when later entries are inserted. The C# shares the model's own vertex
// arrays; the port copies them once into the cache, which is the same read-only
// data. Single-threaded: the C#'s lock guards its parallel test runner, and the
// port ticks one world at a time.
std::map<const mdk::formats::Model*, ObjectPose> g_restPoses;
std::map<std::pair<const mdk::formats::Model*, const mdk::formats::ModelAnimation*>,
         std::vector<ObjectPose> > g_baked;
const ObjectPose g_noPose;

const std::vector<ObjectPose>& baked(const mdk::formats::Model* model,
                                     const mdk::formats::ModelAnimation* animation)
{
    const std::pair<const mdk::formats::Model*, const mdk::formats::ModelAnimation*> key(model, animation);
    std::map<decltype(key), std::vector<ObjectPose> >::iterator found = g_baked.find(key);
    if (found != g_baked.end()) {
        return found->second;
    }
    // The C# bakes the whole animation once per (model, animation) and keeps
    // every frame, so nothing is baked while the game plays.
    return g_baked.emplace(key, animation->bake(*model)).first->second;
}

} // namespace

const std::vector<std::vector<mdk::formats::Vec3f> >& mdk::scripts::MdkObject::pose_parts() const {
    if (model == nullptr) {
        return g_noPose;
    }
    if (animation == nullptr) {
        std::map<const mdk::formats::Model*, ObjectPose>::iterator found = g_restPoses.find(model);
        if (found != g_restPoses.end()) {
            return found->second;
        }
        return g_restPoses.emplace(model, model->restPose()).first->second;
    }
    const std::vector<ObjectPose>& frames = baked(model, animation);
    if (frames.empty()) {
        return g_noPose;
    }
    // The C# indexes with AnimationFrame and would throw out of range; the
    // clamp is the same guard ScriptRuntime::poseBounds already uses, and it
    // cannot be reached from a real animation (the frame counter is taken
    // modulo the frame count).
    int32_t frame = animation_frame;
    if (frame < 0) {
        frame = 0;
    }
    if (frame >= static_cast<int32_t>(frames.size())) {
        frame = static_cast<int32_t>(frames.size()) - 1;
    }
    return frames[static_cast<size_t>(frame)];
}

void mdk::scripts::forgetPoseCaches() {
    g_restPoses.clear();
    g_baked.clear();
}

void mdk::scripts::MdkObject::forget_poses() {
    // The C#'s own name for the drop. The caches live in this translation unit,
    // and scripts/pose_cache.h is the handle the game layer (LevelData::load)
    // calls without pulling in the script VM; both names are the same drop.
    forgetPoseCaches();
}

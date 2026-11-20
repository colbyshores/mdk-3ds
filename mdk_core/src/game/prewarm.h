#pragma once

// ============================================================================
// prewarm.h -- the objects' half of the prewarm (render/surface_cache.h's
// THE PREWARM): their layouts, the textures they draw with and their poses,
// made before the frame that first draws them.
//
// WHAT AN OBJECT'S FIRST DRAW MAKES. ObjectView::addObject lays the model out
// (layoutOf: the batches, a MaterialResolver walk), poses it (pose_parts: the
// rest pose, or a bake of every frame of its animation -- scripts/pose_cache.h),
// and ObjectDraw asks the surface cache for each batch's texture through the
// arena's palette -- which on the 3DS is the palette expansion into tiled RGBA8,
// the filter preparation and the upload. All of it is first-use work inside the
// frame's draw build, the span the Old 3DS profile names (2026-11-19, level 8:
// 20-40 ms instead of ~8 in clusters of 13-16 frames). The C# does it at the
// level's load (Viewer.PreloadModels, ScriptRuntime.PreparePoses).
//
// GAMEPLAY IS NOT TOUCHED. Everything made here is derived data the draw would
// make from the same inputs: a layout is a pure function of (model, look), a
// pose of (model, animation), a texture of (texture, palette). Making them
// earlier changes no simulation state, no random draw and no script's order --
// only WHEN the cost is paid. The walk reads the objects; it never writes one.
//
// No virtual, no template, no std::function (AGENTS.md 10): the look is asked
// through a plain function pointer, as the surface cache's replacement lookup is.
// ============================================================================

#include <stdint.h>

#include <deque>
#include <string>

#include "game/object_view.h"
#include "render/surface_cache.h"
#include "scripts/object.h"

namespace mdk {

// The model's layout and every batch texture it draws with through the look's
// palette (SurfaceCache::prewarm). The surface is the batch's own, unshined: the
// cache keys on (texture, palette), so a shining draw of the batch finds the same
// texture (an explosion's shine changes the shading the draw is marked with, not
// its texture). A mod surface's own picture (batch.image) is ObjectDraw's and is
// left to its draw. Returns true when every batch's texture is made.
bool prewarmModel(ObjectView* view, const formats::Model* model, const ObjectLook& look,
                  SurfaceCache* surfaces, PrewarmBudget* budget);

// The look an object in `arena` draws with, or null when it must not be
// prewarmed now (its arena is not on the GPU: a texture made through the palette
// of an arena with no view would be the orphan sweep's to give back).
typedef const ObjectLook* (*PrewarmLook)(void* user, const std::string& arena);

// Whether the runtime still lists an object (ScriptRuntime::isRemoved), asked
// through a plain pointer so this file does not include the script VM.
typedef bool (*PrewarmRemoved)(void* user, const scripts::MdkObject* obj);

// The walk over the scripts' objects, a few a frame. Its cursor is the only
// state; reset() starts it over (a respawn's fresh runtime).
class ObjectPrewarm {
public:
    struct Stats {
        int32_t visited = 0;   // objects looked at
        int32_t poses = 0;     // rest poses and bakes made by the walk
    };

    // Visit up to `count` objects from the cursor (wrapping), each one's model
    // prewarmed through its arena's look and, when `poses`, its CURRENT pose made
    // (pose_parts). Dead and removed objects and objects without a model are
    // passed over. Stops early when the budget is spent. Returns how many
    // objects it visited.
    int32_t step(ObjectView* view, const std::deque<scripts::MdkObject>& objects,
                 PrewarmLook look, PrewarmRemoved removed, void* user, SurfaceCache* surfaces,
                 PrewarmBudget* budget, int32_t count, bool poses);

    void reset() { cursor_ = 0; }
    const Stats& stats() const { return stats_; }

private:
    size_t cursor_ = 0;
    Stats stats_;
};

} // namespace mdk

#pragma once

// ============================================================================
// arena_views.h -- the arenas that are on the GPU, following the scripts.
//
// The C#'s LevelView (src/Mdk.Game/Level/LevelView.cs) builds a view of EVERY
// arena when the level loads and Draw(arenas) picks the ones the scripts say
// are drawn (Viewer.cs:702: ScriptRuntime.DrawnArenas -- Kurt's arena and the
// ACTIVE second one, the original's 0x41e344). A PC can hold a level's worth of
// meshes and textures; the 3DS cannot, and the original did not either. Full-
// level residency was tried on the Old 3DS 2026-11-22 and refused twice: a
// level's meshes exhaust the 40 MiB main heap at load (OOM at 38.8 MiB) and
// its ETC1 exhausts the linear pool (762 of level 7's 1686 makes fail). So the
// port keeps the original's residency -- and what changed instead is where the
// BYTES live: the whole MTO sits in RAM from level load (game/level_data.cpp),
// so no arena data is read from the SD card during play at all. The streaming
// this replaces paced 32 KiB SD reads inside gameplay frames, which was the
// transition stutter on an SD-launched build.
//
//   kept   Kurt's arena and the second arena (the loaded pair: the second may be
//          behind a closed door or preloading, so its view is ready the frame
//          the door opens);
//          everything the frame DRAWS (the scripts' drawn list is the middle
//          of their tick and the pair is its end: after a scripted teleport the
//          frame still draws the arena Kurt left -- review, 2026-11-18);
//          the corridors CONNECTED to the pair -- the original keeps the
//          corridors of its resident arena (0x419ac0), and keeping them is what
//          stops a door that opens and closes from rebuilding its corridor
//          every time.
//   built  when an arena first enters the kept set -- for the second arena,
//          hundreds of frames before its door, so the crossing pays no mesh
//          build; its textures are pumped in the meantime (pumpSurfaces).
//   freed  when it is no longer kept: its mesh, and every GPU texture drawn
//          through ITS palette (SurfaceCache::releasePalette) -- an arena's
//          textures, the level's textures in its colours, its objects'.
//   swept  every kOrphanSweepSyncs: textures made through the palette of an
//          arena that has NO view (an effect or a piece of debris left in an
//          arena Kurt walked away from is still drawn, as the C# draws it, and
//          nothing else would ever give those textures back).
//
// A view that cannot be built (no GPU memory for its mesh: on the 3DS the
// departing arena's frees land at the NEXT frame's start, so the first attempt
// can simply be early) is tried again -- the next few frames, then every
// kRetrySyncs -- and never given up on: a transient miss must not hide an arena
// for the rest of the level.
//
// draw() is the C#'s Draw(arenas): the drawn list, in its order. An arena in
// the list with no view (its build failed) is skipped; the log says which.
//
// The frame's order is: scripts tick -> ArenaViews::sync (meshes) ->
// pumpSurfaces (textures, from RAM) -> draw.
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

#include "game/kurt_cover.h"
#include "game/level_data.h"
#include "game/level_view.h"
#include "render/render.h"
#include "render/surface_cache.h"

namespace mdk {

class ArenaViews {
public:
    struct Stats {
        int32_t built = 0;
        int32_t released = 0;
        int32_t failed = 0;          // build attempts that failed
        int32_t orphansReleased = 0; // palettes the sweep gave back
        int32_t syncFinishes = 0;    // drawn views finished synchronously at sync (the guarantee)
        int32_t buildSteps = 0;      // sliced build steps run (sync's and the frontend's stepBuild)
    };

    // The sweep's period, and a failed build's retry period after its first
    // kQuickRetries (which are one sync apart), in sync() calls: ten and two
    // seconds at 60 frames a second.
    static const int32_t kRetrySyncs = 120;
    static const int32_t kQuickRetries = 3;
    // How often sync() sweeps orphan palette textures (arena_views.cpp).
    static const int32_t kOrphanSweepSyncs = 600;

    ArenaViews() = default;
    // The views' GPU objects are released by clear(), which needs the renderer:
    // call it before the renderer goes. The destructor frees only the memory.
    ~ArenaViews();
    ArenaViews(const ArenaViews&) = delete;
    ArenaViews& operator=(const ArenaViews&) = delete;

    // Everything is BORROWED and must outlive this object (or clear() first).
    void start(MdkRenderer renderer, LevelData* level, SurfaceCache* surfaces, Shading shading);

    // Follow the scripts: free what is no longer kept, build what is missing of
    // the loaded pair and of the drawn list. Returns how many views this call
    // built.
    int32_t sync(const std::string& current, const std::string& second,
                 const std::vector<std::string>& drawn);
    // With door-bubble hints (the frontend's doorBubbleArenas): hinted
    // arenas are kept and built like the pair, so their views are ready
    // long before the scripts make them the second. A DRAWN view still
    // building at sync is finished here, synchronously (the guarantee:
    // a drawn arena is never left half-built; the bubble makes it rare).
    // A drawn arena whose build FAILED waits out the normal retry
    // schedule like any other -- the guarantee finishes builds in
    // flight, it does not retry failures every frame.
    int32_t sync(const std::string& current, const std::string& second,
                 const std::vector<std::string>& drawn, const std::vector<std::string>& hints);
    // The same with the drawn list the pair implies (tools and tests).
    int32_t sync(const std::string& current, const std::string& second);

    // One more build step for the oldest view still building (sync takes one
    // itself): the frontend calls it while its per-frame time budget lasts, so
    // a large arena's build (LEVEL4's MEAT_5: 1,251 triangles) is spread in
    // small steps under a bound instead of one large step a frame. With
    // `sliceOnly`, only a scene slice is taken: the Mesh and Large steps are
    // left to the next sync's own step, so they never stack on a frame's
    // slices. False when no step was taken. Between frames, like sync.
    bool stepBuild(bool sliceOnly);

    // Queue the drawn arenas' visible groups, in the list's order, the animated
    // textures at the frames setAnimatedTextures names (LevelView::draw).
    //
    // THE CAMERA'S ARENA OVER THE OTHER ONE (upstream e1eb36bb; the original's
    // 0x41df35): with two arenas drawn, `cameraArena` (ScriptRuntime::cameraArena
    // for the camera at `eye`) is drawn over the other one wherever the two
    // overlap on screen, as the original's painter's order has it. The other
    // arena's batches that may lie between the eye and the camera's arena are
    // queued `back`, and the camera arena's batches they may hide `cover`
    // (render/arena_scene.h's box arithmetic; render_types.h MeshDraw; render/
    // draw_queue.h). With one arena drawn, or none such, nothing is marked and
    // the frame is drawn as before. `eyeMargin` is the stereo separation (at
    // least kLayerEyeMargin). Returns the other arena's name while it is drawn
    // `back`, "" otherwise: the frontend draws that arena's objects `back` too
    // (ObjectStream::back), so its glass stays over them.
    std::string draw(const std::vector<std::string>& drawn, const std::string& cameraArena,
                     const Vec3& eye, float eyeMargin);

    // The scripts' texture frames (ScriptRuntime::animatedTextures, opcode 133) the
    // next draw() uses; null draws every animated texture at frame 0. Borrowed: the
    // frontend sets it each frame before draw(), so a runtime replaced at a level
    // change is never read through a stale pointer.
    void setAnimatedTextures(const scripts::AnimatedTextures* frames) { frames_ = frames; }

    // Resolve up to `budget` textures across the kept views (LevelView::
    // pumpSurfaces), so an arena's first drawn frame does not pay them all.
    // The pixels are in RAM (the resident MTO, game/level_data.cpp), so a
    // pumped make is decode and upload with no SD read. Call it every frame; a
    // resolved view costs one comparison.
    void pumpSurfaces(int32_t budget);

    // The end of a level: the torn triangles vanish from that arena's view
    // (LevelView::tear; the C#'s LevelView.DrawEnd, Viewer.cs:703). Call it inside
    // the frame, before draw(). False when the view's dynamic mesh was refused (the
    // arena stays whole this frame); true otherwise, and when the arena has no view
    // (the C# returns there too).
    bool tear(const std::string& arena, const std::vector<int32_t>& torn);

    // A group of the arena took another texture (group_set_texture; the C#'s
    // TriangleGroups.Changed -> LevelView.Rebuild, package #15): its view, if it has one,
    // is rebuilt (LevelView::refresh). Call it between frames, as sync() is. False when
    // the rebuilt mesh was refused (the view then draws nothing until the next refresh or
    // reload, which the frontend logs); true otherwise, and when the arena has no view.
    bool refresh(const std::string& arena);

    // The drawn arenas whose surfaces Kurt's sprite may be drawn over
    // (game/kurt_cover.h): each ready view of `drawn` once, at most `max`.
    // Returns how many were written.
    int32_t kurtCover(const std::vector<std::string>& drawn, KurtCover::Source* out,
                      int32_t max) const;

    // An arena's view, or null.
    const LevelView* find(const std::string& name) const;
    LevelView* find(const std::string& name);

    // Release every view (and its palette's GPU textures).
    void clear();

    int32_t count() const { return (int32_t)views_.size(); }
    const Stats& stats() const { return stats_; }
    // The arena the last sync() built a view for, for the frontend's log.
    const std::string& lastBuilt() const { return lastBuilt_; }

private:
    const scripts::AnimatedTextures* frames_ = nullptr;

    struct Entry {
        std::string name;
        LevelView* view = nullptr;
        // The pair or drawn (set by sync): only these views re-ask their made
        // textures (LevelView::pumpSurfaces).
        bool reask = false;
        // The arena's box (LevelView::bounds, once at build), for draw()'s
        // layering. False: no geometry, so never marked.
        bool hasBox = false;
        Vec3 boxMin{};
        Vec3 boxMax{};
    };

    // pumpSurfaces()'s rotating start, so a view past the frame's budget
    // is not starved by the ones before it.
    size_t pumpCursor_ = 0;

    struct Failed {
        std::string name;
        int32_t attempts = 0;
        int32_t retryAt = 0;   // the sync() count at which to try again
        LevelViewBuildFailure kind = LevelViewBuildFailure::TransientAllocation;
    };

    bool keeps(const std::string& name, const std::string& current, const std::string& second,
               const std::vector<std::string>& drawn) const;
    bool keeps(const std::string& name, const std::string& current, const std::string& second,
               const std::vector<std::string>& drawn, const std::vector<std::string>& hints) const;
    // Whether a DTI connection (a type-6 record pair) joins the two.
    bool connected(const std::string& a, const std::string& b) const;
    bool build(const std::string& name);
    void release(size_t index);
    // Whether a failed build of `name` is still waiting for its retry.
    bool waiting(const std::string& name) const;
    void noteFailed(const std::string& name, LevelViewBuildFailure failure);
    void forgetFailed(const std::string& name);
    void sweepOrphans(const std::string& current, const std::string& second,
                      const std::vector<std::string>& drawn);
    // Whether a resident view draws through this palette object. A corridor
    // shares its arena's (LevelData::paletteOf), so a palette can outlive the
    // arena named by the view that first made it; `exceptIndex` skips the view
    // being released (views_.size() to skip none).
    bool paletteInUse(const formats::Palette& palette, size_t exceptIndex);

    MdkRenderer renderer_ = nullptr;
    LevelData* level_ = nullptr;
    SurfaceCache* surfaces_ = nullptr;
    Shading shading_ = Shading::Original;
    std::vector<Entry> views_;
    // Each connection once, as the two arena names its record pair names.
    std::vector<std::pair<std::string, std::string>> connections_;
    // Arenas whose view could not be built, and when each is tried again.
    std::vector<Failed> failed_;
    int32_t syncs_ = 0;
    int32_t nextSweep_ = kOrphanSweepSyncs;   // the sync() count of the next orphan sweep
    std::string lastBuilt_;
    Stats stats_;
};

} // namespace mdk

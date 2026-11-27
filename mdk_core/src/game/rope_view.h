#pragma once

// ============================================================================
// rope_view.h — the objects' rope lines (C# Mdk.Game/Objects/RopeView.cs).
//
// The original draws an object's ropes (obj+0x2d0) as lines in a colour of its
// arena's palette (arena_build_drawlist 0x4185f0). Two writers set them: a
// swinging object's rope from the object to its pivot (ObjectMotion.Swing, after
// jump_to, opcode 226 -- LEVEL6 OLYM_6's pendulum), and set_2d0_block (opcode
// 242). The line ends are the object's (MdkObject::rope_lines); this file only
// draws them, once a frame, after the objects and before the sniper view and the
// effects, where the C#'s Viewer.DrawScene draws them (Viewer.cs:705).
//
// TWO BACKENDS, ONE CODE PATH, BY CAPABILITY (render_types.h line_primitive):
//
//   * with a line primitive (Vulkan): one Lines draw per object, a flat colour,
//     Pass::Solid -- the C#'s DrawLines(mesh, first, count, Material.Flat(...)).
//   * without one (the PICA200): each line is a thin quad facing the camera,
//     about 1.5 pixels of a 240-line view wide at any distance, textured with an
//     8x8 RGBA texture of the rope's colour (the 3DS scene vertex has no colour
//     and its draw needs a texture: stream_view.cpp's tube does the same). The
//     colour textures are made the first time a colour is seen (a handful a
//     level: rope colour 1 of each arena's palette) and freed with the level's
//     renderer scope. No file is read, no texture is imported per frame.
//
// The cost is a few dozen vertices a frame while a rope is in view: one object
// on LEVEL6, the dog on LEVEL5 and LEVEL8. Not a hot path (AGENTS.md section 2).
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

#include "render/render.h"
#include "render/render_types.h"

namespace mdk {

class LevelData;

namespace scripts {
class ScriptRuntime;
} // namespace scripts

class RopeView {
public:
    RopeView() = default;

    // Queues the ropes of the live, visible objects as the frame `tickAlpha` of
    // the way between ticks draws them (1 = blending off), under `camera` (the
    // quads face it). `level` gives each arena's palette. `backArena` is the arena
    // ArenaViews::draw drew `back` this frame ("" for none): an object of it has
    // its rope queued `back` too (MeshDraw::back), as the frontends queue the
    // object itself (mdk_render_set_back), so the camera's arena covers both.
    void draw(MdkRenderer renderer, const scripts::ScriptRuntime& runtime, LevelData& level,
              const Camera& camera, float tickAlpha, const std::string& backArena);

    bool allocationFailed() const { return failed_; }

private:
    // RopeView.MaxVertices: the line vertices drawn at most a frame.
    static const int32_t kMaxVertices = 1 << 12;
    // The quad path's colour textures, by RGB (kMaxColours of them a level).
    static const int32_t kMaxColours = 32;

    struct Queued {
        int32_t first;
        int32_t count;
        uint32_t rgba;
        bool back;
    };

    bool ensureMesh(MdkRenderer renderer);
    MdkTextureId colourTexture(MdkRenderer renderer, uint32_t rgba);

    MdkMeshId mesh_ = -1;
    bool failed_ = false;
    std::vector<Vertex> vertices_;
    std::vector<Queued> queued_;
    uint32_t colourKeys_[kMaxColours] = {};
    MdkTextureId colourIds_[kMaxColours] = {};
    int32_t colourCount_ = 0;
};

} // namespace mdk

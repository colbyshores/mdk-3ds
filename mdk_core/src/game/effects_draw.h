#pragma once

// ============================================================================
// effects_draw.h -- the effects pass's GPU half.
//
// EffectsView (game/effects_view.h) builds a frame's pieces and sprite
// effects into an EffectsFrame; this uploads that frame to one dynamic
// mesh and issues its queued draws, exactly as ObjectDraw does for the
// object pass. The mesh grows geometrically to what frames reach rather
// than committing the oracle's fixed 1<<16-vertex buffer (1.3 MiB of
// linear memory on the 3DS), and a refused mesh is recorded for the
// frontends to print, never silently swallowed.
// ============================================================================

#include <stdint.h>

#include "game/effects_view.h"
#include "render/render.h"

namespace mdk {

class SurfaceCache;

class EffectsDraw {
public:
    EffectsDraw() = default;

    // Uploads the frame once and issues its draws. Call between
    // mdk_render_begin_frame and mdk_render_end_frame, after the object
    // pass and before Kurt's sprite. Does nothing for an empty frame.
    void draw(MdkRenderer renderer, SurfaceCache* surfaces, const EffectsFrame& frame);

    bool allocationFailed() const { return failed_; }
    int32_t attemptedCapacity() const { return attemptedCapacity_; }

private:
    bool ensure(MdkRenderer renderer, int32_t need);

    MdkMeshId mesh_ = -1;
    int32_t capacity_ = 0;
    int32_t attemptedCapacity_ = 0;
    bool failed_ = false;
};

} // namespace mdk

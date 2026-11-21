#pragma once

// ============================================================================
// heads_view.h -- the Score-O-matic's heads (C# src/Mdk.Game/Menu/HeadsView.cs;
// the original's 0x433268, stats_screen.gd).
//
// The XGHEAD model of MISC/STATS.BNI (its textures in MISC/STATS.MTI, its
// colours through the page's palette: SYS_PAL's 0-63 and STATS.BNI's PAL
// 64-255), at most 16, spinning at 157 degrees a second, seen from the origin
// along +Y with a focal length of 250 pixels; one row below 5 heads, else two
// rows of (count + 1) / 2 (8 at most). Drawn under the page's text and fades.
//
//   row 0 (z -3):     o   o   o   o        x = 20 (k + 1) / (m + 1) - 10, y 8
//   row 1 (z -4.5):     o   o   o
//
// THE INSET. The C# draws the heads in a renderer inset (BeginInset with the
// page's 600 x 360 view as the rectangle, under the canvas). Neither backend
// implements mdk_render_begin_inset's rectangle (render.h), so the inset is
// the frame's camera with its projection remapped into the view's rectangle of
// the canvas -- the same picture wherever the heads stay inside the view, which
// they do (|x| < 10 at y 8 is inside the 300-pixel half width at focal 250).
// On the 3DS the view IS the canvas (scale 1), so the remap is the identity.
//
// The meshes, palette and textures are made through the renderer inside the
// screen's renderer scope (the frontend's mdk_render_mark), and released with it.
//
// Cold code (between levels). No virtuals, no templates, no exceptions.
// ============================================================================

#include <stdint.h>

#include <vector>

#include "render/render.h"
#include "render/render_types.h"

namespace mdk {

namespace formats {
class Bni;
class MdkData;
}

class HeadsView {
public:
    // C# MaxHeads.
    static const int32_t kMaxHeads = 16;

    // C# Places: where the `shown` heads of `count` are (one row below 5 heads,
    // else two rows); returns how many it wrote to `out` (at most kMaxHeads).
    static int32_t places(int32_t shown, int32_t count, Vec3 out[kMaxHeads]);

    // C# Update: the shown heads turn.
    void update(int32_t shown, float delta);
    // A head's angle in degrees (the goldens).
    float angle(int32_t i) const { return i >= 0 && i < kMaxHeads ? angles_[i] : 0.0f; }

    // C# constructor: the model through the page's palette (`systemRgb`, 768
    // bytes of SYS_PAL: colours 0-63 the system's, 64-255 PAL's). False (and
    // nothing drawn) when the BNI has no XGHEAD or PAL, as the C#'s `_mesh < 0`.
    bool load(MdkRenderer renderer, const formats::Bni& bni, const uint8_t* systemRgb,
              const formats::MdkData& data);
    bool ready() const { return mesh_ >= 0; }

    // C# Draw: `shown` of `count` heads in the view's rectangle of the canvas
    // (canvas units), the canvas `canvasWidth` x kCanvasHeight. Mesh draws:
    // call it before mdk_render_canvas_begin (the C#'s InsetLayer.UnderCanvas).
    void draw(MdkRenderer renderer, int32_t shown, int32_t count, float areaX, float areaY,
              float areaWidth, float areaHeight, float canvasWidth) const;

    // Forget the GPU objects (the frontend's scope release frees them).
    void forget();

private:
    struct Batch {
        int32_t first = 0;
        int32_t count = 0;
        Material material;
    };

    int32_t mesh_ = -1;
    std::vector<Batch> batches_;
    float angles_[kMaxHeads] = {};
};

} // namespace mdk

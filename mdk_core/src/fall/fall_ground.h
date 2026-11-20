#pragma once

// ============================================================================
// fall_ground.h — the fall's ground (0x41357c).
//
// THE C# ORIGINAL is src/Mdk.Game/Fall/FallGround.cs, ported operation for
// operation. Not 3D: the texture LEVELn (1024²) mapped affinely onto the view,
// zooming in as the camera drops and scrolling down the screen, with the
// minecrawler's track (PODn) written into it under and behind the crawler.
//
//   u = 512 + 0.36 cx + S (sx − w/2)      S = cz / 5280 texels per pixel
//   v = A   − 0.36 cy + S (sy − 180)      A = 824 − 624 t / 33 (the crawler's row)
//
//   track rows from R = round(A − 33.75):   ┌──┐  16 + 2j/3 texels each side of 512 (row R + j)
//                                          ┌┘  └┐ 32 from 24 rows on
//
// TWO WAYS TO SHOW THE TRACK (fall_view.h). The C# writes it into its copy of
// the texture and re-uploads the 1 MB of indices whenever a row is added (most
// frames): `updateTrack` + `indices()`, which the desktop does as the C# does. A
// 3DS re-upload is a CPU palette expansion of the whole 1024² texture (SPEC.md
// 7.3), so that view leaves the texture alone and draws PODn over it instead,
// through the same texel->screen map, from `trackRow()` down: `trackRowFor` and
// `trackWidth` are the shape it draws.
//
// Cold (once a frame): AGENTS.md §2. No virtuals, no templates (AGENTS.md §10).
// ============================================================================

#include <stdint.h>

#include <vector>

#include "formats/vector_types.h"

namespace mdk {

namespace formats {
class Texture;
}

namespace fall {

// A rectangle (System.Drawing.RectangleF): x, y, width, height.
struct FallRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

class FallGround {
public:
    static constexpr int32_t kSize = 1024;
    // The camera height at which one texel is one pixel.
    static constexpr float kUnitHeight = 5280.0f;
    // The track widens over 24 rows from 16 texels each side to 32.
    static constexpr int32_t kTrackRamp = 24;
    static constexpr int32_t kTrackWide = 32;
    static constexpr float kCentreColumn = 512.0f;

    // C# FallGround(Texture ground, Texture pod): `keepIndices` false skips the
    // writable copy (the 3DS view, which never updates the texture).
    bool load(const formats::Texture& ground, const formats::Texture& pod, bool keepIndices);

    // FallGround.CentreRow: the texel row at the view's centre after `time`
    // seconds of fall.
    static float centreRow(float time);
    // FallGround.TrackWidth: half the track's width at row `j` from its start.
    static int32_t trackWidth(int32_t j);
    // The first track row for a centre row (UpdateTrack's `row`).
    static int32_t trackRowFor(float centreRow);

    // FallGround.UpdateTrack: writes the track from the crawler's row down; false
    // when nothing changed. Without the copy (`keepIndices` false) it only moves
    // trackRow(), as the C#'s would.
    bool updateTrack(float centreRow);
    // The first track row written so far (C# _trackRow; kSize: none yet).
    int32_t trackRow() const { return trackRow_; }

    // FallGround.Indices: the ground with the track as written so far (empty
    // without the copy).
    const std::vector<uint8_t>& indices() const { return indices_; }

    // FallGround.Visible: the part of the view (`width` x 360 pixels) the
    // texture covers, and its texels; false (both empty) when the view is off
    // the texture.
    static bool visible(const formats::Vec3f& camera, float centreRow, float width, FallRect* screen,
                        FallRect* texels);

    // FallGround.Crawler: the minecrawler on the screen, its centre (at texel
    // (512, A)) and its scale.
    static void crawler(const formats::Vec3f& camera, float width, float* centreX, float* centreY,
                        float* scale);

private:
    const uint8_t* pod_ = nullptr;
    int32_t podWidth_ = 0;
    int32_t trackRow_ = kSize;
    std::vector<uint8_t> indices_;
};

} // namespace fall
} // namespace mdk

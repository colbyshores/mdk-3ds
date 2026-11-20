// fall_ground.cpp — see fall_ground.h. The C#'s FallGround.cs, method for method.

#include "fall/fall_ground.h"

#include <math.h>
#include <string.h>

#include "fall/fall_sim.h"
#include "formats/texture.h"

namespace mdk {
namespace fall {

namespace {

constexpr float kParallax = 0.36f;
constexpr float kStartRow = 824.0f;
constexpr float kRowsOverFall = 624.0f;
// The track starts this far ahead of the crawler's row (108 × 80 / 256).
constexpr float kTrackAhead = 108.0f * 80.0f / 256.0f;
constexpr float kTrackNarrow = 16.0f;
constexpr float kTrackWiden = 2.0f / 3.0f;
constexpr float kViewCentreY = 180.0f;
constexpr float kCrawlerScale = 0.75f;

float maxf(float a, float b) { return a > b ? a : b; }
float minf(float a, float b) { return a < b ? a : b; }

// System.Drawing.RectangleF.Intersect.
FallRect intersect(const FallRect& a, const FallRect& b) {
    const float x1 = maxf(a.x, b.x);
    const float x2 = minf(a.x + a.width, b.x + b.width);
    const float y1 = maxf(a.y, b.y);
    const float y2 = minf(a.y + a.height, b.y + b.height);
    FallRect r;
    if (x2 >= x1 && y2 >= y1) {
        r.x = x1;
        r.y = y1;
        r.width = x2 - x1;
        r.height = y2 - y1;
    }
    return r;
}

} // namespace

bool FallGround::load(const formats::Texture& ground, const formats::Texture& pod, bool keepIndices) {
    if (ground.width() != kSize || ground.height() != kSize || pod.height() < kSize ||
        pod.width() < 2 * kTrackWide) {
        return false;
    }
    pod_ = pod.indices();
    podWidth_ = pod.width();
    trackRow_ = kSize;
    indices_.clear();
    if (keepIndices) {
        // C# `Indices = (byte[])_ground.Clone()`.
        indices_.assign(ground.indices(), ground.indices() + (size_t)kSize * (size_t)kSize);
    }
    return true;
}

float FallGround::centreRow(float time) {
    return kStartRow - kRowsOverFall * time / FallSim::kEndTime;
}

int32_t FallGround::trackWidth(int32_t j) {
    return j < kTrackRamp ? (int32_t)rintf(kTrackNarrow + kTrackWiden * (float)j) : kTrackWide;
}

int32_t FallGround::trackRowFor(float centreRow) {
    // C# Math.Clamp((int)MathF.Round(centreRow - TrackAhead), 0, Size).
    const int32_t row = (int32_t)rintf(centreRow - kTrackAhead);
    return row < 0 ? 0 : (row > kSize ? kSize : row);
}

bool FallGround::updateTrack(float centreRow) {
    const int32_t row = trackRowFor(centreRow);
    if (row >= trackRow_) {
        return false;
    }

    // Rows within 24 of the new start, and the ones that were within 24 of the old one.
    if (!indices_.empty()) {
        const int32_t end = trackRow_ + kTrackRamp < kSize ? trackRow_ + kTrackRamp : kSize;
        for (int32_t y = row; y < end; y++) {
            const int32_t w = trackWidth(y - row);
            const int32_t podX = podWidth_ / 2 - w;
            memcpy(indices_.data() + (size_t)y * kSize + (size_t)((int32_t)kCentreColumn - w),
                   pod_ + (size_t)y * (size_t)podWidth_ + (size_t)podX, (size_t)(2 * w));
        }
    }

    trackRow_ = row;
    return true;
}

bool FallGround::visible(const formats::Vec3f& camera, float centreRow, float width, FallRect* screen,
                         FallRect* texels) {
    const float s = camera.z / kUnitHeight;
    const float u0 = kCentreColumn + kParallax * camera.x - s * width / 2.0f;
    const float v0 = centreRow - kParallax * camera.y - s * kViewCentreY;
    FallRect view;
    view.x = u0;
    view.y = v0;
    view.width = s * width;
    view.height = s * 2.0f * kViewCentreY;
    FallRect texture;
    texture.width = (float)kSize;
    texture.height = (float)kSize;
    const FallRect t = intersect(view, texture);
    if (t.width <= 0.0f || t.height <= 0.0f || s <= 0.0f) {
        *screen = FallRect();
        *texels = FallRect();
        return false;
    }

    screen->x = (t.x - u0) / s;
    screen->y = (t.y - v0) / s;
    screen->width = t.width / s;
    screen->height = t.height / s;
    *texels = t;
    return true;
}

void FallGround::crawler(const formats::Vec3f& camera, float width, float* centreX, float* centreY,
                         float* scale) {
    const float s = camera.z / kUnitHeight;
    *centreX = width / 2.0f - kParallax * camera.x / s;
    *centreY = kViewCentreY + kParallax * camera.y / s;
    *scale = kCrawlerScale / s;
}

} // namespace fall
} // namespace mdk

#pragma once

// ============================================================================
// slideshow.h -- the main menu's slideshow (C# src/Mdk.Game/Menu/Slideshow.cs;
// the original's 0x4279a0).
//
// Once MDK12.FLC ends, an idle menu shows MISC/MDKS_001.GIF after 5 s (without
// the menu items) for 4 s, then the others 2 s each with the items, then the
// video's last frame again, and so on. Any input starts the wait again; on the
// first image it shows the next one at once.
//
//   last frame --5 s--> MDKS_001 (no items) --4 s--> MDKS_002 --2 s--> ... --2 s--> last frame
//
// THE START IS THE VIDEO'S END. The C# starts it from VideoPlayer.Finished, the
// end of MDK12.FLC behind the menu, which is package #20c's (the VideoPlayer);
// with no video the C#'s menu shows MDKOPT and never a slide, and so do the
// frontends until #20c calls start(). Image 0 is the video's last frame, which
// the VideoPlayer keeps (its Still); this class names the image, the frontend
// (or #20c's player) shows it -- image() and gif() are what to draw, and
// itemsShown() is the menu items' visibility (MenuItems.Visible).
//
// Cold code (the menu). No virtuals, no templates, no exceptions.
// ============================================================================

#include <stdint.h>

#include <vector>

#include "formats/gif.h"

namespace mdk {

namespace formats {
class MdkData;
}

class Slideshow {
public:
    // C# Bare: the image shown without the menu items.
    static const int32_t kBare = 1;

    // C# Start: the slides loaded once (MISC/MDKS_001.GIF, _002, ... up to the
    // first that is missing or not a GIF), the wait started from the video's
    // last frame (image 0).
    void start(const formats::MdkData& data);
    // C# Update: the time; at the image's duration, the next one.
    void update(float delta);
    // C# Reset: menu input -- the wait starts again, and the first slide gives
    // way at once.
    void reset();

    bool running() const { return running_; }
    // The image shown: 0 the video's last frame, 1... the slides.
    int32_t image() const { return image_; }
    // Whether the menu items show over the image (C# ItemsShown).
    bool itemsShown() const { return image_ != kBare; }
    float time() const { return time_; }
    int32_t slideCount() const { return (int32_t)slides_.size(); }
    // Slide `image` (1...), or null for image 0 or one out of range.
    const formats::Gif* gif(int32_t image) const;

private:
    float duration() const;
    void next();

    std::vector<formats::Gif> slides_;
    float time_ = 0.0f;
    bool running_ = false;
    int32_t image_ = 0;
};

} // namespace mdk

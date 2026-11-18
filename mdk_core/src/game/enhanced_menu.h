#pragma once

// ============================================================================
// enhanced_menu.h -- the in-game Video/Graphics Settings menu
// (SPEC.md 7.9): the five Enhanced Mode toggles, flipped live during
// gameplay for side-by-side A/B comparison.
//
// This class is the menu's LOGIC and its canvas drawing. It owns no
// platform input and no file: the frontend translates its buttons into
// the edge struct below, calls handle(), and when handle() says a
// toggle changed, applies it (mdk_render_set_enhanced_toggles) and
// persists it (writeEnhancedConfig to the platform's config.ini). The
// gameplay is paused by the frontend while the menu is open -- this
// class only ever sees one frame's edges at a time.
// ============================================================================

#include <stdint.h>

#include "render/render.h"
#include "render/render_types.h"

namespace mdk {

class FontView;

class EnhancedMenu {
public:
    // One frame's button EDGES (pressed this frame, not held).
    struct Input {
        bool up = false;
        bool down = false;
        bool left = false;
        bool right = false;
        bool confirm = false;  // flip the selected toggle
        bool close = false;    // leave the menu
    };

    static const int32_t kRowCount = 8;

    bool open() const { return open_; }
    void setOpen(bool open) { open_ = open; }
    int32_t selected() const { return selected_; }

    // Advance the menu by one frame's edges over `toggles`. Returns
    // true when a toggle's value changed (the caller applies and
    // persists). Closing is reported through open().
    bool handle(const Input& input, EnhancedToggles* toggles);

    // Draw the panel and its rows on the canvas (between
    // mdk_render_canvas_begin/end): the title in the big font, the
    // rows and the hint in the small one (the oracle's Fonts pair).
    // `canvasWidth` is the canvas's current width; the panel centres
    // itself in it.
    void draw(MdkRenderer renderer, FontView& big, FontView& small, float canvasWidth,
              const EnhancedToggles& toggles) const;

private:
    bool open_ = false;
    int32_t selected_ = 0;
};

} // namespace mdk

#pragma once

// ============================================================================
// enhanced_menu.h -- the in-game PAUSE menu: Resume, the Video/Graphics
// Settings (SPEC.md 7.9: the Enhanced Mode toggles, flipped live during
// gameplay for side-by-side A/B comparison), and Quit.
//
// START opens it (owner, 2026-11-19: START used to quit the game outright on
// one press, and the effects menu on SELECT was undiscovered); SELECT opens the
// same menu. It opens on RESUME, so a stray A resumes rather than quits; QUIT is
// a row of its own, reached with the D-pad (up from RESUME wraps to it) and
// confirmed with A -- never a single press.
//
// This class is the menu's LOGIC and its canvas drawing. It owns no
// platform input and no file: the frontend translates its buttons into
// the edge struct below, calls handle(), and acts on its answer: a toggle
// changed (apply with mdk_render_set_enhanced_toggles, persist with
// writeEnhancedConfig to the platform's config.ini), the menu closed, or
// QUIT chosen. The gameplay is paused by the frontend while the menu is
// open -- this class only ever sees one frame's edges at a time.
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
        bool confirm = false;  // A: flip the selected toggle, or take RESUME / QUIT
        bool close = false;    // START, B or SELECT: leave the menu (resume)
    };

    // What one frame's edges did.
    enum class Action : uint8_t {
        None,
        Toggled,   // a toggle's value changed (the caller applies and persists)
        Closed,    // the menu closed (RESUME, or a close edge): play resumes
        Quit,      // QUIT confirmed with A: the caller ends the run
    };

    // The rows: RESUME, the toggles, QUIT.
    static const int32_t kToggleCount = 10;
    static const int32_t kResumeRow = 0;
    static const int32_t kQuitRow = kToggleCount + 1;
    static const int32_t kRowCount = kToggleCount + 2;

    bool open() const { return open_; }
    // Opening always starts on RESUME.
    void setOpen(bool open) {
        open_ = open;
        if (open) {
            selected_ = kResumeRow;
        }
    }
    int32_t selected() const { return selected_; }

    // Advance the menu by one frame's edges over `toggles`. Up and down wrap
    // (up from RESUME is QUIT). Left/right/A flip a toggle row; A on RESUME
    // closes; A on QUIT is Quit (the menu stays open: the caller is leaving).
    Action handle(const Input& input, EnhancedToggles* toggles);

    // The label of a row ("RESUME", "HAZE", "QUIT").
    static const char* rowLabel(int32_t row);

    // Draw the panel and its rows on the canvas (between
    // mdk_render_canvas_begin/end): the title in the big font, the
    // rows and the hint in the small one (the oracle's Fonts pair).
    // `canvasWidth` is the canvas's current width; the panel centres
    // itself in it.
    void draw(MdkRenderer renderer, FontView& big, FontView& small, float canvasWidth,
              const EnhancedToggles& toggles) const;

private:
    bool open_ = false;
    int32_t selected_ = kResumeRow;
};

} // namespace mdk

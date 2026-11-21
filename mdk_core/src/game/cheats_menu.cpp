// cheats_menu.cpp -- see cheats_menu.h. Cold path: runs only while the game is
// paused, one frame's edges at a time.

#include "game/cheats_menu.h"

#include "game/font_view.h"
#include "render/render_types.h"

namespace mdk {
namespace {

// A flat canvas rectangle (enhanced_menu.cpp's fillRect).
void fillRect(MdkRenderer renderer, float x, float y, float w, float h, const Colour& colour) {
    CanvasQuad q;
    q.x0 = x; q.y0 = y;
    q.x1 = x + w; q.y1 = y;
    q.x2 = x + w; q.y2 = y + h;
    q.x3 = x; q.y3 = y + h;
    q.u0 = 0.0f; q.v0 = 0.0f;
    q.u1 = 1.0f; q.v1 = 0.0f;
    q.u2 = 1.0f; q.v2 = 1.0f;
    q.u3 = 0.0f; q.v3 = 1.0f;
    q.colour = colour;
    q.texture = -1;
    q.palette = -1;
    mdk_render_canvas_quad(renderer, &q);
}

bool* switchOf(int32_t row, CheatsMenu::Switches* switches) {
    switch (row) {
        case CheatsMenu::kGoreRow: return &switches->gore;
        case CheatsMenu::kGodRow: return &switches->god;
        case CheatsMenu::kNoclipRow: return &switches->noclip;
        default: return nullptr;
    }
}

bool switchValue(int32_t row, const CheatsMenu::Switches& switches) {
    switch (row) {
        case CheatsMenu::kGoreRow: return switches.gore;
        case CheatsMenu::kGodRow: return switches.god;
        case CheatsMenu::kNoclipRow: return switches.noclip;
        default: return false;
    }
}

} // namespace

const char* CheatsMenu::rowLabel(int32_t row) {
    switch (row) {
        case kGoreRow: return "GORE";
        case kGodRow: return "GOD MODE";
        case kNoclipRow: return "NOCLIP";
        case kBackRow: return "BACK";
        default: return "";
    }
}

CheatsMenu::Action CheatsMenu::handle(const EnhancedMenu::Input& input, Switches* switches) {
    if (!open_ || !switches) {
        return Action::None;
    }
    if (input.close) {
        open_ = false;
        return Action::Closed;
    }
    if (input.up) {
        selected_ = selected_ > 0 ? selected_ - 1 : kRowCount - 1;
    }
    if (input.down) {
        selected_ = selected_ < kRowCount - 1 ? selected_ + 1 : 0;
    }
    if (selected_ == kBackRow) {
        if (input.confirm) {
            open_ = false;
            return Action::Closed;
        }
        return Action::None;
    }
    if (!(input.confirm || input.left || input.right)) {
        return Action::None;
    }
    bool* flag = switchOf(selected_, switches);
    *flag = !*flag;
    return Action::Changed;
}

void CheatsMenu::draw(MdkRenderer renderer, FontView& big, FontView& small, float canvasWidth,
                      const Switches& switches) const {
    if (!open_ || !big.ready() || !small.ready()) {
        return;
    }
    // The START menu's panel (enhanced_menu.cpp), four rows tall.
    const float kPanelW = 400.0f;
    const float kPanelH = 172.0f;
    const float px = (canvasWidth - kPanelW) * 0.5f;
    const float py = 8.0f;
    const Colour kPanel{0.0f, 0.0f, 0.0f, 0.72f};
    const Colour kTitle{1.0f, 0.85f, 0.4f, 1.0f};
    const Colour kRow{0.75f, 0.75f, 0.75f, 1.0f};
    const Colour kSelected{1.0f, 1.0f, 1.0f, 1.0f};
    const Colour kOn{0.45f, 1.0f, 0.45f, 1.0f};
    const Colour kOff{1.0f, 0.45f, 0.45f, 1.0f};
    const Colour kHintColour{0.55f, 0.55f, 0.55f, 1.0f};
    const Colour kMark{1.0f, 0.85f, 0.4f, 1.0f};

    fillRect(renderer, px, py, kPanelW, kPanelH, kPanel);
    const char* title = "CHEATS";
    big.draw(renderer, title, px + (kPanelW - (float)big.width(title)) * 0.5f, py + 34.0f, 1.0f,
             kTitle);
    for (int32_t row = 0; row < kRowCount; row++) {
        const float baseline = py + 62.0f + (float)row * 22.0f;
        const bool isSelected = row == selected_;
        if (isSelected) {
            small.draw(renderer, ">", px + 14.0f, baseline, 1.5f, kMark);
        }
        small.draw(renderer, rowLabel(row), px + 30.0f, baseline, 1.5f,
                   isSelected ? kSelected : kRow);
        if (row == kBackRow) {
            continue;
        }
        const bool on = switchValue(row, switches);
        const char* value = on ? "ON" : "OFF";
        small.draw(renderer, value, px + kPanelW - 24.0f - (float)small.width(value) * 1.5f,
                   baseline, 1.5f, on ? kOn : kOff);
    }
    small.draw(renderer, "A: TOGGLE   B: BACK", px + 30.0f, py + kPanelH - 10.0f, 1.0f,
               kHintColour);
}

} // namespace mdk

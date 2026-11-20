#pragma once

// ============================================================================
// briefing.h -- the briefing before a level (C# src/Mdk.Game/Menu/StatsScreen.cs
// with StatsPages.Briefing, its one page; and Menu/Typesetter.cs).
//
// The level's map (MISC/STATS.BNI's L<n>_MAP) fades in from black over 0.5 s,
// BRIEF<n> (MDKFONT.FTI) is typed over it in FONTBIG at 15 characters a second
// (60 while Fire or Jump is held) with a blinking cursor, then the page waits
// for a key and fades out to black; then the fall, then the level (flow
// Event::Fall). Esc finishes the typing at once; holding Fire or Jump runs it
// all twice as fast.
//
//   new game / a save made before a level -> briefing -> fall -> level
//
// The statistics' pages (intermission, debriefing, Score-O-matic) and the save
// prompt between them are package #20b's and #19's; StatsScreen's last page is
// this briefing again, so #20b reuses this class for it.
//
// NOT PORTED, WITH CAUSE: the TELETYPE sound per character (the menus are
// silent: docs/OPEN_ITEMS.md).
//
// Cold code (between levels). No virtuals, no templates, no exceptions.
// ============================================================================

#include <stdint.h>

#include <vector>

#include "game/game_flow.h"
#include "render/render.h"

namespace mdk {

class FontView;
struct ScreenView;

namespace formats {
class Font;
class Fti;
}

// C# Typesetter: lays out the briefings' and debriefings' texts like 0x4335c0.
// Codes (a decimal number N, possibly negative, may precede the letter):
//   \c, \Nc  end the line; the next is centred on x 300 (or N); y unchanged
//   \n, \Nn  end the line; x 0, left-aligned; y += 36 (+ N)
//   \y, \Ny  end the line; x 0, left-aligned; y += 36 (or N)
//   \x, \Nx  end the line; left-aligned at x N (or 0)
//   \Np      pause: the budget loses N characters
//   \d, \i   characters cost one / appear at once
struct TypesetLine {
    std::vector<uint8_t> text;
    float x = 0.0f;  // its centre when centred
    float y = 0.0f;
    bool centred = false;
    // The character count before it.
    int32_t start = 0;
};

// The lines of a text whose first baseline is `y`; returns its character count.
int32_t typesetLayout(const uint8_t* text, int32_t length, float y,
                      std::vector<TypesetLine>* lines);

class Briefing {
public:
    // C# StatsScreen(StatsPages.Briefing): the briefing of `level` (its index in
    // the order, or the first). `fti` gives BRIEF<n>; null types nothing.
    void open(const formats::Fti* fti, int32_t level);

    // C# Frame and Update: `anyPressed` is the C#'s AnyPressed (any key or
    // button this frame), `backPressed` Esc (MenuKey.Back), `fastHeld` Fire or
    // Jump held. Returns Event::Fall once the page has
    // faded out, None before.
    flow::Event frame(bool anyPressed, bool backPressed, bool fastHeld, float elapsed);

    // C# Draw: the map (`map`: the page's image, may be empty), the typed text
    // in `big`, the fade (between mdk_render_canvas_begin and _end).
    void draw(MdkRenderer renderer, const ScreenView& view, const FontView& big, int32_t mapTexture,
              int32_t mapPalette, int32_t mapWidth, int32_t mapHeight) const;

    // The index of the level in the order (C# _index): the map is L<index+1>_MAP.
    int32_t index() const { return index_; }
    // For the goldens.
    float fadeIn() const { return fadeIn_; }
    float fadeOut() const { return fadeOut_; }
    int32_t current() const { return current_; }
    float count() const { return count_; }
    int32_t shown() const { return shown_; }
    float ticks() const { return ticks_; }
    flow::Event next() const { return next_; }

private:
    bool updateTyping(float delta);
    void update(float delta, bool fastHeld);
    void drawTyped(MdkRenderer renderer, const ScreenView& view, const FontView& big,
                   const std::vector<uint8_t>& text, float y, int32_t budget) const;

    int32_t index_ = 0;
    std::vector<std::vector<uint8_t>> texts_;
    std::vector<float> textY_;
    float fadeIn_ = 0.0f;
    float fadeOut_ = -1.0f;
    bool fast_ = false;
    bool skip_ = false;
    bool pressed_ = false;
    float ticks_ = 0.0f;
    int32_t current_ = 0;
    float count_ = 1.0f;
    int32_t shown_ = 0;
    flow::Event next_ = flow::Event::None;
};

} // namespace mdk

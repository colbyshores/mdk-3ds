#pragma once

// ============================================================================
// briefing.h -- the briefings' and debriefings' typesetting (C#
// src/Mdk.Game/Menu/Typesetter.cs).
//
// The briefing page itself -- the level's map fading in, BRIEF<n> typed over it,
// a key to fade out, then the fall -- was package #20a's `Briefing` class; since
// package #20b it is StatsScreen's Briefing page (game/stats_screen.h), as in the
// C#, where the briefing is StatsScreen with StatsPages.Briefing. What stays here
// is the Typesetter both typed pages (the debriefing, the briefing) lay out with.
//
// Cold code (between levels). No virtuals, no templates, no exceptions.
// ============================================================================

#include <stdint.h>

#include <vector>

namespace mdk {

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

} // namespace mdk

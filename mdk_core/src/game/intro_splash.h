#pragma once

// ============================================================================
// intro_splash.h -- the INTRO1A splash before the main menu (C#
// src/Mdk.Game/Menu/IntroSplash.cs; 0x426edc): at start, after Kurt dies and
// after the end movies. A 600 x 360 still (MISC/OPTIONS.BNI's INTRO1A: two
// 768-byte palettes, then run-length rows, 0x426e04) whose palette fades; any
// key skips it.
//
//   black --1 s--> palette 1 --3 s--> --2 s--> palette 2 --3 s--> --1 s--> black
//
// The logic, the unpacking and the blend; the frontend draws. A frontend that
// can change a palette cheaply (Linux: the palette is its own GPU texture) draws
// the picture through palette(), the C#'s exact Draw. One that cannot (the 3DS
// expands an index texture through its palette on the CPU, a whole 600 x 360
// picture each time) draws the two palettes' pictures once each and
// cross-fades them on the GPU with step()'s weights: from*(1-w) + to*w is
// Blend's arithmetic, up to the framebuffer's rounding (owner 2026-11-18,
// performance over accuracy on the 3DS).
//
// Cold code (a menu). No virtuals, no templates, no exceptions (AGENTS.md §10).
// ============================================================================

#include <stdint.h>

#include <vector>

namespace mdk {

namespace formats {
class Bni;
}

namespace menu {

class IntroSplash {
public:
    static constexpr int32_t kWidth = 600;
    static constexpr int32_t kHeight = 360;
    static constexpr int32_t kPaletteSize = 768;
    static constexpr int32_t kStepCount = 5;

    // C# Steps: seconds, the palette at its start and at its end (0 black, 1, 2).
    struct Step {
        float seconds;
        int32_t from;
        int32_t to;
    };
    static const Step kSteps[kStepCount];

    // The C# constructor: OPTIONS.BNI's INTRO1A, or Done at once without it.
    void open(const formats::Bni& options);

    // C# Update: any key ends it; otherwise the clock, and the next step when
    // this one's seconds are up (the clock starts again from 0).
    void update(bool anyPressed, float delta);

    bool done() const { return done_; }
    int32_t step() const { return step_; }
    float time() const { return time_; }

    // The picture (kWidth x kHeight indices) and palettes 0 (black), 1, 2.
    const std::vector<uint8_t>& picture() const { return picture_; }
    const uint8_t* paletteRgb(int32_t which) const { return palettes_[which]; }

    // C# Draw's palette: Blend(from, to, clamp(time / seconds, 0, 1)) of the
    // step. Null when the splash is done (Draw draws nothing).
    const uint8_t* palette();
    // C# Draw's blend, as the weight of `to` (0..256, Blend's rounding) and the
    // two palettes it is between: the GPU cross-fade's numbers. False when done.
    bool fade(int32_t* from, int32_t* to, int32_t* weight) const;

    // C# Blend (0x417040, 0x4170ec): `t` of the way between two palettes, the
    // weight rounded to 1/256 (MathF.Round: half to even).
    static void blend(const uint8_t* from, const uint8_t* to, float t, int32_t count,
                      uint8_t* out);
    static int32_t weightOf(float t);
    // C# Unpack: runs from `p`; a count above 127 copies 256 - count bytes, a
    // smaller one repeats the next byte, 0 ends. `size` bytes out (zeros past the
    // end). The C# throws on a run that overruns the input; this stops there.
    static std::vector<uint8_t> unpack(const uint8_t* bytes, int32_t length, int32_t p,
                                       int32_t size);

private:
    uint8_t palettes_[3][kPaletteSize] = {};
    uint8_t blended_[kPaletteSize] = {};
    std::vector<uint8_t> picture_;
    int32_t step_ = 0;
    float time_ = 0.0f;
    bool done_ = true;
};

} // namespace menu
} // namespace mdk

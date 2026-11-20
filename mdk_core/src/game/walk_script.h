#pragma once

// ============================================================================
// walk_script.h -- the hardware harness's scripted walk (a HARNESS AID).
//
// The hardware profile runs on a console nobody touches, so a hitch that
// needs Kurt to MOVE (the owner's "hitch when entering a room") could not be
// reproduced by it. A walk script is a tiny list of frame ranges, each holding
// game actions (the mdk::Key names), which the frontend writes into the
// frame's Input after the platform poll and before the game reads it:
//
//     forward@60-600;turnleft@600-640;forward+turbo@640-900
//
//   * a segment is `<keys>@<first>-<end>`: the keys are held on every frame f
//     with first <= f < end (END IS EXCLUSIVE, so `a@60-600;b@600-640` abut);
//   * `<keys>` is one action name or several joined by '+';
//   * segments are separated by ';', and may overlap (the keys are OR'd);
//   * frame numbers are the frontend's frame counter, from 0. With a fixed
//     step (the 3DS's FIXED=1, Linux's --fixed-step) frame f runs simulation
//     step f, so the same script reaches the same place on both frontends.
//
// While a script is set it OWNS the actions: every frame the Input's held
// actions and its accumulated look are cleared and the script's keys are set,
// so a bumped console or a stray pad cannot steer the walk.
//
// The action names (lower case): forward, back, strafeleft, straferight,
// turnleft, turnright, jump, turbo, fire, useitem, itemnext, itemprevious, sniper,
// zoomin, zoomout (sniper mode and its scope zoom).
// Escape is not walkable: the frontends read the quit chord elsewhere.
//
// Cold code: parsed once at boot. walkKeysAt is a per-frame scan of at most
// kMaxSegments entries, all integer.
// ============================================================================

#include <stddef.h>
#include <stdint.h>

#include "game/input.h"

namespace mdk {

struct WalkSegment {
    uint32_t first = 0;   // the first frame holding the keys
    uint32_t end = 0;     // one past the last (exclusive)
    uint32_t keys = 0;    // bit k = mdk::Key k
};

struct WalkScript {
    static const int32_t kMaxSegments = 32;
    // A frame number past this is refused: 1.6e6 frames is over seven hours
    // at 60 Hz, and the bound keeps every sum in the parser far from overflow.
    static const uint32_t kMaxFrame = 1u << 24;
    WalkSegment segments[kMaxSegments];
    int32_t count = 0;
};

// Parse `text` into `out`. On a refusal returns false, leaves `out` empty and
// writes one sentence naming the offending segment into `error` (which may be
// null). An empty or null text is a refusal: a WALK build with no walk is a
// mistake, not a default.
bool parseWalkScript(const char* text, WalkScript* out, char* error, size_t errorSize);

// The keys held at `frame` (the OR of every segment covering it).
uint32_t walkKeysAt(const WalkScript& script, uint32_t frame);

// Index of the first segment that STARTS at `frame`, or -1 (a cold per-frame
// question for the "segment started" log line; several segments starting on
// the same frame are reported by the first).
int32_t walkSegmentStartingAt(const WalkScript& script, uint32_t frame);

// Write the frame's keys into `input`: every action cleared, then the script's
// keys set; the accumulated look is cleared too (the script owns the input).
void applyWalk(const WalkScript& script, uint32_t frame, Input* input);

// The action's script name ("forward"), or "?" for a key outside the table.
const char* walkKeyName(Key key);

// The canonical spelling of `keys` ("forward+turbo"), into `buffer`.
// Returns the length written (truncated to fit, always terminated).
int32_t formatWalkKeys(uint32_t keys, char* buffer, size_t size);

} // namespace mdk

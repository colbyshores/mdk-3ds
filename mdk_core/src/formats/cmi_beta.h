#pragma once

// ============================================================================
// cmi_beta.h — the 1996 demo's LEVELn.CMI.
//
// A 1:1 port of src/Mdk.Formats/Cmi.Beta.cs.
//
// THE FILE: a `u32 size`, the four directories of the retail file (the
// offsets are from 4 and the names are in lower case, so they are stored in
// UPPER CASE here), the scripts (an earlier bytecode) and their paths, the
// models (the retail format) and the animations, which the scripts point at.
//
// AN ARENA'S RECORD IS TWO STRINGS: its music and its ambient loop. The
// demo's arenas have no scripts yet.
//
// THE PATHS. The demo's scripts follow paths, and the retail VM wants
// splines. So the paths are turned into retail spline records and APPENDED
// to the file, and the old offset of each path is mapped to the new one.
//
// A path is a `u32 count`, the first position, and then `count - 1` steps
// of three floats, one per frame. A spline key has a frame, the position,
// and the incoming and the outgoing tangents.
// ============================================================================

#include <stdint.h>

#include <vector>

namespace mdk {
namespace formats {

// The size of one step of a demo path: three floats.
extern const int32_t kBetaPathStepSize;

// The size of a retail spline key: the frame, the position, the incoming
// tangent and the outgoing tangent.
extern const int32_t kBetaPathKeySize;

// The size of the path's count.
extern const int32_t kBetaPathCountSize;

// A demo path as a spline with a key per frame whose tangents are the steps
// to its neighbours (straight lines between them); it starts again from its
// first position after the last frame.
//
// The source is the file the path is in, and the path is its absolute
// offset.
std::vector<uint8_t> betaSplineOf(const uint8_t* source, int32_t byteCount, int32_t path);

} // namespace formats
} // namespace mdk

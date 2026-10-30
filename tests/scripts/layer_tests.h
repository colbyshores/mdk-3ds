#pragma once

// ============================================================================
// layer_tests.h — the depth layers and the depth pull, checked against the
// oracle's own expectations.
//
// The C# twins are tests/Mdk.Game.Tests/LayerTests.cs and
// tests/Mdk.Game.Tests/DepthPullTests.cs. This file runs the same cases: the
// same synthetic arenas, the same two level-6 arenas, and the same camera
// the depth-pull tests use.
//
// The function returns the number of failed checks, so the caller adds it to
// its own counter.
// ============================================================================

#include <string>

namespace mdk {
namespace formats {
class MdkData;
} // namespace formats

namespace test {

// The depth layers of the coplanar arena triangles and the depth pull that
// keeps them apart. `data` is the MDK installation (the level-6 arenas come
// from it).
int runDepthLayerTests(const formats::MdkData& data);

} // namespace test
} // namespace mdk

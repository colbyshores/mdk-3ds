#pragma once

// ============================================================================
// object_warm_tests.h -- the object warm (game/object_view.h warmObjectStep)
// against the object pass's own layouts, on real level data: every textured
// batch a layout draws, for the arena's own models and every global (CMI)
// model, is handed to the warm's callback with the SAME (texture, palette) key
// the object pass will ask the surface cache for -- or the warm's pending id
// would never be found at the draw. One model per call, and a finished arena
// hands over nothing more. Returns the number of failed checks.
// ============================================================================

namespace mdk {
namespace formats {
class MdkData;
} // namespace formats

namespace test {
int runObjectWarmTests(const formats::MdkData& data);
} // namespace test
} // namespace mdk

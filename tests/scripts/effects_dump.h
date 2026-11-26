#pragma once

// ============================================================================
// effects_dump.h — EffectsView::build over a debris storm (golden/L8_effects_storm.txt).
//
//   tick1 dump effects
//
// runs the L8_hazards_debris schedule (a second XBANG exploding beside GUNT_2's: up to
// 361 debris pieces and 96 sprite effects) and, after every step, builds the effects
// frame under six cameras -- the 3DS's frustum and tiny-debris culls at the default and
// the widest stereo margin, a scope-like zoom, an eye inside the cloud (near plane), a
// frustum edge through the cloud, and no cull (the Linux build) -- printing, per
// camera, the counts and an FNV-1a hash of every vertex, draw and halo as BITS. Then the
// edge cases: a zero-size and a NaN-size spark, a texture measured late (its pixels
// away at the entry's making, back at a later build), the view cleared and rebuilt, and
// the sprites drawn through another arena's palette.
//
// The golden was written by the build before the effects caches (the culling radius
// and the halo tint), so it is the old-versus-new differential: any change in a cull
// decision, a tint bit or the draw order fails it.
//
// MDK_FX_BENCH=<n> in the environment also times n extra builds of each camera's frame
// per step and prints the totals to stderr (never to the golden).
// ============================================================================

#include <string>

namespace mdk {
class LevelData;
class Player;
namespace scripts {
class ScriptRuntime;
}  // namespace scripts
namespace test {

struct EffectsTrace;

EffectsTrace* effectsTraceNew();
// After a step: the six cameras' frames.
void effectsTraceStep(EffectsTrace* trace, LevelData& level, scripts::ScriptRuntime& runtime,
                      const Player& player, int step);
// The edge cases, then the text; frees the trace.
void effectsTraceEnd(EffectsTrace* trace, LevelData& level, scripts::ScriptRuntime& runtime,
                     const Player& player, std::string* out);

}  // namespace test
}  // namespace mdk

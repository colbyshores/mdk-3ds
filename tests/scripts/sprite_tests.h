#pragma once

// ============================================================================
// sprite_tests.h — the sprite cache's residency policy, checked.
//
// THE C# HAS NO SUCH POLICY, so there is no C# twin to copy: the C# keeps
// every sprite frame it has ever drawn (KurtSprite's texture cache is keyed
// by (name, frame) and nothing evicts from it), and the desktop can afford
// that. The 3DS cannot, so the port bounds the cache, and THIS file pins
// the bound's arithmetic and its choice of victim.
//
// It is a POLICY test, not an oracle test: the numbers here are the port's
// own, and the picture they produce is pinned by the frame golden instead.
//
// The function returns the number of failed checks, so the caller adds it to
// its own counter.
// ============================================================================

namespace mdk {
namespace test {

// The residency policy: the budget, the accounting, and the victim.
int runSpriteResidencyTests();

} // namespace test
} // namespace mdk

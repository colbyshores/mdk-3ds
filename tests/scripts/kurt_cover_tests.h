#pragma once

// ============================================================================
// kurt_cover_tests.h -- the surfaces Kurt's sprite is drawn over
// (game/kurt_cover.h; docs/DIVERGENCES.md D8), on hand-made scenes:
//   * the floor under him and the wall he walks along (their planes leave the
//     camera and his middle on one side) are marked;
//   * a pillar's face between the camera and him is not (it may hide him);
//   * a Solid back face is not (it is not drawn), a glass one is, as glass;
//   * a triangle outside the pyramid from the eye through his sprite, or
//     beyond his depth, is not;
//   * a one-triangle scene and a many-triangle one both build their box tree
//     (the one-leaf tree is its own root) and find what they should;
//   * the marks are cut to the pyramid: no corner outside it.
// Returns the number of failed checks.
// ============================================================================

namespace mdk {
namespace test {
int runKurtCoverTests();
} // namespace test
} // namespace mdk

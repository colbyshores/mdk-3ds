#pragma once

// ============================================================================
// mirror_cube_tests.h -- the 3DS mirrors' cube maps (render/mirror_cube.h)
// checked on the host:
//   * the face convention: every texel's direction (mirrorCubeDirection), put
//     through the PICA's cube LOOKUP written out here independently (major
//     axis, OpenGL's (sc, tc) per face, t = 1 at the first memory row), comes
//     back to the same face and the same texel;
//   * every texel's cell against the oracle's mirror.hlsl mapping in libm
//     (double), within one panorama cell, and nearly all of them exactly;
//   * the row shift and the wrap (mirrorCubeFace) against mirror.hlsl's
//     fmod(fmod(row + shift, H) + H, H);
//   * a mirror draw finds the cube of ITS row shift (pica::mirrorCubeFor,
//     MaterialResolver's shift for each of MIRRLOW...MIRRHIGH), and the
//     nearest made one for a shift the level's arenas do not use;
//   * the face expansion keeps index 0 as palette[0]'s colour (no discard);
//   * with game data: the retail levels' row shifts (LEVEL6: -80..80 by 40,
//     LEVEL7: none).
// Returns the number of failed checks.
// ============================================================================

namespace mdk {
namespace formats {
class MdkData;
}
namespace test {
int runMirrorCubeTests();
int runMirrorCubeDataTests(const formats::MdkData& data);
} // namespace test
} // namespace mdk

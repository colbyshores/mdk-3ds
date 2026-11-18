#pragma once

// ============================================================================
// audio_stream_tests.h -- arena streaming Phase 5: the music archive read by its
// directory, the bank's stepped prefetch, and the level's music.
//
// What is pinned, and against what:
//
//   * an SNI that was open()ed (directory only) answers every entry of every
//     retail archive with the bytes the whole-file parse answers: MEASURED, the
//     two forms of the port read the same file;
//   * a bank over the opened music archive decodes every sound to the samples
//     the whole-file bank decodes: MEASURED the same way;
//   * the prefetch's step count is the original's rule (0x4317d8: 0xf768 bytes
//     while more than 0x10000 remain, then the rest), computed in the test from
//     those two constants and the entry's length;
//   * LevelMusic is TRANSLITERATED from src/Mdk.Game/Audio/LevelMusic.cs: the
//     C#'s Viewer drives it and the headless oracle does not, so there is no
//     golden; every expected number is computed from the C#'s constants
//     (FadeIn = 0x80 * 1000/34, FadeOut = 0x100 * 1000/34) and cited.
//
// Needs the game data. Returns the number of failed checks.
// ============================================================================

namespace mdk {
namespace formats {
class MdkData;
} // namespace formats

namespace test {

int runAudioStreamTests(const formats::MdkData& data);

} // namespace test
} // namespace mdk

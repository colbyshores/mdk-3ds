#pragma once

// ============================================================================
// movie_tests.h -- the movies, the splash and the loading screen (package #20c)
// against the C# oracle.
//
// THE ORACLE IS tests/scripts/movie_probe: the C#'s own Flc and Mve decoders,
// IntroSplash, VideoPlayer, MainMenu (its splash and MDK12.FLC), EndMovie and
// LoadingScreen at the game's 1/60 s step. Its output is
// golden/movie_<section>.txt; movieDump(section) drives the C++ port the same
// way and must print the same bytes (`vm_tests dump-movie <section>`). The
// sections: decode, splash, menu, endmovie, loading.
//
// Needs the game data. The function returns the number of failed checks.
// ============================================================================

#include <string>

namespace mdk {
namespace formats {
class MdkData;
}

namespace test {

// The C++ side of one movie_probe section, or empty for an unknown one.
std::string movieDump(const formats::MdkData& data, const char* section);

// Every section against its golden.
int runMovieTests(const formats::MdkData& data);

} // namespace test
} // namespace mdk

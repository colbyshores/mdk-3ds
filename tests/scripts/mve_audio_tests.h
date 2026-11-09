#pragma once

namespace mdk {
namespace test {

// The MVE audio decode (formats/mve.cpp's decodePcm), checked against the C#
// oracle's own expression on a movie built here, so it runs without game data.
int runMveAudioTests();

}  // namespace test
}  // namespace mdk

#pragma once

namespace mdk {
namespace test {

// The MVE audio decode (formats/mve.cpp's decodeAudio, decodePcm and the
// silence opcode), checked against the C# oracle's own expressions on a movie
// built here, so it runs without game data.
int runMveAudioTests();

}  // namespace test
}  // namespace mdk

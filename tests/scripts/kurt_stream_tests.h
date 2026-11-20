#pragma once

// ============================================================================
// kurt_stream_tests.h -- Kurt's pack at level load and its stream, checked
// against a fake backend (no GPU, no reader thread, no game data).
//
// THE 3DS CANNOT BE RUN HERE, so this is the only check of the stream's logic:
// the start set imported from one buffer with no request made; a frame outside
// it standing in as the index texture while its animation's ONE range read is
// in flight, and upgraded when the bytes land; one read in flight at a time; a
// failed or short read logged once and never asked again; a stale read (a
// re-load since) dropped; a bad index entry costing only its own frame; and the
// staging cache held to its bound, least recently drawn first.
//
// The function returns the number of failed checks.
// ============================================================================

namespace mdk {
namespace test {

int runKurtStreamTests();

} // namespace test
} // namespace mdk

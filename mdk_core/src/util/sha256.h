#pragma once

// ============================================================================
// sha256.h -- the SHA-256 hash (FIPS 180-4), incremental.
//
// Needed by the mods' texture keys (game/hd_key.h): a mod image names the
// exact texture-through-a-palette it replaces by the first eight bytes of
// this hash, exactly as the C#'s HdKey does with SHA256.HashData. There is
// no project hash today, and pulling a library for one 16-hex-digit key
// would be a dependency out of proportion to the job, so the compression
// function lives here and is pinned against the standard test vectors in
// tests/formats (the empty string and "abc").
// ============================================================================

#include <stdint.h>
#include <stddef.h>

namespace mdk {

// An incremental SHA-256. Feed bytes with update(), read the digest with
// finish(). finish() is terminal: call reset() before reusing the object.
class Sha256 {
public:
    Sha256();

    void reset();
    void update(const uint8_t* data, size_t size);
    void finish(uint8_t out[32]);

private:
    void transform(const uint8_t* block);

    uint32_t state_[8];
    uint64_t bytes_ = 0;
    uint8_t block_[64];
    size_t blockSize_ = 0;
};

} // namespace mdk

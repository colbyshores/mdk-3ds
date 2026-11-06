// ============================================================================
// limiter.cpp — the master limiter (port of Limiter.cs).
// ============================================================================

#include "audio/limiter.h"

#include <math.h>

namespace mdk {
namespace audio {

namespace {

const float kCeilingDb = -0.5f;
// Godot's default release.
const float kReleaseSeconds = 0.1f;

} // namespace

Limiter::Limiter(int32_t sampleRate) {
    // Limiter's `Ceiling`, computed once from -0.5 dB. The C# computes it in a
    // static initializer; caching it here keeps a powf out of the per-frame mix
    // and is the same value, because the expression has no inputs. Checked
    // bit-for-bit against MathF.Pow on this host (0x3F71ADF9).
    /* @vfp-exempt R6 — the limiter's ceiling constant: one powf per Limiter,
     * not per sample or per frame, and the expression is constant so the cache
     * cannot drift. measured n/a. Verified 2026-11-05. */
    ceiling_ = powf(10.0f, kCeilingDb / 20.0f);
    // `_release = 1f - MathF.Exp(-1f / (ReleaseSeconds * sampleRate))`, once for
    // the same reason (0x396DC000 against MathF.Exp).
    /* @vfp-exempt R6 — the limiter's release constant, one expf per Limiter for
     * a constant expression; the per-frame path reads the cached float.
     * measured n/a. Verified 2026-11-05. */
    release_ = 1.0f - expf(-1.0f / (kReleaseSeconds * (float)sampleRate));
}

float Limiter::ceiling() const { return ceiling_; }

void Limiter::process(float* stereo, int32_t frameCount) {
    for (int32_t f = 0; f < frameCount; f++) {
        float* pair = stereo + (size_t)f * 2u;
        const float left = fabsf(pair[0]);
        const float right = fabsf(pair[1]);
        const float peak = left > right ? left : right;
        /* @vfp-exempt R3 — the limiter's peak test: `peak > Ceiling ? Ceiling /
         * peak : 1` is the oracle's own predicate and a different one changes
         * which frames get attenuated, i.e. the samples. measured n/a (corpus
         * empty). Verified 2026-11-05. */
        const float limit = peak > ceiling_ ? ceiling_ / peak : 1.0f;
        // MathF.Min(_gain + (1 - _gain) * _release, limit), in the same order.
        gain_ = gain_ + (1.0f - gain_) * release_;
        gain_ = gain_ < limit ? gain_ : limit;
        if (gain_ >= 1.0f) {
            continue;
        }
        pair[0] *= gain_;
        pair[1] *= gain_;
    }
}

} // namespace audio
} // namespace mdk

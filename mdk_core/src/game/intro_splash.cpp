// intro_splash.cpp -- see intro_splash.h. C# src/Mdk.Game/Menu/IntroSplash.cs,
// method by method.

#include "game/intro_splash.h"

#include <math.h>
#include <string.h>

#include "formats/bni.h"

namespace mdk {
namespace menu {
namespace {

// C# Entry, FullWeight, WeightShift, CopyLimit, ByteRange.
const char kEntry[] = "INTRO1A";
const int32_t kFullWeight = 256;
const int32_t kWeightShift = 8;
const int32_t kCopyLimit = 127;
const int32_t kByteRange = 256;

} // namespace

const IntroSplash::Step IntroSplash::kSteps[IntroSplash::kStepCount] = {
    {1.0f, 0, 1}, {3.0f, 1, 1}, {2.0f, 1, 2}, {3.0f, 2, 2}, {1.0f, 2, 0},
};

// The C# constructor: no INTRO1A, Done at once; else palettes 0 (black), 1, 2
// and the picture.
void IntroSplash::open(const formats::Bni& options) {
    *this = IntroSplash();
    const formats::ArchiveIndex::Entry* entry = options.index().find(kEntry);
    const std::vector<uint8_t>& bytes = options.bytes();
    if (!entry || entry->offset < 0 || entry->size < kPaletteSize * 2 ||
        (int64_t)entry->offset + entry->size > (int64_t)bytes.size()) {
        done_ = true;
        return;
    }
    const uint8_t* at = bytes.data() + entry->offset;
    memcpy(palettes_[1], at, (size_t)kPaletteSize);
    memcpy(palettes_[2], at + kPaletteSize, (size_t)kPaletteSize);
    picture_ = unpack(at, entry->size, kPaletteSize * 2, kWidth * kHeight);
    done_ = false;
}

// C# Update.
void IntroSplash::update(bool anyPressed, float delta) {
    if (done_) {
        return;
    }
    if (anyPressed) {
        done_ = true;
        return;
    }
    time_ += delta;
    if (time_ >= kSteps[step_].seconds) {
        time_ = 0.0f;
        step_++;
        done_ = step_ >= kStepCount;
    }
}

// C# Draw's Math.Clamp(_time / seconds, 0f, 1f).
static float clampedT(float time, float seconds) {
    const float t = time / seconds;
    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

const uint8_t* IntroSplash::palette() {
    if (done_) {
        return nullptr;
    }
    const Step& step = kSteps[step_];
    blend(palettes_[step.from], palettes_[step.to], clampedT(time_, step.seconds), kPaletteSize,
          blended_);
    return blended_;
}

bool IntroSplash::fade(int32_t* from, int32_t* to, int32_t* weight) const {
    if (done_) {
        return false;
    }
    const Step& step = kSteps[step_];
    *from = step.from;
    *to = step.to;
    *weight = weightOf(clampedT(time_, step.seconds));
    return true;
}

// C# Blend: `(int)MathF.Round(t * FullWeight)`.
int32_t IntroSplash::weightOf(float t) {
    return (int32_t)nearbyintf(t * (float)kFullWeight);
}

void IntroSplash::blend(const uint8_t* from, const uint8_t* to, float t, int32_t count,
                        uint8_t* out) {
    const int32_t weight = weightOf(t);
    for (int32_t i = 0; i < count; i++) {
        out[i] = (uint8_t)((from[i] * (kFullWeight - weight) + to[i] * weight) >> kWeightShift);
    }
}

// C# Unpack.
std::vector<uint8_t> IntroSplash::unpack(const uint8_t* bytes, int32_t length, int32_t p,
                                         int32_t size) {
    std::vector<uint8_t> output((size_t)size, 0);
    int32_t n = 0;
    while (p < length && n < size) {
        const int32_t count = bytes[p++];
        if (count == 0) {
            break;
        }
        if (count > kCopyLimit) {
            const int32_t run = kByteRange - count;
            const int32_t copy = run < size - n ? run : size - n;
            if (p + copy > length) {
                break;   // the C# throws here (AsSpan past the end)
            }
            memcpy(&output[(size_t)n], bytes + p, (size_t)copy);
            p += run;
            n += copy;
            continue;
        }
        if (p >= length) {
            break;   // the C# throws here (bytes[p] past the end)
        }
        const int32_t fill = count < size - n ? count : size - n;
        memset(&output[(size_t)n], bytes[p++], (size_t)fill);
        n += count;
    }
    return output;
}

} // namespace menu
} // namespace mdk

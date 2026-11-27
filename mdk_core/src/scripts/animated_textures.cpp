// ============================================================================
// animated_textures.cpp -- see animated_textures.h.
// ============================================================================

#include "scripts/animated_textures.h"

#include <cmath>

namespace mdk {
namespace scripts {

void AnimatedTextures::set(const std::string& arena, const std::string& texture, Mode mode,
                           float value, float dt) {
    const int32_t count = lookup_ ? lookup_(user_, arena, texture) : 0;
    if (count < 1) {
        return;
    }

    const std::pair<std::string, std::string> key(arena, texture);
    const std::map<std::pair<std::string, std::string>, float>::const_iterator found =
        frames_.find(key);
    const float current = found != frames_.end() ? found->second : 0.0f;
    float target = current;
    switch (mode) {
        case Mode::Absolute:
            target = value - 1.0f;
            break;
        case Mode::PerSecond:
            target = current + value * dt;
            break;
        case Mode::Frames:
            target = current + value;
            break;
    }

    // Into [0, count): a forward loop, never ping-pong. The C#'s % on a
    // float keeps the dividend's sign, as fmod does.
    target = std::fmod(target, (float)count);
    if (target < 0.0f) {
        target += (float)count;
    }
    frames_[key] = target;
}

} // namespace scripts
} // namespace mdk

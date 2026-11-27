#pragma once

// ============================================================================
// animated_textures.h -- the frames of arena textures that scripts animate,
// ported 1:1 from src/Mdk.Game/Scripts/AnimatedTextures.cs (opcode 133,
// arena_texture_frame).
//
// No clock drives them: a script adds frames each tick and the drawn frame
// follows. The three modes:
//
//   mode 0 (Absolute):  frame = value - 1
//   mode 1 (PerSecond): frame += value * dt
//   else   (Frames):    frame += value
//
// The frame wraps into [0, frames) -- a forward loop, never ping-pong --
// and the drawn frame is its truncation. The frame count of a texture
// comes through the lookup table, filled by the runtime (its level's
// archives), so this class stays a pure state machine the synthetic
// harness can drive without a level.
// ============================================================================

#include <stdint.h>

#include <map>
#include <string>
#include <utility>

namespace mdk {
namespace scripts {

class AnimatedTextures {
public:
    // How opcode 133's value sets the frame.
    enum class Mode {
        Absolute,
        PerSecond,
        Frames,
    };

    // A texture's frame count by (arena, texture name); 0 for none.
    typedef int32_t (*FrameCountLookup)(void* user, const std::string& arena,
                                        const std::string& texture);

    AnimatedTextures() = default;
    AnimatedTextures(FrameCountLookup lookup, void* user) : lookup_(lookup), user_(user) {}

    // The script's mode number as a Mode (the C#'s ModeOf).
    static Mode modeOf(int32_t mode) {
        return mode == 0 ? Mode::Absolute : (mode == 1 ? Mode::PerSecond : Mode::Frames);
    }

    // Opcode 133 for an arena's texture, `dt` seconds a tick.
    void set(const std::string& arena, const std::string& texture, Mode mode, float value,
             float dt);

    // The frame drawn for an arena's texture (0 until a script sets it). Inline: the
    // level draw (game/level_view.cpp) reads it, and tests/formats links that draw
    // without any scripts translation unit but pose_cache.cpp (its link-graph guard).
    int32_t frameOf(const std::string& arena, const std::string& texture) const {
        const std::map<std::pair<std::string, std::string>, float>::const_iterator found =
            frames_.find(std::pair<std::string, std::string>(arena, texture));
        return found != frames_.end() ? (int32_t)found->second : 0;
    }

    // The textures animated so far, by (arena, name), and their frames.
    const std::map<std::pair<std::string, std::string>, float>& frames() const { return frames_; }

private:
    FrameCountLookup lookup_ = nullptr;
    void* user_ = nullptr;
    std::map<std::pair<std::string, std::string>, float> frames_;
};

} // namespace scripts
} // namespace mdk

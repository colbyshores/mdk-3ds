#pragma once

// ============================================================================
// level_music.h -- the music of Kurt's arena.
//
// A 1:1 port of src/Mdk.Game/Audio/LevelMusic.cs (the original's 0x419158,
// fades 0x418ffc). An arena plays its track (the CMI's), a corridor (no track)
// the level's CORRIDOR, NONE nothing. A new track fades in from -25 dB to full
// in about 8.7 s while the previous one fades out in about 4.35 s, then stops;
// going back to the fading track swaps them.
//
//   arena --> track --+- same as current: nothing
//                     +- the fading one: they swap (it goes on where it was)
//                     +- else: current --> fading (-0x100/frame),
//                              new --> current (+0x80/frame)
//
// The C#'s Track.Name is a nullable string (null: silence). A track name is
// never empty here (trackOf answers CORRIDOR for an arena without one), so the
// EMPTY string is the C#'s null.
//
// WHERE THE BYTES COME FROM is not this class's business: playMusic asks the
// bank, and the bank reads the track if it has not been prefetched (see
// SoundBank::prefetch -- the original preloads an arena's music with the arena,
// 0x4192c4, a step a frame, 0x4193a4 / 0x4317d8). trackOf() is public so the
// frontend can name the track of the arena that is being preloaded.
// ============================================================================

#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

#include "audio/sound_mixer.h"

namespace mdk {
namespace audio {

class LevelMusic {
public:
    typedef std::vector<std::pair<std::string, std::string>> NameMap;

    // The original's frames: 34 ms each.
    static constexpr float kFramesPerSecond = 1000.0f / 34.0f;
    static constexpr float kFadeIn = (float)0x80 * kFramesPerSecond;
    static constexpr float kFadeOut = (float)0x100 * kFramesPerSecond;

    // `tracks` is the CMI's arena-to-music table (Cmi::arenaMusic) and
    // `ambience` its arena-to-ambient-loop table (the 1996 demo's; may be
    // null). Both are BORROWED and must outlive this object.
    LevelMusic(SoundMixer* mixer, const NameMap* tracks, const NameMap* ambience = nullptr);

    // The track of an arena: its own, or CORRIDOR without one.
    std::string trackOf(const std::string& arena) const;

    // Kurt is in `arena` (empty outside every arena: no change).
    void enter(const std::string& arena);

    // One frame of the fades: the current track up, the fading one down and
    // stopped below 0.
    void update(float delta);

    // The track fading in or playing (empty: silence), and its volume.
    const std::string& playing() const { return current_.name; }
    float volume() const { return current_.volume; }
    // The track fading out, and its volume.
    const std::string& fading() const { return fading_.name; }
    float fadingVolume() const { return fading_.volume; }

    static const char* corridor() { return "CORRIDOR"; }

private:
    // A track and its volume (0-0x7FFF); no name is silence.
    struct Track {
        std::string name;
        int32_t voice = 0;
        float volume = 0.0f;
    };

    void switchTo(const std::string& name);
    void playAmbience(const std::string& name);
    void apply(const Track& track);

    SoundMixer* mixer_;
    const NameMap* tracks_;
    const NameMap* ambience_;
    Track current_;
    Track fading_;
    std::string arena_;
    std::string ambienceName_;
    int32_t ambienceVoice_ = 0;
};

} // namespace audio
} // namespace mdk

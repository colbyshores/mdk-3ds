// ============================================================================
// level_music.cpp -- see level_music.h (port of LevelMusic.cs).
// ============================================================================

#include "audio/level_music.h"

namespace mdk {
namespace audio {

namespace {

// GetValueOrDefault(arena, ""): the LAST entry of that name, as a dictionary
// built by assignment keeps the last value written.
std::string valueOf(const LevelMusic::NameMap* map, const std::string& key) {
    std::string value;
    if (map) {
        for (size_t i = 0; i < map->size(); i++) {
            if ((*map)[i].first == key) {
                value = (*map)[i].second;
            }
        }
    }
    return value;
}

} // namespace

LevelMusic::LevelMusic(SoundMixer* mixer, const NameMap* tracks, const NameMap* ambience)
    : mixer_(mixer), tracks_(tracks), ambience_(ambience) {}

std::string LevelMusic::trackOf(const std::string& arena) const {
    const std::string name = valueOf(tracks_, arena);
    return name.empty() ? std::string(corridor()) : name;
}

void LevelMusic::enter(const std::string& arena) {
    if (arena.empty() || arena == arena_) {
        return;
    }
    arena_ = arena;
    switchTo(trackOf(arena));
    if (ambience_ && !ambience_->empty()) {
        playAmbience(valueOf(ambience_, arena));
    }
}

void LevelMusic::playAmbience(const std::string& name) {
    if (name == ambienceName_) {
        return;
    }
    mixer_->stopVoice(ambienceVoice_);
    ambienceName_ = name;
    ambienceVoice_ = name.empty() ? 0 : mixer_->playLooped(name);
}

void LevelMusic::update(float delta) {
    current_.volume += kFadeIn * delta;
    if (current_.volume > (float)SoundMixer::kFullVolume) {
        current_.volume = (float)SoundMixer::kFullVolume;
    }
    fading_.volume -= kFadeOut * delta;
    if (fading_.volume < 0.0f && !fading_.name.empty()) {
        mixer_->stopVoice(fading_.voice);
        fading_ = Track();
    }
    apply(current_);
    apply(fading_);
}

void LevelMusic::switchTo(const std::string& name) {
    if (name == current_.name) {
        return;
    }
    // Back to the fading track: they swap.
    if (name == fading_.name) {
        const Track was = current_;
        current_ = fading_;
        fading_ = was;
        return;
    }
    // The current track fades out, the new one fades in from silence.
    if (fading_.voice != 0) {
        mixer_->stopVoice(fading_.voice);
    }
    fading_ = current_;
    current_ = Track();
    current_.name = name;
    current_.voice = mixer_->playMusic(name);
}

void LevelMusic::apply(const Track& track) {
    if (track.voice != 0) {
        mixer_->setVolume(track.voice, (int32_t)(track.volume > 0.0f ? track.volume : 0.0f));
    }
}

} // namespace audio
} // namespace mdk

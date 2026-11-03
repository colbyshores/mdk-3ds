// ============================================================================
// sound_mixer.cpp — the game's sound effects (port of SoundMixer.cs).
// ============================================================================

#include "audio/sound_mixer.h"

#include <math.h>

#include "util/math_minmax.h"

namespace mdk {
namespace audio {

namespace {

// SoundMixer's constants.
const float kFloorDb = -25.0f;
const float kNear = 20.0f;
const float kFar = 250.0f;
const float kSpeedOfSound = 1100.0f;
const float kPitchMin = 0.25f;
const float kPitchMax = 3.0f;
// Scope listening (0x403750): the scope is 384 x 280 pixels, focal 384 / zoom.
const float kScopeAxis = 2.0f;
const float kScopeRatio = 384.0f / 280.0f;
const float kScopeDepth = 300.0f;
const float kScopeEdge = 1.3f;

// Math.Clamp: a NaN stays NaN (both comparisons are false), as .NET's does.
float mathClamp(float value, float min, float max) {
    if (value < min) {
        return min;
    }
    return value > max ? max : value;
}

// mathMin is the oracle's MathF.Min and lives in util/math_minmax.h, ONE
// definition for the whole tree: NaN-propagating, and -0.0 sorts below +0.0
// there, which a ternary cannot see.

float length(const Vec3& v) { return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z); }

float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// ScriptMath.RotatedZ (ScriptMath.cs:36): the XY turned about Z, Z kept. The
// conversion is spelled as the port spells float.DegreesToRadians everywhere
// (see object.cpp's ReferencePoint), and MathF.SinCos gives the same pair as
// sinf/cosf.
Vec3 rotatedZ(const Vec3& v, float degrees) {
    const float radians = degrees * 3.14159265358979323846f / 180.0f;
    /* @vfp-exempt R6 — the sound source's offset rotation (MdkObject.SoundPosition,
     * once per positioned voice per frame); glibc cosf/sinf match the C# oracle's
     * MathF.SinCos bit-for-bit on this machine (probed), and the conversion is
     * .NET's left-associative spelling; the fastSin/fastCos LUT's error would move
     * a voice's pan. measured n/a (corpus empty). Verified 2026-10-25. */
    const float c = cosf(radians);
    const float s = sinf(radians);
    Vec3 out;
    out.x = v.x * c - v.y * s;
    out.y = v.x * s + v.y * c;
    out.z = v.z;
    return out;
}

} // namespace

SoundMixer::SoundMixer(SoundBank* bank)
    : bank_(bank),
      listenerRight(),
      listenerForward(),
      listenerUp() {
    // The C#'s field initializers: UnitX, UnitY, UnitZ.
    listenerRight.x = 1.0f;
    listenerForward.y = 1.0f;
    listenerUp.z = 1.0f;
}

int32_t SoundMixer::play(const std::string& name, int32_t start) {
    return launch(name, start, Flat, Vec3(), nullptr, nullptr, false, SoundDevice::Effects);
}

int32_t SoundMixer::playAt(const std::string& name, const Vec3& point, int32_t start) {
    return launch(name, start, Point, point, nullptr, nullptr, false, SoundDevice::Effects);
}

int32_t SoundMixer::playOn(const std::string& name, const Vec3* position, const float* yaw,
                           const Vec3& offset, int32_t start) {
    return launch(name, start, Source, offset, position, yaw, false, SoundDevice::Effects);
}

int32_t SoundMixer::playMusic(const std::string& name) {
    return launch(name, New, Flat, Vec3(), nullptr, nullptr, false, SoundDevice::Music);
}

int32_t SoundMixer::playLooped(const std::string& name) {
    return launch(name, New, Flat, Vec3(), nullptr, nullptr, true, SoundDevice::Effects);
}

bool SoundMixer::isPlaying(const std::string& name) const {
    for (size_t i = 0; i < voices_.size(); i++) {
        if (voices_[i].name == name) {
            return true;
        }
    }
    return false;
}

bool SoundMixer::isVoicePlaying(int32_t id) const {
    return id != 0 && voiceOf(id) != nullptr;
}

void SoundMixer::stop(const std::string& name) {
    for (size_t i = voices_.size(); i-- > 0;) {
        if (voices_[i].name == name) {
            device_.stop(voices_[i].id);
            forget(i);
        }
    }
}

void SoundMixer::stopVoice(int32_t id) {
    device_.stop(id);
    for (size_t i = voices_.size(); i-- > 0;) {
        if (voices_[i].id == id) {
            forget(i);
        }
    }
}

void SoundMixer::setVolume(int32_t id, int32_t volume) {
    Voice* voice = voiceOf(id);
    if (voice == nullptr) {
        return;
    }

    voice->volume = volume;
    if (voice->place == Flat) {
        device_.set(id, gain((float)volume), 1.0f, 0.0f);
    }
}

void SoundMixer::setPitch(int32_t id, float pitch) {
    Voice* voice = voiceOf(id);
    if (voice != nullptr && voice->place == Flat) {
        device_.set(id, gain((float)voice->volume), pitch, 0.0f);
    }
}

void SoundMixer::update(float delta) {
    for (size_t i = voices_.size(); i-- > 0;) {
        if (!device_.isPlaying(voices_[i].id)) {
            forget(i);
        }
    }

    for (size_t i = 0; i < voices_.size(); i++) {
        if (voices_[i].place != Flat) {
            update3d(voices_[i], delta);
        }
    }
}

float SoundMixer::gain(float volume) {
    const float db =
        kFloorDb * (1.0f - mathClamp(volume, 0.0f, (float)kFullVolume) / (float)kFullVolume);
    return powf(10.0f, db / 20.0f);
}

float SoundMixer::scopeGain(const Vec3& local, float zoom) {
    const float depth = local.z;
    if (depth <= 0.0f) {
        return 0.0f;
    }

    const float r = sqrtf(local.x * local.x + local.y * local.y);
    float screenX = 0.0f;
    float screenY = 0.0f;
    if (r > kScopeAxis) {
        const float ox = local.x - local.x * kScopeAxis / r;
        const float oy = local.y - local.y * kScopeAxis / r;
        screenX = ox * 2.0f / zoom;
        screenY = oy * 2.0f * kScopeRatio / zoom;
    }

    const float g = mathMin(1.0f, kScopeDepth / (zoom * depth));
    const float spread = sqrtf(screenX * screenX + screenY * screenY);
    return mathClamp(g * (kScopeEdge - spread / depth), 0.0f, 1.0f);
}

SoundMixer::Voice* SoundMixer::voiceOf(int32_t id) {
    for (size_t i = 0; i < voices_.size(); i++) {
        if (voices_[i].id == id) {
            return &voices_[i];
        }
    }
    return nullptr;
}

const SoundMixer::Voice* SoundMixer::voiceOf(int32_t id) const {
    for (size_t i = 0; i < voices_.size(); i++) {
        if (voices_[i].id == id) {
            return &voices_[i];
        }
    }
    return nullptr;
}

void SoundMixer::forget(size_t index) {
    // The C# clears the source before pooling the voice so a reused voice can
    // not outlive its object; here the voice is destroyed, which does the same.
    voices_.erase(voices_.begin() + (long)index);
}

Vec3 SoundMixer::voicePosition(const Voice& voice) {
    if (voice.place == Point) {
        return voice.point;
    }
    // Place.Source: MdkObject.SoundPosition(offset) — the offset in the
    // object's frame, so turned by its yaw and added to its position.
    const Vec3 turned = rotatedZ(voice.point, *voice.sourceYaw);
    Vec3 out;
    out.x = voice.sourcePosition->x + turned.x;
    out.y = voice.sourcePosition->y + turned.y;
    out.z = voice.sourcePosition->z + turned.z;
    return out;
}

int32_t SoundMixer::launch(const std::string& name, int32_t start, int32_t place,
                           const Vec3& point, const Vec3* sourcePosition, const float* sourceYaw,
                           bool loopForever, int32_t bus) {
    SoundBank::Entry entry;
    if (bank_ == nullptr || !bank_->get(name, &entry)) {
        return 0;
    }
    if (start == Once && isPlaying(name)) {
        return 0;
    }

    if (start == Restart) {
        stop(name);
    }

    if (loopForever) {
        // Looped(): a looping copy of the sound. The C# caches the copy because
        // a Sound is a heap object; an Entry is a value, so the copy is free.
        entry.looping = true;
    }

    const int32_t id = device_.play(entry, gain((float)entry.volume), 1.0f, 0.0f, bus);
    if (id == 0) {
        return 0;
    }

    Voice voice;
    voice.id = id;
    voice.name = name;
    voice.volume = entry.volume;
    voice.distance = -1.0f;
    voice.place = place;
    voice.point = point;
    voice.sourcePosition = sourcePosition;
    voice.sourceYaw = sourceYaw;
    voices_.push_back(voice);
    if (voices_.back().place != Flat) {
        update3d(voices_.back(), 0.0f);
    }

    return id;
}

void SoundMixer::update3d(Voice& voice, float delta) {
    const Vec3 place = voicePosition(voice);
    Vec3 offset;
    offset.x = place.x - listenerPosition.x;
    offset.y = place.y - listenerPosition.y;
    offset.z = place.z - listenerPosition.z;
    const float distance = mathMax(length(offset), 1.0f);

    // Doppler: coming closer raises the pitch.
    float pitch = 1.0f;
    if (voice.distance >= 0.0f && delta > 0.0f) {
        pitch = mathClamp(1.0f + (voice.distance - distance) / (delta * kSpeedOfSound), kPitchMin,
                          kPitchMax);
    }

    voice.distance = distance;
    float voiceGain;
    if (scopeZoom > 0.0f) {
        Vec3 local;
        local.x = dot(offset, listenerRight);
        local.y = dot(offset, listenerUp);
        local.z = dot(offset, listenerForward);
        voiceGain = scopeGain(local, scopeZoom);
    } else {
        voiceGain = mathClamp((kFar - distance) / (kFar - kNear), 0.0f, 1.0f);
    }
    const float side = dot(offset, listenerRight) / distance;
    device_.set(voice.id, gain((float)voice.volume * voiceGain), pitch, side);
}

} // namespace audio
} // namespace mdk

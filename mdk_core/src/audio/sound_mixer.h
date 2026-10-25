#pragma once

// ============================================================================
// sound_mixer.h — the game's sound effects, mixed like the original.
//
// A 1:1 port of src/Mdk.Game/Audio/SoundMixer.cs (the original's 0x402b40
// -0x403c70, listener update 0x403348):
//
//   * volume is linear in decibels over 25 dB, so a sound is never silent;
//   * 3D voices are full volume to 20 units, linear to 0 at 250, with Doppler
//     at 1100 u/s clamped to 0.25-3;
//   * in sniper mode the scope listens (0x403750): sounds near the crosshair
//     are loud, those behind the listener are silent;
//   * a new sound is dropped when every voice is busy.
//
// Voices are made once and reused: playing a sound makes nothing. The C# keeps
// a stack of spare Voice objects because a Voice is a heap allocation there;
// here a voice is a value in a vector, so the pool would pool nothing and is not
// ported — every field the C# resets on reuse is set on every launch here.
//
// THE SOURCE OF A 3D VOICE. The C# takes an ISoundSource (an object answers
// `SoundPosition(offset)`). This tree has no virtual dispatch (AGENTS.md 10),
// so a voice holds the two fields of its source that answer that question —
// the object's position and yaw — and does the rotation itself, which is what
// MdkObject.SoundPosition is. The caller passes fields of an object in the
// runtime's object deque, whose elements keep their address for the runtime's
// life; a voice that outlives its object is stopped by ScriptRuntime::remove,
// as in the C#.
//
// The `PlayOn(name, Func<Vector3>)` overload (follow a callback) is not ported:
// nothing in the script seam uses it, and a callback is exactly what this tree
// does not have.
//
// The headless oracle never calls update() (its driver ticks the scripts and
// nothing else), so in the corpus a voice ends only when it is stopped. The
// per-frame 3D update is here for the real frame loop.
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

#include "audio/sound_bank.h"
#include "audio/sound_device.h"
#include "render/render_types.h"

namespace mdk {
namespace audio {

class SoundMixer {
public:
    // A new voice starts even if the sound plays (New), stops its voices first
    // (Restart), or starts only if it isn't playing (Once).
    enum Start { New, Restart, Once };

    // Where a voice is heard from: nowhere (2D), a point, or something moving.
    enum Place { Flat, Point, Source };

    // A sound and its default volume (0-0x7FFF, from the SNI entry).
    struct Voice {
        int32_t id = 0;
        std::string name;
        int32_t volume = 0;
        int32_t place = Flat;
        Vec3 point;                        // Place.Point, or the offset for Place.Source
        const Vec3* sourcePosition = nullptr;  // Place.Source: the object's position
        const float* sourceYaw = nullptr;      // ... and its yaw, in degrees
        float distance = -1.0f;            // at the last update, -1 before the first
    };

    static constexpr int32_t kFullVolume = 0x7FFF;

    explicit SoundMixer(SoundBank* bank);

    // Play a sound without position. Returns the voice, or 0.
    int32_t play(const std::string& name, int32_t start = New);
    // 3D, fixed at a point.
    int32_t playAt(const std::string& name, const Vec3& point, int32_t start = New);
    // 3D, following an object, at `offset` in its frame.
    int32_t playOn(const std::string& name, const Vec3* position, const float* yaw,
                   const Vec3& offset, int32_t start = New);
    // The music (on the music bus, without position).
    int32_t playMusic(const std::string& name);
    // Without position, looping even if its SNI entry doesn't (Kurt's chain gun).
    int32_t playLooped(const std::string& name);

    bool isPlaying(const std::string& name) const;
    // Whether a voice (as returned by the play methods) still plays.
    bool isVoicePlaying(int32_t id) const;

    void stop(const std::string& name);
    void stopVoice(int32_t id);

    // A voice's volume (0-0x7FFF), e.g. the XD2's quieter DUMMY (0x4032e8).
    void setVolume(int32_t id, int32_t volume);
    // A 2D voice's pitch (1: the sound's rate), e.g. BUTSLIDE at 15000 Hz.
    void setPitch(int32_t id, float pitch);

    // Once a frame: forgets ended voices and updates the 3D ones from the listener.
    void update(float delta);

    // Decibels to a linear gain, for a volume (0-0x7FFF): linear over 25 dB.
    static float gain(float volume);
    // The gain of a sound through the scope (0x403750), from its place seen from
    // the listener (`local`: right, up, depth).
    static float scopeGain(const Vec3& local, float zoom);

    const std::vector<Voice>& voices() const { return voices_; }
    int32_t nextVoiceId() const { return device_.nextId(); }
    const SoundDevice& device() const { return device_; }

    // The listener: position and right (MDK coordinates), line of sight and up
    // for the scope, and the scope's zoom (1 to 0.25) while Kurt snipes, else 0.
    Vec3 listenerPosition;
    Vec3 listenerRight;
    Vec3 listenerForward;
    Vec3 listenerUp;
    float scopeZoom = 0.0f;

private:
    // A voice by id, or null (no callback: called every frame).
    Voice* voiceOf(int32_t id);
    const Voice* voiceOf(int32_t id) const;
    // A voice ended or stopped: back to the spares.
    void forget(size_t index);
    // Where a voice is heard from now.
    static Vec3 voicePosition(const Voice& voice);
    int32_t launch(const std::string& name, int32_t start, int32_t place, const Vec3& point,
                   const Vec3* sourcePosition, const float* sourceYaw, bool loopForever,
                   int32_t bus);
    // Volume, pan and pitch of a 3D voice from its place relative to the
    // listener (0x40347c).
    void update3d(Voice& voice, float delta);

    SoundBank* bank_;
    SoundDevice device_;
    std::vector<Voice> voices_;
};

} // namespace audio
} // namespace mdk

// ============================================================================
// audio_sdl.cpp — the Linux sink (the output half of AudioDevice.cs).
// ============================================================================

#include "audio/audio_sdl.h"

#include <SDL3/SDL.h>

#include <cstdio>

namespace mdk {
namespace {

// AudioDevice's stream, and nothing else. One per process, as the C# has one
// AudioDevice per game (Flow/Game.cs:206).
SDL_AudioStream* g_stream = nullptr;
// What audioSink pushed (audioStats).
AudioStats g_stats;

// AudioDevice.OutputChannels and OutputRate.
constexpr int kOutputChannels = 2;
constexpr int kOutputRate = 44100;

} // namespace

bool audioOpen() {
    if (g_stream != nullptr) {
        return true;
    }
    // The C#'s constructor, branch for branch: a failed init or a failed open
    // leaves _stream null and the device muted, and is not an error.
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::printf("[audio] SDL audio did not init: %s — running muted\n", SDL_GetError());
        return false;
    }
    SDL_AudioSpec spec;
    spec.format = SDL_AUDIO_F32LE;
    spec.channels = kOutputChannels;
    spec.freq = kOutputRate;
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (g_stream == nullptr) {
        std::printf("[audio] no playback device: %s — running muted\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    SDL_ResumeAudioStreamDevice(g_stream);
    std::printf("[audio] %d Hz stereo F32LE\n", kOutputRate);
    return true;
}

void audioClose() {
    if (g_stream != nullptr) {
        std::printf("[audio] closed: %lld frames sent (%.1f s), peak %.3f\n",
                    (long long)g_stats.frames, (double)g_stats.frames / kOutputRate,
                    (double)g_stats.peak);
        SDL_DestroyAudioStream(g_stream);
        g_stream = nullptr;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
}

bool audioIsOpen() { return g_stream != nullptr; }

// AudioDevice.Update's tail: `SDL_PutAudioStreamData(_stream, data,
// mix.Length * sizeof(float))`. The bytes are the interleaved float frames the
// device just mixed, copied into SDL's queue by SDL; the buffer itself is the
// device's and is only good until the next update.
void audioSink(void* /*context*/, const float* interleaved, int32_t frames) {
    if (g_stream == nullptr || frames <= 0) {
        return;
    }
    SDL_PutAudioStreamData(g_stream, interleaved, (int)(frames * kOutputChannels * sizeof(float)));
    g_stats.frames += frames;
    for (int32_t i = 0; i < frames * kOutputChannels; i++) {
        const float magnitude = interleaved[i] < 0.0f ? -interleaved[i] : interleaved[i];
        if (magnitude > g_stats.peak) {
            g_stats.peak = magnitude;
        }
    }
}

AudioStats audioStats() { return g_stats; }

// AudioDevice.Update's head: `SDL_GetAudioStreamQueued(_stream) / bytesPerFrame`,
// the frames still waiting. This is the number that decides how much the device
// mixes next, so it is the C#'s own call and not an estimate of it.
int32_t audioQueued(void* /*context*/) {
    if (g_stream == nullptr) {
        return 0;
    }
    const int bytes = SDL_GetAudioStreamQueued(g_stream);
    return bytes > 0 ? bytes / (kOutputChannels * (int)sizeof(float)) : 0;
}

} // namespace mdk

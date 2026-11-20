#pragma once

// ============================================================================
// game_settings.h -- the player's settings (C# src/Mdk.Game/Flow/Settings.cs).
//
// Volumes, the music filter, the mouse and the pad, the window and the frame,
// anti-aliasing, the difficulty, gore, the look, stereo, the touch controls,
// the key bindings and the mods switched off -- every field the C# keeps, so a
// settings.cfg written by either program reads back in the other one byte for
// byte (Parse, then Format: golden/menu_settings.txt).
//
//   master_volume=80
//   bind.Forward=W
//   mod.hd-textures=Off
//
// WHO USES WHAT. The C#'s Settings.Apply hands the values to its window, its
// renderer, its audio device and its input. This port's frontends consume the
// ones they have a seam for (applyAudio below; the difficulty and gore the
// level reads); the rest are KEPT -- parsed, written back unchanged -- so the
// file stays the C#'s. That is the C#'s own rule for a setting a platform does
// not show (DisplayMenu.Items(Host.Android) hides the window mode, and the
// file still carries it).
//
// THE 3DS writes the lines it shows (formatConsole: the volumes, the music
// filter, the difficulty and gore) into its existing sdmc:/3ds/mdk/config.ini,
// after the Enhanced Mode toggles; each parser skips the other's keys
// (enhanced_config.h: "Unknown keys and malformed lines are ignored", and the
// C#'s Set has no default arm).
//
// .NET's parsers, reproduced where the file can reach them: int.TryParse and
// Enum.TryParse trim white space and take a sign; Enum.TryParse also takes a
// number (difficulty=2 is Hard); bool.TryParse is case-insensitive. Floats are
// written as .NET's shortest round-trip text ("0.25", "10", "1").
//
// Cold code: read once at start, written when a menu changes a value. No
// virtuals, no templates, no exceptions (AGENTS.md §10).
// ============================================================================

#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

namespace mdk {

namespace audio {
class SoundDevice;
}

struct GameSettings {
    // C# Settings.MaxVolume.
    static const int32_t kMaxVolume = 100;

    // The C# enums, by value; their names are the file's (the *Names below).
    enum Fullscreen { FullscreenOff, FullscreenDesktop, FullscreenExclusive };
    enum VSync { VSyncOn, VSyncOff, VSyncAdaptive };
    enum GpuBackend { BackendAuto, BackendDirect3D12, BackendVulkan, BackendMetal };
    enum Difficulty { Easy, Normal, Hard };
    enum Graphics { GraphicsOriginal, GraphicsEnhanced };
    enum AntiAliasing { AntiAliasingOff, AntiAliasingX2, AntiAliasingX4 };
    enum Stereo {
        StereoOff, StereoSbs, StereoSbsFull, StereoCrossView, StereoInterlaced,
        StereoInterlacedReversed, StereoTab, StereoTabReversed, StereoVr
    };
    enum HeadTracking { HeadTrackingOff, HeadTrackingOn };
    enum TouchButtons { TouchAuto, TouchOff, TouchOn };

    // C# Resolution: a size in pixels, written 1280x960. `set` false is the C#'s
    // null (ExclusiveSize only).
    struct Resolution {
        int32_t width = 0;
        int32_t height = 0;
        bool set = false;
    };

    int32_t masterVolume = 80;
    int32_t musicVolume = 50;
    int32_t effectsVolume = 70;
    bool musicFilter = true;
    float mouseSensitivity = 1.0f;
    bool invertMouse = false;
    float padSensitivity = 1.0f;
    bool invertPad = false;
    Fullscreen fullscreen = FullscreenOff;
    // C# Settings.DefaultWindow: the original's 640 x 480, doubled.
    Resolution windowSize = {1280, 960, true};
    Resolution exclusiveSize;
    // C# Renderer.FullScale; Renderer.Scales are the values the file may hold.
    int32_t renderScale = 100;
    VSync vsync = VSyncOn;
    // C# FrameLimiter.Off; FrameLimiter.Limits are the values the file may hold.
    int32_t frameLimit = 0;
    GpuBackend backend = BackendAuto;
    // C# Renderer.DefaultGamma, clamped into MinGamma-MaxGamma when read.
    float gamma = 1.0f;
    Difficulty difficulty = Normal;
    // Gore (0x5742dc, on by default): green sparks, slime, blown off parts, the
    // head shots row. The level hands it to the scripts (ScriptRuntime::option).
    bool gore = true;
    Graphics graphics = GraphicsOriginal;
    AntiAliasing antiAliasing = AntiAliasingOff;
    Stereo stereo = StereoOff;
    float stereoSeparation = 0.25f;
    float stereoConvergence = 10.0f;
    HeadTracking headTracking = HeadTrackingOff;
    TouchButtons touch = TouchAuto;
    // Rebound actions, action name -> control name, in the order the C#'s
    // Dictionary keeps them (insertion; a rebind replaces in place).
    std::vector<std::pair<std::string, std::string>> bindings;
    std::vector<std::pair<std::string, std::string>> padBindings;
    // Mods by folder, "On" or "Off"; the C# dictionary ignores case.
    std::vector<std::pair<std::string, std::string>> mods;
    std::string upscalerUrl;
    std::string upscalerSha256;

    // C# Settings.Parse: the defaults, then each `name=value` line; unknown or bad
    // lines keep the defaults.
    static GameSettings parse(const char* text);
    // Apply the lines of `text` over this one (Parse without the fresh defaults:
    // the 3DS reads its game lines out of config.ini this way).
    void parseOver(const char* text);
    // C# Settings.Format: every line, in the C#'s order, newline-terminated.
    std::string format() const;
    // The lines the 3DS shows and keeps in config.ini (volumes, music filter,
    // difficulty, gore), in Format's order and spelling.
    std::string formatConsole() const;

    // C# Settings.Apply's audio half (Game.cs Settings.Apply: MasterGain, the
    // buses' gains, the music filter), onto a device.
    void applyAudio(audio::SoundDevice* device) const;

    // The names the file uses, by value.
    static const char* difficultyName(Difficulty value);
    static const char* graphicsName(Graphics value);
    static const char* antiAliasingFileName(AntiAliasing value);
};

// C# Settings.Gain: 0-100 to a linear gain.
float settingsGain(int32_t volume);

// .NET's float.ToString(CultureInfo.InvariantCulture) for the values the file
// holds: the shortest text that reads back as the same float.
std::string formatShortestFloat(float value);

} // namespace mdk

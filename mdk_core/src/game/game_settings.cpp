// game_settings.cpp -- see game_settings.h. A transliteration of
// src/Mdk.Game/Flow/Settings.cs: Parse/Set (:121-303), Format (:138-174), Gain
// (:350) and Apply's audio lines (:318-321). Cold code.

#include "game/game_settings.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/sound_device.h"

namespace mdk {
namespace {

const char* const kFullscreenNames[] = {"Off", "Desktop", "Exclusive"};
const char* const kVSyncNames[] = {"On", "Off", "Adaptive"};
const char* const kBackendNames[] = {"Auto", "Direct3D12", "Vulkan", "Metal"};
const char* const kDifficultyNames[] = {"Easy", "Normal", "Hard"};
const char* const kGraphicsNames[] = {"Original", "Enhanced"};
const char* const kAntiAliasingNames[] = {"Off", "X2", "X4"};
const char* const kStereoNames[] = {"Off", "Sbs", "SbsFull", "CrossView", "Interlaced",
                                    "InterlacedReversed", "Tab", "TabReversed", "Vr"};
const char* const kHeadTrackingNames[] = {"Off", "On"};
const char* const kTouchNames[] = {"Auto", "Off", "On"};
const char* const kModStateNames[] = {"On", "Off"};
// C# Mdk.Engine.Platform.Key, by value (the bind./pad. lines' names).
const char* const kKeyNames[] = {
    "Forward", "Back", "StrafeLeft", "StrafeRight", "TurnLeft", "TurnRight", "Jump", "Turbo",
    "Fire", "UseItem", "ItemNext", "ItemPrevious", "Item1", "Item2", "Item3", "Item4", "Item5",
    "Up", "Down", "Escape", "Screenshot", "Fly", "Sniper", "ZoomIn", "ZoomOut", "RollLeft",
    "RollRight", "Teleport", "QuickSave", "QuickLoad", "AlwaysRun"};

#define MDK_COUNT(a) ((int32_t)(sizeof(a) / sizeof((a)[0])))

// C# Renderer.Scales, FrameLimiter.Limits, Renderer.MinGamma/MaxGamma,
// StereoModes.MaxSeparation/MaxConvergence, HdGenerator.ModFolder.
const int32_t kScales[] = {50, 100, 150, 200};
const int32_t kLimits[] = {0, 60, 120, 144};
const float kMinGamma = 0.5f;
const float kMaxGamma = 2.0f;
const float kMaxSeparation = 2.0f;
const float kMaxConvergence = 4000.0f;
const char kHdModFolder[] = "hd-textures";
const char kBindPrefix[] = "bind.";
const char kPadPrefix[] = "pad.";
const char kModPrefix[] = "mod.";

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

// C# string.Trim().
std::string trim(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && isSpace(text[begin])) {
        begin++;
    }
    while (end > begin && isSpace(text[end - 1])) {
        end--;
    }
    return text.substr(begin, end - begin);
}

// C# int.TryParse (NumberStyles.Integer: white space around, a leading sign).
bool parseInt(const std::string& raw, int32_t* out) {
    const std::string text = trim(raw);
    if (text.empty()) {
        return false;
    }
    size_t i = 0;
    bool negative = false;
    if (text[i] == '+' || text[i] == '-') {
        negative = text[i] == '-';
        i++;
    }
    if (i == text.size()) {
        return false;
    }
    int64_t value = 0;
    for (; i < text.size(); i++) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
        value = value * 10 + (text[i] - '0');
        if (value > (int64_t)2147483648LL) {
            return false;
        }
    }
    value = negative ? -value : value;
    if (value > 2147483647LL || value < -2147483648LL) {
        return false;
    }
    *out = (int32_t)value;
    return true;
}

// C# int.TryParse with NumberStyles.None: digits only.
bool parseDigits(const std::string& text, int32_t* out) {
    if (text.empty()) {
        return false;
    }
    int64_t value = 0;
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
        value = value * 10 + (text[i] - '0');
        if (value > 2147483647LL) {
            return false;
        }
    }
    *out = (int32_t)value;
    return true;
}

// C# bool.TryParse: "True" or "False", any case, white space around.
bool parseBool(const std::string& raw, bool* out) {
    const std::string text = trim(raw);
    if (text.size() == 4 && strncasecmp(text.c_str(), "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (text.size() == 5 && strncasecmp(text.c_str(), "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

// C# float.TryParse(InvariantCulture) for the decimal spellings the file holds
// ("0.25", "1e3", "-2"). .NET's "Infinity"/"NaN" spellings are not taken: no
// setting is written that way.
bool parseFloat(const std::string& raw, float* out) {
    const std::string text = trim(raw);
    if (text.empty()) {
        return false;
    }
    for (size_t i = 0; i < text.size(); i++) {
        const char c = text[i];
        if (!((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' || c == 'e' ||
              c == 'E')) {
            return false;
        }
    }
    char* end = nullptr;
    errno = 0;
    const float value = strtof(text.c_str(), &end);
    if (end != text.c_str() + text.size()) {
        return false;
    }
    *out = value;
    return true;
}

// C# Enum.TryParse<T>: a name (exact case) or a number (any value; the callers
// that need a defined one say so with `defined`).
bool parseEnum(const std::string& raw, const char* const* names, int32_t count, int32_t* out) {
    const std::string text = trim(raw);
    int32_t number = 0;
    if (!text.empty() && (isdigit((unsigned char)text[0]) || text[0] == '-' || text[0] == '+') &&
        parseInt(text, &number)) {
        *out = number;
        return true;
    }
    for (int32_t i = 0; i < count; i++) {
        if (text == names[i]) {
            *out = i;
            return true;
        }
    }
    return false;
}

bool parseDefinedEnum(const std::string& raw, const char* const* names, int32_t count,
                      int32_t* out) {
    int32_t value = 0;
    if (!parseEnum(raw, names, count, &value) || value < 0 || value >= count) {
        return false;
    }
    *out = value;
    return true;
}

// C# Resolution.TryParse: "1280x960", both sides positive.
bool parseResolution(const std::string& text, GameSettings::Resolution* out) {
    const size_t x = text.find('x');
    if (x == std::string::npos || text.find('x', x + 1) != std::string::npos) {
        return false;
    }
    int32_t width = 0;
    int32_t height = 0;
    if (!parseDigits(text.substr(0, x), &width) || !parseDigits(text.substr(x + 1), &height) ||
        width <= 0 || height <= 0) {
        return false;
    }
    out->width = width;
    out->height = height;
    out->set = true;
    return true;
}

bool contains(const int32_t* values, int32_t count, int32_t value) {
    for (int32_t i = 0; i < count; i++) {
        if (values[i] == value) {
            return true;
        }
    }
    return false;
}

float clampFloat(float value, float lo, float hi) {
    return value < lo ? lo : (value > hi ? hi : value);
}

// C# Settings.Volume.
int32_t volumeOf(const std::string& value, int32_t fallback) {
    int32_t volume = 0;
    if (!parseInt(value, &volume)) {
        return fallback;
    }
    return volume < 0 ? 0 : (volume > GameSettings::kMaxVolume ? GameSettings::kMaxVolume : volume);
}

// The name an enum value is written as: its name, or the number (a value the
// file gave as a number outside the enum, which the C# keeps and writes back).
std::string enumText(int32_t value, const char* const* names, int32_t count) {
    if (value >= 0 && value < count) {
        return names[value];
    }
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%d", (int)value);
    return buffer;
}

// Dictionary[key] = value: replaces in place, else appends.
void setPair(std::vector<std::pair<std::string, std::string>>* pairs, const std::string& key,
             const std::string& value, bool ignoreCase) {
    for (size_t i = 0; i < pairs->size(); i++) {
        const std::string& have = (*pairs)[i].first;
        const bool same = ignoreCase ? (have.size() == key.size() &&
                                        strncasecmp(have.c_str(), key.c_str(), key.size()) == 0)
                                     : have == key;
        if (same) {
            (*pairs)[i].second = value;
            return;
        }
    }
    pairs->push_back(std::make_pair(key, value));
}

bool hasKeyIgnoreCase(const std::vector<std::pair<std::string, std::string>>& pairs,
                      const std::string& key) {
    for (size_t i = 0; i < pairs.size(); i++) {
        if (pairs[i].first.size() == key.size() &&
            strncasecmp(pairs[i].first.c_str(), key.c_str(), key.size()) == 0) {
            return true;
        }
    }
    return false;
}

const char* boolText(bool value) {
    return value ? "True" : "False";
}

std::string resolutionText(const GameSettings::Resolution& size) {
    if (!size.set) {
        return std::string();
    }
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%dx%d", (int)size.width, (int)size.height);
    return buffer;
}

// StringComparer.OrdinalIgnoreCase's order: upper-cased chars, ordinal.
bool lessIgnoreCase(const std::string& a, const std::string& b) {
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; i++) {
        const int ca = toupper((unsigned char)a[i]);
        const int cb = toupper((unsigned char)b[i]);
        if (ca != cb) {
            return ca < cb;
        }
    }
    return a.size() < b.size();
}

void set(GameSettings* s, const std::string& name, const std::string& value) {
    if (name.compare(0, sizeof(kBindPrefix) - 1, kBindPrefix) == 0 ||
        name.compare(0, sizeof(kPadPrefix) - 1, kPadPrefix) == 0) {
        const bool pad = name.compare(0, sizeof(kPadPrefix) - 1, kPadPrefix) == 0;
        const std::string action = name.substr(pad ? sizeof(kPadPrefix) - 1 : sizeof(kBindPrefix) - 1);
        int32_t key = 0;
        if (parseEnum(action, kKeyNames, MDK_COUNT(kKeyNames), &key)) {
            setPair(pad ? &s->padBindings : &s->bindings,
                    enumText(key, kKeyNames, MDK_COUNT(kKeyNames)), value, false);
        }
        return;
    }
    if (name.compare(0, sizeof(kModPrefix) - 1, kModPrefix) == 0) {
        int32_t state = 0;
        if (parseDefinedEnum(value, kModStateNames, MDK_COUNT(kModStateNames), &state)) {
            setPair(&s->mods, name.substr(sizeof(kModPrefix) - 1), kModStateNames[state], true);
        }
        return;
    }

    int32_t number = 0;
    float real = 0.0f;
    bool flag = false;
    GameSettings::Resolution size;
    if (name == "master_volume") {
        s->masterVolume = volumeOf(value, s->masterVolume);
    } else if (name == "music_volume") {
        s->musicVolume = volumeOf(value, s->musicVolume);
    } else if (name == "effects_volume") {
        s->effectsVolume = volumeOf(value, s->effectsVolume);
    } else if (name == "music_filter") {
        if (parseBool(value, &flag)) s->musicFilter = flag;
    } else if (name == "mouse_sensitivity") {
        if (parseFloat(value, &real)) s->mouseSensitivity = real;
    } else if (name == "invert_mouse") {
        if (parseBool(value, &flag)) s->invertMouse = flag;
    } else if (name == "pad_sensitivity") {
        if (parseFloat(value, &real)) s->padSensitivity = real;
    } else if (name == "invert_pad") {
        if (parseBool(value, &flag)) s->invertPad = flag;
    } else if (name == "touch_controls") {
        if (parseDefinedEnum(value, kTouchNames, MDK_COUNT(kTouchNames), &number)) {
            s->touch = (GameSettings::TouchButtons)number;
        }
    } else if (name == "fullscreen") {
        // Older settings: True or False.
        if (parseBool(value, &flag)) {
            s->fullscreen = flag ? GameSettings::FullscreenDesktop : GameSettings::FullscreenOff;
        } else if (parseDefinedEnum(value, kFullscreenNames, MDK_COUNT(kFullscreenNames), &number)) {
            s->fullscreen = (GameSettings::Fullscreen)number;
        }
    } else if (name == "window_size") {
        if (parseResolution(value, &size)) s->windowSize = size;
    } else if (name == "exclusive_size") {
        if (parseResolution(value, &size)) s->exclusiveSize = size;
    } else if (name == "render_scale") {
        if (parseInt(value, &number) && contains(kScales, MDK_COUNT(kScales), number)) {
            s->renderScale = number;
        }
    } else if (name == "vsync") {
        if (parseDefinedEnum(value, kVSyncNames, MDK_COUNT(kVSyncNames), &number)) {
            s->vsync = (GameSettings::VSync)number;
        }
    } else if (name == "frame_limit") {
        if (parseInt(value, &number) && contains(kLimits, MDK_COUNT(kLimits), number)) {
            s->frameLimit = number;
        }
    } else if (name == "gpu_backend") {
        if (parseDefinedEnum(value, kBackendNames, MDK_COUNT(kBackendNames), &number)) {
            s->backend = (GameSettings::GpuBackend)number;
        }
    } else if (name == "gamma") {
        if (parseFloat(value, &real)) s->gamma = clampFloat(real, kMinGamma, kMaxGamma);
    } else if (name == "difficulty") {
        // The C# takes any number here (no IsDefined), and an undefined one then
        // indexes past DifficultyNames on the Game page. Only the three are kept.
        if (parseDefinedEnum(value, kDifficultyNames, MDK_COUNT(kDifficultyNames), &number)) {
            s->difficulty = (GameSettings::Difficulty)number;
        }
    } else if (name == "gore") {
        if (parseBool(value, &flag)) s->gore = flag;
    } else if (name == "graphics") {
        if (parseDefinedEnum(value, kGraphicsNames, MDK_COUNT(kGraphicsNames), &number)) {
            s->graphics = (GameSettings::Graphics)number;
        }
    } else if (name == "antialiasing") {
        if (parseDefinedEnum(value, kAntiAliasingNames, MDK_COUNT(kAntiAliasingNames), &number)) {
            s->antiAliasing = (GameSettings::AntiAliasing)number;
        }
    } else if (name == "stereo") {
        if (parseDefinedEnum(value, kStereoNames, MDK_COUNT(kStereoNames), &number)) {
            s->stereo = (GameSettings::Stereo)number;
        }
    } else if (name == "stereo_separation") {
        if (parseFloat(value, &real)) s->stereoSeparation = clampFloat(real, 0.0f, kMaxSeparation);
    } else if (name == "stereo_convergence") {
        if (parseFloat(value, &real)) {
            s->stereoConvergence = clampFloat(real, 0.0f, kMaxConvergence);
        }
    } else if (name == "head_tracking") {
        if (parseDefinedEnum(value, kHeadTrackingNames, MDK_COUNT(kHeadTrackingNames), &number)) {
            s->headTracking = (GameSettings::HeadTracking)number;
        }
    } else if (name == "upscaler_url") {
        s->upscalerUrl = value;
    } else if (name == "upscaler_sha256") {
        s->upscalerSha256 = value;
    } else if (name == "textures" && (value == "Hd" || value == "Original")) {
        // Older settings' HD switch: now the HD textures mod's (TryAdd).
        if (!hasKeyIgnoreCase(s->mods, kHdModFolder)) {
            s->mods.push_back(std::make_pair(std::string(kHdModFolder),
                                             std::string(value == "Hd" ? "On" : "Off")));
        }
    }
}

} // namespace

std::string formatShortestFloat(float value) {
    if (value == 0.0f) {
        return signbit(value) ? "-0" : "0";
    }
    // The fewest significant digits that read back as the same float.
    char digits[64];
    int precision = 1;
    for (; precision <= 9; precision++) {
        snprintf(digits, sizeof(digits), "%.*e", precision - 1, (double)value);
        if (strtof(digits, nullptr) == value) {
            break;
        }
    }
    // digits is "[-]d[.ddd]e[+-]XX": its mantissa digits and its exponent.
    const char* p = digits;
    std::string sign;
    if (*p == '-') {
        sign = "-";
        p++;
    }
    std::string mantissa;
    while (*p && *p != 'e') {
        if (*p != '.') {
            mantissa.push_back(*p);
        }
        p++;
    }
    const int exponent = *p == 'e' ? atoi(p + 1) : 0;
    while (mantissa.size() > 1 && mantissa[mantissa.size() - 1] == '0') {
        mantissa.erase(mantissa.size() - 1);
    }
    const int count = (int)mantissa.size();
    std::string out = sign;
    if (exponent >= 15 || exponent < -5) {
        // .NET's scientific form: "1.5E-05", "1E+15".
        out += mantissa[0];
        if (count > 1) {
            out += '.';
            out += mantissa.substr(1);
        }
        char tail[16];
        snprintf(tail, sizeof(tail), "E%c%02d", exponent < 0 ? '-' : '+',
                 exponent < 0 ? -exponent : exponent);
        out += tail;
        return out;
    }
    if (exponent < 0) {
        out += "0.";
        for (int i = 0; i < -exponent - 1; i++) {
            out += '0';
        }
        out += mantissa;
        return out;
    }
    if (count <= exponent + 1) {
        out += mantissa;
        for (int i = count; i < exponent + 1; i++) {
            out += '0';
        }
        return out;
    }
    out += mantissa.substr(0, (size_t)exponent + 1);
    out += '.';
    out += mantissa.substr((size_t)exponent + 1);
    return out;
}

float settingsGain(int32_t volume) {
    return (float)volume / (float)GameSettings::kMaxVolume;
}

GameSettings GameSettings::parse(const char* text) {
    GameSettings settings;
    settings.parseOver(text);
    return settings;
}

void GameSettings::parseOver(const char* text) {
    if (!text) {
        return;
    }
    // C# text.Split('\n'), each line Trim()med and split at its first '='.
    const char* line = text;
    for (;;) {
        const char* end = strchr(line, '\n');
        const std::string whole = trim(end ? std::string(line, (size_t)(end - line)) : std::string(line));
        const size_t equals = whole.find('=');
        if (equals != std::string::npos) {
            set(this, trim(whole.substr(0, equals)), trim(whole.substr(equals + 1)));
        }
        if (!end) {
            break;
        }
        line = end + 1;
    }
}

std::string GameSettings::format() const {
    std::string out;
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "master_volume=%d\n", (int)masterVolume);
    out += buffer;
    snprintf(buffer, sizeof(buffer), "music_volume=%d\n", (int)musicVolume);
    out += buffer;
    snprintf(buffer, sizeof(buffer), "effects_volume=%d\n", (int)effectsVolume);
    out += buffer;
    out += std::string("music_filter=") + boolText(musicFilter) + "\n";
    out += "mouse_sensitivity=" + formatShortestFloat(mouseSensitivity) + "\n";
    out += std::string("invert_mouse=") + boolText(invertMouse) + "\n";
    out += "pad_sensitivity=" + formatShortestFloat(padSensitivity) + "\n";
    out += std::string("invert_pad=") + boolText(invertPad) + "\n";
    out += "touch_controls=" + enumText(touch, kTouchNames, MDK_COUNT(kTouchNames)) + "\n";
    out += "fullscreen=" + enumText(fullscreen, kFullscreenNames, MDK_COUNT(kFullscreenNames)) + "\n";
    out += "window_size=" + resolutionText(windowSize) + "\n";
    out += "exclusive_size=" + resolutionText(exclusiveSize) + "\n";
    snprintf(buffer, sizeof(buffer), "render_scale=%d\n", (int)renderScale);
    out += buffer;
    out += "vsync=" + enumText(vsync, kVSyncNames, MDK_COUNT(kVSyncNames)) + "\n";
    snprintf(buffer, sizeof(buffer), "frame_limit=%d\n", (int)frameLimit);
    out += buffer;
    out += "gpu_backend=" + enumText(backend, kBackendNames, MDK_COUNT(kBackendNames)) + "\n";
    out += "gamma=" + formatShortestFloat(gamma) + "\n";
    out += "difficulty=" + enumText(difficulty, kDifficultyNames, MDK_COUNT(kDifficultyNames)) + "\n";
    out += std::string("gore=") + boolText(gore) + "\n";
    out += "graphics=" + enumText(graphics, kGraphicsNames, MDK_COUNT(kGraphicsNames)) + "\n";
    out += "antialiasing=" +
           enumText(antiAliasing, kAntiAliasingNames, MDK_COUNT(kAntiAliasingNames)) + "\n";
    out += "stereo=" + enumText(stereo, kStereoNames, MDK_COUNT(kStereoNames)) + "\n";
    out += "stereo_separation=" + formatShortestFloat(stereoSeparation) + "\n";
    out += "stereo_convergence=" + formatShortestFloat(stereoConvergence) + "\n";
    out += "head_tracking=" +
           enumText(headTracking, kHeadTrackingNames, MDK_COUNT(kHeadTrackingNames)) + "\n";
    out += "upscaler_url=" + upscalerUrl + "\n";
    out += "upscaler_sha256=" + upscalerSha256 + "\n";
    for (size_t i = 0; i < bindings.size(); i++) {
        out += kBindPrefix + bindings[i].first + "=" + bindings[i].second + "\n";
    }
    for (size_t i = 0; i < padBindings.size(); i++) {
        out += kPadPrefix + padBindings[i].first + "=" + padBindings[i].second + "\n";
    }
    // Mods.OrderBy(key, OrdinalIgnoreCase): a stable insertion sort.
    std::vector<std::pair<std::string, std::string>> sorted = mods;
    for (size_t i = 1; i < sorted.size(); i++) {
        for (size_t j = i; j > 0 && lessIgnoreCase(sorted[j].first, sorted[j - 1].first); j--) {
            sorted[j].swap(sorted[j - 1]);
        }
    }
    for (size_t i = 0; i < sorted.size(); i++) {
        out += kModPrefix + sorted[i].first + "=" + sorted[i].second + "\n";
    }
    return out;
}

std::string GameSettings::formatConsole() const {
    std::string out;
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "master_volume=%d\n", (int)masterVolume);
    out += buffer;
    snprintf(buffer, sizeof(buffer), "music_volume=%d\n", (int)musicVolume);
    out += buffer;
    snprintf(buffer, sizeof(buffer), "effects_volume=%d\n", (int)effectsVolume);
    out += buffer;
    out += std::string("music_filter=") + boolText(musicFilter) + "\n";
    out += "difficulty=" + enumText(difficulty, kDifficultyNames, MDK_COUNT(kDifficultyNames)) + "\n";
    out += std::string("gore=") + boolText(gore) + "\n";
    return out;
}

void GameSettings::applyAudio(audio::SoundDevice* device) const {
    if (!device) {
        return;
    }
    device->setMasterGain(settingsGain(masterVolume));
    device->setBusGain(audio::SoundDevice::Music, settingsGain(musicVolume));
    device->setBusGain(audio::SoundDevice::Effects, settingsGain(effectsVolume));
    device->setMusicFilter(musicFilter);
}

const char* GameSettings::difficultyName(Difficulty value) {
    const uint32_t index = (uint32_t)value;
    return index < (uint32_t)MDK_COUNT(kDifficultyNames) ? kDifficultyNames[index] : "?";
}

const char* GameSettings::graphicsName(Graphics value) {
    const uint32_t index = (uint32_t)value;
    return index < (uint32_t)MDK_COUNT(kGraphicsNames) ? kGraphicsNames[index] : "?";
}

const char* GameSettings::antiAliasingFileName(AntiAliasing value) {
    const uint32_t index = (uint32_t)value;
    return index < (uint32_t)MDK_COUNT(kAntiAliasingNames) ? kAntiAliasingNames[index] : "?";
}

} // namespace mdk

// enhanced_config.cpp -- see enhanced_config.h. Cold path: config is
// read once at boot and written when a toggle changes, never per frame.

#include "game/enhanced_config.h"

#include <stdio.h>
#include <string.h>

namespace mdk {

const LookDial kLookDials[kLookDialCount] = {
    {"hazeDensity", &EnhancedToggles::hazeDensity, 0, 400, 25},
    {"glowStrength", &EnhancedToggles::glowStrength, 0, 300, 25},
    {"shadowDarkness", &EnhancedToggles::shadowDarkness, 0, 100, 10},
    {"exposure", &EnhancedToggles::exposure, 50, 200, 10},
    {"tintSaturation", &EnhancedToggles::tintSaturation, 0, 300, 25},
};

int32_t clampLookDial(const LookDial& dial, int32_t value) {
    return value < dial.lo ? dial.lo : (value > dial.hi ? dial.hi : value);
}

namespace {

bool keyIs(const char* begin, const char* end, const char* name) {
    const size_t length = (size_t)(end - begin);
    return strlen(name) == length && strncmp(begin, name, length) == 0;
}

// The value word after '=': 1/true/on/yes read on, 0/false/off/no read
// off (any case, surrounding spaces ignored). Anything else leaves the
// toggle alone.
bool readValue(const char* begin, const char* end, bool* out) {
    while (begin < end && (*begin == ' ' || *begin == '\t')) {
        begin++;
    }
    while (end > begin && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) {
        end--;
    }
    const size_t length = (size_t)(end - begin);
    char word[8];
    if (length == 0 || length >= sizeof(word)) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        char c = begin[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        word[i] = c;
    }
    word[length] = '\0';
    if (strcmp(word, "1") == 0 || strcmp(word, "true") == 0 || strcmp(word, "on") == 0 ||
        strcmp(word, "yes") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(word, "0") == 0 || strcmp(word, "false") == 0 || strcmp(word, "off") == 0 ||
        strcmp(word, "no") == 0) {
        *out = false;
        return true;
    }
    return false;
}

// A whole-number value (spaces and a trailing CR ignored); false when the
// word is not one.
bool readNumber(const char* begin, const char* end, int32_t* out) {
    while (begin < end && (*begin == ' ' || *begin == '\t')) {
        begin++;
    }
    while (end > begin && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) {
        end--;
    }
    if (begin >= end || end - begin > 6) {
        return false;
    }
    int32_t value = 0;
    for (const char* c = begin; c < end; c++) {
        if (*c < '0' || *c > '9') {
            return false;
        }
        value = value * 10 + (*c - '0');
    }
    *out = value;
    return true;
}

void applyLine(const char* begin, const char* end, EnhancedToggles* toggles) {
    // Lines starting with ';' or '#' are comments; blank lines pass.
    while (begin < end && (*begin == ' ' || *begin == '\t')) {
        begin++;
    }
    if (begin >= end || *begin == ';' || *begin == '#') {
        return;
    }
    const char* equals = begin;
    while (equals < end && *equals != '=') {
        equals++;
    }
    if (equals >= end) {
        return;
    }
    const char* keyEnd = equals;
    while (keyEnd > begin && (keyEnd[-1] == ' ' || keyEnd[-1] == '\t')) {
        keyEnd--;
    }
    // The one number: the 3DS shadow map's side (render_types.h). Only the
    // sizes the renderer makes are taken; anything else means automatic.
    if (keyIs(begin, keyEnd, "shadowMapSize")) {
        int32_t size = 0;
        if (readNumber(equals + 1, end, &size)) {
            toggles->shadowMapSize = (size == 128 || size == 256 || size == 512) ? size : 0;
        }
        return;
    }
    // ... and how often the map is redrawn (render_types.h): 1-4, else automatic.
    if (keyIs(begin, keyEnd, "shadowMapRefresh")) {
        int32_t every = 0;
        if (readNumber(equals + 1, end, &every)) {
            toggles->shadowMapRefresh = (every >= 1 && every <= 4) ? every : 0;
        }
        return;
    }
    // The tiny-debris cull (render_types.h): 0-3 pixels, else the default 2.
    if (keyIs(begin, keyEnd, "tinyDebrisPixels")) {
        int32_t pixels = 0;
        if (readNumber(equals + 1, end, &pixels)) {
            toggles->tinyDebrisPixels = (pixels >= 0 && pixels <= 3) ? pixels : 2;
        }
        return;
    }
    // ... and the tiny-enemy cull's, likewise.
    if (keyIs(begin, keyEnd, "tinyEnemyPixels")) {
        int32_t pixels = 0;
        if (readNumber(equals + 1, end, &pixels)) {
            toggles->tinyEnemyPixels = (pixels >= 0 && pixels <= 3) ? pixels : 2;
        }
        return;
    }
    // The look dials (render_types.h), clamped into their range.
    for (int32_t i = 0; i < kLookDialCount; i++) {
        if (keyIs(begin, keyEnd, kLookDials[i].key)) {
            int32_t percent = 0;
            if (readNumber(equals + 1, end, &percent)) {
                toggles->*(kLookDials[i].field) = clampLookDial(kLookDials[i], percent);
            }
            return;
        }
    }
    bool value = false;
    if (!readValue(equals + 1, end, &value)) {
        return;
    }
    if (keyIs(begin, keyEnd, "enableSkyShadows")) {
        toggles->enableSkyShadows = value;
    } else if (keyIs(begin, keyEnd, "enableFragmentLights")) {
        toggles->enableFragmentLights = value;
    } else if (keyIs(begin, keyEnd, "enableHaze")) {
        toggles->enableHaze = value;
    } else if (keyIs(begin, keyEnd, "enableMirrors")) {
        toggles->enableMirrors = value;
    } else if (keyIs(begin, keyEnd, "enableGlowBillboards")) {
        toggles->enableGlowBillboards = value;
    } else if (keyIs(begin, keyEnd, "enableFiltering")) {
        toggles->enableFiltering = value;
    } else if (keyIs(begin, keyEnd, "enableMipmaps")) {
        toggles->enableMipmaps = value;
    } else if (keyIs(begin, keyEnd, "enableHdTextures")) {
        toggles->enableHdTextures = value;
    } else if (keyIs(begin, keyEnd, "enableLighting")) {
        toggles->enableLighting = value;
    } else if (keyIs(begin, keyEnd, "enableEdgeSmoothing")) {
        toggles->enableEdgeSmoothing = value;
    } else if (keyIs(begin, keyEnd, "shadowMapFlipBytes")) {
        toggles->shadowMapFlipBytes = value;
    } else if (keyIs(begin, keyEnd, "edgeSmoothing2x2")) {
        toggles->edgeSmoothing2x2 = value;
    } else if (keyIs(begin, keyEnd, "glowBloom")) {
        toggles->glowBloom = value;
    } else if (keyIs(begin, keyEnd, "texture16")) {
        toggles->texture16 = value;
    } else if (keyIs(begin, keyEnd, "kurtCompressed")) {
        toggles->kurtCompressed = value;
    } else if (keyIs(begin, keyEnd, "cap30Fps")) {
        toggles->cap30Fps = value;
    } else if (keyIs(begin, keyEnd, "movieHalfRate")) {
        toggles->movieHalfRate = value;
    } else if (keyIs(begin, keyEnd, "scopeMono")) {
        toggles->scopeMono = value;
    } else if (keyIs(begin, keyEnd, "scopeClip")) {
        toggles->scopeClip = value;
    } else if (keyIs(begin, keyEnd, "sniperRoundCams")) {
        toggles->sniperRoundCams = value;
    }
}

void appendToggle(std::string* out, const char* name, bool value) {
    out->append(name);
    out->append(value ? "=1\n" : "=0\n");
}

} // namespace

bool parseEnhancedConfig(const char* text, EnhancedToggles* toggles) {
    if (!text || !toggles) {
        return false;
    }
    const char* line = text;
    while (*line != '\0') {
        const char* end = line;
        while (*end != '\0' && *end != '\n') {
            end++;
        }
        applyLine(line, end, toggles);
        line = *end == '\n' ? end + 1 : end;
    }
    return true;
}

std::string writeEnhancedConfig(const EnhancedToggles& toggles) {
    std::string out = "; MDK enhanced mode (SPEC.md 7.9). 1 = on, 0 = off.\n";
    appendToggle(&out, "enableSkyShadows", toggles.enableSkyShadows);
    appendToggle(&out, "enableFragmentLights", toggles.enableFragmentLights);
    appendToggle(&out, "enableHaze", toggles.enableHaze);
    appendToggle(&out, "enableMirrors", toggles.enableMirrors);
    appendToggle(&out, "enableGlowBillboards", toggles.enableGlowBillboards);
    appendToggle(&out, "enableFiltering", toggles.enableFiltering);
    appendToggle(&out, "enableMipmaps", toggles.enableMipmaps);
    appendToggle(&out, "enableHdTextures", toggles.enableHdTextures);
    appendToggle(&out, "enableLighting", toggles.enableLighting);
    appendToggle(&out, "enableEdgeSmoothing", toggles.enableEdgeSmoothing);
    // 3DS: 2x2 rather than the default 2x1 (render_types.h).
    appendToggle(&out, "edgeSmoothing2x2", toggles.edgeSmoothing2x2);
    // 3DS: the bloom rather than the default billboards (render_types.h).
    appendToggle(&out, "glowBloom", toggles.glowBloom);
    // 3DS: 16-bit texture stores, from the next texture made (render_types.h).
    appendToggle(&out, "texture16", toggles.texture16);
    // 3DS: Kurt's frames from the ETC1A4 set, from the next level load (render_types.h).
    appendToggle(&out, "kurtCompressed", toggles.kurtCompressed);
    // A diagnostic, not a look (render_types.h): written so it can be found.
    appendToggle(&out, "shadowMapFlipBytes", toggles.shadowMapFlipBytes);
    // 0 = automatic (render_types.h).
    char size[32];
    snprintf(size, sizeof(size), "shadowMapSize=%d\n", (int)toggles.shadowMapSize);
    out.append(size);
    snprintf(size, sizeof(size), "shadowMapRefresh=%d\n", (int)toggles.shadowMapRefresh);
    out.append(size);
    // 3DS: present at 30 fps (render_types.h).
    appendToggle(&out, "cap30Fps", toggles.cap30Fps);
    // 3DS: the movies' uploads at half rate (render_types.h).
    appendToggle(&out, "movieHalfRate", toggles.movieHalfRate);
    // 3DS: the sniper scope in 2D (render_types.h).
    appendToggle(&out, "scopeMono", toggles.scopeMono);
    // 3DS: the scene clipped to the scope's window (render_types.h).
    appendToggle(&out, "scopeClip", toggles.scopeClip);
    appendToggle(&out, "sniperRoundCams", toggles.sniperRoundCams);
    // 3DS: debris this many pixels or smaller is not drawn, 0 = off (render_types.h).
    snprintf(size, sizeof(size), "tinyDebrisPixels=%d\n", (int)toggles.tinyDebrisPixels);
    out.append(size);
    // 3DS: enemies this many pixels or smaller are not drawn, 0 = off (render_types.h).
    snprintf(size, sizeof(size), "tinyEnemyPixels=%d\n", (int)toggles.tinyEnemyPixels);
    out.append(size);
    // The look dials, percent of the shipped value (render_types.h).
    for (int32_t i = 0; i < kLookDialCount; i++) {
        snprintf(size, sizeof(size), "%s=%d\n", kLookDials[i].key,
                 (int)(toggles.*(kLookDials[i].field)));
        out.append(size);
    }
    return out;
}

} // namespace mdk

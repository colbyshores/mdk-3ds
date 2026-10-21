// ============================================================================
// material_resolver.cpp — see material_resolver.h.
// ============================================================================

#include "render/material_resolver.h"

#include <string.h>

namespace mdk {

const char* const MaterialResolver::kPenPrefix = "PEN_";

namespace {

const float kByteToUnit = 1.0f / 255.0f;

// The shading of a pass: the solid and the double-sided passes take the
// look's shading, and the rest keep the original's.
Shading shadingOf(Pass pass, Shading shading) {
    return (pass == Pass::Solid || pass == Pass::DoubleSided) ? shading : Shading::Original;
}

} // namespace

MaterialResolver::MaterialResolver(const formats::Dti* dti, Shading shading)
    : dti_(dti), shading_(shading) {
}

bool MaterialResolver::resolve(int32_t value,
                               const std::vector<std::string>& names,
                               const formats::Palette& palette,
                               const std::vector<const formats::TextureArchive*>& archives,
                               Surface* out) const {
    if (value < 0) {
        return resolveColour(-value, palette, out);
    }

    if (value < 0 || (size_t)value >= names.size()) {
        return false;
    }
    const std::string& name = names[(size_t)value];

    // A texture, in the first archive that has it.
    for (size_t i = 0; i < archives.size(); i++) {
        const formats::Texture* texture = archives[i]->findTexture(name);
        if (!texture) {
            continue;
        }
        out->texture = texture;
        out->frameCount = texture->frameCount();
        out->pass = Pass::Solid;
        out->shading = shadingOf(Pass::Solid, shading_);
        out->colour = Colour{1.0f, 1.0f, 1.0f, 1.0f};
        out->rowShift = 0.0f;
        return true;
    }

    // Then an archive colour, by name.
    for (size_t i = 0; i < archives.size(); i++) {
        const int32_t index = archives[i]->findColor(name);
        if (index >= 0) {
            return resolveColour(index, palette, out);
        }
    }

    // Then PEN_n: a flat palette colour named by a material.
    if (name.compare(0, strlen(kPenPrefix), kPenPrefix) == 0 &&
        name.size() > strlen(kPenPrefix)) {
        bool digits = true;
        for (size_t c = strlen(kPenPrefix); c < name.size(); c++) {
            if (name[c] < '0' || name[c] > '9') {
                digits = false;
                break;
            }
        }
        if (digits) {
            return resolveColour(atoi(name.c_str() + strlen(kPenPrefix)), palette, out);
        }
    }

    return false;
}

bool MaterialResolver::resolveColour(int32_t index, const formats::Palette& palette, Surface* out) const {
    if (index < kSpecialFirst) {
        const uint8_t* rgba = palette.rgba();
        if (index < 0 || index >= formats::Palette::kSize) {
            return false;
        }
        out->texture = nullptr;
        out->colour = Colour{rgba[index * 4 + 0] * kByteToUnit,
                             rgba[index * 4 + 1] * kByteToUnit,
                             rgba[index * 4 + 2] * kByteToUnit,
                             1.0f};
        out->frameCount = 1;
        out->pass = Pass::Solid;
        out->shading = shadingOf(Pass::Solid, shading_);
        out->rowShift = 0.0f;
        return true;
    }

    // The glass: the level's colour blended by its alpha, both faces.
    if (index >= kGlassFirst && index <= kGlassLast) {
        if (!dti_) {
            return false;
        }
        const std::vector<std::vector<uint32_t>>& glass = dti_->glass();
        const size_t at = (size_t)(index - kGlassFirst);
        if (at >= glass.size() || glass[at].size() < 4) {
            return false;
        }
        out->texture = nullptr;
        out->colour = Colour{glass[at][0] * kByteToUnit,
                             glass[at][1] * kByteToUnit,
                             glass[at][2] * kByteToUnit,
                             glass[at][3] * kByteToUnit};
        out->frameCount = 1;
        out->pass = Pass::Blended;
        out->shading = Shading::Original;
        out->rowShift = 0.0f;
        return true;
    }

    // The mirrors: the panorama, shifted by rows.
    if (index >= kMirrorFirst && index <= kMirrorLast) {
        out->texture = nullptr;
        out->colour = Colour{1.0f, 1.0f, 1.0f, 1.0f};
        out->frameCount = 1;
        out->pass = Pass::Mirror;
        out->shading = Shading::Original;
        out->rowShift = (kMirrorMiddle - index) * kMirrorRowsPerValue;
        return true;
    }

    // NONE, PEN_ENV and RIPPLE are not drawn.
    return false;
}

bool MaterialResolver::isDrawnColour(int32_t index) {
    return index < kSpecialFirst ||
           (index >= kGlassFirst && index <= kGlassLast) ||
           (index >= kMirrorFirst && index <= kMirrorLast);
}

bool MaterialResolver::isDrawn(int32_t value,
                               const std::vector<std::string>& names,
                               const std::vector<const formats::TextureArchive*>& archives) {
    if (value < 0) {
        return isDrawnColour(-value);
    }
    if ((size_t)value >= names.size()) {
        return false;
    }
    const std::string& name = names[(size_t)value];
    for (size_t i = 0; i < archives.size(); i++) {
        if (archives[i]->findTexture(name)) {
            return true;
        }
    }
    for (size_t i = 0; i < archives.size(); i++) {
        const int32_t index = archives[i]->findColor(name);
        if (index >= 0) {
            return isDrawnColour(index);
        }
    }
    if (name.compare(0, strlen(kPenPrefix), kPenPrefix) == 0 &&
        name.size() > strlen(kPenPrefix)) {
        bool digits = true;
        for (size_t c = strlen(kPenPrefix); c < name.size(); c++) {
            if (name[c] < '0' || name[c] > '9') {
                digits = false;
                break;
            }
        }
        if (digits) {
            return isDrawnColour(atoi(name.c_str() + strlen(kPenPrefix)));
        }
    }
    return false;
}

} // namespace mdk

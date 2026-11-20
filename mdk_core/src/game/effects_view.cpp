// ============================================================================
// effects_view.cpp -- see effects_view.h.
// ============================================================================

#include "game/effects_view.h"

#include <math.h>
#include <string.h>

#include "game/level_data.h"
#include "render/post_params.h"
#include "scripts/debris.h"
#include "scripts/effects.h"
#include "scripts/script_runtime.h"

namespace mdk {

namespace {

Vec3 add(const Vec3& a, const Vec3& b) { return Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 sub(const Vec3& a, const Vec3& b) { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 scaled(const Vec3& a, float s) { return Vec3{a.x * s, a.y * s, a.z * s}; }
Vec3 cross(const Vec3& a, const Vec3& b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 normalized(const Vec3& a) {
    const float length = sqrtf(dot(a, a));
    if (length == 0.0f) {
        return a;
    }
    return scaled(a, 1.0f / length);
}

// Vector3.Transform(value, quaternion) of System.Numerics.
Vec3 transformByQuaternion(const Vec3& v, const float q[4]) {
    const Vec3 qv{q[0], q[1], q[2]};
    const Vec3 inner = add(cross(qv, v), scaled(v, q[3]));
    return add(v, scaled(cross(qv, inner), 2.0f));
}

bool sameSurface(const Surface& a, const Surface& b) {
    return a.texture == b.texture && a.colour.r == b.colour.r && a.colour.g == b.colour.g &&
           a.colour.b == b.colour.b && a.colour.a == b.colour.a && a.frameCount == b.frameCount &&
           a.pass == b.pass && a.shading == b.shading && a.rowShift == b.rowShift;
}

} // namespace

void effectsBillboardBasis(const Mat4& viewProjection, Vec3* right, Vec3* up, Vec3* forward) {
    const float* m = viewProjection.m;
    *forward = normalized(Vec3{m[3], m[7], m[11]});
    *right = normalized(Vec3{m[0], m[4], m[8]});
    *up = cross(*right, *forward);
}

int32_t effectsSparkColourIndex(int32_t base, int32_t range, const Vec3& a, const Vec3& b,
                                const Vec3& c, const Vec3& forward) {
    const Vec3 normal = cross(sub(b, a), sub(c, a));
    float facing = 0.0f;
    if (normal.x != 0.0f || normal.y != 0.0f || normal.z != 0.0f) {
        facing = fabsf(dot(normalized(normal), forward));
    }
    // MathF.Round rounds ties to even; rintf does the same in the default
    // rounding mode, and no code here changes that mode.
    int32_t index = (int32_t)rintf((float)base + (float)range * facing);
    if (index < 0) {
        index = 0;
    }
    if (index > 255) {
        index = 255;
    }
    return index;
}

void effectsQuadVertices(const Vec3& center, const Vec3& right, const Vec3& up, int32_t width,
                         int32_t height, Vertex out[6]) {
    const Vec3 halfWidth = scaled(right, (float)width * 0.5f);
    const Vec3 halfHeight = scaled(up, (float)height * 0.5f);
    const Vec3 negHalfHeight{-halfHeight.x, -halfHeight.y, -halfHeight.z};
    // Corner(x, y) of the oracle: centre + halfWidth * (2x - 1) +
    // halfHeight * (1 - 2y), with uv (x, y).
    const Vec3 corners[4] = {
        add(sub(center, halfWidth), halfHeight),  // (0, 0)
        add(add(center, halfWidth), halfHeight),  // (1, 0)
        add(add(center, halfWidth), negHalfHeight),  // (1, 1)
        add(sub(center, halfWidth), negHalfHeight),  // (0, 1)
    };
    const Vec2 uvs[4] = {Vec2{0.0f, 0.0f}, Vec2{1.0f, 0.0f}, Vec2{1.0f, 1.0f}, Vec2{0.0f, 1.0f}};
    const int32_t order[6] = {0, 1, 2, 0, 2, 3};
    for (int32_t i = 0; i < 6; i++) {
        out[i] = Vertex{corners[order[i]], uvs[order[i]], kVertexWhite};
    }
}

void effectsGlowOf(const uint8_t* indices, int32_t texelCount, const uint8_t* paletteRgba,
                   float* glow, float colour[3]) {
    *glow = 0.0f;
    colour[0] = colour[1] = colour[2] = 0.0f;
    if (!indices || !paletteRgba || texelCount <= 0) {
        return;
    }
    // Which palette entries pass, decided once per entry (256 powf), then a
    // count over the texels: integer work per texel.
    bool bright[256];
    for (int32_t i = 0; i < 256; i++) {
        bright[i] = false;
        for (int32_t c = 0; c < 3; c++) {
            const float srgb = (float)paletteRgba[i * 4 + c] / 255.0f;
            if (powf(srgb, 2.2f) > kGlowThreshold) {
                bright[i] = true;
            }
        }
    }
    float excess[256][3];
    for (int32_t i = 0; i < 256; i++) {
        for (int32_t c = 0; c < 3; c++) {
            const float linear = powf((float)paletteRgba[i * 4 + c] / 255.0f, 2.2f);
            excess[i][c] = linear > kGlowThreshold ? linear - kGlowThreshold : 0.0f;
        }
    }
    int32_t opaque = 0;
    int32_t passing = 0;
    float sum[3] = {0.0f, 0.0f, 0.0f};
    for (int32_t t = 0; t < texelCount; t++) {
        const uint8_t index = indices[t];
        if (index == 0) {
            continue;   // transparent
        }
        opaque++;
        if (bright[index]) {
            passing++;
            for (int32_t c = 0; c < 3; c++) {
                sum[c] += excess[index][c];
            }
        }
    }
    if (opaque == 0 || passing == 0) {
        return;
    }
    *glow = (float)passing / (float)opaque;
    for (int32_t c = 0; c < 3; c++) {
        colour[c] = sum[c] / (float)passing;
    }
}

const ObjectLook& EffectsView::lookOf(const std::string& arena) {
    std::map<std::string, ObjectLook>::iterator found = looks_.find(arena);
    if (found != looks_.end()) {
        return found->second;
    }
    ObjectLook look;
    const formats::Arena* owner = level_->arenaNamed(arena);
    if (owner) {
        look.palette = &level_->paletteOf(owner);
        look.archives = level_->archivesOf(owner);
    } else {
        look.palette = &level_->dti().palette();
        look.archives.push_back(&level_->levelTextures());
    }
    return looks_.emplace(arena, look).first->second;
}

EffectsView::SpriteEntry& EffectsView::sprite(const std::string& arena,
                                              const std::string& texture) {
    const std::pair<std::string, std::string> key(arena, texture);
    std::map<std::pair<std::string, std::string>, SpriteEntry>::iterator found = spriteCache_.find(key);
    if (found != spriteCache_.end()) {
        return found->second;
    }
    const ObjectLook& look = lookOf(arena);
    SpriteEntry entry;
    entry.palette = look.palette;
    std::vector<std::string> names(1, texture);
    Surface surface;
    if (resolver_->resolve(0, names, *look.palette, look.archives, &surface, Pass::DoubleSided) &&
        surface.texture != nullptr) {
        surface.shading = sprites_;
        entry.present = true;
        entry.surface = surface;
        // The glow, measured on every frame of the texture (an explosion's
        // late frames are its dim ones; the mean is the sprite's).
        // (pixelsAway / residentBytes, never indices() first: indices() LOADS
        // pixels that are away -- a mid-frame re-parse, review 2026-11-19.)
        const formats::Texture& t = *surface.texture;
        if (!t.pixelsAway() && t.residentBytes() > 0 && look.palette != nullptr) {
            effectsGlowOf(t.indices(), t.width() * t.height() * t.frameCount(),
                          look.palette->rgba(), &entry.glow, entry.glowColour);
            entry.glowMeasured = true;
        }
    }
    return spriteCache_.emplace(key, entry).first->second;
}

EffectsView::Batch& EffectsView::batchFor(const Surface& surface, const formats::Palette* palette) {
    for (int32_t i = 0; i < batchCount_; i++) {
        if (batches_[i].palette == palette && sameSurface(batches_[i].surface, surface)) {
            return batches_[i];
        }
    }
    if (batchCount_ == (int32_t)batches_.size()) {
        batches_.push_back(Batch());
    }
    Batch& batch = batches_[batchCount_++];
    batch.surface = surface;
    batch.palette = palette;
    batch.vertices.clear();
    return batch;
}

void EffectsView::build(const scripts::ScriptRuntime& scripts, const Camera& camera,
                        EffectsFrame* frame) {
    frame->reset();
    batchCount_ = 0;

    Mat4 view;
    Mat4 projection;
    memcpy(view.m, camera.view, sizeof(view.m));
    memcpy(projection.m, camera.projection, sizeof(projection.m));
    const Mat4 viewProjection = multiply(view, projection);
    Vec3 right;
    Vec3 up;
    Vec3 forward;
    effectsBillboardBasis(viewProjection, &right, &up, &forward);

    // The pieces, batched by surface in the order their surfaces came.
    const std::vector<scripts::Debris::Piece*>& pieces = scripts.debris().pieces();
    for (size_t p = 0; p < pieces.size(); p++) {
        const scripts::Debris::Piece& piece = *pieces[p];
        const ObjectLook& look = lookOf(piece.arena);
        const int32_t triangles = (int32_t)piece.corners.size() / 3;
        for (int32_t t = 0; t < triangles; t++) {
            const Vec3 a = transformByQuaternion(piece.corners[t * 3], piece.orientation);
            const Vec3 b = transformByQuaternion(piece.corners[t * 3 + 1], piece.orientation);
            const Vec3 c = transformByQuaternion(piece.corners[t * 3 + 2], piece.orientation);
            Surface surface;
            if (piece.isSpark()) {
                const int32_t index =
                    effectsSparkColourIndex(piece.colourBase, piece.colourRange, a, b, c, forward);
                const uint8_t* rgba = look.palette->rgba();
                const Colour colour{(float)rgba[index * 4] / 255.0f,
                                    (float)rgba[index * 4 + 1] / 255.0f,
                                    (float)rgba[index * 4 + 2] / 255.0f, 1.0f};
                resolver_->resolveFlat(colour, &surface, Pass::DoubleSided);
            } else if (!resolver_->resolve(piece.materials[t], *piece.names, *look.palette,
                                           look.archives, &surface, Pass::DoubleSided)) {
                continue;
            }
            Batch& batch = batchFor(surface, look.palette);
            const float uvScaleX = surface.texture ? 1.0f / (float)surface.texture->width() : 0.0f;
            const float uvScaleY = surface.texture ? 1.0f / (float)surface.texture->height() : 0.0f;
            const Vec3 world[3] = {add(piece.center, a), add(piece.center, b), add(piece.center, c)};
            for (int32_t k = 0; k < 3; k++) {
                const Vec2 uv = piece.isSpark()
                                    ? Vec2{0.0f, 0.0f}
                                    : Vec2{piece.uvs[t * 3 + k].x * uvScaleX,
                                           piece.uvs[t * 3 + k].y * uvScaleY};
                batch.vertices.push_back(Vertex{world[k], uv, kVertexWhite});
            }
        }
    }
    for (int32_t i = 0; i < batchCount_; i++) {
        const Batch& batch = batches_[i];
        int32_t count = (int32_t)batch.vertices.size();
        const int32_t remaining = kMaxVertices - (int32_t)frame->vertices.size();
        if (count > remaining) {
            count = remaining / 3 * 3;
        }
        if (count == 0) {
            break;
        }
        EffectsDrawItem queued;
        queued.first = (int32_t)frame->vertices.size();
        queued.count = count;
        queued.surface = batch.surface;
        queued.palette = batch.palette;
        queued.frame = 0;
        frame->draws.push_back(queued);
        frame->vertices.insert(frame->vertices.end(), batch.vertices.begin(),
                               batch.vertices.begin() + count);
    }

    // The sprite effects: a quad per effect, one frame of its texture.
    const std::vector<scripts::Effects::Effect*>& effects = scripts.effects.all();
    lastSprites_ = 0;
    lastGlowing_ = 0;
    lastUnmeasured_ = 0;
    for (size_t i = 0; i < effects.size(); i++) {
        const scripts::Effects::Effect& effect = *effects[i];
        SpriteEntry& entry = sprite(effect.arena, effect.texture);
        if (!entry.present) {
            continue;
        }
        lastSprites_++;
        if (!entry.glowMeasured) {
            // Its pixels were away when the entry was made: measured now if
            // they are back (once), counted as unmeasured if not.
            const formats::Texture& t = *entry.surface.texture;
            if (!t.pixelsAway() && t.residentBytes() > 0 && entry.palette != nullptr) {
                effectsGlowOf(t.indices(), t.width() * t.height() * t.frameCount(),
                              entry.palette->rgba(), &entry.glow, entry.glowColour);
                entry.glowMeasured = true;
            } else {
                lastUnmeasured_++;
            }
        }
        if ((int32_t)frame->vertices.size() + 6 > kMaxVertices) {
            continue;
        }
        const float texel = effect.scale / scripts::Effects::kTexelsPerUnit;
        Vertex quad[6];
        effectsQuadVertices(effect.position, scaled(right, texel), scaled(up, texel),
                            entry.surface.texture->width(), entry.surface.texture->height(), quad);
        EffectsDrawItem queued;
        queued.first = (int32_t)frame->vertices.size();
        queued.count = 6;
        queued.surface = entry.surface;
        queued.palette = entry.palette;
        queued.frame = effect.frame();
        frame->draws.push_back(queued);
        frame->vertices.insert(frame->vertices.end(), quad, quad + 6);
        // ITS HALO, if the PC's bloom would catch it: the same quad, kGlowHaloScale
        // times the size, the measured colour at the bloom's gain and share.
        if (entry.glow > 0.0f) {
            GlowQuad glow;
            const float haloTexel = texel * kGlowHaloScale;
            // Half the sprite's size towards the camera: the sprite writes depth,
            // and a coplanar halo tested LEQUAL could round behind it on the
            // console's 24-bit floats and leave a hole where it is brightest.
            const int32_t w = entry.surface.texture->width();
            const int32_t h = entry.surface.texture->height();
            const float nudge = texel * (float)(w > h ? w : h) * 0.5f;
            effectsQuadVertices(sub(effect.position, scaled(forward, nudge)),
                                scaled(right, haloTexel), scaled(up, haloTexel), w, h,
                                glow.corners);
            // What the PC's bloom adds over a dark ground: to_srgb(gain * the
            // excess), and as much of it as the texture's bright share.
            for (int32_t c = 0; c < 3; c++) {
                glow.colour[c] =
                    powf(kGlowIntensity * entry.glowColour[c], 0.45454545f) * entry.glow;
            }
            frame->glows.push_back(glow);
            lastGlowing_++;
        }
    }
}

} // namespace mdk

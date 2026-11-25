// ============================================================================
// effects_view.cpp -- see effects_view.h.
// ============================================================================

#include "game/effects_view.h"

#include <math.h>
#include <string.h>

#include "formats/arena.h"
#include "game/level_data.h"
#include "render/post_params.h"
#include "scripts/debris.h"
#include "scripts/effects.h"
#include "scripts/end_level.h"
#include "scripts/script_runtime.h"
#include "util/math_lut.h"
#include "util/math_minmax.h"

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

int32_t effectsRibbonVertices(const scripts::Ribbon& ribbon, int32_t width, int32_t height,
                              Vertex out[scripts::Ribbon::kMaxCorners]) {
    Vec3 positions[scripts::Ribbon::kMaxCorners];
    Vec2 uvs[scripts::Ribbon::kMaxCorners];
    const int32_t count = ribbon.triangles(positions, uvs);
    // AddRibbon: `uv * new Vector2(1f / Width, 1f / Height)`.
    const float scaleU = 1.0f / (float)width;
    const float scaleV = 1.0f / (float)height;
    for (int32_t i = 0; i < count; i++) {
        out[i] = Vertex{positions[i], Vec2{uvs[i].x * scaleU, uvs[i].y * scaleV}, kVertexWhite};
    }
    return count;
}

void effectsGlowOf(const uint8_t* indices, int32_t texelCount, const uint8_t* paletteRgba,
                   float* glow, float colour[3]) {
    *glow = 0.0f;
    colour[0] = colour[1] = colour[2] = 0.0f;
    if (!indices || !paletteRgba || texelCount <= 0) {
        return;
    }
    // Which palette entries pass and what each adds, decided once per entry,
    // then a HISTOGRAM of the texels -- one integer increment per texel --
    // summed per entry. The per-texel float sums it replaces were most of a
    // large sprite's effects-warm step on the Old 3DS (L7 walk, the corridor
    // crossing: SL_BIG's 64x1920 strip, 11.3 ms); the means differ from them by
    // rounding only (a count times an excess against that many additions).
    //
    // A channel byte's linear value is one of 256, so the powf is taken once
    // per byte value for the run (the same powf of the same input: the same
    // floats), not 768 times per sprite.
    static float linearOf[256];
    static bool linearReady = false;
    if (!linearReady) {
        for (int32_t v = 0; v < 256; v++) {
            linearOf[v] = powf((float)v / 255.0f, 2.2f);
        }
        linearReady = true;
    }
    bool bright[256];
    float excess[256][3];
    for (int32_t i = 0; i < 256; i++) {
        bright[i] = false;
        for (int32_t c = 0; c < 3; c++) {
            const float linear = linearOf[paletteRgba[i * 4 + c]];
            if (linear > kGlowThreshold) {
                bright[i] = true;
            }
            excess[i][c] = linear > kGlowThreshold ? linear - kGlowThreshold : 0.0f;
        }
    }
    // Four histograms, one per texel of each group of four, added after: a
    // run of one index (a sprite's flat areas) would otherwise make every
    // increment wait on the previous one's store to the same counter.
    uint32_t counts[4][256];
    memset(counts, 0, sizeof(counts));
    int32_t t = 0;
    for (; t + 4 <= texelCount; t += 4) {
        counts[0][indices[t]]++;
        counts[1][indices[t + 1]]++;
        counts[2][indices[t + 2]]++;
        counts[3][indices[t + 3]]++;
    }
    for (; t < texelCount; t++) {
        counts[0][indices[t]]++;
    }
    for (int32_t i = 0; i < 256; i++) {
        counts[0][i] += counts[1][i] + counts[2][i] + counts[3][i];
    }
    int32_t opaque = 0;
    int32_t passing = 0;
    float sum[3] = {0.0f, 0.0f, 0.0f};
    for (int32_t i = 1; i < 256; i++) {   // index 0 is transparent
        opaque += (int32_t)counts[0][i];
        if (bright[i] && counts[0][i] > 0) {
            passing += (int32_t)counts[0][i];
            for (int32_t c = 0; c < 3; c++) {
                sum[c] += (float)counts[0][i] * excess[i][c];
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
    if (lastSpriteEntry_ != nullptr && lastSpriteArena_ == arena &&
        lastSpriteTexture_ == texture) {
        return *lastSpriteEntry_;
    }
    const std::pair<std::string, std::string> key(arena, texture);
    std::map<std::pair<std::string, std::string>, SpriteEntry>::iterator found = spriteCache_.find(key);
    if (found != spriteCache_.end()) {
        lastSpriteArena_ = arena;
        lastSpriteTexture_ = texture;
        lastSpriteEntry_ = &found->second;
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
    SpriteEntry& placed = spriteCache_.emplace(key, entry).first->second;
    lastSpriteArena_ = arena;
    lastSpriteTexture_ = texture;
    lastSpriteEntry_ = &placed;
    return placed;
}

namespace {
// The script effects' textures (scripts/effects.cpp: the wound, the two
// drops, the bubble and its pop, the trail).
const char* const kWarmTextures[] = {"TRAIL", "SL_BIG", "SL_MED", "SL_SMA", "BUBB", "BUBB_POP"};
const size_t kWarmTextureCount = sizeof(kWarmTextures) / sizeof(kWarmTextures[0]);
}  // namespace

void EffectsView::warm(const std::string& arena, SurfaceStartFn ask, SurfaceStartFn start,
                       void* user) {
    warmAt_[arena] = 0;   // the full pass, as before warmStep existed
    while (!warmStep(arena, ask, start, user)) {
    }
}

bool EffectsView::warmStep(const std::string& arena, SurfaceStartFn ask, SurfaceStartFn start,
                           void* user) {
    if (ask == nullptr || start == nullptr || arena.empty()) {
        return true;
    }
    size_t& at = warmAt_[arena];
    if (at >= kWarmTextureCount) {
        return true;
    }
    // Resolved through sprite(), so the entry the first effect draws is
    // the entry warmed here.
    SpriteEntry& entry = sprite(arena, kWarmTextures[at]);
    at++;
    const bool done = at >= kWarmTextureCount;
    if (entry.present && entry.palette != nullptr && entry.surface.texture != nullptr) {
        // EVERY ONE IS ASKED FOR ITS PACKED ORIGINAL, whatever its size: a
        // pending id is no expansion, and the small ones were the hitch -- the
        // first explosion's drops (SL_MED 32x960, SL_SMA 16x480) are under the
        // cut below and were expanded whole in their first draw (Old 3DS
        // level-7 walk, frame 650: 42.7 + 11.0 ms).
        ask(user, entry.surface, entry.palette);
        // A make started for the large ones only: a small texture's make here
        // would be a whole expansion, and making six of them synchronously
        // would be the stall this avoids. The large ones are what a sliced make
        // spreads (the same 32768-texel rule as the arena pump's).
        const formats::Texture& texture = *entry.surface.texture;
        const int64_t texels = (int64_t)texture.width() * (int64_t)texture.height() *
                               (int64_t)texture.frameCount();
        if (texels >= 32768) {
            start(user, entry.surface, entry.palette);
        }
    }
    // Computed before the callback: `at` references the map, and the
    // callback must not be able to invalidate the return value.
    return done;
}

bool EffectsView::resolveCached(int32_t material, const std::vector<std::string>& names,
                                const ObjectLook& look, Pass pass, Surface* out) {
    // The memo's lifetime is the view's: clear() runs wherever the
    // runtime is rebuilt (level load/respawn, main_3ds.cpp), which is
    // also where the recycled-address hazard its comment names lives.
    // The names vector's address is part of the key, so two arenas'
    // material tables are never confused; the surface is plain data
    // (texture pointer, name, colour, counts) with no frame state.
    const std::pair<std::pair<int32_t, Pass>,
                    std::pair<const std::vector<std::string>*,
                              std::pair<const formats::Palette*,
                                        const formats::TextureArchive*> > >
        key(std::make_pair(material, pass),
            std::make_pair(&names, std::make_pair(look.palette, look.archives.empty()
                                                                      ? nullptr
                                                                      : look.archives[0])));
    std::map<std::pair<std::pair<int32_t, Pass>,
                       std::pair<const std::vector<std::string>*,
                                 std::pair<const formats::Palette*,
                                           const formats::TextureArchive*> > >,
             Surface>::iterator found = resolveMemo_.find(key);
    if (found != resolveMemo_.end()) {
        *out = found->second;
        return true;
    }
    Surface surface;
    if (!resolver_->resolve(material, names, *look.palette, look.archives, &surface, pass)) {
        return false;
    }
    resolveMemo_.emplace(key, surface);
    *out = surface;
    return true;
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

// C# LevelView.DrawPieces / AddPiece (LevelView.cs:194-249): each piece's triangle, its
// corners about their middle turned by the piece's angle (Matrix4x4.CreateRotationZ) and
// put at its centre, with the arena's UVs over its surface's texture size, both faces.
// Presentation: the turn goes through the shared LUT (R6), as the camera's does.
void EffectsView::addEndPieces(const scripts::EndLevel& end) {
    const std::vector<scripts::EndLevel::Piece>& pieces = end.pieces();
    if (pieces.empty()) {
        return;
    }
    const formats::Arena* arena = level_->arenaNamed(end.arena());
    if (arena == nullptr) {
        return;
    }
    const ObjectLook& look = lookOf(end.arena());
    const std::vector<formats::Vec3f>& positions = arena->vertices();
    const std::vector<int32_t>& indices = arena->triangleIndices();
    const std::vector<formats::Vec2f>& uvs = arena->triangleUvs();
    const std::vector<uint32_t>& flags = arena->triangleFlags();
    const std::vector<int32_t>& materials = arena->triangleMaterials();
    const float third = 1.0f / 3.0f;
    for (size_t p = 0; p < pieces.size(); p++) {
        const scripts::EndLevel::Piece& piece = pieces[p];
        const int32_t t = piece.triangle;
        if (t < 0 || t >= arena->triangleCount() ||
            (flags[(size_t)t] & (arena->clipFlag() | formats::Arena::kGroupDisabled)) != 0) {
            continue;
        }
        // The view's material: the group's stored one, else the triangle's
        // (LevelView.Build: group.Material ?? TriangleMaterials[t]).
        int32_t material = materials[(size_t)t];
        int32_t groupMaterial = 0;
        if (arena->groupMaterial((int32_t)(flags[(size_t)t] >> 24), &groupMaterial)) {
            material = groupMaterial;
        }
        Surface surface;
        if (!resolveCached(material, arena->materials(), look, Pass::DoubleSided, &surface)) {
            continue;
        }
        Batch& batch = batchFor(surface, look.palette);
        const float uvScaleX = surface.texture ? 1.0f / (float)surface.texture->width() : 0.0f;
        const float uvScaleY = surface.texture ? 1.0f / (float)surface.texture->height() : 0.0f;
        Vec3 corners[3];
        Vec3 middle{0.0f, 0.0f, 0.0f};
        for (int32_t k = 0; k < 3; k++) {
            const formats::Vec3f& v = positions[(size_t)indices[(size_t)t * 3 + (size_t)k]];
            corners[k] = Vec3{v.x, v.y, v.z};
            middle = add(middle, scaled(corners[k], third));
        }
        const float radians = piece.angle * 0.017453292f;
        const float c = fastCos(radians);
        const float s = fastSin(radians);
        for (int32_t k = 0; k < 3; k++) {
            const Vec3 d = sub(corners[k], middle);
            const Vec3 world{piece.centre.x + d.x * c - d.y * s, piece.centre.y + d.x * s + d.y * c,
                             piece.centre.z + d.z};
            const formats::Vec2f& uv = uvs[(size_t)t * 3 + (size_t)k];
            batch.vertices.push_back(Vertex{world, Vec2{uv.x * uvScaleX, uv.y * uvScaleY}, kVertexWhite});
        }
    }
}

// One sprite's quad at `position`, `texel` world units per texel, showing
// `textureFrame`, and its halo when the texture glows: the sprite effects' and the
// spawn_box boxes' common half of EffectsView.Draw (AddQuad).
void EffectsView::queueSprite(SpriteEntry& entry, const Vec3& position, float texel,
                              int32_t textureFrame, const Vec3& right, const Vec3& up,
                              const Vec3& forward, EffectsFrame* frame) {
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
        return;
    }
    Vertex quad[6];
    effectsQuadVertices(position, scaled(right, texel), scaled(up, texel),
                        entry.surface.texture->width(), entry.surface.texture->height(), quad);
    EffectsDrawItem queued;
    queued.first = (int32_t)frame->vertices.size();
    queued.count = 6;
    queued.surface = entry.surface;
    queued.palette = entry.palette;
    queued.frame = textureFrame;
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
        effectsQuadVertices(sub(position, scaled(forward, nudge)),
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

bool EffectsView::spriteCulled(const SpriteEntry& entry, const Vec3& position,
                               float texel) const {
    if (!hasCullFrustum_) {
        return false;
    }
    const int32_t w = entry.surface.texture->width();
    const int32_t h = entry.surface.texture->height();
    const float side = texel * (float)(w > h ? w : h);
    // The halo's half-diagonal (0.7072 of its side, kGlowHaloScale sides) plus
    // the half-side it is nudged by: a sphere around the quad and its halo.
    const float radius = side * (0.7072f * kGlowHaloScale + 0.5f);
    return frustumSphereOutside(cullFrustum_, position, radius + cullMargin_);
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

    // The end of a level's pieces (LevelView.DrawEnd), then the debris pieces,
    // batched by surface in the order their surfaces came.
    if (const scripts::EndLevel* end = scripts.endLevel()) {
        addEndPieces(*end);
    }
    const std::vector<scripts::Debris::Piece*>& pieces = scripts.debris().pieces();
    for (size_t p = 0; p < pieces.size(); p++) {
        const scripts::Debris::Piece& piece = *pieces[p];
        if (hasCullFrustum_) {
            // The piece's reach: its corners are offsets from its centre, turned by
            // its orientation, which keeps their lengths.
            float reach2 = 0.0f;
            for (size_t k = 0; k < piece.corners.size(); k++) {
                const Vec3& v = piece.corners[k];
                const float d2 = v.x * v.x + v.y * v.y + v.z * v.z;
                reach2 = mathMax(reach2, d2);
            }
            if (frustumSphereOutside(cullFrustum_, piece.center, sqrtf(reach2) + cullMargin_)) {
                continue;
            }
        }
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
            } else if (!resolveCached(piece.materials[t], *piece.names, look, Pass::DoubleSided,
                                      &surface)) {
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
        const float texel = effect.scale / scripts::Effects::kTexelsPerUnit;
        if (spriteCulled(entry, effect.position, texel)) {
            continue;
        }
        queueSprite(entry, effect.position, texel, effect.frame(), right, up, forward, frame);
    }

    // spawn_box sprites (EffectsView.cs:66-76): as wide as the box, playing at 30
    // frames per second -- the FIRE and PULSE hazards the scripts make (opcode 159).
    const std::vector<scripts::MdkObject*>& boxes = scripts.boxes();
    for (size_t i = 0; i < boxes.size(); i++) {
        const scripts::MdkObject& box = *boxes[i];
        if (box.dead || !box.visible || box.model == nullptr) {
            continue;
        }
        SpriteEntry& entry = sprite(box.arena, box.model->name);
        if (!entry.present) {
            continue;
        }
        const float sizeX = box.model->bounds.max.x - box.model->bounds.min.x;
        const float sizeZ = box.model->bounds.max.z - box.model->bounds.min.z;
        const int32_t width = entry.surface.texture->width();
        const float texel = mathMax(sizeX, sizeZ) / (float)(width > 1 ? width : 1);
        if (spriteCulled(entry, box.position, texel)) {
            continue;
        }
        const int32_t frames = entry.surface.frameCount > 0 ? entry.surface.frameCount : 1;
        /* @vfp-exempt R11 — the box sprite's frame, a 1:1 port of the C#'s
         * TickCount % FrameCount (EffectsView.cs:75); one modulo per live spawn_box box per
         * frame (a handful), by the texture's small positive frame count. measured n/a
         * (corpus empty). Verified 2026-11-20. */
        const int32_t boxFrame = scripts.tickCount() % frames;
        queueSprite(entry, box.position, texel, boxFrame, right, up, forward, frame);
    }

    // The twisters' ribbons (AddRibbon): one draw of WMIBTEX each, its corners in
    // order. The oracle's check order: nothing to draw, no room, no texture.
    const std::vector<scripts::Twister>& twisters = scripts.twisters();
    for (size_t i = 0; i < twisters.size(); i++) {
        const scripts::Twister& twister = twisters[i];
        if (twister.ribbon().count() < 2) {
            continue;
        }
        const int32_t corners = (twister.ribbon().count() - 1) * 6;
        if ((int32_t)frame->vertices.size() + corners > kMaxVertices) {
            continue;
        }
        SpriteEntry& entry = sprite(twister.arena(), scripts::Ribbon::kTexture);
        if (!entry.present) {
            continue;
        }
        Vertex ribbon[scripts::Ribbon::kMaxCorners];
        const int32_t count =
            effectsRibbonVertices(twister.ribbon(), entry.surface.texture->width(),
                                  entry.surface.texture->height(), ribbon);
        EffectsDrawItem queued;
        queued.first = (int32_t)frame->vertices.size();
        queued.count = count;
        queued.surface = entry.surface;
        queued.palette = entry.palette;
        queued.frame = 0;
        frame->draws.push_back(queued);
        frame->vertices.insert(frame->vertices.end(), ribbon, ribbon + count);
    }
}

} // namespace mdk

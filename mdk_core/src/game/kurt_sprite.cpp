// ============================================================================
// kurt_sprite.cpp — Kurt's world-space sprite. See kurt_sprite.h.
//
// This is the C# src/Mdk.Game/Kurt/KurtSprite.cs, with the operation order
// kept where it is visible in the pixels: the dot products, the divisions,
// the clamps and the two-row depth pull are all in the oracle's order.
// ============================================================================

#include "game/kurt_sprite.h"

#include <math.h>
#include <string.h>

#include <algorithm>
#include <utility>

#include "formats/bni.h"
#include "formats/palette.h"
#include "formats/sprite_animation.h"
#include "formats/texture.h"
#include "game/arena_space.h"
#include "game/mod_images.h"
#include "util/math_lut.h"

namespace mdk {

namespace {

const float kPi = 3.14159265358979323846f;
const float kViewHeight = 360.0f;
const int32_t kFeetOffset = 101;
const int32_t kQuadVertices = 6;
const int32_t kFrameVertices = 2 * kQuadVertices;
const float kFeetRise = 0.5f;
const float kFloorBelow = 0.5f;
const char* kChuteOpening = "K_CHUTE";
const char* kChuteSway = "K_CHUTEC";
const int32_t kChuteOpenedFrame = 5;
const int32_t kChuteClosedFrame = 7;

const char* kLevelAnimations[] = {"K_SURF", "K_SURFJ", "K_SLIP", "K_SLIDE", "K_FSLIDE", "K_BSLIDE"};

Vec3 subtract(const Vec3& a, const Vec3& b) {
    return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 addVectors(const Vec3& a, const Vec3& b) {
    return Vec3{a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 scale(const Vec3& v, float s) {
    return Vec3{v.x * s, v.y * s, v.z * s};
}

Vec3 cross(const Vec3& a, const Vec3& b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

// System.Numerics.Vector3.Normalize divides by the length. It does not
// multiply by a reciprocal, and the last bit is visible in the sprite's
// width, so this deliberately follows the oracle rather than the engine's
// camera helper.
Vec3 normalize(const Vec3& v) {
    const float length = sqrtf(dot(v, v));
    if (length <= 0.0f) {
        return Vec3{0.0f, 0.0f, 0.0f};
    }
    return Vec3{v.x / length, v.y / length, v.z / length};
}

float clampf(float value, float low, float high) {
    return value < low ? low : (value > high ? high : value);
}

// The seam between the renderer's Vec3 and the collision's formats::Vec3f.
inline Vec toVec(const Vec3& v) { return Vec{v.x, v.y, v.z}; }
inline Vec3 fromVec(const Vec& v) { return Vec3{v.x, v.y, v.z}; }

} // namespace

Vec3 KurtSprite::corner(const Quad& at, float ax, float ay, float px, float py) {
    return addVectors(addVectors(at.feet, scale(at.right, px - ax)), scale(at.up, ay - py));
}

int32_t KurtSprite::rows(Vertex* into, const Quad& at, float ax, float ay, int32_t width,
                          int32_t height, float top, float bottom, const Vec3& eye, float pull) {
    if (bottom <= top) {
        return 0;
    }

    // The local function in the C# becomes this ordinary helper here. It is
    // not a project template.
    struct CornerVertex {
        static Vertex make(const Quad& at, float ax, float ay, float px, float py, int32_t width,
                           int32_t height, const Vec3& eye, float pull) {
            const Vec3 world =
                addVectors(eye, scale(subtract(corner(at, ax, ay, px, py), eye), pull));
            Vertex vertex;
            vertex.pos = world;
            vertex.uv = Vec2{px / (float)width, py / (float)height};
            vertex.colour = kVertexWhite;
            return vertex;
        }
    };

    into[0] = CornerVertex::make(at, ax, ay, 0.0f, top, width, height, eye, pull);
    into[1] = CornerVertex::make(at, ax, ay, (float)width, top, width, height, eye, pull);
    into[2] = CornerVertex::make(at, ax, ay, (float)width, bottom, width, height, eye, pull);
    into[3] = CornerVertex::make(at, ax, ay, 0.0f, top, width, height, eye, pull);
    into[4] = CornerVertex::make(at, ax, ay, (float)width, bottom, width, height, eye, pull);
    into[5] = CornerVertex::make(at, ax, ay, 0.0f, bottom, width, height, eye, pull);
    return kQuadVertices;
}

KurtSprite::KurtSprite() {
    stateSpecs_.push_back(StateSpec{"K_STILL", Once});
    stateSpecs_.push_back(StateSpec{"K_IDLE", Once});
    stateSpecs_.push_back(StateSpec{"K_RUN", Loop});
    stateSpecs_.push_back(StateSpec{"K_SIDE", Loop});
    stateSpecs_.push_back(StateSpec{"K_TRN45", Loop});
    stateSpecs_.push_back(StateSpec{"K_JUMP", Once});
    stateSpecs_.push_back(StateSpec{"K_RJMP", Once});
    stateSpecs_.push_back(StateSpec{"K_FALL", Loop});
    stateSpecs_.push_back(StateSpec{"K_CHUTE", Once});
    stateSpecs_.push_back(StateSpec{"K_LAND", Once});
    stateSpecs_.push_back(StateSpec{"K_SHOT", Loop});
    stateSpecs_.push_back(StateSpec{"K_RUNFIR", Loop});
    stateSpecs_.push_back(StateSpec{"K_SPWEP", Once});
    stateSpecs_.push_back(StateSpec{"K_BANG", Once});
    stateSpecs_.push_back(StateSpec{"K_BFLIP", Once});
    stateSpecs_.push_back(StateSpec{"K_BANG", Once});
    stateSpecs_.push_back(StateSpec{"K_SLIP", Once});
    stateSpecs_.push_back(StateSpec{"K_SLIDE", Loop});
    stateSpecs_.push_back(StateSpec{"K_FSLIDE", Loop});
    stateSpecs_.push_back(StateSpec{"K_BSLIDE", Loop});
    stateSpecs_.push_back(StateSpec{"K_HANG", Once});
    stateSpecs_.push_back(StateSpec{"K_CRASHL", Once});
    stateSpecs_.push_back(StateSpec{"K_ROLLL", Once});
    stateSpecs_.push_back(StateSpec{"K_ROLLR", Once});
    stateSpecs_.push_back(StateSpec{"K_HELM", Once});
    stateSpecs_.push_back(StateSpec{"K_HELM", Once});
    stateIndices_.assign(stateSpecs_.size(), -1);
}

int KurtSprite::levelAnimationCount() {
    return (int)(sizeof(kLevelAnimations) / sizeof(kLevelAnimations[0]));
}

const char* KurtSprite::levelAnimationName(int index) {
    return index >= 0 && index < levelAnimationCount() ? kLevelAnimations[index] : "";
}

bool KurtSprite::load(MdkRenderer renderer, const formats::Bni& sprites,
                      const formats::Palette& palette) {
    if (!renderer) {
        return false;
    }

    renderer_ = renderer;
    paletteRgba_.assign(palette.rgba(), palette.rgba() + 256 * 4);
    palette_ = mdk_render_create_palette(renderer, paletteRgba_.data());
    mesh_ = mdk_render_create_dynamic_mesh(renderer, kFrameVertices);
    if (palette_ < 0 || mesh_ < 0) {
        return false;
    }

    // THE RESIDENCY BOUND. The backend names what it can hold: the desktop
    // has the memory for every frame the C# keeps (its answer is 0, "no
    // bound"), and the 3DS names a megabyte or so. See SpriteResidency.
    residency_.setBudget(mdk_render_capabilities(renderer).sprite_residency_bytes);

    // A RE-LOAD IS NOT A RESET OF THE GPU: the previous level's drawn frames
    // have textures now, and they have to be released before the animation
    // list that names them goes away. The palette and mesh's release is
    // recorded separately.
    for (size_t i = 0; i < animations_.size(); i++) {
        releaseAnimation((int32_t)i);
    }
    residency_.clear();
    animations_.clear();
    for (size_t i = 0; i < stateSpecs_.size(); i++) {
        if (findAnimation(stateSpecs_[i].name) >= 0) {
            continue;
        }
        formats::SpriteAnimation source;
        if (!sprites.animation(stateSpecs_[i].name, &source)) {
            continue;
        }
        if (!loadAnimation(renderer, stateSpecs_[i].name, source)) {
            continue;
        }
    }

    // The chute's sway, the muzzle flash and the beta back-up are separate
    // files in the same archive.
    const char* const extras[] = {kChuteSway, "K_MUZZF", "K_BCKUP"};
    for (size_t i = 0; i < sizeof(extras) / sizeof(extras[0]); i++) {
        formats::SpriteAnimation source;
        if (sprites.animation(extras[i], &source)) {
            loadAnimation(renderer, extras[i], source);
        }
    }

    resolveAnimations();
    // K_STILL is the frame every fresh Kurt needs. The other missing
    // animations belong to the ride, slide and beta subsystems and are
    // recorded as deferred; they must not make an ordinary level fail.
    return stateIndices_[(size_t)Kurt::Still] >= 0;
}

bool KurtSprite::add(MdkRenderer renderer, const std::string& name,
                     const formats::SpriteAnimation& animation) {
    if (!renderer || renderer_ != renderer || name.empty()) {
        return false;
    }

    Animation loaded;
    loaded.name = name;
    loaded.repeat = Once;
    if (!loadAnimation(renderer, name, animation, &loaded)) {
        return false;
    }

    const int32_t existing = findAnimation(name);
    if (existing >= 0) {
        // THE REPLACED ANIMATION'S GPU COPIES GO FIRST. The C#'s cache is
        // keyed by (name, frame), so a replaced animation's textures are the
        // same objects and nothing leaks there; here every frame has its own
        // texture, and the old ones have to be released or they are lost for
        // ever (a GPU copy nobody can name).
        releaseAnimation(existing);
        animations_[(size_t)existing] = std::move(loaded);
    } else {
        animations_.push_back(std::move(loaded));
    }
    resolveAnimations();
    return true;
}

bool KurtSprite::loadAnimation(MdkRenderer renderer, const std::string& name,
                               const formats::SpriteAnimation& animation) {
    Animation loaded;
    loaded.name = name;
    loaded.repeat = Once;
    if (!loadAnimation(renderer, name, animation, &loaded)) {
        return false;
    }
    animations_.push_back(std::move(loaded));
    return true;
}

bool KurtSprite::loadAnimation(MdkRenderer renderer, const std::string& name,
                               const formats::SpriteAnimation& animation, Animation* out) {
    if (!renderer || !out || paletteRgba_.empty()) {
        return false;
    }

    out->name = name;
    out->frames.clear();
    for (int32_t i = 0; i < animation.frameCount(); i++) {
        formats::SpriteAnimation::Frame decoded;
        if (!animation.frame(i, &decoded)) {
            out->frames.clear();
            return false;
        }
        const formats::Texture& image = decoded.image;
        if (image.width() <= 0 || image.height() <= 0) {
            out->frames.clear();
            return false;
        }
        Frame frame;
        frame.width = image.width();
        frame.height = image.height();
        frame.hotspotX = decoded.hotspotX;
        frame.hotspotY = decoded.hotspotY;
        // THE INDICES ARE COPIED. The archive is borrowed only while the load
        // runs, and the frame has to be uploadable again after its texture
        // has been dropped -- which is what makes the 3DS's residency bound
        // possible at all.
        //
        // The copy is one byte a pixel, which is a quarter of what the 3DS
        // spends on the GPU copy and an eighth of what it spends on the
        // padded one. The whole of TRAVSPRT.BNI's Kurt animations come to
        // 5.30 MiB this way (measured: 19 animations, 360 frames), against
        // the 49.94 MiB the same frames cost as GPU textures.
        const size_t pixels = (size_t)image.width() * (size_t)image.height();
        frame.indices.assign(image.indices(), image.indices() + pixels);
        out->frames.push_back(std::move(frame));
    }
    return !out->frames.empty();
}

int32_t KurtSprite::findAnimation(const std::string& name) const {
    for (size_t i = 0; i < animations_.size(); i++) {
        if (animations_[i].name == name) {
            return (int32_t)i;
        }
    }
    return -1;
}

void KurtSprite::resolveAnimations() {
    stateIndices_.assign(stateSpecs_.size(), -1);
    for (size_t i = 0; i < stateSpecs_.size(); i++) {
        stateIndices_[i] = findAnimation(stateSpecs_[i].name);
        if (stateIndices_[i] >= 0) {
            animations_[(size_t)stateIndices_[i]].repeat = stateSpecs_[i].repeat;
        }
    }
    chuteSway_ = findAnimation(kChuteSway);
    backUp_ = findAnimation("K_BCKUP");
    muzzleFlash_ = findAnimation("K_MUZZF");
}

Player::FrameCounts KurtSprite::frameCounts() const {
    Player::FrameCounts counts;
    for (size_t i = 0; i < stateSpecs_.size() && i < (size_t)Player::kStateCount; i++) {
        const int32_t index = stateIndices_[i];
        if (index >= 0) {
            counts.values[i] = (int32_t)animations_[(size_t)index].frames.size();
        }
    }
    return counts;
}

bool KurtSprite::pick(const Player& player, Pick* out) const {
    if (!out) {
        return false;
    }
    out->animation = -1;
    out->frame = 0;

    // The chute's closing frame is shown once when it closes in the air.
    if (player.chuteClosing()) {
        const int32_t index = findAnimation(kChuteOpening);
        if (index < 0 || (int32_t)animations_[(size_t)index].frames.size() <= kChuteClosedFrame) {
            return false;
        }
        out->animation = index;
        out->frame = kChuteClosedFrame;
        return true;
    }

    const size_t state = (size_t)player.state();
    if (state >= stateIndices_.size()) {
        return false;
    }
    const int32_t index = stateIndices_[state];
    if (index < 0) {
        return false;
    }
    const Animation& animation = animations_[(size_t)index];
    const int32_t count = (int32_t)animation.frames.size();
    if (count <= 0) {
        return false;
    }

    int32_t frame = (int32_t)floorf(player.animationFrame());
    if (player.state() == Kurt::Chute && frame >= kChuteOpenedFrame && chuteSway_ >= 0) {
        const int32_t swayCount = (int32_t)animations_[(size_t)chuteSway_].frames.size();
        // `last` is the far end of the sway and the modulo's half-period. A
        // one-frame (or empty) K_CHUTEC would make it zero, and the C# would
        // throw a DivideByZeroException there; this falls through to the
        // ordinary frame instead, which is the same picture as frame 0.
        if (swayCount > 1) {
            const int32_t last = swayCount - 1;
            const int32_t k = (frame - kChuteOpenedFrame) % (last * 2);
            out->animation = chuteSway_;
            out->frame = k <= last ? k : last * 2 - k;
            return true;
        }
    }

    if (animation.repeat == Loop) {
        frame = Player::wrapAnimationFrame(frame, count);
    } else {
        frame = clampf((float)frame, 0.0f, (float)(count - 1));
    }
    if (player.state() == Kurt::HelmetOff) {
        frame = count - 1 - frame;
    }
    out->animation = index;
    out->frame = frame;
    return true;
}

bool KurtSprite::draw(MdkRenderer renderer, const Player& player, const Vec3& feet, const Vec3& eye,
                      const Vec3& forward, const Vec3& up, float fieldOfView, ArenaSpace* space) {
    Pick selected;
    if (!pick(player, &selected)) {
        return false;
    }
    if (selected.animation < 0 || selected.animation >= (int32_t)animations_.size()) {
        return false;
    }
    const Animation& animation = animations_[(size_t)selected.animation];
    if (selected.frame < 0 || selected.frame >= (int32_t)animation.frames.size()) {
        return false;
    }

    // World units per sprite pixel at the feet's depth.
    const float depth = dot(subtract(feet, eye), forward);
    if (depth <= 0.0f) {
        return false;
    }

    // R6: the tangent comes from the shared trig table, as the camera's
    // projection does. The C# calls MathF.Tan here; the table is this port's
    // hardware-safe equivalent on both targets.
    const float radians = fieldOfView * kPi / 180.0f;
    const float halfFov = radians * 0.5f;
    const float tangent = fastSin(halfFov) / fastCos(halfFov);
    const float pixel = 2.0f * depth * tangent / kViewHeight;
    if (pixel <= 0.0f) {
        return false;
    }

    const Vec3 right = normalize(cross(forward, up));
    // K_ROLLL and K_ROLLR are the same file, a roll to the right: mirrored
    // for the left. A left strafe mirrors K_SIDE too.
    const float flip = (player.state() == Kurt::Side && player.strafeSpeed < 0.0f) ||
                               player.state() == Kurt::RollLeft
                           ? -1.0f
                           : 1.0f;
    Quad at;
    at.feet = feet;
    at.right = scale(right, pixel * flip);
    at.up = scale(up, pixel);

    const Frame& frame = animation.frames[(size_t)selected.frame];
    const float rise = kFeetRise / pixel;
    float floor = 0.0f;
    const bool hasFloor = floorDepth(space, eye, forward, at, frame, rise, &floor);
    Pull pull;
    pull.body = inFront(depth, hasFloor, floor);
    pull.feet = nearer(depth, hasFloor, floor);
    pull.rise = rise;
    // THE FRAME'S TEXTURE IS MADE HERE, the first time the frame is drawn.
    // The C# uploads every frame of every animation at level load
    // (KurtSprite.Preload) and keeps them; the 3DS cannot afford that, so
    // the upload waits for the frame that is actually wanted.
    if (!ensureTexture(selected.animation, selected.frame)) {
        return false;
    }
    return drawFrame(renderer, selected.animation, selected.frame, eye, at, 0, 0, pull);
}

bool KurtSprite::ensureTexture(int32_t animationIndex, int32_t frameIndex) {
    if (animationIndex < 0 || animationIndex >= (int32_t)animations_.size()) {
        return false;
    }
    Animation& animation = animations_[(size_t)animationIndex];
    if (frameIndex < 0 || frameIndex >= (int32_t)animation.frames.size()) {
        return false;
    }
    Frame& frame = animation.frames[(size_t)frameIndex];
    const uint32_t now = residency_.tick();
    if (frame.texture < 0) {
        // THE C#'s Texture(name, frame, image): the frame's texture, made
        // the first time the frame is drawn.
        const int32_t texture = mdk_render_create_index_texture(
            renderer_, frame.width, frame.height, frame.indices.data(), paletteRgba_.data());
        if (texture < 0) {
            return false;
        }
        frame.texture = texture;
        // A mod's image for this frame (the C#'s KurtSprite.Upscaled: the
        // enhanced look's sprites only, named ANIMATION_frame). Given to
        // the renderer as the frame's colour texture; the index texture
        // stays for the original look.
        if (mods_ && shading_ == Shading::Sprite) {
            formats::Texture image;
            image.setName(animation.name);
            image.setWidth(frame.width);
            image.setHeight(frame.height);
            image.setFrameCount(1);
            image.setIndices(frame.indices.data(), (int32_t)frame.indices.size());
            formats::Palette palette;
            memcpy(palette.mutableRgba(), paletteRgba_.data(), 256 * 4);
            HdImage replacement;
            const std::string name =
                animation.name + "_" + std::to_string(frameIndex);
            if (mods_->texture(name, image, palette, &replacement)) {
                mdk_render_replace_colour(renderer_, texture, palette_, 1, replacement.width,
                                          replacement.height, replacement.rgba.data());
            }
        }
        SpriteResidency::Entry entry;
        entry.animation = animationIndex;
        entry.frame = frameIndex;
        // WHAT IT COSTS. The backend's own arithmetic: the 3DS expands the
        // indices through the palette into RGBA8 padded to powers of two,
        // and the desktop keeps the indices as they are.
        entry.bytes = mdk_render_index_texture_bytes(renderer_, frame.width, frame.height);
        if (entry.bytes <= 0) {
            entry.bytes = frame.width * frame.height;
        }
        entry.lastUsed = now;
        residency_.add(entry);
    } else {
        residency_.touch(animationIndex, frameIndex, now);
    }
    trimResidency(animationIndex, frameIndex);
    return true;
}

void KurtSprite::trimResidency(int32_t keepAnimation, int32_t keepFrame) {
    while (residency_.overBudget()) {
        SpriteResidency::Entry victim;
        if (!residency_.victim(keepAnimation, keepFrame, &victim)) {
            // Nothing else may go: the frame being drawn is the only one
            // left, and dropping it would drop the picture.
            return;
        }
        releaseFrame(victim.animation, victim.frame);
    }
}

void KurtSprite::releaseFrame(int32_t animationIndex, int32_t frameIndex) {
    // The accounting and the GPU copy are separate facts. A frame can hold a
    // texture even when its residency entry has already gone (for example
    // after an animation is replaced), and the copy must be released either
    // way or it becomes unreachable.
    residency_.remove(animationIndex, frameIndex);
    if (animationIndex < 0 || animationIndex >= (int32_t)animations_.size()) {
        return;
    }
    Animation& animation = animations_[(size_t)animationIndex];
    if (frameIndex < 0 || frameIndex >= (int32_t)animation.frames.size()) {
        return;
    }
    Frame& frame = animation.frames[(size_t)frameIndex];
    if (frame.texture >= 0) {
        mdk_render_release_texture(renderer_, frame.texture);
        frame.texture = -1;
    }
}

void KurtSprite::releaseAnimation(int32_t animationIndex) {
    if (animationIndex < 0 || animationIndex >= (int32_t)animations_.size()) {
        return;
    }
    const Animation& animation = animations_[(size_t)animationIndex];
    for (size_t i = 0; i < animation.frames.size(); i++) {
        releaseFrame(animationIndex, (int32_t)i);
    }
}

bool KurtSprite::drawFrame(MdkRenderer renderer, int32_t animationIndex, int32_t frameIndex,
                           const Vec3& eye, const Quad& at, int32_t dx, int32_t dy, const Pull& pull) {
    if (animationIndex < 0 || animationIndex >= (int32_t)animations_.size()) {
        return false;
    }
    const Animation& animation = animations_[(size_t)animationIndex];
    if (frameIndex < 0 || frameIndex >= (int32_t)animation.frames.size()) {
        return false;
    }
    const Frame& frame = animation.frames[(size_t)frameIndex];
    const float ax = (float)(frame.hotspotX - dx);
    const float ay = (float)(frame.hotspotY + kFeetOffset - dy);
    const float split = clampf(ay - pull.rise, 0.0f, (float)frame.height);

    Vertex vertices[kFrameVertices];
    int32_t count = rows(vertices, at, ax, ay, frame.width, frame.height, 0.0f, split, eye,
                         pull.body);
    count += rows(vertices + count, at, ax, ay, frame.width, frame.height, split,
                  (float)frame.height, eye, pull.feet);
    if (count <= 0) {
        return false;
    }

    mdk_render_update_mesh(renderer, mesh_, vertices, count);
    MeshDraw draw;
    draw.mesh = mesh_;
    draw.first_vertex = 0;
    draw.vertex_count = count;
    draw.material.texture = frame.texture;
    draw.material.palette = palette_;
    draw.material.frame_count = 1;
    draw.material.pass = Pass::DoubleSided;
    draw.material.shading = shading_;
    draw.material.colour = Colour{1.0f, 1.0f, 1.0f, 1.0f};
    draw.world[0] = 1.0f;
    draw.world[5] = 1.0f;
    draw.world[10] = 1.0f;
    draw.world[15] = 1.0f;
    mdk_render_draw_mesh(renderer, &draw);
    return true;
}

bool KurtSprite::floorDepth(ArenaSpace* space, const Vec3& eye, const Vec3& forward, const Quad& at,
                            const Frame& frame, float rise, float* outDepth) {
    if (!space || !outDepth) {
        return false;
    }

    const float ax = (float)frame.hotspotX;
    const float ay = (float)(frame.hotspotY + kFeetOffset);
    const float columns[3] = {0.0f, ax, (float)frame.width};
    const float rows[3] = {0.0f, clampf(ay - rise, 0.0f, (float)frame.height), (float)frame.height};

    bool have = false;
    float nearest = 0.0f;
    for (int32_t i = 0; i < 9; i++) {
        const Vec3 point = corner(at, ax, ay, columns[i % 3], rows[i / 3]);
        ArenaSpace::Floor floor;
        if (!space->floorCrossing(toVec(eye), toVec(point), &floor)) {
            continue;
        }
        if (dot(subtract(at.feet, fromVec(floor.point)), fromVec(floor.normal)) < -kFloorBelow) {
            continue;
        }
        const float floorAt = dot(subtract(fromVec(floor.point), eye), forward);
        if (!have || floorAt < nearest) {
            nearest = floorAt;
            have = true;
        }
    }
    if (have) {
        *outDepth = nearest;
    }
    return have;
}

} // namespace mdk

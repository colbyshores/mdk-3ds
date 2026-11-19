#pragma once

// ============================================================================
// kurt_sprite.h — Kurt's world-space sprite, as the C# draws it.
//
// This is the rendering half of src/Mdk.Game/Kurt/KurtSprite.cs. A frame is
// a fixed-size world-space quad at Kurt's feet: one sprite pixel is 1/360 of
// the view's height, the hotspot is 101 pixels above the feet, and the rows
// around the feet are brought nearer than the body so the sprite wins over
// the floor it stands on.
//
// The frames come from TRAVERSE/TRAVSPRT.BNI and the level's SNI archives.
// They are decoded at level load and uploaded when first drawn, subject to the
// backend's residency bound; the per-frame path only rewrites twelve vertices
// and draws the selected frame.
//
// The ride pose, the beta-only roll/helmet frames and the muzzle flash belong
// to subsystems that are not ported yet, and neither are Kurt.Visible nor his
// sniper mode, so the C#'s branches on all of them are taken the way they are
// false. The ordinary movement, jump, fall, chute, landing and death states ARE
// implemented here; see Player's state machine. The missing consumers are
// recorded in docs/OPEN_ITEMS.md. The frontend draws him at Kurt's own feet:
// the C# draws at FrameBlend.Feet, which is Kurt's feet exactly when blending
// is off (a step is drawn as it is), and sub-step blending is not ported.
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

#include "game/player.h"
#include "render/render.h"
#include "render/render_types.h"

namespace mdk {

class ModImages;

namespace formats {
class Bni;
class Palette;
class SpriteAnimation;
} // namespace formats

class ArenaSpace;

// The bound on the sprite frames kept on the GPU, and the choice of which
// one goes when the bound is exceeded.
//
// WHY THIS EXISTS. The C# uploads every frame of every sprite animation at
// level load (KurtSprite.Preload) and keeps them for ever: its texture
// cache is keyed by (name, frame) and nothing ever evicts from it. The
// desktop can afford that. The 3DS cannot: the same set is 49.9 MiB of
// palette-expanded RGBA8 padded to powers of two (measured over
// TRAVSPRT.BNI: 19 animations, 360 frames) against a machine whose whole
// texture heap is megabytes. So the port uploads a frame when it is first
// drawn and keeps only as many as the backend says it can hold.
//
// IT KNOWS NOTHING ABOUT THE RENDERER. The numbers come in from the caller,
// so the policy can be pinned by a test with no GPU at all
// (tests/scripts/sprite_tests.cpp).
class SpriteResidency {
public:
    // One resident frame, named by where it lives.
    struct Entry {
        int32_t animation = -1;
        int32_t frame = -1;
        int32_t bytes = 0;      // the GPU memory it costs while resident
        uint32_t lastUsed = 0;  // the draw counter when it was last drawn
        // PINNED: made at level load and kept for the level (Kurt's start set,
        // KurtSprite::preloadStartSet). Counted in residentBytes, but outside
        // the budget's rotation: never a victim, and not part of overBudget.
        bool pinned = false;
    };

    // The bound, in GPU bytes. 0 means "no bound": nothing is ever dropped.
    void setBudget(int32_t bytes) { budget_ = bytes > 0 ? bytes : 0; }
    int32_t budget() const { return budget_; }
    bool bounded() const { return budget_ > 0; }
    int32_t residentBytes() const { return residentBytes_; }
    int32_t pinnedBytes() const { return pinnedBytes_; }
    int32_t residentCount() const { return (int32_t)resident_.size(); }
    // The ROTATING frames over the budget: a pinned frame is held outside it.
    bool overBudget() const { return budget_ > 0 && residentBytes_ - pinnedBytes_ > budget_; }

    // A frame became resident.
    void add(const Entry& entry) {
        residentBytes_ += entry.bytes;
        if (entry.pinned) {
            pinnedBytes_ += entry.bytes;
        }
        resident_.push_back(entry);
    }

    // A frame stopped being resident. False when it was not resident.
    bool remove(int32_t animation, int32_t frame) {
        for (size_t i = 0; i < resident_.size(); i++) {
            if (resident_[i].animation != animation || resident_[i].frame != frame) {
                continue;
            }
            residentBytes_ -= resident_[i].bytes;
            if (resident_[i].pinned) {
                pinnedBytes_ -= resident_[i].bytes;
            }
            resident_[i] = resident_.back();
            resident_.pop_back();
            return true;
        }
        return false;
    }

    // The whole set stopped being resident: a new level's `load()` starts
    // from an empty cache, even if a previous load had already drawn frames.
    void clear() {
        residentBytes_ = 0;
        pinnedBytes_ = 0;
        resident_.clear();
    }

    // Every frame of an animation stopped being resident. The swap-and-pop
    // re-examines the entry moved into the hole, so the result does not
    // depend on the order the entries were added.
    void removeAnimation(int32_t animation) {
        size_t i = 0;
        while (i < resident_.size()) {
            if (resident_[i].animation == animation) {
                residentBytes_ -= resident_[i].bytes;
                if (resident_[i].pinned) {
                    pinnedBytes_ -= resident_[i].bytes;
                }
                resident_[i] = resident_.back();
                resident_.pop_back();
                continue;
            }
            i++;
        }
    }

    // A frame was drawn now.
    void touch(int32_t animation, int32_t frame, uint32_t now) {
        for (size_t i = 0; i < resident_.size(); i++) {
            if (resident_[i].animation == animation && resident_[i].frame == frame) {
                resident_[i].lastUsed = now;
                return;
            }
        }
    }

    // The resident frame that has gone unused the longest. The frame being
    // drawn (`keep`) is never the victim: dropping it would drop the very
    // picture that is about to be drawn. False when there is no candidate.
    bool victim(int32_t keepAnimation, int32_t keepFrame, Entry* out) const {
        const Entry* oldest = nullptr;
        for (size_t i = 0; i < resident_.size(); i++) {
            const Entry& entry = resident_[i];
            if (entry.pinned ||
                (entry.animation == keepAnimation && entry.frame == keepFrame)) {
                continue;
            }
            if (!oldest || entry.lastUsed < oldest->lastUsed) {
                oldest = &entry;
            }
        }
        if (!oldest) {
            return false;
        }
        if (out) {
            *out = *oldest;
        }
        return true;
    }

    // The draw counter. It only ever goes up, and it wraps after four
    // billion draws, which no session reaches.
    uint32_t tick() { return ++clock_; }

private:
    int32_t budget_ = 0;
    int32_t residentBytes_ = 0;
    int32_t pinnedBytes_ = 0;
    uint32_t clock_ = 0;
    std::vector<Entry> resident_;
};

class KurtSprite {
public:
    KurtSprite();

    enum Repeat { Once, Loop };

    // One frame and the anchor the C#'s SpriteAnimation.Frame has.
    //
    // THE TEXTURE IS MADE WHEN THE FRAME IS FIRST DRAWN, not when the
    // animation is loaded: see SpriteResidency above. `texture` is -1 while
    // the frame is not resident. (The one exception is the start set, made at
    // level load from compressed copies: preloadStartSet.)
    struct Frame {
        int32_t texture = -1;
        int32_t width = 0;
        int32_t height = 0;
        int32_t hotspotX = 0;
        int32_t hotspotY = 0;
        // The decoded palette indices. The C# keeps them in its
        // SpriteAnimation's frame cache; the port copies them here, because
        // a frame whose texture has been dropped has to be uploaded again
        // and the archive it came from is gone by then.
        std::vector<uint8_t> indices;
        // The frame's COMPRESSED copy, a .t3x file's bytes (useCompressedFrames;
        // EnhancedToggles::kurtCompressed), or empty. Read at level load -- a
        // frame's file is 1-6 KB, the whole set 1.2 MB -- so making the texture
        // on first draw is an import from memory and never an SD read.
        std::vector<uint8_t> compressed;
    };

    // An animation's decoded frames. `repeat` is the C#'s Repeat enum.
    struct Animation {
        std::string name;
        Repeat repeat = Once;
        std::vector<Frame> frames;
    };

    // The animation and frame a player's state selects. The animation is an
    // index into the loaded set.
    struct Pick {
        int32_t animation = -1;
        int32_t frame = 0;
    };

    // Loads the ordinary animations from TRAVSPRT.BNI and decodes their
    // frames. The archive is borrowed only while this call runs: the decoded
    // frames are copied, and the GPU copies are made when a frame is first
    // drawn. Returns false when the renderer or the still frame could not be
    // loaded.
    bool load(MdkRenderer renderer, const formats::Bni& sprites,
              const formats::Palette& palette);

    // The look's shading for Kurt's draws (the C#'s KurtSprite shading:
    // Sprite under the enhanced look, Original otherwise). The frontend
    // sets it before load; Original is the shipped default.
    void setShading(Shading shading) { shading_ = shading; }

    // The mods whose images replace Kurt's frames under the Sprite
    // shading (the C#'s KurtSprite mods; null: none). Set before load.
    void setMods(ModImages* mods) { mods_ = mods; }

    // KURT'S COMPRESSED FRAMES (EnhancedToggles::kurtCompressed): the set made
    // offline from the player's TRAVSPRT.BNI (mdk_3ds/tools/kurt_etc1_convert.py),
    // indexed like the HD pack (ModImages::fromList; files by name, a level's
    // own folder first). Every frame that has a file gets its bytes read now --
    // level load, after load() -- and from then on its texture is the compressed
    // one; a frame without one keeps the index texture. Null: none (the toggle
    // off: every frame the index texture, in the backend's texture16 store).
    struct CompressedReport {
        int32_t frames = 0;        // frames loaded
        int32_t compressed = 0;    // of them, with a compressed copy
        int32_t fileBytes = 0;     // the copies' bytes, held in memory
        int32_t startFrames = 0;   // the start set's frames (preloadStartSet)
        int32_t preloaded = 0;     // of them, made now and pinned
        int32_t preloadBytes = 0;  // their GPU bytes
        int32_t preloadBudget = 0; // the bound they were made under
    };
    void useCompressedFrames(ModImages* set, CompressedReport* report);

    // THE START SET (K_STILL, K_FALL, K_LAND, K_IDLE: what every level opens on)
    // made NOW, at level load, from the compressed copies, and pinned for the
    // level -- while it fits the backend's preload bound
    // (MdkCapabilities::sprite_preload_bytes; a frame past the bound is made on
    // first draw as before). Frames without a compressed copy are not preloaded:
    // their making is the ARM11 expansion the preload exists to avoid, and the
    // set would not fit in that store anyway. After useCompressedFrames.
    void preloadStartSet(CompressedReport* report);

    // The names of the start set, in preload order.
    static int startSetCount();
    static const char* startSetName(int index);

    // Adds one level animation (K_SURF, K_SLIDE, ...). A name already loaded
    // from the BNI is replaced, as the C#'s level animations take precedence.
    bool add(MdkRenderer renderer, const std::string& name,
             const formats::SpriteAnimation& animation);

    // The C# KurtSprite.LevelAnimations: the level's SNI archives may carry
    // the snowboard's and slide's frames.
    static int levelAnimationCount();
    static const char* levelAnimationName(int index);

    // The number of frames in every state animation, for Player's state
    // machine. A missing animation has zero frames.
    Player::FrameCounts frameCounts() const;

    // The frame the player's state selects, or false when that animation is
    // not present. The animation is an index into the loaded set, which the
    // next add() may grow.
    bool pick(const Player& player, Pick* out) const;

    // Draws Kurt at `feet` for a camera at `eye`. `forward`, `up` and the
    // field of view are the camera's, and `space` is the collision world used
    // to find the floor in front of the sprite.
    bool draw(MdkRenderer renderer, const Player& player, const Vec3& feet, const Vec3& eye,
              const Vec3& forward, const Vec3& up, float fieldOfView, ArenaSpace* space);

    // The C# KurtSprite's pull constants.
    static constexpr float kFloorMargin = 0.02f;
    static constexpr float kMinFloorShare = 0.1f;
    static constexpr float kShadowPull = 2.0f;
    static constexpr float kMinDepthShare = 0.5f;

    // The scale towards the eye that brings a frame at `depth` in front of a
    // floor at `floorDepth` (1 when there is no floor).
    static float inFront(float depth, bool hasFloor, float floorDepth) {
        if (!hasFloor) {
            return 1.0f;
        }
        const float scale = floorDepth / depth * (1.0f - kFloorMargin);
        return scale < kMinFloorShare ? kMinFloorShare : (scale > 1.0f ? 1.0f : scale);
    }

    // The scale that brings the rows around the feet two world units nearer,
    // and still in front of the floor.
    static float nearer(float depth, bool hasFloor, float floorDepth) {
        const float pull = (depth - kShadowPull) / depth;
        const float atLeastHalf = pull < kMinDepthShare ? kMinDepthShare : pull;
        const float front = inFront(depth, hasFloor, floorDepth);
        return front < atLeastHalf ? front : atLeastHalf;
    }

private:
    struct StateSpec {
        const char* name;
        Repeat repeat;
    };

    // Where the frame goes: the feet, and the world vectors of one sprite
    // pixel right and up.
    struct Quad {
        Vec3 feet;
        Vec3 right;
        Vec3 up;
    };

    // A frame scaled towards the eye: the body and the feet have different
    // scales, and the split is this many pixels above the feet.
    struct Pull {
        float body = 1.0f;
        float feet = 1.0f;
        float rise = 0.0f;
    };

    bool loadAnimation(MdkRenderer renderer, const std::string& name,
                       const formats::SpriteAnimation& animation);
    bool loadAnimation(MdkRenderer renderer, const std::string& name,
                       const formats::SpriteAnimation& animation, Animation* out);
    int32_t findAnimation(const std::string& name) const;
    void resolveAnimations();

    // The C#'s Texture(name, frame, image): the frame's GPU texture, made
    // the first time the frame is drawn and kept until the residency budget
    // says it has to go. False when the texture could not be made.
    bool ensureTexture(int32_t animation, int32_t frame);

    // Makes a frame's GPU copy: the compressed copy when it has one and the
    // backend takes it, else the index texture; the HD pack's link either way.
    // The texture id, or -1.
    int32_t makeFrameTexture(int32_t animation, int32_t frame);

    // What a made frame costs resident: the backend's count of the texture it
    // actually made (its real store, a compressed copy's ETC bytes), else the
    // index-texture estimate, else the indices.
    int32_t frameBytes(const Frame& frame) const;

    // Drops the frames that have gone unused the longest until the resident
    // set is inside the budget. The frame being drawn is never dropped.
    void trimResidency(int32_t keepAnimation, int32_t keepFrame);

    // Drops one frame's texture, and forgets that it was resident.
    void releaseFrame(int32_t animation, int32_t frame);

    // Drops every frame of an animation's textures. Used when an animation
    // is replaced, so its GPU copies do not leak.
    void releaseAnimation(int32_t animation);

    bool drawFrame(MdkRenderer renderer, int32_t animation, int32_t frame, const Vec3& eye,
                   const Quad& at, int32_t dx, int32_t dy, const Pull& pull);
    bool floorDepth(ArenaSpace* space, const Vec3& eye, const Vec3& forward, const Quad& at,
                    const Frame& frame, float rise, float* outDepth);
    static Vec3 corner(const Quad& at, float ax, float ay, float px, float py);
    static int32_t rows(Vertex* into, const Quad& at, float ax, float ay, int32_t width,
                        int32_t height, float top, float bottom, const Vec3& eye, float pull);

    MdkRenderer renderer_ = nullptr;
    Shading shading_ = Shading::Original;
    ModImages* mods_ = nullptr;
    int32_t palette_ = -1;
    int32_t mesh_ = -1;
    std::vector<uint8_t> paletteRgba_;
    std::vector<Animation> animations_;
    std::vector<StateSpec> stateSpecs_;
    std::vector<int32_t> stateIndices_;
    int32_t chuteSway_ = -1;
    int32_t backUp_ = -1;
    int32_t muzzleFlash_ = -1;
    // What is on the GPU, and how much of it there is. See SpriteResidency.
    SpriteResidency residency_;
};

} // namespace mdk

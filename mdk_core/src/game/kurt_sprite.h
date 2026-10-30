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
// They are decoded and uploaded once at level load; the per-frame path only
// rewrites twelve vertices and draws the selected frame.
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

namespace formats {
class Bni;
class Palette;
class SpriteAnimation;
} // namespace formats

class ArenaSpace;

class KurtSprite {
public:
    KurtSprite();

    enum Repeat { Once, Loop };

    // One uploaded frame and the anchor the C#'s SpriteAnimation.Frame has.
    struct Frame {
        int32_t texture = -1;
        int32_t width = 0;
        int32_t height = 0;
        int32_t hotspotX = 0;
        int32_t hotspotY = 0;
    };

    // An animation's decoded frames. `repeat` is the C#'s Repeat enum.
    struct Animation {
        std::string name;
        Repeat repeat = Once;
        std::vector<Frame> frames;
    };

    // The animation and frame a player's state selects.
    struct Pick {
        const Animation* animation = nullptr;
        int32_t frame = 0;
    };

    // Loads the ordinary animations from TRAVSPRT.BNI and uploads their
    // frames. The archive is borrowed only while this call runs: the decoded
    // frames are copied and the GPU keeps its own copy. Returns false when
    // the renderer or the still frame could not be loaded.
    bool load(MdkRenderer renderer, const formats::Bni& sprites,
              const formats::Palette& palette);

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
    // not present. The animation points into the loaded set: it is valid until
    // the next add(), which may grow that set, so draw with it before loading
    // another animation.
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
    bool drawFrame(MdkRenderer renderer, const Animation& animation, int32_t frame,
                   const Vec3& eye, const Quad& at, int32_t dx, int32_t dy, const Pull& pull);
    bool floorDepth(ArenaSpace* space, const Vec3& eye, const Vec3& forward, const Quad& at,
                    const Frame& frame, float rise, float* outDepth);
    static Vec3 corner(const Quad& at, float ax, float ay, float px, float py);
    static int32_t rows(Vertex* into, const Quad& at, float ax, float ay, int32_t width,
                        int32_t height, float top, float bottom, const Vec3& eye, float pull);

    MdkRenderer renderer_ = nullptr;
    int32_t palette_ = -1;
    int32_t mesh_ = -1;
    std::vector<uint8_t> paletteRgba_;
    std::vector<Animation> animations_;
    std::vector<StateSpec> stateSpecs_;
    std::vector<int32_t> stateIndices_;
    int32_t chuteSway_ = -1;
    int32_t backUp_ = -1;
    int32_t muzzleFlash_ = -1;
};

} // namespace mdk

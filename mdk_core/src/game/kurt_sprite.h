#pragma once

// ============================================================================
// kurt_sprite.h — Kurt's world-space sprite, as the C# draws it.
//
// This is the rendering half of src/Mdk.Game/Kurt/KurtSprite.cs. A frame is
// a fixed-size world-space quad at Kurt's feet: one sprite pixel is 1/360 of
// the view's height, the hotspot is 101 pixels above the feet, and the rows
// around the feet are brought 2 units nearer than the body (the shadow painted
// in his frames, over a platform he rides).
//
// WHAT MAY HIDE HIM IS NOT UPSTREAM'S (docs/DIVERGENCES.md D8, owner 2026-11-27):
// upstream's C++ (src/game/kurt/KurtSprite.cpp FloorDepth) pulls the sprite in
// front of floors at most half a unit above his feet, so walls he walked along
// cut him. This port marks
// the surfaces of his own space in the stencil and draws him over them
// (game/kurt_cover.h; render_types.h KurtPass), as the original's BSP order does.
//
// The frames come from TRAVERSE/TRAVSPRT.BNI and the level's SNI archives.
// They are decoded at level load and uploaded when first drawn, subject to the
// backend's residency bound; the per-frame path only rewrites twelve vertices
// and draws the selected frame.
//
// The ride pose and the beta-only roll/helmet frames belong to subsystems that
// are not ported yet, and neither are Kurt.Visible nor his sniper mode, so the
// C#'s branches on all of them are taken the way they are false. The chain
// gun's muzzle flash (K_MUZZF, Kurt::muzzle()) is drawn behind him as the C#
// draws it. The ordinary movement, jump, fall, chute, landing and death states ARE
// implemented here; see Player's state machine. The missing consumers are
// recorded in docs/OPEN_ITEMS.md. The frontend draws him at Kurt's own feet:
// the C# draws at FrameBlend.Feet, which is Kurt's feet exactly when blending
// is off (a step is drawn as it is), and sub-step blending is not ported.
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

#include "game/kurt_cover.h"
#include "game/player.h"
#include "render/render.h"
#include "render/render_types.h"
#include "util/matrix.h"

namespace mdk {

class ModImages;

namespace formats {
class Bni;
class KurtPack;
class Palette;
class SpriteAnimation;
} // namespace formats

class ArenaSpace;
namespace formats {
struct Box;
} // namespace formats

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

// A buffer the backend's reader malloc'd (mdk_render_poll_range_read), owned:
// freed when it goes or is replaced. Move-only, so an Animation that holds one
// can be moved (the animation list grows and is replaced) but never copied into
// a second owner.
class StagedBytes {
public:
    StagedBytes() = default;
    ~StagedBytes() { reset(); }
    StagedBytes(const StagedBytes&) = delete;
    StagedBytes& operator=(const StagedBytes&) = delete;
    StagedBytes(StagedBytes&& other) noexcept : data_(other.data_), size_(other.size_) {
        other.data_ = nullptr;
        other.size_ = 0;
    }
    StagedBytes& operator=(StagedBytes&& other) noexcept {
        if (this != &other) {
            reset();
            data_ = other.data_;
            size_ = other.size_;
            other.data_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }
    // Takes ownership of a malloc'd buffer (the old one is freed).
    void adopt(uint8_t* data, uint32_t size) {
        reset();
        data_ = data;
        size_ = data ? size : 0;
    }
    void reset();
    const uint8_t* data() const { return data_; }
    uint32_t size() const { return size_; }

private:
    uint8_t* data_ = nullptr;
    uint32_t size_ = 0;
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
    // level load from the pack's front: preloadStartSet.)
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
        // The frame's COMPRESSED copy in Kurt's pack (romfs:/kurt/kurt.pak;
        // useCompressedPack, EnhancedToggles::kurtCompressed): where its .t3x
        // bytes are, packSize 0 for none. The bytes are NOT held per frame (that
        // was 1.1 MB of main heap for the level): the start set's are imported
        // at level load from one read, the others' come with their animation's
        // stream (Animation::staged).
        uint32_t packOffset = 0;
        uint32_t packSize = 0;
        // The size of the picture INSIDE the compressed copy's texture (the pack
        // entry's image size): width x height, unless the converter fitted a
        // frame past 256x256 (the chute's) into 256x256. The frame is still DRAWN
        // at width x height -- the quad is the frame's, its uv 0..1 maps onto the
        // image's rectangle (pica::compressedFrameRect) -- so a fitted frame is
        // exactly as large on screen as an unfitted one.
        int32_t packWidth = 0;
        int32_t packHeight = 0;
    };

    // Where an animation's pack bytes are (THE STREAM, useCompressedPack).
    enum class StreamState : uint8_t {
        None,      // nothing to stream: no frame of it has a pack copy
        Idle,      // in the pack, not staged: the next draw that needs it asks
                   // (and HOLDS Kurt's previous frame until the bytes land)
        Pending,   // a read is in flight (the backend's reader thread)
        Ready,     // staged: `staged` holds [packBegin, packEnd)
        Failed,    // the read failed: logged once, its frames are 16-bit from then on
    };

    // An animation's decoded frames. `repeat` is the C#'s Repeat enum.
    struct Animation {
        std::string name;
        Repeat repeat = Once;
        std::vector<Frame> frames;
        // THE STREAM: this level's frames' bytes in the pack, one contiguous
        // range (kurt_pack.py lays an animation's frames out together), read in
        // one request when a frame of it is first wanted without its bytes, and
        // held in the bounded staging cache (kStagedCacheBytes) while recent.
        uint32_t packBegin = 0;
        uint32_t packEnd = 0;
        StreamState stream = StreamState::None;
        StagedBytes staged;
        uint32_t lastDrawn = 0;   // the residency clock when a frame was last drawn
        bool prefetched = false;  // asked for ahead of a draw once this level
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
    // offline from the player's TRAVSPRT.BNI (mdk_3ds/tools/kurt_etc1_convert.py)
    // in ONE file (kurt_pack.py, formats/kurt_pack.h), whose index the caller
    // has read. Each frame is matched to its entry by name for `level` (a
    // level's own copy first) and must agree on its size; a frame with no
    // entry, an invalid one or a size that disagrees keeps the index texture
    // (the 16-bit path). NO BYTES ARE READ HERE: the start set's come from the
    // caller's one read (preloadStartSet), the others' from `path` through the
    // backend's reader (the stream), so after level load this holds no .t3x
    // bytes at all. Null pack: none (every frame the index texture).
    struct CompressedReport {
        int32_t frames = 0;         // frames loaded
        int32_t compressed = 0;     // of them, with a valid pack entry
        int32_t badFrames = 0;      // of them, whose entry was invalid or the wrong size
        int32_t streamable = 0;     // animations with a pack range (the stream's unit)
        int32_t startFrames = 0;    // the start set's frames (preloadStartSet)
        int32_t preloaded = 0;      // of them, made now and pinned
        int32_t preloadBytes = 0;   // their GPU bytes
        int32_t preloadBudget = 0;  // the bound they were made under
        int32_t startFileBytes = 0; // the .t3x bytes those imports were made from
    };
    void useCompressedPack(const formats::KurtPack* pack, int32_t level, const char* path,
                           CompressedReport* report);

    // THE START SET (K_STILL, K_FALL, K_LAND, K_IDLE: what every level opens on;
    // and the chute's K_CHUTE and K_CHUTEC, which level 7 opens on and any fall
    // can open: 20 frames, fitted to 256x256, 1,130,496 GPU bytes; and the
    // jump's K_JUMP and K_RJMP, 39 frames, 1,048,576 GPU bytes) made NOW, at
    // level load, and pinned for the level -- while it fits the backend's
    // preload bound (MdkCapabilities::sprite_preload_bytes; a frame past the
    // bound is streamed and made on first draw like the others). A pinned frame
    // is never dropped by the residency; the backend keeps each one's .t3x as
    // its re-make source, so even a backend eviction re-imports from memory and
    // never goes back to the stream.
    // `bytes` are the pack's bytes [begin, begin + size) -- the caller's one
    // read of the pack's front, which kurt_pack.py fills with the start set --
    // and are only borrowed: the caller frees them after this returns. Frames
    // without a compressed copy are not preloaded: their making is the ARM11
    // expansion the preload exists to avoid. After useCompressedPack.
    void preloadStartSet(const uint8_t* bytes, uint32_t begin, uint32_t size,
                         CompressedReport* report);

    // THE STREAM'S PUMP, once a frame before draw(): takes the pack read that
    // finished, if any, into the staging cache. No I/O happens here (the read
    // ran on the backend's reader thread); a failed or short read marks its
    // animation Failed -- never retried -- and is reported once
    // (takeStreamFailure). With no read in flight it then PREFETCHES: the next
    // animation of the state machine's order not yet staged, once a level, and
    // only while it fits the staging bound without dropping anything -- so the
    // first run, jump or shot after level load is usually made from memory
    // rather than held (chooseFrame) while its read is in flight.
    void pumpStream();
    // A LEVEL'S TEARDOWN (package 2): the pack read in flight, if any, is collected
    // and freed WITHOUT asking for another (pumpStream would prefetch). True once no
    // read of this sprite's is outstanding. The frontend calls it until true before
    // the sprite is destroyed: the next level's sprite starts at the same generation,
    // so a read left in the backend's slot would land on it as one of its own (the
    // same animation index, the previous level's bytes).
    bool settleStream();

    // The stream's first failure as one log line, once; null otherwise.
    const char* takeStreamFailure();

    // What the stream has done, for one line at exit (not per frame).
    struct StreamStats {
        int32_t reads = 0;           // animations read
        int32_t failures = 0;        // reads that failed (their frames stay 16-bit)
        int32_t bytesRead = 0;
        // THE HOLD (chooseFrame): draws that showed Kurt's previously drawn frame
        // because the wanted one's bytes were still being read -- never a
        // 16-bit stand-in made inside the frame -- and draws that had nothing
        // to hold (no frame drawn yet this level), so Kurt was not drawn.
        int32_t holds = 0;
        int32_t holdMisses = 0;
        // Frames made from their compressed copy for which the HD pack was NOT
        // asked (makeFrameTexture: its 128x128 Kurt is no better, and the ask
        // is an SD read and an import on the main thread).
        int32_t hdSkipped = 0;
        int32_t importFailures = 0;  // compressed bytes the backend refused
        int32_t dropped = 0;         // staged animations dropped to stay in the bound
        uint32_t staged = 0;         // bytes staged now
        uint32_t stagedPeak = 0;     // the most ever staged
    };
    const StreamStats& streamStats() const { return stream_; }

    // The staging cache's bound: the recently drawn animations' bytes, so a
    // frame the GPU residency dropped is made again from memory, not read again.
    // The largest streamed animation (K_IDLE's and K_BANG's are about 100 KB of
    // .t3x; the chute's, once the largest, are preloaded) fits with others.
    static constexpr uint32_t kStagedCacheBytes = 256 * 1024;
    // No animation's range is near this (see kStagedCacheBytes); a range past it
    // is not streamed (its frames stay 16-bit).
    static constexpr uint32_t kMaxStreamBytes = 1024 * 1024;

    // The names of the start set (the chute's included), in preload order.
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
    // field of view are the camera's. `cover` names the drawn arenas whose
    // surfaces of his own space he is drawn over (game/kurt_cover.h; none: he is
    // only depth-tested), and `eyeSpread` is half the stereo eyes' distance in
    // world units (0 in mono).
    bool draw(MdkRenderer renderer, const Player& player, const Vec3& feet, const Vec3& eye,
              const Vec3& forward, const Vec3& up, float fieldOfView,
              const KurtCover::Source* cover, int32_t coverCount, float eyeSpread);

    // The surfaces of his own space found for the last frame drawn.
    const KurtCover::Stats& coverStats() const { return cover_.stats(); }

    // The most triangles marked a frame, and the runs (depth layers) they fall in.
    static constexpr int32_t kMarkTriangles = 512;
    static constexpr int32_t kMarkRuns = 8;

    // The C# KurtSprite's pull constants (the floor's: the ridden board's,
    // boardPull; the sprite's own floor pull is gone, see the header).
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

    // THE RIDDEN BOARD IN FRONT OF THE FLOORS IT STANDS ABOVE (upstream a27ca2f4,
    // 6eb0963b; C# FloorPull.Of). The original draws an object after the floor of
    // its BSP leaf, so the board shows whole even where it dips into the slope
    // (uphill its feet lag a step under it, banked turns sink an edge); with a
    // depth buffer that part hid. As this sprite, it is scaled towards the eye:
    // the same on screen, nearer than the floors the eye's rays to 18 points of
    // its box (a 3 x 3 grid at its bottom and top) cross within the board's
    // length of the point (03ee9031: it sinks deeper than it is thick). `box` is the
    // board model's bounds, `transform` the board's as drawn; the eye looks along
    // `forward`. Returns `transform`, scaled about the eye when a floor is met.
    static Mat4 boardPull(const formats::Box& box, const Mat4& transform, const Vec3& eye,
                          const Vec3& forward, ArenaSpace* space);

    // The scale that brings the rows around the feet two world units nearer,
    // and still in front of the floor (the sprite passes no floor now).
    static float nearer(float depth, bool hasFloor, float floorDepth) {
        const float pull = (depth - kShadowPull) / depth;
        const float atLeastHalf = pull < kMinDepthShare ? kMinDepthShare : pull;
        const float front = inFront(depth, hasFloor, floorDepth);
        return front < atLeastHalf ? front : atLeastHalf;
    }

private:
    // The host tests' access to the stream's private steps
    // (tests/scripts/kurt_stream_tests.cpp): no game data, no Player.
    friend struct KurtSpriteTestAccess;

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
    // says it has to go. Ready: it has one now. Waiting: it has a compressed
    // copy whose bytes are not in memory yet -- their read is asked for, NO
    // texture is made (the caller holds the previous frame). Failed: none
    // could be made.
    enum class Ensure : uint8_t { Ready, Waiting, Failed };
    Ensure ensureTexture(int32_t animation, int32_t frame);

    // THE HOLD. The frame to draw for `wanted`: `wanted` itself when its
    // texture is Ready (it becomes the held frame), else -- its bytes are still
    // being read -- the frame drawn last, as it is (no texture is made: a
    // 16-bit stand-in of a 512x512 frame was the quarter-second hitch when the
    // chute opened). The 16-bit index texture is made only for a frame with no
    // compressed copy left (none in the pack, a failed read, a refused
    // import): its permanent store. False: nothing to draw (no frame drawn yet
    // this level, or the texture could not be made).
    bool chooseFrame(const Pick& wanted, Pick* drawn);

    // Makes a frame's GPU copy: from `t3x` (the frame's compressed bytes, an
    // image of packWidth x packHeight) when given and the backend takes them,
    // else the index texture; the HD pack's link either way. The texture id,
    // or -1.
    int32_t makeFrameTexture(int32_t animation, int32_t frame, const uint8_t* t3x);

    // THE STREAM. The frame's bytes when its animation is staged, else null.
    static const uint8_t* stagedFrame(const Animation& animation, const Frame& frame);
    // Asks for an Idle animation's range when no read is in flight.
    void requestStream(int32_t animation);
    // The pump's prefetch (see pumpStream).
    void prefetchNext();
    // An animation's staged bytes go (back to Idle).
    void dropStaged(int32_t animation);
    // Drops the least recently drawn staged animations, never `keep`, until the
    // cache is inside kStagedCacheBytes.
    void trimStaged(int32_t keep);
    // A read failed: the animation is Failed and its frames are plain 16-bit.
    void streamFailed(int32_t animation, const char* why);
    // Forgets every frame's pack copy and every animation's stream.
    void forgetPack();

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

    // `marked`: the frame is drawn twice, Tested and Over (render_types.h
    // KurtPass); otherwise Tested only.
    bool drawFrame(MdkRenderer renderer, int32_t mesh, int32_t animation, int32_t frame,
                   const Vec3& eye,
                   const Quad& at, int32_t dx, int32_t dy, const Pull& pull, bool marked);
    // Marks the surfaces of his own space for this frame; true when an opaque
    // one was (the Over draw is wanted).
    bool drawMarks(MdkRenderer renderer, const KurtCover::Source* cover, int32_t coverCount,
                   const KurtCover::View& view);
    static Vec3 corner(const Quad& at, float ax, float ay, float px, float py);
    static int32_t rows(Vertex* into, const Quad& at, float ax, float ay, int32_t width,
                        int32_t height, float top, float bottom, const Vec3& eye, float pull);

    MdkRenderer renderer_ = nullptr;
    Shading shading_ = Shading::Original;
    ModImages* mods_ = nullptr;
    int32_t palette_ = -1;
    int32_t mesh_ = -1;
    // C# KurtSprite._muzzleMesh: the muzzle flash's own quad, so the flash drawn behind
    // Kurt does not overwrite the vertices of the frame drawn just before it.
    int32_t muzzleMesh_ = -1;
    // The surfaces of his own space (game/kurt_cover.h): their search, and the
    // dynamic mesh their marks are drawn from.
    KurtCover cover_;
    int32_t markMesh_ = -1;
    // An 8x8 white texture for the marks: the 3DS draws nothing untextured (a
    // flat colour is a texture there), and the mark's colour is never written.
    int32_t markTexture_ = -1;
    std::vector<Vertex> markVertices_;
    std::vector<uint8_t> paletteRgba_;
    std::vector<Animation> animations_;
    std::vector<StateSpec> stateSpecs_;
    std::vector<int32_t> stateIndices_;
    int32_t chuteSway_ = -1;
    int32_t backUp_ = -1;
    int32_t muzzleFlash_ = -1;
    // What is on the GPU, and how much of it there is. See SpriteResidency.
    SpriteResidency residency_;
    // THE STREAM (useCompressedPack): the pack's path, the tag of the read in
    // flight (-1: none), the generation that tells a stale read from a current
    // one (a re-load bumps it), and the counters.
    std::string packPath_;
    // The frame drawn last (chooseFrame's hold); -1: none this level.
    Pick held_;
    int32_t streamTag_ = -1;
    int32_t generation_ = 0;
    StreamStats stream_;
    bool failureTaken_ = true;
    char failureNote_[160] = {0};
};

} // namespace mdk

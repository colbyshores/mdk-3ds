#pragma once

// ============================================================================
// stream_flight.h — the stream's play (C# src/Mdk.Game/Stream/Flight.cs;
// original game state 5, each frame 0x4352ac; godot-mdk stream.gd).
//
// Kurt flies down the tube, steering, drifting on bends and bouncing off its
// walls; the lights fly back, the health bonus walks ahead, then Bones' crane
// picks him up (or, after LEVEL8, Gunter leads him to the planet). No drawing:
// the screen (#18b, C# StreamScreen/StreamView) shows it.
//
//   fade in -> Kurt (steer, drift, walls) -> others -> camera -+  segment 177 or 1 health:
//      ^                                                       |  Bones (normal) / tube's end (Gunter)
//      +-------------------------------------------------------+  -> fade out -> Ended
//
// WHO OWNS WHAT. As in the C#, the tube and the random generator are the
// caller's and the Flight keeps pointers to them; the tube must be built from
// the SAME generator before the Flight (the order of draws is the game). The
// simplest owner is StreamRun below, which builds the three in that order.
//
// THE FLOW'S VIEW OF IT (C# Game.ShowStream / AfterStream, StreamScreen):
//   in:  the level just played, its health, the difficulty, and a seed —
//        WatcomRandom::kDefaultSeed for a stream started directly (--stream),
//        the tick count otherwise (game_flow.h, kStreamDefaultSeed);
//   out: ended(), then outcome() == Died -> Event::Menu, else StreamEnded with
//        health() (LEVEL5 starts with it after the Gunter stream).
//
// Fixed-size event and sound lists (at most a handful per update) instead of
// the C#'s Lists: no per-frame growth (AGENTS.md §10).
// ============================================================================

#include <stdint.h>

#include <vector>

#include "game/kurt.h"
#include "stream/stream_tube.h"
#include "stream/tube_basis.h"
#include "stream/watcom_random.h"
#include "util/matrix.h"

namespace mdk {
namespace stream {

// Something flying along the tube: where along it (segments), across it, its
// speed (segments/s), its yaw around the tube's forward axis and its
// animation frame. C# Thing.
struct Thing {
    float t = 0.0f;
    float x = 0.0f;
    float z = 0.0f;
    float speed = 0.0f;
    float yaw = 0.0f;
    float frame = 0.0f;
};

// Kurt's steering keys: -1, 0 or 1 to the right and up. C# Steering. The
// frontends map their input to it (C# StreamScreen.SteeringOf: turn or strafe
// right minus left, forward minus back, each signed).
struct Steering {
    int32_t right = 0;
    int32_t up = 0;
};

// An animation's length and speed (1 is 30 frames per second). C# Clip.
struct Clip {
    int32_t frameCount = 0;
    float speed = 1.0f;
};

// The screen's fade: towards white by `whiten` (1 = none), the red of death,
// then darkened by `dark` (1 = none). C# Fade.
struct Fade {
    float whiten = 0.0f;
    float dark = 1.0f;
    float red = 0.0f;
};

// What happened during an update, for the screen's sounds and messages.
enum FlightEvent { BonesCame, KurtDied, WallHit, BonusTaken, Ended };

// How the stream ended: Kurt goes on, or he died in the Gunter tube (game over).
enum Outcome { Survived, Died };

// A light or the planet the tube made, and its flight. C# (Spawn, Thing).
struct Sprite {
    StreamTube::Spawn spawn;
    Thing thing;
};

class Flight {
public:
    static constexpr float kTicksPerSecond = 30.0f;
    static constexpr float kNeutralYaw = 90.0f;
    // Bones comes after this segment (normal variant).
    static constexpr int32_t kRescueSegment = 177;
    static constexpr int32_t kLowHealth = 1;
    static constexpr int kMaxEvents = 8;
    static constexpr int kMaxSounds = 8;

    // C# Flight(tube, kind, difficulty, random, health, kurt, rescue, other).
    // `kurt`, `rescue` and `other` are KURTANIM, BONESANIM and SWHANM (normal)
    // or GUNTANIM (Gunter) of STREAM/STREAM.BNI.
    Flight(StreamTube* tube, StreamTube::Kind kind, Kurt::Difficulty difficulty, WatcomRandom* random,
           int32_t health, Clip kurt, Clip rescue, Clip other);

    Flight(const Flight&) = delete;
    Flight& operator=(const Flight&) = delete;

    // Esc: the stream fades out (not in the original). C# Stop.
    void stop() { ending_ = true; }

    // One frame of `delta` seconds. C# Update.
    void update(float delta, Steering steering);

    // Where something is and how it's turned: the tube's frame at its place,
    // turned by its yaw (row vectors, as Matrix4x4). C# Transform.
    Mat4 transform(const Thing& thing) const;
    // Kurt's transform: the tube's frame turned by his yaw and pitch. C# KurtTransform.
    Mat4 kurtTransform() const;
    // A looping animation's frame for a thing. C# OtherFrame.
    int32_t otherFrame(const Thing& thing) const;
    // Kurt's frame: looping while he flies, held at the last of the rescue. C# KurtFrame.
    int32_t kurtFrame() const;
    // Bones' frame of the rescue, held at the last. C# BonesFrame.
    int32_t bonesFrame() const;

    const Thing& kurt() const { return kurt_; }
    // Kurt plays the rescue (BONESANIM) once Bones has come. C# KurtClip.
    Clip kurtClip() const { return hasBones_ ? rescueClip_ : kurtClip_; }
    float pitch() const { return pitch_; }
    Vec3 kurtPosition() const { return kurtPosition_; }
    // Bones (null in the C# until he comes).
    bool hasBones() const { return hasBones_; }
    const Thing& bones() const { return bones_; }
    // The health bonus (SWH150, normal tube) or Gunter carrying Bones (Gunter
    // tube); null in the C# once taken or gone.
    bool hasOther() const { return hasOther_; }
    const Thing& other() const { return other_; }
    int32_t health() const { return health_; }
    bool ended() const { return ended_; }
    Outcome outcome() const { return health_ < kLowHealth ? Died : Survived; }
    // Counts ticks (0-31) for the health's blink.
    int32_t blink() const { return blink_; }
    Fade fade() const { return fade_; }
    float time() const { return time_; }

    // The camera: its eye, forward, right and up (MDK coordinates).
    Vec3 eye() const { return eye_; }
    Vec3 forward() const { return forward_; }
    Vec3 right() const { return right_; }
    Vec3 up() const { return screenUp_; }
    // The background's top-left pixel.
    Vec2 backgroundOffset() const { return backgroundOffset_; }

    // Events and sounds of the last update (sound names are BNI entries:
    // HURT1-7, APPLE, RESCUE).
    int eventCount() const { return eventCount_; }
    FlightEvent event(int i) const { return events_[i]; }
    bool hasEvent(FlightEvent e) const;
    int soundCount() const { return soundCount_; }
    const char* sound(int i) const { return sounds_[i]; }

    const StreamTube& tube() const { return *tube_; }
    StreamTube::Kind kind() const { return kind_; }
    // The lights and the planet, in the C#'s list order.
    const std::vector<Sprite>& sprites() const { return sprites_; }
    // How many lights the first 31 rings made. C# LightCount.
    int32_t lightCount() const { return lightCount_; }

    // State the C# keeps private, exposed for the goldens.
    Vec3 direction() const { return direction_; }
    float tickFraction() const { return tickFraction_; }
    float fadeLevel() const { return fadeLevel_; }
    bool ending() const { return ending_; }
    // How many sprites were ever added (the newest are the list's last).
    int32_t spawnedTotal() const { return spawnedTotal_; }

private:
    bool controlled() const { return !hasBones_ && health_ > 0; }
    void checkEnd();
    bool updateFade(float dt);
    int32_t addSpawns();
    bool move(Thing* thing, float dt) const;
    void updateOthers(float dt);
    void takeBonus();
    void updateKurt(float dt, Steering steering);
    void driftKurt(const TubeBasis& frame, float dt);
    void steer(float dt, Steering steering);
    void collide(Vec3 centre, const TubeBasis& frame);
    void hurt();
    void startRescue();
    void updateCamera();
    void addEvent(FlightEvent e);
    void addSound(const char* name);

    StreamTube* tube_;
    StreamTube::Kind kind_;
    Kurt::Difficulty difficulty_;
    WatcomRandom* random_;
    Clip kurtClip_;
    Clip rescueClip_;
    Clip otherClip_;
    std::vector<Sprite> sprites_;
    std::vector<StreamTube::Spawn> taken_;
    Vec3 direction_{0.0f, 1.0f, 0.0f};
    Vec3 screenUp_{0.0f, 0.0f, 1.0f};
    bool hasViewRows_ = false;
    Vec3 viewRows_[3];
    float fadeLevel_ = 0.0f;
    bool ending_ = false;
    float tickFraction_ = 0.0f;

    Thing kurt_;
    float pitch_ = 0.0f;
    Vec3 kurtPosition_;
    bool hasBones_ = false;
    Thing bones_;
    bool hasOther_ = false;
    Thing other_;
    int32_t health_;
    bool ended_ = false;
    int32_t blink_ = 0;
    Fade fade_;
    float time_ = 0.0f;
    Vec3 eye_;
    Vec3 forward_;
    Vec3 right_;
    Vec2 backgroundOffset_;
    FlightEvent events_[kMaxEvents];
    int eventCount_ = 0;
    const char* sounds_[kMaxSounds];
    int soundCount_ = 0;
    int32_t lightCount_ = 0;
    int32_t spawnedTotal_ = 0;
};

// The level's stream as StreamScreen builds it: the level's index in the order
// (clamped to 0-4) and its kind (Gunter after the last level with statistics).
int32_t streamIndexOf(int32_t level);
StreamTube::Kind streamKindOf(int32_t index);

// The random generator, the tube and the flight, built in the C#'s order
// (StreamScreen's constructor: the tube draws first, then the Flight).
struct StreamRun {
    WatcomRandom random;
    StreamTube tube;
    Flight flight;

    StreamRun(int32_t level, Kurt::Difficulty difficulty, uint32_t seed, int32_t health, Clip kurt, Clip rescue,
              Clip other);
};

} // namespace stream
} // namespace mdk

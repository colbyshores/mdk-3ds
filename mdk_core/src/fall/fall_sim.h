#pragma once

// ============================================================================
// fall_sim.h — the fall before a level, its simulation (fall.gd; game state 2,
// fall_3d.c: init 0x410018, each frame 0x4114a4, the intro 0x41106c).
//
// THE C# ORIGINAL is src/Mdk.Game/Fall/FallSim.cs and its partials
// FallSim.Missiles.cs, FallSim.Pickups.cs and FallSim.Radar.cs, ported operation
// for operation and in the C#'s tick order (FallSim::updateFall). The missile
// itself is fall/fall_missile.h. What is NOT here is the screen (FallScreen: the
// keys, the sounds' voices, the HUD), the view (FallView, FallModels,
// FallGround, FallPalette, FallHud): they read this class and draw it.
//
//   intro in space (150 ticks) ──► fall (33 s) ─────────────────────► Landed (the level)
//                                   │ steering until 30 s, dark from 31 s
//                                   │ radar sees Kurt ──► missiles ──► hits ──► health 0 ──► GameOver
//                                   └ pickups fall on chutes ──► inventory
//
// Kurt falls at 66.67 u/s from z 5270; the camera is 10 units above him.
//
// DETERMINISM. Every random decision comes from one mdk::Random (the port of a
// seeded System.Random), drawn in the C#'s order, so a seed and an input
// schedule give the same fall on both targets and in the oracle. FallScreen
// builds its sim with an unseeded `new Random()`; the screen that calls this
// picks the seed (a clock, say). Pinned by golden/L*_skydive_*.txt, byte for byte
// against tests/scripts/fall_probe (the C# FallSim).
//
// THE C# EVENTS (Sound, Message) are a function-pointer hook, as Kurt's
// PickupHook is (§10 forbids std::function): called synchronously where the C#
// invokes the event, with the FALL3D.SNI sound name or the MDKFONT.FTI text name.
//
// THE INVENTORY is a Kurt: the C# FallSim owns an Inventory (and a separate
// Health it passes by ref to Inventory.Collect); this port's inventory lives in
// game/kurt.h, so the fall keeps a Kurt for its slots, ammo and super chain gun
// and round-trips its own health through it on a take, as Kurt.Collect does.
//
// WHAT GOES ON TO THE LEVEL (FallScreen.Next, Viewer.CarryFromFall): when the
// fall lands, `carry()` is the FallCarry -- the health and the pickups taken, in
// order -- and `carryIntoLevel()` is what the level's Kurt does with it: takes
// the pickups again (Kurt::collect, without their messages) and then the health.
// When Kurt dies the outcome is GameOver and the C# goes to the menu with no
// carry.
//
// COST. One update per frame, a handful of missiles and pickups: COLD under
// AGENTS.md §2 (not ≥256 per frame, not in a profile). Its float comparisons are
// the C#'s gameplay predicates and stay floats; its loops count integer ticks.
// The containers grow only when a missile, pickup or explosion is created, which
// is the C#'s own design; they are reserved up front.
//
// No virtuals, no templates, no exceptions, no std::function (AGENTS.md §10).
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

#include "fall/fall_missile.h"
#include "formats/vector_types.h"
#include "game/game_state.h"
#include "game/kurt.h"
#include "util/random.h"

namespace mdk {

namespace formats {
class Bni;
}

namespace fall {

class FallSim {
public:
    // FallSim.Outcome.
    enum Outcome { Running, Landed, GameOver };
    // FallSim.KurtAnimation (Fall, Hit).
    enum KurtAnimation { AnimFall, AnimHit };
    // FallSim.RadarState.
    enum RadarState { Extend, Track, Retract };

    // FallSim.Animation: a model animation's speed (1 = 30 frames/s) and length.
    struct Animation {
        float speed = 1.0f;
        int32_t frameCount = 0;
    };

    // FallSim.Setup: what a fall starts from -- the fall index (0-4), the
    // difficulty, the pickups of FALLPU_n and the animations KURTANIM, KURT_HIT,
    // BONESANM. fallSetup() below builds it as FallScreen does.
    struct Setup {
        int32_t index = 0;
        Kurt::Difficulty difficulty = Kurt::Normal;
        std::vector<std::string> pickups;
        Animation kurt;
        Animation kurtHit;
        Animation bones;
    };

    // FallSim.Explosion: where, its random yaw (radians) and its age in ticks.
    struct Explosion {
        formats::Vec3f position;
        float yaw = 0.0f;
        float ticks = 0.0f;
    };

    // FallSim.Pickup: a pickup falling, then braking under its chute.
    struct Pickup {
        std::string name;
        formats::Vec3f position;
        formats::Vec3f velocity;
        // Yaw in degrees.
        float yaw = 0.0f;
        bool chute = false;
        int32_t fallTicks = 0;
        int32_t ticks = 0;
    };

    // The C#'s Sound and Message events. Either pointer may be null (the event
    // with no subscriber).
    struct Hooks {
        void* context = nullptr;
        // A sound to play (FALL3D.SNI).
        void (*sound)(void* context, const char* name) = nullptr;
        // A message to show (MDKFONT.FTI text name, zoomed, 3 seconds).
        void (*message)(void* context, const char* name) = nullptr;
    };

    // One frame's input, as FallScreen.Frame hands it over: the steering (−1, 0
    // or 1 per axis: x right, y up the screen; only the sign counts) and Esc,
    // which skips the fall (FallSim.Skip, the port's addition) before the update.
    struct Input {
        int32_t steerX = 0;
        int32_t steerY = 0;
        bool skip = false;
    };

    // ---- FallSim.cs constants the screen and view read ----
    static constexpr float kTicksPerSecond = 30.0f;
    static constexpr int32_t kIntroTicks = 150;
    // Kurt steers until 30 s, the view goes black from 31 s, the fall ends at 33 s.
    static constexpr float kSteerTime = 30.0f;
    static constexpr float kEndTime = 33.0f;
    static constexpr float kKurtStartZ = 5270.0f;
    static constexpr float kKurtSpeed = 2000.0f / 30.0f;
    // Steering (vel_accel_dt): 11.765 u/s per tick up to 117.65 u/s, within
    // ±58.82 × ±35.29.
    static constexpr float kSteerStep = 200.0f / 17.0f;
    static constexpr float kSteerMax = 2000.0f / 17.0f;
    static constexpr float kLimitX = 1000.0f / 17.0f;
    static constexpr float kLimitY = 600.0f / 17.0f;
    // The wind level (0x5209b8): 0xC00 is full, the haze's level is it >> 8.
    static constexpr int32_t kWindFull = 0xC00;
    static constexpr int32_t kWindShift = 8;
    static constexpr int32_t kHazeFrames = 16;
    static constexpr int32_t kCrawlerFrames = 8;
    // FallSim.Missiles.cs: an explosion lasts 26 ticks (its texture's frames).
    static constexpr int32_t kExplosionTicks = 26;
    // FallSim.Radar.cs: the beam's spot radius.
    static constexpr float kRadarRadius = 10.0f;

    // C# FallSim(Setup, Random): `seed` seeds the port of System.Random(seed).
    FallSim(const Setup& setup, int32_t seed);

    void setHooks(const Hooks& hooks) { hooks_ = hooks; }

    // FallSim.Update: one frame of `delta` seconds, steering -1/0/1 per axis.
    void update(float delta, int32_t steerX, int32_t steerY);
    // FallScreen.Frame's order: Esc (skip) first, then the update.
    void update(float delta, const Input& input);
    // FallSim.Skip: Esc skips the fall (the port's addition), unless dying.
    void skip();

    // FallSim.Steer: one axis of Kurt's steering (vel_accel_dt 0x409270 with a
    // key, 0x4687a4 without).
    static float steer(float speed, int32_t direction, float dt);
    // FallSim.CrossesBox: whether a move crosses Kurt's box (0x45ef80).
    static bool crossesBox(const formats::Vec3f& centre, const formats::Vec3f& from, const formats::Vec3f& to);

    // ---- The difficulty's numbers (FallSim's constructor, 0x410018) ----
    float beamSpeed() const { return beamSpeed_; }
    int32_t missilesPerDetection() const { return missilesPerDetection_; }
    float spread() const { return spread_; }
    int32_t missileGap() const { return missileGap_; }
    int32_t radarGap() const { return radarGap_; }

    // ---- The state (FallSim's properties) ----
    Outcome state() const { return state_; }
    // Whole ticks (1/30 s) of the last update.
    int32_t ticks() const { return ticks_; }
    bool inIntro() const { return introLeft_ > 0; }
    // Ticks left of the intro (0 once the fall started).
    int32_t introLeft() const { return introLeft_ > 0 ? introLeft_ : 0; }
    // Seconds since the intro ended.
    float time() const { return time_; }
    bool dying() const { return dying_; }
    int32_t health() const { return health_; }
    // FallSim.Inventory: the fall's own slots, ammo and super chain gun (the
    // health is health() above, not the Kurt's).
    const Kurt& inventory() const { return inventory_; }
    // FallSim.Collected: the pickups taken, in order (they go on to the level).
    const std::vector<std::string>& collected() const { return collected_; }

    const formats::Vec3f& kurtPosition() const { return kurtPosition_; }
    bool kurtVisible() const { return kurtVisible_; }
    KurtAnimation kurtPose() const { return kurtPose_; }
    float kurtFrame() const { return kurtFrame_; }
    const formats::Vec3f& cameraPosition() const { return cameraPosition_; }
    // FallSim.BonesPosition (a nullable): Bones falls with Kurt above index 3.
    bool hasBones() const { return hasBones_; }
    const formats::Vec3f& bonesPosition() const { return bonesPosition_; }
    float bonesFrame() const { return bonesFrame_; }

    int32_t wind() const { return wind_; }
    int32_t hazeFrame() const { return hazeFrame_; }
    float crawlerFrame() const { return crawlerFrame_; }
    // The screen towards white (1 none), darkened (1 none) and red (0 none).
    float whiten() const { return whiten_; }
    float dark() const { return dark_; }
    float red() const { return red_; }
    // The wind loop's volume.
    float windVolume() const;
    // The minecrawler's grind: from 2/3 to full over 30 s.
    float grindVolume() const;

    const std::vector<FallMissile>& missiles() const { return missiles_; }
    const std::vector<Explosion>& explosions() const { return explosions_; }
    const std::vector<Pickup>& pickups() const { return pickups_; }

    bool radarActive() const { return radarActive_; }
    RadarState radar() const { return radar_; }
    // The beam's spot and radius.
    const formats::Vec3f& radarSpot() const { return radarSpot_; }
    float radarSpotRadius() const { return radarSpotRadius_; }

    // FallScreen.Next's carry for Outcome Landed: the health and the pickups
    // taken. False (out untouched) unless the fall landed: a GameOver goes to the
    // menu with no carry, and a running fall has nothing to carry yet.
    bool carry(flow::FallCarry* out) const;

private:
    // ---- FallSim.cs ----
    void updateIntro(int32_t ticks);
    void startFall();
    void updateFall(float dt, int32_t ticks, int32_t steerX, int32_t steerY);
    void updateKurt(float dt, int32_t ticks, int32_t steerX, int32_t steerY);
    void animateKurt(int32_t ticks);
    void updateCamera(float dt);
    void updateBones(float dt, int32_t ticks);
    void updatePalette(float dt, int32_t ticks);
    void flash(float dt, int32_t ticks);
    void setScreen(float whiten, float dark);
    // RandomPoint: a random point within Kurt's limits (x drawn first).
    void randomPoint(float* x, float* y);
    float randomRange(float range);
    void sound(const char* name);

    // ---- FallSim.Missiles.cs ----
    void updateMissiles(float dt, int32_t ticks);
    void launch();
    void hit(size_t missile);
    void updateExplosions(int32_t ticks);

    // ---- FallSim.Pickups.cs ----
    void startPickups();
    void updatePickups(float dt, int32_t ticks);
    void movePickup(Pickup* pickup, float dt, int32_t ticks);
    void take(size_t pickup);
    void spawn(const std::string& name);

    // ---- FallSim.Radar.cs ----
    void startRadars();
    void updateRadar(float dt, int32_t ticks);
    bool startRadar(int32_t ticks);
    void extend(float dt);
    void track(float dt, int32_t ticks);
    void retract(float dt);
    void detected();
    void chooseTarget(float* x, float* y);

    Setup setup_;
    Random random_;
    Hooks hooks_;

    float beamSpeed_ = 0.0f;
    int32_t missilesPerDetection_ = 0;
    float spread_ = 0.0f;
    int32_t missileGap_ = 0;
    int32_t radarGap_ = 0;

    int32_t introLeft_ = kIntroTicks;
    float tickFraction_ = 0.0f;
    float cameraSpeed_ = -kKurtSpeed;
    bool kurtFinished_ = false;
    bool bonesPassed_ = false;

    // Palette effects (0x5209c8 and its target and rate).
    float brightness_ = 0.0f;
    float target_ = 1.0f;
    float rate_ = 0.5f;

    Outcome state_ = Running;
    int32_t ticks_ = 0;
    float time_ = 0.0f;
    bool dying_ = false;
    int32_t health_ = Kurt::kMaxHealth;
    Kurt inventory_;
    std::vector<std::string> collected_;

    formats::Vec3f kurtPosition_;
    float kurtVelocityX_ = 0.0f;
    float kurtVelocityY_ = 0.0f;
    bool kurtVisible_ = false;
    KurtAnimation kurtPose_ = AnimFall;
    float kurtFrame_ = 0.0f;
    formats::Vec3f cameraPosition_;
    bool hasBones_ = false;
    formats::Vec3f bonesPosition_;
    float bonesFrame_ = 0.0f;
    float bonesTicks_ = 0.0f;

    int32_t wind_ = 0;
    int32_t hazeFrame_ = 0;
    float crawlerFrame_ = 0.0f;
    float whiten_ = 1.0f;
    float dark_ = 0.0f;
    float red_ = 0.0f;

    // Missiles.
    std::vector<FallMissile> missiles_;
    std::vector<Explosion> explosions_;
    int32_t missileQueue_ = 0;
    int32_t missileWait_ = 0;

    // Pickups.
    std::vector<std::string> pickupNames_;
    std::vector<Pickup> pickups_;
    int32_t pickupWait_ = 0;

    // Radar.
    int32_t radarWait_ = 0;
    float radarHeight_ = 0.0f;
    float radarTargetX_ = 0.0f;
    float radarTargetY_ = 0.0f;
    float radarVelocityX_ = 0.0f;
    float radarVelocityY_ = 0.0f;
    int32_t radarTicks_ = 0;
    bool radarActive_ = false;
    RadarState radar_ = Extend;
    formats::Vec3f radarSpot_;
    float radarSpotRadius_ = 0.0f;
};

// FallScreen's choice of fall: Math.Clamp(GameState.IndexOf(level), 0, 4) --
// the falls are those of the level indices 0-4 (LEVEL5 has none, and clamps to
// LEVEL8's).
int32_t fallIndex(int32_t level);

// FallScreen's setup: the index for `level`, the difficulty, FALLPU_n's pickups
// and the KURTANIM, KURT_HIT and BONESANM animations of FALL3D.BNI
// (FallModels.Animation). False when an animation is missing or does not parse
// (the C# throws there).
bool fallSetup(const formats::Bni& bni, int32_t level, Kurt::Difficulty difficulty, FallSim::Setup* out);

// Viewer.CarryFromFall: the level's Kurt keeps the fall's pickups (taken again
// through Kurt::collect, in order) and then its health (Kurt::setHealth). The C#
// runs it BEFORE the viewer subscribes Inventory.PickedUp (Viewer.cs:246 vs :249),
// so the carried pickups show no messages; this clears Kurt's PickupHook for the
// takes and restores it after, so a caller may bind the hook first or later.
void carryIntoLevel(const flow::FallCarry& carry, Kurt* kurt);

} // namespace fall
} // namespace mdk

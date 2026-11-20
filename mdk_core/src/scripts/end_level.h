#pragma once

// ============================================================================
// end_level.h -- the end of a level, ported 1:1 from src/Mdk.Game/Scripts/EndLevel.cs
// (endlev.c: 0x40a9e0, 0x40ad9c each frame, pieces 0x40b280, Kurt 0x40b558).
//
// special_event 0-50 (opcode 131) is ScriptRuntime::endTheLevel, which builds one
// of these and starts it in Kurt's arena. The visible triangles of that arena are
// taken from the highest down: each tick 0-7 more are torn off and fly up
// (vz += 0.025 per tick) spinning around Kurt (the spin grows by 0.15 degrees per
// tick up to 2.5), until they are 500 above him. Kurt takes off (K_TAKEOF, then
// K_FLOATC), turns ever faster (0.15 degrees per tick^2, up to 2.5 per tick) and
// rises with them (0.025 per tick^2); once he rises faster than 3 units a tick
// his rise speeds up three times as fast and the view tilts up (22.5 degrees a
// second, to (-60 - the arena's pitch) / 2), then the screen goes white (8 per
// tick); past 300 the level is over.
//
//   arena triangles, highest first --0-7 a tick--> pieces: rise, spin around Kurt --500 above--> gone
//   Kurt: rise --vz > 3--> tilt up --> white flash --> 300--> finished (the runtime's LevelEnded)
//
// THE C#'s `Finished` EVENT is update()'s return value (AGENTS.md section 10
// forbids the delegate): true on the one tick the flash passes its end, after
// which update() does nothing. ScriptRuntime latches it as levelEnded().
//
// WHAT READS IT. The runtime's tick (update, after Debris.Update, as the C# RunTick
// does), the frontends' arena draw (torn(): the triangles collapse out of the
// arena's mesh, LevelView::tear) and the effects pass (pieces(): drawn with the
// arena's surfaces, both faces, EffectsView::build) -- the C#'s LevelView.DrawEnd.
//
// Kurt is the bound Player (his feet, yaw, pose, camera tilt and white flash are
// written here, as the C# writes its Kurt). With no Player bound -- a harness
// that passes null to the runtime -- there is no Kurt to lift and start() still
// tears the arena, but update() returns at once, as the C#'s does with no
// runtime.
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

#include "render/render_types.h"

namespace mdk {

class LevelData;
class Player;
class Random;

namespace formats {
class Arena;
} // namespace formats

namespace scripts {

class EndLevel {
public:
    // EndLevel.Piece: a torn-off triangle, where its centre is and how far it
    // turned (degrees).
    struct Piece {
        int32_t triangle = 0;
        Vec3 centre{};
        float angle = 0.0f;
        float riseSpeed = 0.0f;
        float spin = 0.0f;
    };

    // EndLevel.Takeoff / Floating: Kurt's two poses (TRAVSPRT.BNI animations).
    static const char* const kTakeoff;
    static const char* const kFloating;
    // EndLevel.StartShake: the shake the runtime raises when the end starts.
    static constexpr float kStartShake = 5.0f;

    // C# `new EndLevel(takeoffFrames)`: K_TAKEOF's frame count, after which the
    // pose is K_FLOATC.
    explicit EndLevel(int32_t takeoffFrames) : takeoffFrames_(takeoffFrames) {}

    // EndLevel.Start: queues the visible triangles of Kurt's arena (`arena`,
    // ScriptRuntime.CurrentArena) highest first, and Kurt stops (Frozen) and
    // rises (Rising). The shake (StartShake) is raised by the caller, which owns
    // it. Everything is BORROWED for the life of this object.
    void start(LevelData* level, const std::string& arena, Player* kurt, Random* rng);

    // EndLevel.Update: one tick. Returns true on the tick it finishes (the C#'s
    // Finished event).
    bool update(float ticks);

    // EndLevel.HighestFirst: the visible triangles of an arena (shown, with a
    // drawn material), the highest top first (a stable order, as OrderByDescending).
    static void highestFirst(const LevelData& level, const formats::Arena& arena,
                             std::vector<int32_t>* out);

    // EndLevel.RiseSpeed: Kurt's rise speed after a step, 0.025 per tick^2, three
    // times that above 3 per tick.
    static float riseSpeed(float speed, float ticks);

    // EndLevel.PitchOffset: added to the camera pitch (degrees, negative looks
    // up; 0x573b1c). Kurt's cameraTilt is where the camera reads it.
    float pitchOffset() const { return pitchOffset_; }
    // EndLevel.Arena: the arena torn apart ("" when it was not found).
    const std::string& arena() const { return arenaName_; }
    // EndLevel.Torn: the triangles torn off so far, in order (the view hides them).
    const std::vector<int32_t>& torn() const { return torn_; }
    // EndLevel.Pieces: the pieces flying.
    const std::vector<Piece>& pieces() const { return pieces_; }
    // Whether the end has finished (the C#'s _done).
    bool done() const { return done_; }

    // The C#'s private state, read by the golden harness (the oracle probe reads
    // the same fields by reflection).
    float riseSpeedNow() const { return riseSpeed_; }
    float turn() const { return turn_; }
    float flash() const { return flash_; }
    float takeoff() const { return takeoff_; }

private:
    void tearOff(int32_t count);
    void movePieces(const Vec3& kurt, float ticks);

    int32_t takeoffFrames_;
    LevelData* level_ = nullptr;
    const formats::Arena* arena_ = nullptr;
    std::string arenaName_;
    Player* kurt_ = nullptr;
    Random* rng_ = nullptr;
    bool started_ = false;

    // The C#'s Queue<int>: the triangles still to tear, read from `queueHead_` on.
    std::vector<int32_t> queue_;
    size_t queueHead_ = 0;
    std::vector<int32_t> torn_;
    std::vector<Piece> pieces_;
    float riseSpeed_ = 0.0f;
    float turn_ = 0.0f;
    float flash_ = 0.0f;
    float takeoff_ = 0.0f;
    float pitchOffset_ = 0.0f;
    bool done_ = false;
};

} // namespace scripts
} // namespace mdk

#pragma once

// ============================================================================
// sniper_rounds.h — Kurt's sniper rounds (3 slots at 0x573c98, updated by
// 0x462708 after the objects). A 1:1 port of src/Mdk.Game/Scripts/SniperRounds.cs
// (itself a port of godot-mdk's sniper_rounds.gd, docs/gameplay.md "Sniper
// mode"). A round starts at the eye along the view and is tested every tick
// along the segment it moved: objects (their boxes, then their parts' faces),
// then the arena.
//
//   type 0  bullet          1100 u/s, 75 ticks, 8 damage
//   type 1  homing bullet    400 u/s, 240 ticks, steers at the locked object
//   type 2  grenade         as the bullet, explodes
//   type 3  homing grenade  as the homing bullet, explodes
//   type 4  mortar          thrown at 150 u/s, drag, gravity, bounces; bomb_follow_path guides it
//
//   FREE ──fire──► FLYING ──object──► HIT │ KILLED (camera watches 30 ticks)
//                    │    ──wall, end─► DEAD (miss) │ EXPLODED ──linger──► FREE
//
// The runtime owns one (ScriptRuntime::sniperRounds(), the C#'s
// ScriptRuntime.SniperRounds) and updates it once a tick after the debris
// (ScriptRuntime.cs:956). Kurt's clip fires through ScriptRuntime::fireSniper
// (the C#'s FireSniper, bound as Kurt.SniperFire), and opcode 28
// (bomb_follow_path) reaches guideLastMortar.
//
// WHAT A ROUND WRITES. On an object: hit_event (the part + 1, or -2 for the
// whole box), hit_type (the round's type: if_hit_weapon 0, 2, 3, 4 in the retail
// scripts), hit_direction (the round's yaw: push_hit_dir, opcode 81), shot_part
// and shot_point (special_130's bullet hole, package #16), the health and the
// kill. On a wall: the group's hit (kind 1, weapon = the type), its sparks, and
// the explosion of a grenade. The seam for #16's bullet holes on walls is in
// moveStraight, where the round stops on the arena.
//
// TRIG AND EXACTNESS. Every cos/sin/atan2 here is libm, as the C#'s MathF on
// the oracle's host (glibc): the rounds are gameplay -- what they hit, the
// mortar puzzle's path -- and golden/L*_rounds_*.txt pins them bit for bit.
// Cold path: three slots, a handful of calls per tick.
// ============================================================================

#include <stdint.h>

#include "render/render_types.h"
#include "scripts/object.h"

namespace mdk {
namespace scripts {

class ScriptRuntime;

class SniperRounds {
public:
    static const int32_t kSlots = 3;

    // C# SniperRounds.State.
    enum State { Free, Flying, Killed, Hit, Dead, Exploded };
    // C# SniperRounds.Type.
    enum Type { Bullet, Homing, Grenade, HomingGrenade, Mortar };

    // What a round's camera shows (0x461bcc, C# SniperRounds.Shot).
    enum Shot { ShotEmpty, ShotWatching, ShotHit, ShotKill, ShotMiss };

    // C# SniperRounds.RoundCamera: from behind the round while it watches, then a
    // colour or the miss animation (`time`: ticks since the miss).
    struct RoundCamera {
        Shot shot;
        Vec3 position;
        Vec3 forward;
        float time;
    };

    // C# SniperRounds.Round, one slot. Public for the frontends (#7's round view)
    // and the golden's dump; written only here.
    struct Round {
        State state = Free;
        Type type = Bullet;
        Vec3 position{};
        float yaw = 0.0f;
        // Positive looks down.
        float pitch = 0.0f;
        float roll = 0.0f;
        float speed = 0.0f;
        // The mortar's vertical speed.
        float vertical = 0.0f;
        float life = 0.0f;
        float linger = 0.0f;
        MdkObject* target = nullptr;
        int32_t targetPart = -1;
        float yawRate = 0.0f;
        // bomb_follow_path (opcode 28): the path and its time.
        int32_t path = 0;
        float pathTime = 0.0f;
        // The C#'s Visual (a fresh MdkObject per shot, not in the object list):
        // the round's model in flight, valid while hasVisual.
        bool hasVisual = false;
        MdkObject visual;
        float cameraDistance = 0.0f;
        Vec3 cameraPosition{};
        float watch = 0.0f;
    };

    explicit SniperRounds(ScriptRuntime& runtime) : runtime_(runtime) {}
    SniperRounds(const SniperRounds&) = delete;
    SniperRounds& operator=(const SniperRounds&) = delete;

    // C# Fire (0x461e88): a round of `type` from `eye` along the view; homing
    // rounds chase `target`. False when all the slots are busy.
    bool fire(int32_t type, const Vec3& eye, float yaw, float pitch, MdkObject* target);

    // C# Camera: what the camera of slot `index` shows.
    RoundCamera camera(int32_t index) const;

    // C# GuideLastMortar, bomb_follow_path (opcode 28): the mortar round that just
    // hit a triangle group follows a path.
    void guideLastMortar(int32_t path);

    // C# Update: the rounds by `ticks`.
    void update(float ticks);

    // C# Visuals: the flying rounds' models (drawn with the objects). Slot i's is
    // visual(i) when visualFlying(i).
    bool visualFlying(int32_t index) const {
        return rounds_[index].state == Flying && rounds_[index].hasVisual;
    }
    const MdkObject& visual(int32_t index) const { return rounds_[index].visual; }

    const Round& round(int32_t index) const { return rounds_[index]; }

private:
    // C# ObjectHit.
    struct ObjectHit {
        MdkObject* object;
        Vec3 point;
        int32_t part;
    };

    static int32_t headOf(const MdkObject* obj);
    void makeVisual(Round* round);
    void fly(Round* round, float ticks, float dt);
    void moveStraight(Round* round, float dt);
    void steer(Round* round, float dt);
    void moveMortar(Round* round, float dt);
    void followPath(Round* round, float ticks);
    bool testObjects(const Vec3& start, const Vec3& end, ObjectHit* hit);
    static bool hitPart(const MdkObject* obj, const Vec3& start, const Vec3& end, Vec3* point,
                        int32_t* part);
    void hitObject(Round* round, MdkObject* obj, int32_t part, const Vec3& point);
    void explode(Round* round, float radius, MdkObject* source);
    static void end(Round* round, State state, float linger);

    ScriptRuntime& runtime_;
    Round rounds_[kSlots];
    // The mortar round that just hit a triangle group (0x491ef0, for bomb_follow_path).
    Round* lastMortar_ = nullptr;
};

} // namespace scripts
} // namespace mdk

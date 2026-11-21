#pragma once

// ============================================================================
// rides.h — the objects Kurt rides (C# src/Mdk.Game/Scripts/Rides.cs; 0x573c30,
// damp_control 0x466368; rides.gd): package #9, the shared core and the XD2.
//
// A script makes an object rideable (flag 0x2000000). Kurt gets on when he
// touches it without firing -- the XD2 (L7 DANT_9, 0x46a840) only while he is on
// a floor -- and off when the script clears the flag or the object goes:
//
//   rideable + Kurt touches it ──► riding ──(flag cleared / dead)──► off
//                                                (the XD2: a running jump)
//
// In the XD2 Kurt walks hidden, without strafing or jumping (Player's
// Walk::Riding, game/player.cpp), and it goes where he is: Update puts it on the
// tick's copy of his feet and yaw, Follow on his live feet after every step. Its
// DUMMY loop plays while he steers, firing sounds the alarm, and hits on Kurt go
// to it (ScriptRuntime::hurtKurt); if it goes while he rides it he takes 50.
//
// THE SEAMS. Rides.cs also mounts the snowboard of L4 (XSNOWB, package #10) and
// the XE bomber of L7's DANT_5 (package #11). Their classes (Snowboard.cs,
// Bomber.cs) are not ported, so TryMount finds their types and leaves Kurt off
// them -- what this port did before this package -- and onBoard()/bomber() stay
// false. The branches that read them are written the C#'s way (Update's
// off-test, Follow, Dismount, Lost, takesHits) so those packages fill in a
// pointer, not a control flow. Player's half of the seam is Player::RideHook
// (the C#'s Kurt.Ride delegate), Player::glide and Player::getOffBoard.
// ============================================================================

#include <stdint.h>

namespace mdk {
namespace scripts {

class ScriptRuntime;
struct MdkObject;

class Rides {
public:
    // C# Rides.FlagRideable / FlagLocked (Rides.cs:18-19): rideable, and the
    // controls locked (the board, the XE).
    static const int32_t kFlagRideable = 0x2000000;
    static const int32_t kFlagLocked = 0x4000000;

    explicit Rides(ScriptRuntime& runtime) : runtime_(runtime) {}

    // C# Rides.Ridden (Rides.cs:40): the object Kurt rides, or null.
    MdkObject* ridden() const { return ridden_; }
    // C# Rides.OnBoard (Rides.cs:89): whether Kurt rides the snowboard. Package #10.
    bool onBoard() const { return board_; }
    // Whether Kurt rides the XE (C# `Rides.Bomber != null`). Package #11.
    bool bomber() const { return bomber_; }
    // C# Viewer.Driven (Viewer.cs:600): what Kurt rides that follows him each step
    // (the XD2, the board), drawn with him rather than between two ticks.
    MdkObject* driven() const { return bomber_ ? nullptr : ridden_; }

    // C# Rides.Update (Rides.cs:46): each tick, after Kurt moved -- getting on,
    // riding the XD2, getting off. ScriptRuntime::runTick calls it after
    // KurtTouchesGroups (ScriptRuntime.cs:937).
    void update();
    // C# Rides.Follow (Rides.cs:69): each step, after the ticks -- the XD2 is where
    // Kurt is. ScriptRuntime::update calls it last (ScriptRuntime.cs:392).
    void follow();
    // C# Rides.RideWalker (Rides.cs:92): puts Kurt on a walker at once, as touching
    // it does (the tests' --ride, RideTest.Start).
    void rideWalker(MdkObject* obj);
    // C# Rides.Hides (Rides.cs:99): the XE once the view is inside it. Package #11;
    // false while there is no bomber.
    bool hides(const MdkObject* obj) const;
    // C# Rides.TakesHits (Rides.cs:102): hits on Kurt go to what he rides (the XD2,
    // the XE), not on the board.
    bool takesHits() const { return ridden_ != nullptr && !board_; }
    // C# Rides.Lost (Rides.cs:105, 0x43d734): the ridden object went -- Kurt is off,
    // and but for the board he takes 50. ScriptRuntime::remove calls it first.
    void lost(MdkObject* obj);

private:
    void tryMount();
    void mountWalker(MdkObject* obj);
    void updateWalker();
    void dismount();

    ScriptRuntime& runtime_;
    MdkObject* ridden_ = nullptr;
    // C# Rides._board / Rides.Bomber: the seams of packages #10 and #11, never set here.
    bool board_ = false;
    bool bomber_ = false;
    // C# Rides._walkerSound: the XD2's DUMMY voice while Kurt steers.
    int32_t walkerSound_ = 0;
};

}  // namespace scripts
}  // namespace mdk

#pragma once

// ============================================================================
// harness_aids.h -- test aids that put Kurt straight into a set piece, for
// emulator and hardware playthroughs: the 3DS harness's GIVE=, RIDE= and AIM=
// (mdk_3ds/Makefile, harness builds only -- compiled out of a release and refused
// by build_cia.sh) and the Linux build's --give, --ride and --aim.
//
//   GIVE=SW_BONES[,SW_HOME...]  the pickups put in Kurt's inventory at spawn, as a
//                               touched pickup would (Kurt::collect; the items
//                               probe's `give:`).
//   RIDE=XE | XD2 | XSNOWB      from 1 s on, Kurt mounted on the nearest live one
//                               of that model in his arena through the ported
//                               Rides code (Rides::mount): a walker by RideWalker
//                               (the C#'s --ride, RideTest.Start), the board and a
//                               bomber once their script has made them rideable.
//                               In DANT_5 an XE that is not rideable yet is called
//                               first, as the C#'s RideTest.StepBomber calls it
//                               (HitGroup(DANT_5, 16, 1, the chain gun)).
//   AIM=yaw,pitch               Kurt's yaw at spawn, and the scope's yaw and pitch
//                               the first time he snipes (entering the scope
//                               resets its pitch): a shot that does not depend on
//                               turns rounded through the trig table.
//
// Every aid is the run's FIRST level's only, as the C#'s test options are
// (Game.cs:487). Nothing here runs unless a frontend asks.
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

namespace mdk {

class Player;

namespace scripts {
class ScriptRuntime;
}

struct HarnessAids {
    std::vector<std::string> gives;
    std::string ride;
    bool aim = false;
    float aimYaw = 0.0f;
    float aimPitch = 0.0f;

    // The run's state.
    float rideTime = 0.0f;
    bool rideDone = false;
    bool rideCalled = false;
    bool scopeAimed = false;

    bool any() const { return !gives.empty() || !ride.empty() || aim; }
};

// GIVE's list: comma-separated pickup models. False (and why) when empty or
// holding an empty name.
bool parseHarnessGive(const char* text, std::vector<std::string>* out, std::string* why);
// RIDE's model: one of the Rides models (XE, X_STRIKE, XD, XD2, XSNOWB).
bool parseHarnessRide(const char* text, std::string* out, std::string* why);
// AIM's "yaw,pitch" in degrees (the scope's pitch is clamped to its limit).
bool parseHarnessAim(const char* text, float* yaw, float* pitch, std::string* why);

// What the aids print (the frontend's log line).
typedef void (*HarnessPrint)(const char* line);

// At Kurt's spawn: GIVE's pickups collected, AIM's yaw set.
void harnessAidsAtSpawn(HarnessAids* aids, Player* player, HarnessPrint print);
// Before each of Kurt's steps: RIDE's mount, AIM's scope.
void harnessAidsStep(HarnessAids* aids, Player* player, scripts::ScriptRuntime* runtime,
                     float step, HarnessPrint print);

} // namespace mdk

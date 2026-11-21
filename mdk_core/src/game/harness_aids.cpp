// harness_aids.cpp -- see harness_aids.h.

#include "game/harness_aids.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <deque>

#include "game/player.h"
#include "game/scope.h"
#include "scripts/object.h"
#include "scripts/rides.h"
#include "scripts/script_runtime.h"

namespace mdk {
namespace {

// RIDE waits for the level to settle first (the C#'s TestStart, Viewer.cs:109),
// and gives up when nothing of the model became rideable in this long.
const float kRideStart = 1.0f;
const float kRideGiveUp = 30.0f;
// C# RideTest's call of the XE: DANT_5's group 16, hit once by the chain gun
// (RideTest.cs:32-33, StepBomber; ScriptRuntime.HitChainGun is 2).
const char kBomberArena[] = "DANT_5";
const int32_t kBomberCallGroup = 16;
const int32_t kHitChainGun = 2;

const char* const kRideModels[] = {"XE", "X_STRIKE", "XD", "XD2", "XSNOWB"};

bool sameName(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); i++) {
        char x = a[i];
        char y = b[i];
        if (x >= 'a' && x <= 'z') x = (char)(x - 'a' + 'A');
        if (y >= 'a' && y <= 'z') y = (char)(y - 'a' + 'A');
        if (x != y) {
            return false;
        }
    }
    return true;
}

bool isWalker(const std::string& model) {
    return sameName(model, "XD") || sameName(model, "XD2");
}

bool isBomber(const std::string& model) {
    return sameName(model, "XE") || sameName(model, "X_STRIKE");
}

void say(HarnessPrint print, const char* line) {
    if (print) {
        print(line);
    }
}

} // namespace

bool parseHarnessGive(const char* text, std::vector<std::string>* out, std::string* why) {
    out->clear();
    const std::string all = text ? text : "";
    size_t at = 0;
    while (at <= all.size()) {
        size_t comma = all.find(',', at);
        if (comma == std::string::npos) {
            comma = all.size();
        }
        const std::string name = all.substr(at, comma - at);
        if (name.empty()) {
            *why = "an empty pickup name in \"" + all + "\"";
            out->clear();
            return false;
        }
        out->push_back(name);
        at = comma + 1;
    }
    return true;
}

bool parseHarnessRide(const char* text, std::string* out, std::string* why) {
    const std::string model = text ? text : "";
    for (size_t i = 0; i < sizeof(kRideModels) / sizeof(kRideModels[0]); i++) {
        if (sameName(model, kRideModels[i])) {
            *out = kRideModels[i];
            return true;
        }
    }
    *why = "\"" + model + "\" is not a ride (XE, X_STRIKE, XD, XD2, XSNOWB)";
    return false;
}

bool parseHarnessAim(const char* text, float* yaw, float* pitch, std::string* why) {
    float y = 0.0f;
    float p = 0.0f;
    char extra = 0;
    if (!text || sscanf(text, "%f,%f%c", &y, &p, &extra) != 2) {
        *why = std::string("want yaw,pitch in degrees, got \"") + (text ? text : "") + "\"";
        return false;
    }
    if (p < -Scope::kPitchLimit || p > Scope::kPitchLimit) {
        *why = "the pitch must be within the scope's +-50 degrees";
        return false;
    }
    *yaw = y;
    *pitch = p;
    return true;
}

void harnessAidsAtSpawn(HarnessAids* aids, Player* player, HarnessPrint print) {
    char line[160];
    for (size_t i = 0; i < aids->gives.size(); i++) {
        // Kurt::collect, as a touched pickup (the items probe's `give:`).
        player->collect(aids->gives[i]);
        snprintf(line, sizeof(line), "harness: GIVE %s", aids->gives[i].c_str());
        say(print, line);
    }
    if (aids->aim) {
        player->yaw = aids->aimYaw;
        snprintf(line, sizeof(line), "harness: AIM yaw %.3f at spawn (the scope's pitch %.3f "
                 "when he snipes)", aids->aimYaw, aids->aimPitch);
        say(print, line);
    }
}

void harnessAidsStep(HarnessAids* aids, Player* player, scripts::ScriptRuntime* runtime,
                     float step, HarnessPrint print) {
    char line[200];
    // AIM: the scope's facing, the first step it is up (entering resets its pitch).
    if (aids->aim && !aids->scopeAimed && player->sniping()) {
        aids->scopeAimed = true;
        player->yaw = aids->aimYaw;
        player->scope().pitch = aids->aimPitch;
        snprintf(line, sizeof(line), "harness: AIM the scope at yaw %.3f pitch %.3f",
                 aids->aimYaw, aids->aimPitch);
        say(print, line);
    }
    if (aids->ride.empty() || aids->rideDone || runtime == nullptr) {
        return;
    }
    aids->rideTime += step;
    if (aids->rideTime < kRideStart) {
        return;
    }
    scripts::Rides& rides = runtime->rides();
    if (rides.ridden() != nullptr) {
        aids->rideDone = true;
        return;
    }
    // The nearest live one of the model in Kurt's arena; rideable, but for a
    // walker (RideWalker makes it so, as the C#'s --ride).
    const std::string& arena = runtime->currentArena();
    std::deque<scripts::MdkObject>& objects = runtime->objects();
    scripts::MdkObject* best = nullptr;
    float bestDistance = 0.0f;
    for (size_t i = 0; i < objects.size(); i++) {
        scripts::MdkObject& obj = objects[i];
        if (obj.dead || runtime->isRemoved(&obj) || !sameName(obj.type_name, aids->ride) ||
            obj.arena != arena) {
            continue;
        }
        if (!isWalker(aids->ride) && (obj.flags & scripts::Rides::kFlagRideable) == 0) {
            continue;
        }
        const float dx = obj.position.x - player->feet.x;
        const float dy = obj.position.y - player->feet.y;
        const float dz = obj.position.z - player->feet.z;
        const float distance = dx * dx + dy * dy + dz * dz;
        if (best == nullptr || distance < bestDistance) {
            best = &obj;
            bestDistance = distance;
        }
    }
    if (best == nullptr) {
        if (isBomber(aids->ride) && !aids->rideCalled && arena == kBomberArena) {
            // C# RideTest.StepBomber: the XE called by hitting DANT_5's group 16.
            aids->rideCalled = true;
            runtime->hitGroup(kBomberArena, kBomberCallGroup, 1, kHitChainGun, 0);
            snprintf(line, sizeof(line), "harness: RIDE=%s: none rideable yet; called by "
                     "hitting %s group %d", aids->ride.c_str(), kBomberArena,
                     (int)kBomberCallGroup);
            say(print, line);
        } else if (aids->rideTime >= kRideGiveUp) {
            aids->rideDone = true;
            snprintf(line, sizeof(line), "harness: RIDE=%s: no rideable one in %s after %.0f s; "
                     "given up", aids->ride.c_str(), arena.c_str(), (double)kRideGiveUp);
            say(print, line);
        }
        return;
    }
    if (rides.mount(best)) {
        aids->rideDone = true;
        snprintf(line, sizeof(line),
                 "harness: RIDE=%s: mounted %s_%d in %s at (%.1f,%.1f,%.1f), %.2f s in",
                 aids->ride.c_str(), best->type_name.c_str(), (int)best->instance_id,
                 arena.c_str(), best->position.x, best->position.y, best->position.z,
                 (double)aids->rideTime);
        say(print, line);
    }
}

} // namespace mdk

// ============================================================================
// rides.cpp — C# src/Mdk.Game/Scripts/Rides.cs, operation for operation (package
// #9; the XE's half, package #11). See rides.h for the ride's shape and the seam
// package #10 fills.
// Kurt's half (Walk::Riding, rideWalker, jumpOff, the RideHook) is in
// game/player.cpp.
// ============================================================================

#include "scripts/rides.h"

#include <ctype.h>
#include <math.h>

#include <string>

#include "game/player.h"
#include "scripts/script_runtime.h"

namespace mdk {
namespace scripts {

namespace {

// C# Rides.cs:20-35.
const int32_t kFlagRidden = 0x80000;
const int32_t kFlagFiring = 0x1;
// On the board: Kurt passes through it (+0x800) and it's no platform any more (-0x800100).
const int32_t kBoardFlagsSet = 0x80800;
const int32_t kBoardFlagsClear = 0x800100;
const char* const kBoard = "XSNOWB";
const char* const kWalkers[] = {"XD", "XD2"};
const char* const kBombers[] = {"XE", "X_STRIKE"};
// The XD2: DUMMY at volume 0x2000 while steered, ALERT and the alarm while firing;
// Kurt takes 50 damage if it goes while he rides it.
const char* const kWalkerSound = "DUMMY";
const char* const kAlarmSound = "ALERT";
const int32_t kWalkerSoundVolume = 0x2000;
const int32_t kAlarmTicks = 10;
const int32_t kLostDamage = 50;

// C# ScriptMath.Wrap360 (ScriptMath.cs:14): degrees - 360 * Floor(degrees / 360),
// the same floor form script_runtime.cpp's file-local wrap360 is.
float wrap360(float degrees) { return degrees - 360.0f * floorf(degrees / 360.0f); }

// C# `obj.TypeName.ToUpperInvariant()` compared with an upper-case name.
bool typeIs(const std::string& type, const char* name) {
    size_t i = 0;
    for (; i < type.size() && name[i] != '\0'; i++) {
        if (toupper((unsigned char)type[i]) != name[i]) {
            return false;
        }
    }
    return i == type.size() && name[i] == '\0';
}

bool typeIn(const std::string& type, const char* const* names, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (typeIs(type, names[i])) {
            return true;
        }
    }
    return false;
}

}  // namespace

void Rides::update() {
    // C# Rides.Update (Rides.cs:46-66).
    if (ridden_ == nullptr) {
        tryMount();
        return;
    }

    const bool off = (ridden_->flags & kFlagRideable) == 0 || ridden_->dead ||
                     (board_ && runtime_.kurt().health() == 0);
    if (off) {
        dismount();
        return;
    }

    if (!board_ && !bomber_) {
        updateWalker();
    }
}

void Rides::follow() {
    // C# Rides.Follow (Rides.cs:69-86): each step, after the ticks. Kurt moves each
    // step and the XD2 each tick, so without this every other frame draws them apart.
    Player* kurt = runtime_.player_;
    if (ridden_ == nullptr || board_ || kurt == nullptr) {
        return;
    }

    if (bomber_) {
        // Kurt where the XE is.
        kurt->feet = ridden_->position;
        kurt->yaw = ridden_->yaw;
        return;
    }

    ridden_->position = kurt->feet;
    ridden_->yaw = wrap360(kurt->yaw);
}

void Rides::rideWalker(MdkObject* obj) {
    // C# Rides.RideWalker (Rides.cs:92-96).
    obj->flags |= kFlagRideable;
    mountWalker(obj);
}

bool Rides::mount(MdkObject* obj) {
    // TryMount's dispatch (Rides.cs:135-152) for one object, its touch scan left out.
    if (obj == nullptr || obj->dead || ridden_ != nullptr) {
        return false;
    }
    if (typeIn(obj->type_name, kWalkers, sizeof(kWalkers) / sizeof(kWalkers[0]))) {
        rideWalker(obj);
        return true;
    }
    if ((obj->flags & kFlagRideable) == 0) {
        return false;
    }
    if (typeIs(obj->type_name, kBoard)) {
        mountBoard(obj);
        return ridden_ == obj;
    }
    if (typeIn(obj->type_name, kBombers, sizeof(kBombers) / sizeof(kBombers[0]))) {
        mountBomber(obj);
        return ridden_ == obj;
    }
    return false;
}

bool Rides::hides(const MdkObject* obj) const {
    // C# Rides.Hides (Rides.cs:99): `Bomber != null && obj == Ridden && Bomber.HidesXe`.
    return bomber_ && obj == ridden_ && bomberRide_.hidesXe();
}

void Rides::lost(MdkObject* obj) {
    // C# Rides.Lost (Rides.cs:105-118).
    if (obj != ridden_) {
        return;
    }

    const bool hurts = !board_;
    dismount();
    if (hurts) {
        runtime_.kurt().hurt(kLostDamage);
    }
}

void Rides::tryMount() {
    // C# Rides.TryMount (Rides.cs:120-150): the first touched object that is alive and
    // rideable, by type; firing, Kurt gets on nothing.
    Player* kurt = runtime_.player_;
    if (kurt == nullptr || kurt->combat().firing()) {
        return;
    }

    const std::vector<MdkObject*>& touched = kurt->touched();
    for (size_t i = 0; i < touched.size(); i++) {
        MdkObject* obj = touched[i];
        if (obj == nullptr || obj->dead || (obj->flags & kFlagRideable) == 0) {
            continue;
        }

        if (typeIs(obj->type_name, kBoard)) {
            mountBoard(obj);
            return;
        }

        if (typeIn(obj->type_name, kWalkers, sizeof(kWalkers) / sizeof(kWalkers[0])) &&
            kurt->onFloor) {
            mountWalker(obj);
            return;
        }

        if (typeIn(obj->type_name, kBombers, sizeof(kBombers) / sizeof(kBombers[0]))) {
            mountBomber(obj);
            return;
        }
    }
}

void Rides::mountBoard(MdkObject* obj) {
    // C# Rides.MountBoard (Rides.cs:156-163, package #10): the board becomes a floor
    // only (0x800900: Kurt passes through it, and it carries him no more), the scope
    // goes, and the board's step replaces Kurt's walking.
    Player* kurt = runtime_.player_;
    if (kurt == nullptr) {
        return;
    }
    ridden_ = obj;
    obj->flags = (obj->flags | kBoardFlagsSet) & ~kBoardFlagsClear;
    kurt->leaveSniper();
    board_.reset(new Snowboard(runtime_, *kurt, obj));
    Player::RideHook hook;
    hook.context = board_.get();
    hook.step = &Snowboard::step;
    kurt->setRideHook(hook);
}

void Rides::mountWalker(MdkObject* obj) {
    // C# Rides.MountWalker (Rides.cs:161-174): Kurt goes where it is, facing its way,
    // and walks it (without strafing or jumping). His moves ignore it (the solids skip
    // the ridden object, platforms.cpp), so he doesn't collide with it.
    Player* kurt = runtime_.player_;
    if (kurt == nullptr) {
        return;
    }
    ridden_ = obj;
    obj->flags |= kFlagRidden;
    kurt->stopFiring();
    kurt->feet = obj->position;
    kurt->yaw = obj->yaw;
    kurt->forwardSpeed = 0.0f;
    kurt->strafeSpeed = 0.0f;
    kurt->walkMode = Player::WalkRiding;
    kurt->visible = false;
}

void Rides::mountBomber(MdkObject* obj) {
    // C# Rides.MountBomber (Rides.cs:182-190): the XE (also in the air) -- Kurt isn't
    // drawn and goes with it; the Bomber's update is his ride step (Kurt.Ride).
    Player* kurt = runtime_.player_;
    if (kurt == nullptr) {
        return;
    }
    ridden_ = obj;
    kurt->stopFiring();
    kurt->visible = false;
    bomber_ = true;
    bomberRide_.start(obj);
    Player::RideHook hook;
    hook.context = &bomberRide_;
    hook.step = Bomber::stepTrampoline;
    kurt->setRideHook(hook);
}

void Rides::updateWalker() {
    // C# Rides.UpdateWalker (Rides.cs:187-216): the XD2 goes with Kurt (the tick's copy
    // of his feet and yaw), animating while it moves; its sound while the keys are
    // held; firing sounds the alarm.
    Player* kurt = runtime_.player_;
    if (kurt == nullptr) {
        return;
    }
    MdkObject* walker = ridden_;
    walker->position = runtime_.kurtPosition();
    walker->yaw = runtime_.kurtYaw();
    const bool moving = kurt->forwardSpeed != 0.0f || kurt->turnRate != 0.0f;
    walker->animation_rate = moving ? MdkObject::AnimationFps : 0.0f;

    audio::SoundMixer& mixer = runtime_.mixer();
    const Player::RideKeys& keys = kurt->rideKeys();
    if (keys.steer && !mixer.isVoicePlaying(walkerSound_)) {
        walkerSound_ = mixer.play(kWalkerSound, audio::SoundMixer::New);
        mixer.setVolume(walkerSound_, kWalkerSoundVolume);
    } else if (!keys.steer) {
        mixer.stopVoice(walkerSound_);
        walkerSound_ = 0;
    }

    if (keys.fire) {
        walker->flags |= kFlagFiring;
        mixer.play(kAlarmSound, audio::SoundMixer::Once);
        runtime_.alarmTicks = kAlarmTicks;
    } else if ((walker->flags & kFlagFiring) != 0) {
        walker->flags &= ~kFlagFiring;
        runtime_.alarmTicks = 0;
    }
}

void Rides::dismount() {
    // C# Rides.Dismount (Rides.cs:228-251). The board is thrown on (Snowboard::getOff,
    // which also takes the RideHook back); from the XE Kurt drops where it is
    // (Player::getOffBoard) and it shows again. The XD2's ridden flag (0x80000) is left
    // set, as the C#'s is.
    Player* kurt = runtime_.player_;
    if (board_) {
        board_->getOff();
        board_.reset();
    } else if (bomber_) {
        // Kurt drops from where the XE is, and it shows again.
        ridden_->flags &= ~MdkObject::FlagNotSolid;
        if (kurt != nullptr) {
            kurt->getOffBoard(0.0f, 0.0f);
        }
        bomber_ = false;
    } else if (ridden_ != nullptr) {
        runtime_.mixer().stopVoice(walkerSound_);
        walkerSound_ = 0;
        if (kurt != nullptr) {
            kurt->jumpOff();
        }
    }

    ridden_ = nullptr;
}

}  // namespace scripts
}  // namespace mdk

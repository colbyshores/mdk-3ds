// ============================================================================
// player_sniper.cpp — sniper mode, the C#'s partial class Kurt.Sniper.cs
// (kurt.gd _enter_sniper, leave_sniper, _update_sniper; godot-mdk
// docs/gameplay.md "Sniper mode"). See player.h ("Sniper mode") and scope.h.
//
// One file per C# partial, so the sniper mode is one delimited place in the
// movement half: the sniper key, entering and leaving, and the controls. The
// sidestep (Kurt.SniperMove) is in player.cpp beside the walk whose constants
// and Accelerate it shares.
//
// NOT HERE, ON PURPOSE:
//   * The rounds (gap package #6): Scope::updateClip asks the hook
//     setSniperHooks binds (the C#'s Kurt.SniperFire = ScriptRuntime.FireSniper)
//     whether a round slot is free; a null hook fires every shot, the C#'s
//     `SniperFire ?? (_ => true)`.
//   * The demo's beta moves (Kurt.Beta.cs: the helmet on/off around the scope):
//     Kurt.BetaMoves is false in the port (the beta moves are not ported), so the
//     C#'s two BetaMoves branches are taken the way they are false.
// ============================================================================

#include "game/player.h"

namespace mdk {

namespace {

// Falling this fast (or rising) ends sniper mode (Kurt.Sniper.cs:24-25).
const float kSniperFallSpeed = -30.0f;
const float kSniperRiseSpeed = 1.0f;

const char kSniperOnSound[] = "SNIPERON";
const char kSniperOffSound[] = "SNIPEROFF";
const char kBreathSound[] = "BREATH";
const char kZoomSound[] = "ZOOM";
const char kReloadSound[] = "SNIPRELD";
const char kShotSound[] = "SNIPERSHOT";

} // namespace

bool Player::canSnipe() const {
    // C# Kurt.CanSnipe (Kurt.Sniper.cs:61): on a floor, not sliding (Kurt.Slide.cs,
    // package 8), alive, not knocked down, not dead or throwing.
    return onFloor && !_sliding && health() != 0 && !combat_.knocked() &&
           state() != Kurt::Dead && state() != Kurt::Throw;
}

void Player::enterSniper() {
    // C# Kurt.EnterSniper (Kurt.Sniper.cs:43-59), in its order: he stops firing (whose
    // LeaveSniper is a no-op here: not sniping yet), stands, the scope resets, SNIPERON
    // and the BREATH loop.
    if (!canSnipe()) {
        return;
    }
    stopFiring();
    _sniping = true;
    forwardSpeed = 0.0f;
    strafeSpeed = 0.0f;
    turnRate = 0.0f;
    _scope.reset();
    setState(Kurt::Still);
    if (_mixer != nullptr) {
        _mixer->play(kSniperOnSound);
        _breathVoice = _mixer->playLooped(kBreathSound);
    }
}

void Player::leaveSniper(SniperExit exit) {
    // C# Kurt.LeaveSniper (Kurt.Sniper.cs:64-81): the two loops stop, the sidestep
    // stops, and SNIPEROFF only when the sniper key left it.
    if (!_sniping) {
        return;
    }
    _sniping = false;
    strafeSpeed = 0.0f;
    if (_mixer != nullptr) {
        _mixer->stopVoice(_breathVoice);
        _mixer->stopVoice(_zoomVoice);
    }
    _breathVoice = 0;
    _zoomVoice = 0;
    if (exit == SniperWithSound && _mixer != nullptr) {
        _mixer->play(kSniperOffSound);
    }
}

bool Player::updateSniper(const Input& input, bool turbo, float delta) {
    // C# Kurt.UpdateSniper (Kurt.Sniper.cs:84-131). The sniper key toggles the mode (the
    // key is Pressed, so holding it toggles once); leaving by the key returns false, so
    // the rest of Update runs on that step. Then the ways out that are not the key --
    // death, or falling (or rising) off the floor -- and, while sniping, the controls,
    // the sidestep and the clip, and the step is a sniper one: nothing else of Update
    // runs (no items, no chain gun, no state machine; only the state's clock).
    if (pressed(input, kKeySniper)) {
        if (_sniping) {
            leaveSniper(SniperWithSound);
            // `if (!BetaMoves || frameCount(HelmetOff) == 0) return false;` -- BetaMoves
            // is false here, so the demo's helmet (state 900) never comes off.
            return false;
        }
        // `if (BetaMoves && CanSnipe && frameCount(HelmetOn) != 0) PutHelmetOn()`: false.
        enterSniper();
    }

    if (_sniping && (health() == 0 || (!onFloor && (verticalSpeed < kSniperFallSpeed ||
                                                    verticalSpeed > kSniperRiseSpeed)))) {
        leaveSniper();
    }

    if (!_sniping) {
        return false;
    }

    sniperControls(input, turbo, delta);
    sniperMove(input, turbo, delta);
    const Scope::Trigger trigger = input.isDown(kKeyFire) ? Scope::Held : Scope::Released;
    const int32_t clip = _scope.updateClip(combat_, trigger, _sniperFire, delta);
    if ((clip & Scope::ClipLoaded) != 0 && _mixer != nullptr) {
        _mixer->play(kReloadSound);
    }
    if ((clip & Scope::ClipFired) != 0 && _mixer != nullptr) {
        _mixer->play(kShotSound);
    }

    _stateTime += delta;
    return true;
}

void Player::sniperControls(const Input& input, bool turbo, float delta) {
    // C# Kurt.SniperControls (Kurt.Sniper.cs:135-175, 0x467384), in its order: the turn
    // and forward/back keys (and the step's mouse) turn and tilt the view, the zoom keys
    // and the wheel zoom (the ZOOM loop runs while the zoom moves), and the item keys
    // pick the ammo -- unless the wheel moved this step (it zooms here, so it does not
    // pick ammo too, kurt.gd _wheel_used; the item keys' Pressed is then not consulted).
    // The step's mouse is zero in the port's frontends, which take it per frame in
    // mouseLook (the C#'s MouseLook does the same at above-60 Hz); it is kept for the
    // harnesses that leave it in.
    const float turn = input.axis(kKeyTurnRight, kKeyTurnLeft);
    const float tilt = input.axis(kKeyBack, kKeyForward);
    const float mouseX = input.mouseX * kMouseDegrees;
    const float mouseY = input.mouseY * kMouseDegrees;
    yaw += _scope.look(turn, tilt, mouseX, mouseY, turbo ? Scope::TurboOn : Scope::TurboOff, delta);
    _mouseTurnScope = true;

    _scope.wheel(input.wheel);
    const Scope::Zooming zooming = _scope.updateZoom(input.axis(kKeyZoomOut, kKeyZoomIn), delta);
    if (_mixer != nullptr) {
        if (zooming == Scope::Moving && !_mixer->isVoicePlaying(_zoomVoice)) {
            _zoomVoice = _mixer->playLooped(kZoomSound);
        } else if (zooming == Scope::Still && _zoomVoice != 0) {
            _mixer->stopVoice(_zoomVoice);
            _zoomVoice = 0;
        }
    }

    if (input.wheel != 0.0f) {
        return;
    }
    if (pressed(input, kKeyItemNext)) {
        _scope.selectAmmo(combat_, 1);
    }
    if (pressed(input, kKeyItemPrevious)) {
        _scope.selectAmmo(combat_, -1);
    }
}

} // namespace mdk

#pragma once

// ============================================================================
// input.h — the game's input vocabulary (the C# Mdk.Engine.Platform.Input).
//
// The shared game code reads THIS; the platform layer (SDL on Linux, held
// buttons on the 3DS) fills it. No SDL or libctru type crosses here.
//
// It is a latched state, not a poll: `down` holds which actions are held
// across frames, and the relative-mouse motion accumulates into mouseX/mouseY
// until the consumer takes it. One mouse count is 0.15 degrees; the consumer
// (the player / follow camera) applies that scale, exactly as the C# does.
// ============================================================================

#include <stdint.h>

namespace mdk {

// The game's actions (the C# Input.Key). The movement subset is what the first
// player port reads; the rest are declared so the platform's binding map is
// complete and the enum is stable as more lands.
enum Key {
    kKeyForward = 0,
    kKeyBack,
    kKeyStrafeLeft,
    kKeyStrafeRight,
    kKeyTurnLeft,
    kKeyTurnRight,
    kKeyJump,
    kKeyTurbo,
    kKeyFire,
    kKeyUseItem,
    kKeyItemNext,
    kKeyItemPrevious,
    kKeyEscape,
    // Sniper mode on and off (the C#'s Key.Sniper: the pad's left trigger, the right
    // mouse button). Player's sniper step reads it (game/player_sniper.cpp); the scope's
    // overlay and view are the frontends' (gap package #7).
    kKeySniper,
    // The scope's zoom (the C#'s Key.ZoomIn / Key.ZoomOut: Page Up / =, Page Down / -,
    // the pad's d-pad up/down). Read in sniper mode only.
    kKeyZoomIn,
    kKeyZoomOut,
    kKeyCount
};

// The latched input state the game reads each frame.
struct Input {
    bool down[kKeyCount] = {false};
    float mouseX = 0.0f;   // accumulated relative motion, raw counts
    float mouseY = 0.0f;
    // The C# Input.Wheel: wheel notches since the last game step (positive away from
    // the user), which zoom the scope in sniper mode (Scope::wheel). The frontend that
    // fills it clears it after each step, as the C#'s ClearMouse does.
    float wheel = 0.0f;

    bool isDown(Key key) const { return down[key]; }

    // The C# Kurt.Axis: +1 when the positive action is held, -1 when the
    // negative one is, 0 when both or neither.
    float axis(Key positive, Key negative) const {
        return (down[positive] ? 1.0f : 0.0f) - (down[negative] ? 1.0f : 0.0f);
    }

    // Consume the accumulated mouse (the C# TakeMouseX / TakeMouseY).
    float takeMouseX() {
        const float x = mouseX;
        mouseX = 0.0f;
        return x;
    }
    float takeMouseY() {
        const float y = mouseY;
        mouseY = 0.0f;
        return y;
    }
};

} // namespace mdk

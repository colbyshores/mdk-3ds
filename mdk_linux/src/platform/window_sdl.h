#pragma once

// ============================================================================
// window_sdl.h — the Linux platform's window.
//
// The SDL types stay inside the platform tree. The shared game code sees
// only the game's own vocabulary.
// ============================================================================

#include <stdint.h>

#include "game/input.h"

namespace mdk {

struct Window {
    // The platform's window handle. It is a void* so that no SDL type
    // appears in a shared header; the platform layer casts it.
    void* handle = nullptr;
    int32_t width = 0;
    int32_t height = 0;
};

struct WindowDesc {
    const char* title = "MDK";
    int32_t width = 1280;
    int32_t height = 960;
};

// Create the window. Returns false on failure.
bool windowCreate(Window* window, const WindowDesc* desc);

// Destroy the window and the SDL subsystems.
void windowDestroy(Window* window);

// Pump the platform's events, translating them into the game's input. Returns
// false when the application was asked to close. Held keys set `input->down`;
// relative mouse motion accumulates into `input->mouseX/mouseY`.
bool windowPump(Window* window, Input* input);

// Grab or release the mouse (relative mode) for look controls.
void windowCaptureMouse(Window* window, bool on);

} // namespace mdk

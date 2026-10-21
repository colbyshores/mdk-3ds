#pragma once

// ============================================================================
// window_sdl.h — the Linux platform's window.
//
// The SDL types stay inside the platform tree. The shared game code sees
// only the game's own vocabulary.
// ============================================================================

#include <stdint.h>

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

// Pump the platform's events. Returns false when the application was asked
// to close.
bool windowPump(Window* window);

} // namespace mdk

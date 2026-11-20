#pragma once

// ============================================================================
// window_sdl.h — the Linux platform's window.
//
// The SDL types stay inside the platform tree. The shared game code sees
// only the game's own vocabulary.
// ============================================================================

#include <stdint.h>

#include <string>

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

// The menus' keys (the C# MenuKey, Input.cs:121-130: the arrows, Return /
// keypad Enter / Space to accept, Escape to go back): pressed since the last
// take (a key repeat is a press, as the C#'s), Left/Right held, and `any` --
// the C#'s AnyPressed: any key or mouse button pressed, repeats not counted.
// windowPump records them; the menus, the briefing and the pause menu read them.
struct MenuKeyState {
    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;
    bool accept = false;
    bool back = false;
    bool leftHeld = false;
    bool rightHeld = false;
    bool any = false;
};

// The menu keys pumped since the last call; the presses then clear, the held
// state stays.
void windowTakeMenuKeys(MenuKeyState* out);
// THE SAVE KEYS AND THE TYPING (package #19). F2 (quick save) and F9 (quick load) are
// the C#'s Key.QuickSave / Key.QuickLoad (Mdk.Engine Input.cs:84-85), and the save
// prompt reads menu-key presses and the characters typed (Input.WasPressed(MenuKey.*),
// Input.Typed). They are PRESSES since the last take, not held state, so they live
// beside the game's Input rather than in its held-key table.
enum TypingPress : uint32_t {
    kPressEnter = 1u << 0,      // Return, keypad Enter (MenuKey.Accept)
    kPressEscape = 1u << 1,     // MenuKey.Back
    kPressBackspace = 1u << 2,
    kPressDelete = 1u << 3,
    kPressLeft = 1u << 4,
    kPressRight = 1u << 5,
    kPressHome = 1u << 6,
    kPressEnd = 1u << 7,
    kPressUp = 1u << 8,
    kPressDown = 1u << 9,
    kPressSpace = 1u << 10,     // also MenuKey.Accept
    kPressQuickSave = 1u << 11, // F2
    kPressQuickLoad = 1u << 12, // F9
};

// The presses and the text typed since the last call, then forgotten.
void windowTakeTyping(std::string* typed, uint32_t* presses);

// Text input on or off (the prompt's name; SDL delivers typed text only while on).
void windowTextInput(Window* window, bool on);

} // namespace mdk

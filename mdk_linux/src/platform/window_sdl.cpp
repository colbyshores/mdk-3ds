// ============================================================================
// window_sdl.cpp — the Linux platform's window.
//
// The window owns the SDL_Window and the event pump. The renderer owns the
// Vulkan surface, because the surface needs the VkInstance, which the
// renderer creates. The two are wired together by the frontend, which
// passes the SDL_Window* through MdkRenderDesc::surface.
//
// SDL is NOT hidden from the game code: the shared code sees the game's own
// input vocabulary (Key, MenuKey, RawKey, Pointer) and the platform layer
// translates. The SDL types stay in this tree.
// ============================================================================

#include "platform/window_sdl.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdio>

namespace mdk {

bool windowCreate(Window* window, const WindowDesc* desc) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::printf("[window] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    SDL_Window* sdl = SDL_CreateWindow(desc->title, desc->width, desc->height,
                                       SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!sdl) {
        std::printf("[window] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return false;
    }
    window->handle = sdl;
    window->width = desc->width;
    window->height = desc->height;
    // Typed text (names of saved games, cheats): on from the start and never off, as
    // the C#'s Window does (Window.cs:71-75), so TOOSCARYFORME reaches the level and
    // SEETHEWHOLEGAME's keys the main menu (gap package #21).
    SDL_StartTextInput(sdl);
    return true;
}

void windowDestroy(Window* window) {
    if (window && window->handle) {
        SDL_DestroyWindow(static_cast<SDL_Window*>(window->handle));
        window->handle = nullptr;
    }
    SDL_Quit();
}

void windowCaptureMouse(Window* window, bool on) {
    if (window && window->handle) {
        SDL_SetWindowRelativeMouseMode(static_cast<SDL_Window*>(window->handle), on);
    }
}

namespace {

// The default bindings (the C# Input.DefaultKeys: the movement subset, fire and use).
int keyForScancode(SDL_Scancode sc) {
    switch (sc) {
        case SDL_SCANCODE_W:
        case SDL_SCANCODE_UP:
            return kKeyForward;
        case SDL_SCANCODE_S:
        case SDL_SCANCODE_DOWN:
            return kKeyBack;
        case SDL_SCANCODE_A:
            return kKeyStrafeLeft;
        case SDL_SCANCODE_D:
            return kKeyStrafeRight;
        case SDL_SCANCODE_LEFT:
            return kKeyTurnLeft;
        case SDL_SCANCODE_RIGHT:
            return kKeyTurnRight;
        case SDL_SCANCODE_SPACE:
            return kKeyJump;
        case SDL_SCANCODE_LSHIFT:
        case SDL_SCANCODE_RSHIFT:
            return kKeyTurbo;
        case SDL_SCANCODE_LCTRL:
        case SDL_SCANCODE_RCTRL:
            return kKeyFire;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_E:
            return kKeyUseItem;
        // The C# Input.DefaultKeys' item keys: Tab and ] select the next item, [ the
        // previous one. (Its 1-5 slot keys have no action in input.h yet.)
        case SDL_SCANCODE_TAB:
        case SDL_SCANCODE_RIGHTBRACKET:
            return kKeyItemNext;
        case SDL_SCANCODE_LEFTBRACKET:
            return kKeyItemPrevious;
        case SDL_SCANCODE_ESCAPE:
            return kKeyEscape;
        // The C# Input.DefaultKeys' scope zoom: Page Up and = zoom in, Page Down and -
        // zoom out (read in sniper mode only).
        case SDL_SCANCODE_PAGEUP:
        case SDL_SCANCODE_EQUALS:
            return kKeyZoomIn;
        case SDL_SCANCODE_PAGEDOWN:
        case SDL_SCANCODE_MINUS:
            return kKeyZoomOut;
        // The C# Input.DefaultKeys' Key.Down (Input.cs:92): noclip's descent.
        case SDL_SCANCODE_Q:
            return kKeyDown;
        default:
            return -1;
    }
}

// The keys bound to more than one control, held while ANY of their controls is: the C#'s
// Input.IsDown is the union of its key set and its mouse-button set, so letting go of the
// mouse button while Ctrl is still down must not stop the gun (nor RCtrl's release while
// LCtrl is held). One bit per control.
uint32_t fireHeld = 0;  // 1 LCtrl, 2 RCtrl, 4 left mouse button
uint32_t useHeld = 0;   // 1 Return, 2 E
uint32_t nextHeld = 0;  // 1 Tab, 2 ]
uint32_t zoomInHeld = 0;   // 1 Page Up, 2 =
uint32_t zoomOutHeld = 0;  // 1 Page Down, 2 -

// The save keys and the typing since the last windowTakeTyping (package #19).
std::string g_typed;
uint32_t g_presses = 0;

uint32_t pressFor(SDL_Scancode sc) {
    switch (sc) {
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER: return kPressEnter;
        case SDL_SCANCODE_ESCAPE: return kPressEscape;
        case SDL_SCANCODE_BACKSPACE: return kPressBackspace;
        case SDL_SCANCODE_DELETE: return kPressDelete;
        case SDL_SCANCODE_LEFT: return kPressLeft;
        case SDL_SCANCODE_RIGHT: return kPressRight;
        case SDL_SCANCODE_HOME: return kPressHome;
        case SDL_SCANCODE_END: return kPressEnd;
        case SDL_SCANCODE_UP: return kPressUp;
        case SDL_SCANCODE_DOWN: return kPressDown;
        case SDL_SCANCODE_SPACE: return kPressSpace;
        case SDL_SCANCODE_F2: return kPressQuickSave;
        case SDL_SCANCODE_F9: return kPressQuickLoad;
        default: return 0;
    }
}

void hold(Input* input, int key, uint32_t* held, uint32_t bit, bool down) {
    *held = down ? (*held | bit) : (*held & ~bit);
    input->down[key] = *held != 0;
}

// The menu keys since the last windowTakeMenuKeys (C# Input.SetKey's MenuKeys).
MenuKeyState menuKeys;

void menuKey(SDL_Scancode sc, bool down, bool repeat) {
    if (down && !repeat) {
        menuKeys.any = true;
    }
    switch (sc) {
        case SDL_SCANCODE_UP:
            menuKeys.up = menuKeys.up || down;
            break;
        case SDL_SCANCODE_DOWN:
            menuKeys.down = menuKeys.down || down;
            break;
        case SDL_SCANCODE_LEFT:
            menuKeys.left = menuKeys.left || down;
            menuKeys.leftHeld = down;
            break;
        case SDL_SCANCODE_RIGHT:
            menuKeys.right = menuKeys.right || down;
            menuKeys.rightHeld = down;
            break;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
        case SDL_SCANCODE_SPACE:
            menuKeys.accept = menuKeys.accept || down;
            break;
        case SDL_SCANCODE_ESCAPE:
        case SDL_SCANCODE_AC_BACK:
            menuKeys.back = menuKeys.back || down;
            break;
        default:
            break;
    }
}

} // namespace

void windowTakeTyping(std::string* typed, uint32_t* presses) {
    typed->swap(g_typed);
    g_typed.clear();
    *presses = g_presses;
    g_presses = 0;
}

void windowTextInput(Window* window, bool on) {
    if (!window || !window->handle) {
        return;
    }
    SDL_Window* sdl = static_cast<SDL_Window*>(window->handle);
    if (on) {
        SDL_StartTextInput(sdl);
    } else {
        SDL_StopTextInput(sdl);
    }
}

// Pump the platform's events, translating them into the game's input.
bool windowPump(Window* window, Input* input) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_EVENT_QUIT:
                return false;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP: {
                if (!input) {
                    break;
                }
                const bool down = event.type == SDL_EVENT_KEY_DOWN;
                const SDL_Scancode sc = event.key.scancode;
                menuKey(sc, down, event.key.repeat);
                // A press for the save keys and the prompt: the edit keys repeat, F2
                // and F9 do not (one save, one load per press).
                const uint32_t press = pressFor(sc);
                if (down && press != 0 &&
                    !(event.key.repeat && (press & (kPressQuickSave | kPressQuickLoad)))) {
                    g_presses |= press;
                }
                const int key = keyForScancode(sc);
                if (key == kKeyFire) {
                    hold(input, kKeyFire, &fireHeld, sc == SDL_SCANCODE_LCTRL ? 1u : 2u, down);
                } else if (key == kKeyUseItem) {
                    hold(input, kKeyUseItem, &useHeld, sc == SDL_SCANCODE_RETURN ? 1u : 2u, down);
                } else if (key == kKeyItemNext) {
                    hold(input, kKeyItemNext, &nextHeld, sc == SDL_SCANCODE_TAB ? 1u : 2u, down);
                } else if (key == kKeyZoomIn) {
                    hold(input, kKeyZoomIn, &zoomInHeld, sc == SDL_SCANCODE_PAGEUP ? 1u : 2u, down);
                } else if (key == kKeyZoomOut) {
                    hold(input, kKeyZoomOut, &zoomOutHeld, sc == SDL_SCANCODE_PAGEDOWN ? 1u : 2u,
                         down);
                } else if (key >= 0) {
                    input->down[key] = down;
                }
                break;
            }
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                    menuKeys.any = true;
                }
                // The C# Input.DefaultButtons: the left button fires, the right one is
                // the sniper key. (The wheel is SDL_EVENT_MOUSE_WHEEL, below.)
                if (input && event.button.button == SDL_BUTTON_LEFT) {
                    hold(input, kKeyFire, &fireHeld, 4u,
                         event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
                } else if (input && event.button.button == SDL_BUTTON_RIGHT) {
                    input->down[kKeySniper] = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                }
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                // The C# Input.AddWheel (Window.cs:338): the notches add up for the
                // scope's zoom (Input::wheel, Scope::wheel; positive away from the
                // user), and a notch presses its binding until the next game step --
                // up the previous item, down the next (Input.DefaultButtons). The
                // frontend clears both after each step (the C#'s ClearMouse).
                if (input) {
                    const float notches = event.wheel.y;
                    input->wheel += notches;
                    if (notches > 0.0f) {
                        input->wheelKeys |= 1u << kKeyItemPrevious;
                    } else if (notches < 0.0f) {
                        input->wheelKeys |= 1u << kKeyItemNext;
                    }
                }
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (input) {
                    input->mouseX += event.motion.xrel;
                    input->mouseY += event.motion.yrel;
                }
                break;
            case SDL_EVENT_TEXT_INPUT:
                g_typed += event.text.text;
                break;
            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                if (window) {
                    window->width = event.window.data1;
                    window->height = event.window.data2;
                }
                break;
            default:
                break;
        }
    }
    return true;
}

void windowTakeMenuKeys(MenuKeyState* out) {
    *out = menuKeys;
    const bool leftHeld = menuKeys.leftHeld;
    const bool rightHeld = menuKeys.rightHeld;
    menuKeys = MenuKeyState();
    menuKeys.leftHeld = leftHeld;
    menuKeys.rightHeld = rightHeld;
}

} // namespace mdk

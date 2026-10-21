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

// The default bindings (the C# Input.DefaultKeys movement subset).
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
        case SDL_SCANCODE_ESCAPE:
            return kKeyEscape;
        default:
            return -1;
    }
}

} // namespace

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
                const int key = keyForScancode(event.key.scancode);
                if (key >= 0) {
                    input->down[key] = (event.type == SDL_EVENT_KEY_DOWN);
                }
                break;
            }
            case SDL_EVENT_MOUSE_MOTION:
                if (input) {
                    input->mouseX += event.motion.xrel;
                    input->mouseY += event.motion.yrel;
                }
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

} // namespace mdk

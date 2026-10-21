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

// Pump the platform's events. Returns false when the application was asked
// to close.
bool windowPump(Window* window) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_EVENT_QUIT:
                return false;
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

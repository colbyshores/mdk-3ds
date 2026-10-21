// ============================================================================
// main.cpp — the Linux frontend.
//
// It mirrors mdk_3ds/src/main_3ds.cpp: find the data, load level 3, build
// the arena view, and draw it every frame from a camera that frames the
// arena. The game logic (Kurt, the follow camera, the HUD) lands with the
// game port; until then the camera framing is the same standstill the 3DS
// frontend uses, so both targets show the same picture from the same code.
// ============================================================================

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>

#include "render/render.h"
#include "render/camera_builder.h"
#include "game/level_data.h"
#include "game/level_view.h"
#include "game/follow_camera.h"
#include "game/hud_view.h"
#include "game/player.h"
#include "game/input.h"
#include "formats/mdk_data.h"
#include "formats/bni.h"
#include "formats/file_system.h"
#include "platform/filesystem_posix.h"
#include "platform/window_sdl.h"
#include "util/math_lut.h"

int main(int argc, char** argv) {
    // --headless: render with no window. The golden-frame harness needs it,
    // and it is the only way to run where the display server cannot present
    // a Vulkan swapchain.
    bool headless = false;
    int maxFrames = -1;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--headless") == 0) {
            headless = true;
        } else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            maxFrames = std::atoi(argv[++i]);
        }
    }

    // The trig tables MUST be initialised before any fast* call. An
    // uninitialised table is all zeroes, so every trig call returns 0 --
    // which looks like a game that renders but never moves.
    mdk::mathLutInit();

    // The formats library reaches the filesystem through this seam, and the
    // case-insensitive path resolution needs it BEFORE any data file is
    // read. So it is installed first.
    mdk::formats::setFileSystem(mdk::platformFileSystem());

    mdk::Window window;
    if (!headless) {
        mdk::WindowDesc windowDesc;
        windowDesc.title = "MDK";
        windowDesc.width = 1280;
        windowDesc.height = 960;
        if (!mdk::windowCreate(&window, &windowDesc)) {
            return 1;
        }
    }

    MdkRenderDesc desc;
    desc.width = window.width;
    desc.height = window.height;
    desc.surface = window.handle;   // the SDL_Window, for the Vulkan surface
    desc.headless = headless;
    desc.vsync = true;
    desc.validation = true;
    desc.enhanced_look = true;      // the Linux build does the enhanced look
    desc.hidden_width = 1280;
    desc.hidden_height = 960;

    MdkRenderer renderer = mdk_render_create(&desc);
    if (!renderer) {
        std::printf("[main] renderer creation FAILED\n");
        mdk::windowDestroy(&window);
        return 1;
    }
    std::printf("[main] backend: %s\n", mdk_render_backend_name(renderer));

    // ---------------------------------------------------------------------
    // The level. The same sequence the 3DS frontend runs.
    // ---------------------------------------------------------------------
    mdk::formats::MdkData data = mdk::formats::MdkData::find(mdk::formats::fileSystem());
    mdk::LevelData level;
    mdk::LevelView view;
    mdk::HudView hud;
    bool hudReady = false;
    const mdk::formats::Arena* arena = nullptr;
    if (data.found()) {
        std::printf("[main] data: %s\n", data.dir().c_str());
        if (mdk::LevelData::load(&level, &data, 3)) {
            const int32_t start = level.dti().startArena();
            if (start >= 0 && start < (int32_t)level.dti().arenas().size()) {
                arena = level.arenaNamed(level.dti().arenas()[start].name);
            }
            if (!arena && !level.arenas().empty()) {
                arena = level.arenas()[0];
            }
            if (arena) {
                std::printf("[main] arena: %s (%d arenas)\n", arena->name().c_str(),
                            (int)level.arenas().size());
                if (view.load(renderer, &level, arena)) {
                    std::printf("[main] view: %d batches, %d vertices, %d triangles\n",
                                view.batchCount(), view.vertexCount(), arena->triangleCount());
                    // The sky panorama (C# Viewer.CreatePanorama): the DTI's
                    // sky index texture through the base palette.
                    const mdk::formats::Texture& sky = level.dti().sky();
                    const uint8_t* pal = level.dti().palette().rgba();
                    mdk::Panorama pano;
                    pano.sky = mdk_render_create_index_texture(renderer, sky.width(), sky.height(),
                                                               sky.indices(), pal);
                    pano.mirror_sky = pano.sky;
                    pano.palette = mdk_render_create_palette(renderer, pal);
                    pano.wrap_width = (float)level.dti().skyWrapWidth();
                    pano.horizon_row = (float)level.dti().skyHorizonRow();
                    pano.offset = (float)level.dti().skyOffset();
                    pano.height = (float)sky.height();
                    pano.top_colour = (uint32_t)level.dti().skyTopColor();
                    pano.bottom_colour = (uint32_t)level.dti().skyBottomColor();
                    mdk_render_set_panorama(renderer, &pano);
                    std::printf("[main] sky: %dx%d wrap=%d horizon=%d offset=%d top=%d bottom=%d\n",
                                sky.width(), sky.height(), level.dti().skyWrapWidth(),
                                level.dti().skyHorizonRow(), level.dti().skyOffset(),
                                level.dti().skyTopColor(), level.dti().skyBottomColor());

                    // The HUD sprites (C# Viewer: TRAVERSE/TRAVSPRT.BNI),
                    // indexed through the level's palette like the C# HudView.
                    std::vector<uint8_t> spriteBytes;
                    mdk::formats::Bni sprites;
                    if (data.read("TRAVERSE/TRAVSPRT.BNI", &spriteBytes) &&
                        mdk::formats::Bni::parse(&sprites, std::move(spriteBytes)) &&
                        hud.load(renderer, sprites, level.dti().palette())) {
                        hudReady = true;
                        std::printf("[main] hud: loaded\n");
                    } else {
                        std::printf("[main] hud: FAILED\n");
                    }
                } else {
                    std::printf("[main] view: FAILED\n");
                    arena = nullptr;
                }
            }
        } else {
            std::printf("[main] level 3: FAILED to load\n");
        }
    } else {
        std::printf("[main] data: NOT FOUND\n");
    }

    // Kurt at the level's spawn, facing the start angle. The follow camera's
    // arena pitch is the game's default (4 degrees), so with no input the view
    // is the same framing the static camera produced.
    mdk::Input input;
    mdk::Player player;
    bool playerReady = false;
    if (arena) {
        const float* start = level.dti().startPosition();
        player.feet = mdk::Vec3{start[0], start[1], start[2]};
        player.yaw = level.dti().startAngle();
        player.arenaPitch = mdk::follow_camera::kDefaultPitch;
        playerReady = true;
        mdk::windowCaptureMouse(&window, true);
    }

    const float kStep = 1.0f / 60.0f; // the C# fixed simulation step
    float pending = 0.0f;
    auto last = std::chrono::steady_clock::now();

    uint32_t frame = 0;
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        const float elapsed = std::chrono::duration<float>(now - last).count();
        last = now;

        if (!headless && !mdk::windowPump(&window, &input)) {
            break;
        }

        MdkFrameParams params;
        params.elapsed = elapsed;
        params.frame_index = frame;
        params.clear_colour = 0xFF0F1218u;
        params.present = true;
        params.screenshot_path = nullptr;

        if (playerReady) {
            // The C# frame order: the mouse look is per frame, the simulation
            // runs in fixed steps, and the camera is rebuilt from Kurt's feet
            // and facing each frame. The BSP camera-clear (C# Clear/Sight)
            // that keeps the camera out of walls lands with the collision port.
            player.mouseLook(input);
            pending += elapsed;
            if (pending > kStep * 10.0f) {
                pending = kStep * 10.0f; // never spiral after a stall
            }
            while (pending >= kStep) {
                player.update(input, kStep);
                pending -= kStep;
            }
            mdk::Camera camera;
            mdk::buildFollowCamera(&camera, player.feet, player.facing(),
                                   player.arenaPitch + player.lookPitch,
                                   mdk::follow_camera::kDefaultPivot, 1280.0f / 960.0f);
            mdk_render_set_camera(renderer, &camera);
        }

        mdk_render_begin_frame(renderer, &params);
        if (arena) {
            view.draw();
        }
        if (hudReady) {
            // A stand-in HUD state until the player port lands: full health,
            // a few inventory slots, and a target bar, so every HUD element
            // is on screen for the visual check.
            mdk::HudState state;
            state.health = 100;
            state.slots = {{1, 5}, {2, 12}, {3, 1}};
            state.selected = 0;
            state.barHealth = 300;
            state.barMax = 900;
            hud.tick();
            mdk_render_canvas_begin(renderer);
            hud.draw(renderer, state);
            mdk_render_canvas_end(renderer);
        }
        mdk_render_end_frame(renderer);

        frame++;
        if (maxFrames > 0 && (int)frame >= maxFrames) {
            break;
        }
    }

    int32_t draws = 0;
    int32_t tris = 0;
    mdk_render_stats(renderer, &draws, &tris);
    std::printf("[main] exiting after %u frames (last frame: %d draws, %d triangles)\n",
                frame, draws, tris);
    mdk_render_destroy(renderer);
    mdk::windowDestroy(&window);
    return 0;
}

// ============================================================================
// main.cpp — the Linux frontend.
//
// It finds the data, loads level 3, builds the arena view, and drives Kurt and
// the follow camera (input, movement, collision, vertical physics, camera pitch
// and clearance) every frame. The game logic is live on Linux only for now: the
// 3DS frontend (mdk_3ds/src/main_3ds.cpp) still frames the arena with the
// standstill camera until its input layer lands, so the two frontends are
// deliberately at different phases rather than mirroring each other.
// ============================================================================

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <climits>
#include <memory>
#include <random>

#include "audio/sound_bank.h"
#include "formats/cmi.h"
#include "render/render.h"
#include "render/camera_builder.h"
#include "game/level_data.h"
#include "game/level_view.h"
#include "game/follow_camera.h"
#include "game/hud_view.h"
#include "game/player.h"
#include "game/input.h"
#include "game/arena_space.h"
#include "formats/mdk_data.h"
#include "formats/bni.h"
#include "formats/file_system.h"
#include "platform/filesystem_posix.h"
#include "platform/window_sdl.h"
#include "scripts/script_runtime.h"
#include "util/math_lut.h"

namespace {

// A fresh 32-bit seed for an ordinary run. The C# uses its modern unseeded
// Random; the C++ runtime currently exposes the oracle-compatible seeded
// legacy generator, so each construction gets fresh entropy instead.
int32_t freshScriptSeed() {
    std::random_device source;
    const uint32_t bits = static_cast<uint32_t>(source());
    int32_t seed = 0;
    std::memcpy(&seed, &bits, sizeof(seed));
    return seed;
}

} // namespace

int main(int argc, char** argv) {
    // --headless: render with no window. The golden-frame harness needs it,
    // and it is the only way to run where the display server cannot present
    // a Vulkan swapchain.
    bool headless = false;
    int maxFrames = -1;
    bool scriptSeedSet = false;
    int32_t scriptSeed = 0;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--headless") == 0) {
            headless = true;
        } else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            maxFrames = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--seed") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "[main] --seed needs a value\n");
                return 1;
            }
            char* end = nullptr;
            const long value = std::strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || value < INT_MIN || value > INT_MAX) {
                std::fprintf(stderr, "[main] invalid --seed: %s\n", argv[i]);
                return 1;
            }
            scriptSeed = static_cast<int32_t>(value);
            scriptSeedSet = true;
        }
    }

    // The C# Viewer's ordinary runtime uses an unseeded modern Random. The
    // C++ port currently has only the oracle-compatible seeded legacy
    // generator, so an ordinary run gets a fresh entropy seed and a test can
    // request a reproducible one with --seed.

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
    mdk::formats::Cmi cmi;
    mdk::audio::SoundBank bank;
    bool scriptAssetsReady = false;
    bool hudReady = false;
    const mdk::formats::Arena* arena = nullptr;
    if (data.found()) {
        std::printf("[main] data: %s\n", data.dir().c_str());
        if (mdk::LevelData::load(&level, &data, 3)) {
            // The scripts and their sounds are level assets, just as in the
            // C# Viewer. They are kept alive for the runtime below.
            std::vector<uint8_t> cmiBytes;
            if (!data.read("TRAVERSE/LEVEL3/LEVEL3.CMI", &cmiBytes)) {
                std::printf("[main] scripts: CMI did not read\n");
            } else if (!mdk::formats::Cmi::parse(&cmi, cmiBytes.data(),
                                                static_cast<int32_t>(cmiBytes.size()))) {
                std::printf("[main] scripts: CMI did not parse\n");
            } else if (!mdk::audio::SoundBank::forLevel(&bank, data, 3)) {
                std::printf("[main] scripts: sound bank did not load\n");
            } else {
                bank.preload();
                scriptAssetsReady = true;
                std::printf("[main] scripts: assets loaded\n");
            }

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

    // The BSP world Kurt moves through: the arenas reachable from the start
    // (the C# Viewer adds only IsReachable ones -- a hidden arena whose bounds
    // overlap the spawn column would otherwise steal the arena selection).
    mdk::ArenaSpace space;
    for (const mdk::formats::Arena* a : level.arenas()) {
        if (level.isReachable(a->name())) {
            space.add(a);
        }
    }

    // Kurt at the level's spawn, dropped 3 units onto the floor (the C#
    // Viewer StartDrop), facing the start angle. The follow camera owns its own
    // pitch smoothing, starting at the game's default (4 degrees). Used for the
    // first spawn and every respawn after a death: the C# reloads the level on
    // death, and with no scripts the geometry is static, so resetting Kurt and the
    // camera to the start is the same fresh start.
    mdk::Input input;
    mdk::Player player;
    mdk::FollowCamera follow;
    std::unique_ptr<mdk::scripts::ScriptRuntime> runtime;
    bool playerReady = false;
    auto spawnPlayer = [&]() {
        const float* start = level.dti().startPosition();
        player = mdk::Player();
        player.feet = mdk::Vec3{start[0], start[1], start[2] + 3.0f};
        player.yaw = level.dti().startAngle();
        player.setSpace(&space);
        follow = mdk::FollowCamera();
        // Pin Kurt's arena to the one around the spawn (the C# SetSolid of the
        // scripts' current arena). Until the scripts land it is set once here, so
        // bottom() -- and so the fall-out death -- still resolves his arena when he
        // drops below its bounds instead of losing it and falling forever.
        const std::string* here =
            space.arenaAt(mdk::Vec{player.feet.x, player.feet.y, player.feet.z});
        if (here) {
            space.setSolid(*here);
        }

        // The C# reloads the level and constructs a fresh ScriptRuntime when
        // Kurt respawns. Recreate the runtime here too, so its object list,
        // tick count and random stream start over with the player.
        if (scriptAssetsReady) {
            const int32_t runtimeSeed = scriptSeedSet ? scriptSeed : freshScriptSeed();
            runtime.reset(new mdk::scripts::ScriptRuntime(&level, &cmi, runtimeSeed, &bank, &player));
            runtime->option = 0;
            std::printf("[main] scripts: runtime ready (seed %d)\n", runtimeSeed);
        }
    };
    if (arena) {
        spawnPlayer();
        playerReady = true;
        mdk::windowCaptureMouse(&window, true);
    }

    const float kStep = 1.0f / 60.0f; // the C# fixed simulation step
    const float kAspect = 1280.0f / 960.0f;
    // The beat a dead Kurt is left frozen before respawning, standing in for the
    // death animation and the ~4.25 s skull fade (K_BANG, SkullFade/SkullEnd) that
    // are not ported; the HUD shows the skull meanwhile.
    const float kRespawnDelay = 4.0f;
    float deathTimer = 0.0f;
    float pending = 0.0f;
    auto last = std::chrono::steady_clock::now();

    uint32_t frame = 0;
    // Build the camera once up front so the very first frame has a valid matrix
    // even if no simulation step fits in it (a zero camera would collapse every
    // vertex to the clip origin and draw nothing).
    mdk::Camera camera;
    if (playerReady) {
        // Seed the matrix with the NON-clearing placement: the C# places no
        // camera before its first step (FollowCamera.Update, which runs the
        // clearance, is step 1), so seeding with the cleared build would shove
        // Kurt mid-air at spawn before any physics has run.
        mdk::buildFollowCamera(&camera, player.feet, player.facing(),
                               mdk::follow_camera::kDefaultPitch, mdk::follow_camera::kDefaultPivot,
                               kAspect);
    }
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
            // The C# frame order (Viewer.Frame): the mouse look is per frame
            // (Kurt's yaw and the camera's look offset), then the simulation runs
            // in fixed steps, and EACH step updates Kurt and then the camera
            // (UpdateKurt then UpdateCamera) — so the pitch smoothing and the
            // clearance (C# Clear/Sight) run once per step, not once per frame.
            // A frame faster than the step runs no step and reuses the camera the
            // last step placed (the C# blends between steps; that is not ported).
            player.mouseLook(input);
            follow.mouseLook(input);
            pending += elapsed;
            if (pending > kStep * 10.0f) {
                pending = kStep * 10.0f; // never spiral after a stall
            }
            while (pending >= kStep) {
                player.update(input, kStep);
                // The C# Viewer order is Kurt, then the scripts, then the
                // camera (Viewer.cs:505-535). ScriptRuntime keeps its own
                // 1/30-second clock, so this 1/60-second call completes a
                // script tick every second step.
                if (runtime) {
                    runtime->update(kStep);
                }
                // The pitch the camera eases to (Viewer.cs:746-747): a live
                // camera_track request wins; otherwise it is the scripts'
                // current arena's rest pitch. Without the runtime, retain the
                // older geometric-arena stand-in.
                float target = mdk::follow_camera::kDefaultPitch;
                if (runtime) {
                    target = runtime->cameraTrackTicks > 0
                                 ? runtime->cameraTrackPitch
                                 : level.arenaPitch(runtime->currentArena());
                } else {
                    const std::string* here =
                        space.arenaAt(mdk::Vec{player.feet.x, player.feet.y, player.feet.z});
                    if (here) {
                        target = level.arenaPitch(*here);
                    }
                }
                follow.update(&camera, &space, player, target, kStep, kAspect);
                // The C# refreshes the collision world's solid arenas after
                // the camera step (Viewer.cs:533), so the camera sees the
                // previous step's arena just as the oracle does.
                if (runtime) {
                    space.setSolid(runtime->currentArena());
                }
                pending -= kStep;
            }
            // Death: update() froze Kurt (it returns when dead). After the death
            // beat, respawn him at the start (the C# reloads the level; the static
            // geometry makes a reset the same fresh start) and re-seed the camera.
            if (player.dead) {
                deathTimer += elapsed;
                if (deathTimer >= kRespawnDelay) {
                    spawnPlayer();
                    deathTimer = 0.0f;
                    mdk::buildFollowCamera(&camera, player.feet, player.facing(),
                                           mdk::follow_camera::kDefaultPitch,
                                           mdk::follow_camera::kDefaultPivot, kAspect);
                }
            } else {
                deathTimer = 0.0f;
            }
            mdk_render_set_camera(renderer, &camera);
        }

        mdk_render_begin_frame(renderer, &params);
        if (arena) {
            view.draw();
        }
        if (hudReady) {
            // The health and the skull are Kurt's now (the player port landed); the
            // inventory slots and the target bar stay stand-ins until the items and
            // the shooting land, so every HUD element is still on screen.
            mdk::HudState state;
            state.health = player.health;
            state.dead = player.dead;
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

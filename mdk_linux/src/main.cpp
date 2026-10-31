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
#include <deque>
#include <map>
#include <memory>
#include <random>
#include <string>

#include "audio/sound_bank.h"
#include "formats/cmi.h"
#include "render/render.h"
#include "render/camera_builder.h"
#include "game/level_data.h"
#include "game/level_view.h"
#include "game/follow_camera.h"
#include "game/hud_view.h"
#include "game/kurt_sprite.h"
#include "game/object_draw.h"
#include "game/object_view.h"
#include "game/player.h"
#include "game/input.h"
#include "game/arena_space.h"
#include "formats/mdk_data.h"
#include "formats/bni.h"
#include "formats/file_system.h"
#include "platform/filesystem_posix.h"
#include "platform/window_sdl.h"
#include "render/material_resolver.h"
#include "render/surface_cache.h"
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
    std::string capturePath;
    int captureFrame = 0;
    int hiddenWidth = 1280;
    int hiddenHeight = 960;
    int levelNumber = 3;
    bool fixedStep = false;
    bool hasStartOverride = false;
    mdk::Vec3 startOverride{};
    bool hasYawOverride = false;
    float yawOverride = 0.0f;

    auto optionValue = [](const char* arg, const char* name) -> const char* {
        const size_t length = std::strlen(name);
        if (std::strncmp(arg, name, length) != 0 || arg[length] != '=') {
            return nullptr;
        }
        return arg + length + 1;
    };

    for (int i = 1; i < argc; i++) {
        const char* value = nullptr;
        if (std::strcmp(argv[i], "--headless") == 0) {
            headless = true;
        } else if (std::strcmp(argv[i], "--fixed-step") == 0) {
            fixedStep = true;
        } else if ((value = optionValue(argv[i], "--capture")) != nullptr) {
            capturePath = value;
            if (captureFrame == 0) {
                captureFrame = 1;
            }
        } else if ((value = optionValue(argv[i], "--capture-frame")) != nullptr) {
            captureFrame = std::atoi(value);
        } else if ((value = optionValue(argv[i], "--size")) != nullptr) {
            int w = 0;
            int h = 0;
            if (std::sscanf(value, "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0) {
                std::fprintf(stderr, "[main] invalid --size: %s (want WxH)\n", value);
                return 1;
            }
            hiddenWidth = w;
            hiddenHeight = h;
        } else if ((value = optionValue(argv[i], "--level")) != nullptr) {
            levelNumber = std::atoi(value);
            if (levelNumber < 3 || levelNumber > 8) {
                std::fprintf(stderr, "[main] --level must be 3-8\n");
                return 1;
            }
        } else if ((value = optionValue(argv[i], "--at")) != nullptr) {
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
            float yaw = 0.0f;
            const int fields = std::sscanf(value, "%f,%f,%f,%f", &x, &y, &z, &yaw);
            if (fields < 3) {
                std::fprintf(stderr, "[main] invalid --at: %s (want x,y,z[,yaw])\n", value);
                return 1;
            }
            startOverride = mdk::Vec3{x, y, z};
            hasStartOverride = true;
            hasYawOverride = fields == 4;
            yawOverride = yaw;
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
    desc.hidden_width = hiddenWidth;
    desc.hidden_height = hiddenHeight;

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
    // The GPU textures and palettes a resolved surface needs, shared by the arena
    // view and the object pass exactly as the C# shares ONE MaterialResolver
    // between them, so a texture an arena and an object both use is one texture
    // (render/surface_cache.h).
    mdk::SurfaceCache surfaces(renderer);
    mdk::LevelView view;
    mdk::HudView hud;
    mdk::KurtSprite kurtSprite;
    mdk::formats::Cmi cmi;
    mdk::audio::SoundBank bank;
    bool scriptAssetsReady = false;
    bool hudReady = false;
    bool kurtSpriteReady = false;
    const mdk::formats::Arena* arena = nullptr;
    if (data.found()) {
        std::printf("[main] data: %s\n", data.dir().c_str());
        if (mdk::LevelData::load(&level, &data, levelNumber)) {
            // The scripts and their sounds are level assets, just as in the
            // C# Viewer. They are kept alive for the runtime below.
            char cmiPath[128];
            std::snprintf(cmiPath, sizeof(cmiPath), "TRAVERSE/LEVEL%d/LEVEL%d.CMI",
                          levelNumber, levelNumber);
            std::vector<uint8_t> cmiBytes;
            if (!data.read(cmiPath, &cmiBytes)) {
                std::printf("[main] scripts: CMI did not read\n");
            } else if (!mdk::formats::Cmi::parse(&cmi, cmiBytes.data(),
                                                static_cast<int32_t>(cmiBytes.size()))) {
                std::printf("[main] scripts: CMI did not parse\n");
            } else if (!mdk::audio::SoundBank::forLevel(&bank, data, levelNumber)) {
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
                if (view.load(renderer, &level, arena, &surfaces)) {
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

                    // The HUD and Kurt's sprites (C# Viewer:
                    // TRAVERSE/TRAVSPRT.BNI), indexed through the level's
                    // palette like the C# HudView and KurtSprite.
                    std::vector<uint8_t> spriteBytes;
                    mdk::formats::Bni sprites;
                    if (data.read("TRAVERSE/TRAVSPRT.BNI", &spriteBytes) &&
                        mdk::formats::Bni::parse(&sprites, std::move(spriteBytes))) {
                        if (hud.load(renderer, sprites, level.dti().palette())) {
                            hudReady = true;
                            std::printf("[main] hud: loaded\n");
                        } else {
                            std::printf("[main] hud: FAILED\n");
                        }
                        if (kurtSprite.load(renderer, sprites, level.dti().palette())) {
                            kurtSpriteReady = true;
                            // The level's SNI archives carry the slide and
                            // snowboard frames (C# Viewer: KurtSprite.Add over
                            // SoundBank.Animation).
                            for (int i = 0; i < mdk::KurtSprite::levelAnimationCount(); i++) {
                                const char* name = mdk::KurtSprite::levelAnimationName(i);
                                mdk::formats::SpriteAnimation animation;
                                if (bank.animation(name, &animation)) {
                                    kurtSprite.add(renderer, name, animation);
                                }
                            }
                            std::printf("[main] kurt sprite: loaded\n");
                        } else {
                            std::printf("[main] kurt sprite: FAILED\n");
                        }
                    } else {
                        std::printf("[main] sprites: FAILED\n");
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
        if (hasStartOverride) {
            player.feet = startOverride;
            player.yaw = hasYawOverride ? yawOverride : level.dti().startAngle();
        } else {
            player.feet = mdk::Vec3{start[0], start[1], start[2] + 3.0f};
            player.yaw = level.dti().startAngle();
        }
        player.setSpace(&space);
        if (kurtSpriteReady) {
            player.setFrameCounts(kurtSprite.frameCounts());
        }
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

    // ---------------------------------------------------------------------
    // The object pass: the C#'s Viewer._objects, Viewer._looks and DrawObjects.
    //
    // The layout cache is keyed by (model, palette) and the models belong to the
    // level, so it lives as long as the level does -- which here is the process:
    // this frontend loads one level and never changes it. A level change would
    // call ObjectView::clear() next to LevelData::load's pose-cache
    // invalidation, and would rebuild the resolver with the new level's DTI.
    //
    // The shading is the original look's, as LevelView's is: the shared game code
    // draws the original look on both targets, and the enhanced look lives behind
    // the Linux-only header the game never includes.
    // ---------------------------------------------------------------------
    mdk::MaterialResolver objectResolver(&level.dti(), mdk::Shading::Original);
    mdk::ObjectView objectView(objectResolver);
    mdk::ObjectDraw objectDraw;
    mdk::ObjectStream objectStream;
    std::map<std::string, mdk::ObjectLook> objectLooks;

    // An arena's palette and textures for its objects (C# Viewer.ObjectLook):
    // the arena's own when it has one, the level's for an object outside every
    // arena. Cached by arena name, exactly as the C# caches its looks.
    auto objectLook = [&](const std::string& name) -> const mdk::ObjectLook& {
        std::map<std::string, mdk::ObjectLook>::iterator found = objectLooks.find(name);
        if (found != objectLooks.end()) {
            return found->second;
        }
        mdk::ObjectLook look;
        const mdk::formats::Arena* owner = level.arenaNamed(name);
        if (owner) {
            look.palette = &level.paletteOf(owner);
            look.archives = level.archivesOf(owner);
        } else {
            look.palette = &level.dti().palette();
            look.archives.push_back(&level.levelTextures());
        }
        return objectLooks.emplace(name, look).first->second;
    };

    const float kStep = 1.0f / 60.0f; // the C# fixed simulation step
    const float kHudStep = 1.0f / 30.0f; // the C# HUD blink's 30 Hz boundary
    const float kAspect = 1280.0f / 960.0f;
    // The beat a dead Kurt is left frozen before respawning. The C# skull fade
    // runs at SkullFade = 2*Ticks = 60/s from HurtFlash 0 to SkullEnd 255
    // (Kurt.Combat.cs:26-27,238-253). This frontend still has no menu, save or
    // level end, so it respawns in place when that fade completes rather than
    // leaving the level as the oracle does.
    const float kSkullFadePerSecond = 60.0f;
    const float kSkullEnd = 255.0f;
    const float kRespawnDelay = kSkullEnd / kSkullFadePerSecond;
    float deathTimer = 0.0f;
    float hudPending = 0.0f;
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
        // A golden capture MUST NOT depend on how long the host took between
        // frames. --fixed-step advances exactly the C#'s 1/60-second test step
        // every loop, so the same capture frame is the same simulation state.
        const float elapsed = fixedStep ? kStep
                                        : std::chrono::duration<float>(now - last).count();
        last = now;

        if (!headless && !mdk::windowPump(&window, &input)) {
            break;
        }

        MdkFrameParams params;
        params.elapsed = elapsed;
        params.frame_index = frame;
        params.clear_colour = 0xFF0F1218u;
        params.present = true;
        const bool captureThisFrame = captureFrame > 0 && static_cast<int>(frame) + 1 == captureFrame;
        params.screenshot_path = captureThisFrame && !capturePath.empty()
                                     ? capturePath.c_str()
                                     : nullptr;

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
                // the camera step (Viewer.cs:533). This frontend currently
                // pins only the scripts' current arena; the oracle's
                // SolidArenas list also includes the active second arena,
                // whose crossing state is deferred.
                if (runtime) {
                    space.setSolid(runtime->currentArena());
                }
                // The C# advances the HUD blink once per 30 Hz boundary
                // (Viewer.cs:535-538), not once per 60 Hz frame.
                hudPending += kStep;
                if (hudPending >= kHudStep) {
                    hud.tick();
                    hudPending -= kHudStep;
                }
                pending -= kStep;
            }
            // Death: Player::update stays in updateDeath until the beat ends. The
            // frontend then respawns him at the start and re-seeds the camera.
            //
            // Scripts keep running during the beat HERE AND IN THE ORACLE: the C#
            // play tick calls _scripts.Update(Step) with no Dead guard
            // (Viewer.cs:509-512), so this is not a divergence. What actually
            // differs is what happens when the beat ENDS. The C# runs a skull fade
            // of HurtFlash/SkullFade -- SkullFade = 2*Ticks = 60/s, SkullEnd = 255
            // (Kurt.Combat.cs:26-27,238-253), so at most ~4.25 s and less if
            // HurtFlash is already partway up -- and when it completes, Died fires,
            // Game.cs:448-456 writes SaveGames.LastGame and leaves the level for
            // the menu. This stand-in has no menu, no save and no level end: it
            // respawns Kurt in place after the whole 4.25 s fade (kRespawnDelay
            // = SkullEnd/SkullFade). The C# beat can be SHORTER when HurtFlash is
            // already partway up when Kurt dies; that path is unreachable here
            // only because the alive-state damage flash is not ported, so
            // state.hurtFlash is 0 while alive. So the divergences are the
            // level-end flow and the missing HurtFlash head start, not the
            // script clock.
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
        if (runtime) {
            // C# Viewer.DrawScene draws the objects after the arena and before
            // Kurt's sprite: every object the scripts hold, in the order of its
            // list, skipping the invisible ones -- and the ones the scripts have
            // REMOVED, which the C# has taken out of its Objects list and the C++
            // runtime keeps in its deque because other objects hold pointers
            // into it.
            objectStream.reset();
            for (const mdk::scripts::MdkObject& obj : runtime->objects()) {
                if (!obj.visible || runtime->isRemoved(&obj)) {
                    continue;
                }
                // The C# hands ObjectView.Draw `blend.TransformOf(obj)`, which is
                // obj.Transform with frame blending off -- and a fixed-step
                // capture, which is what the goldens are, has it off.
                objectView.addObject(obj, objectLook(obj.arena), obj.transform(),
                                     &objectStream);
            }
            objectDraw.draw(renderer, &surfaces, objectStream);
        }
        if (kurtSpriteReady && playerReady) {
            // C# Viewer.DrawScene draws Kurt after the level, objects and
            // effects, and before the HUD.
            kurtSprite.draw(renderer, player, player.feet, camera.position, camera.forward,
                            camera.up, mdk::follow_camera::kFieldOfView, &space);
        }
        if (hudReady) {
            // The health, skull and its fade are Kurt's now. The C# starts with
            // an empty inventory and no target bar; those writers land with the
            // pickup and shooting subsystems, so the HUD must not invent them.
            mdk::HudState state;
            state.health = player.health;
            state.dead = player.dead;
            state.hurtFlash = player.dead ? deathTimer * kSkullFadePerSecond : 0.0f;
            mdk_render_canvas_begin(renderer);
            hud.draw(renderer, state);
            mdk_render_canvas_end(renderer);
        }
        mdk_render_end_frame(renderer);

        frame++;
        if (captureThisFrame) {
            break;
        }
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

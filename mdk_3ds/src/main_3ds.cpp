// ============================================================================
// main_3ds.cpp — the 3DS frontend.
//
// The 3DS is one of MDK's two frontends. It owns the platform lifecycle
// (apt, the display, the input) and drives the shared game through the
// render.h seam. It never touches a C3D type itself: every GPU call goes
// through the seam, in renderer_c3d.cpp.
//
// WHAT IT DRIVES. The flow (game/game_flow.h) decides which level plays and what
// screen comes next; the level's geometry, Kurt, the follow camera and Kurt's
// sprite are the level's own screen. The order is the C#'s (Game.Run's loop body
// and Viewer.Frame, recorded in game_flow.h's "THE TICK ORDER"): poll the
// controls once, hand the mouse to the game, run the fixed simulation steps, and
// draw once — with the camera placed by the LAST step, not by the frame.
//
// WHAT IS NOT HERE, EACH WITH ITS CAUSE:
//   * The screens the flow can ask for other than the level (menu, briefing,
//     statistics, stream, fall, end movies). Menu/ is cut (owner directive
//     2026-11-05: boot straight into a level), so the frontend starts with
//     Start::Level and treats any other screen as the end of the run rather than
//     pretending to show one.
//   * The HUD. hud_view.cpp is ported and the frontend would call it in three
//     lines, but mdk_render_canvas_quad is a NO-OP on this backend (see the
//     comment by its definition in renderer_c3d.cpp and docs/OPEN_ITEMS.md): a
//     canvas draw has to become a record in the frame's queue so the per-eye
//     replay emits it, and until that lands a HUD call here would draw nothing at
//     all and read as a ported HUD. Deferred with that cause, not stubbed.
//   * The scripts and their objects (ScriptRuntime, ObjectView). Same reason as
//     the Linux frontend's: they are a subsystem of their own.
//   * Audio. The ndsp sink is the next item after this one.
//
// The lifecycle rules are AGENTS.md P1; the frame rules are P2.
// ============================================================================

#include <3ds.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "audio/audio_3ds.h"
#include "audio/sound_bank.h"
#include "audio/sound_mixer.h"
#include "formats/bni.h"
#include "formats/case_insensitive_path.h"
#include "formats/file_system.h"
#include "formats/mdk_data.h"
#include "game/arena_space.h"
#include "game/follow_camera.h"
#include "game/game_flow.h"
#include "game/input.h"
#include "game/kurt_sprite.h"
#include "game/level_data.h"
#include "game/level_view.h"
#include "game/player.h"
#include "render/render.h"
#include "util/math_lut.h"
#include "platform/filesystem_romfs.h"
#include "platform/input_3ds.h"
#include "platform/vfp_fpscr.h"
#include "platform/log_3ds.h"

namespace {

// The game's frame timer. MDK's simulation runs at a fixed 60 Hz step, and
// the frontend accumulates real elapsed time into it.
//
// The 3DS's clock is read from the system tick counter, which is the
// platform's monotonic timer.
double nowSeconds() {
    // svcGetSystemTick() is the ARM11's cycle counter and SYSCLOCK_ARM11 is
    // its clock, so the quotient is seconds. osGetTime() is NOT used: it
    // divides by a tick rate that is only valid once the OS layer has been
    // initialised, and it returned 0 here -- a frame timer stuck at zero
    // would silently freeze the game's fixed-step accumulator.
    return (double)svcGetSystemTick() / (double)SYSCLOCK_ARM11;
}

// The deterministic frame budget for a test run: `--frames N` runs exactly N
// frames and then exits, so a host-side harness gets a reproducible run
// instead of one that has to be interrupted. 0 means "run until asked to
// stop".
uint32_t g_frameBudget = 0;

// The picture's aspect. The top screen is 400x240 LANDSCAPE; the framebuffer is
// 240x400 because the hardware rotates the picture 90 degrees on the way to the
// panel, which is why this is width/height of the LANDSCAPE mode and not of the
// framebuffer axes the renderer names (see mdk_render_canvas_width's note for the
// same inversion). The projection built with the portrait ratio would squash the
// level sideways.
constexpr float kAspect = 400.0f / 240.0f;

} // namespace

int main(int argc, char** argv) {
    // The command line. A 3DS title's arguments are short; --frames is the
    // one the test harness needs.
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            g_frameBudget = (uint32_t)strtoul(argv[i + 1], nullptr, 10);
            i++;
        }
    }

    // ---------------------------------------------------------------------
    // R1: the FPSCR's Flush-to-Zero and Default-NaN bits MUST be set before
    // any application float work, and the read-back must prove they stuck.
    //
    // This is the FIRST thing main does, before gfxInitDefault: on failure
    // we return 1 IMMEDIATELY and never enter a cleanup path, because the
    // cleanup path's own arithmetic would then run with an unverified
    // numerical mode.
    //
    // WHY: decay reaches subnormals. MDK's per-frame geometry build, the
    // script VM's arithmetic and the audio mixer's envelopes all multiply
    // and accumulate small values. FZ and DN are the chosen numerical mode.
    // Modern libctru already initialises these bits on supported entry
    // paths, but MAIN-THREAD STATE ALONE IS NOT EVIDENCE OF A WORKER'S
    // STATE, so each worker sets its own (see audio_3ds.cpp).
    // ---------------------------------------------------------------------
    if (!mdk::vfpSetFzDnMain()) {
        printf("[3ds] FZ/DN could not be set on the main thread; refusing to run.\n");
        return 1;
    }

    // R6: the trig LUT is the ONE trig path on both targets, and an
    // uninitialised table is all zeroes -- every fast* call would return 0.
    // Must run before any camera or geometry trig.
    mdk::mathLutInit();

    mdk::log3dsOpen();
    mdk::log3dsPrintf("main: FZ/DN ok (fpscr=0x%08lx)",
                      (unsigned long)mdk::vfpReadFpscr());

    gfxInitDefault();
    // The top screen's parallax barrier. MDK's flat-display stereo modes do
    // not apply to the 3DS; the hardware's own stereoscopic is used
    // instead, and the slider's depth comes from osGet3DSliderState().
    gfxSet3D(true);

    // The game's data. The RomFS holds the MDK data staged at build time,
    // and the file system seam is what makes the case-insensitive path
    // resolution work against it (AGENTS.md P5).
    const bool romfsOk = mdk::romfsFileSystemInit();
    if (romfsOk) {
        printf("[3ds] romfs mounted\n");
    } else {
        printf("[3ds] romfsInit FAILED -- the game will not find its data\n");
    }

    MdkRenderDesc desc;
    desc.width = 240;
    desc.height = 400;
    desc.surface = nullptr;   // the 3DS owns its own surface
    desc.vsync = true;
    desc.enhanced_look = false;  // the 3DS shows the ORIGINAL look

    MdkRenderer renderer = mdk_render_create(&desc);
    if (!renderer) {
        mdk::log3dsPrintf("renderer creation FAILED");
        mdk::log3dsClose();
        gfxExit();
        return 1;
    }
    mdk::log3dsPrintf("backend: %s", mdk_render_backend_name(renderer));

    // ---------------------------------------------------------------------
    // The flow, BEFORE the level: the flow is what decides which level plays
    // (C# Game owns _state.Level and hands it to the Viewer), so the frontend
    // asks it rather than naming a level of its own.
    //
    // Start::Level is the cut the owner directive of 2026-11-05 made: Menu/ is
    // not ported, so there is no menu to show and the run boots straight into a
    // level. Everything else the flow can ask for is a screen this frontend does
    // not have, and it says so and stops instead of showing the level again under
    // a different name.
    // ---------------------------------------------------------------------
    mdk::flow::FlowOptions options;
    options.start = mdk::flow::Start::Level;
    mdk::flow::GameFlow flow(options);
    mdk::flow::Screen screen = flow.handle(flow.firstEvent());
    mdk::log3dsPrintf("flow: screen=%d level=%d", (int)screen, (int)flow.state().level);
    if (screen != mdk::flow::Screen::Level) {
        mdk::log3dsPrintf("flow asked for screen %d, which this frontend cannot show "
                          "(the menu and its screens are cut); exiting", (int)screen);
        mdk_render_destroy(renderer);
        mdk::romfsFileSystemExit();
        mdk::log3dsClose();
        gfxExit();
        return 0;
    }

    // ---------------------------------------------------------------------
    // The level.
    //
    // The data is found the way the game finds it, through the file-system
    // seam -- which on the 3DS is the ROMFS tree, so the case-insensitive
    // resolution works against it (AGENTS.md P5).
    // ---------------------------------------------------------------------
    mdk::formats::MdkData data = mdk::formats::MdkData::find(mdk::formats::fileSystem());
    mdk::LevelData level;
    // The GPU textures and palettes a resolved surface needs, shared by the arena
    // view (and by the object pass, when this frontend has a script runtime to
    // draw objects with). The C# gives both views one MaterialResolver for
    // exactly this reason.
    mdk::SurfaceCache surfaces(renderer);
    mdk::LevelView view;
    // Kurt's sprites. TRAVERSE/TRAVSPRT.BNI is the same archive the HUD and the
    // scripts read (C# Viewer loads it once and passes it to all three), and the
    // level's palette indexes it, exactly as there.
    mdk::formats::Bni sprites;
    mdk::KurtSprite kurtSprite;
    bool kurtSpriteReady = false;
    const mdk::formats::Arena* arena = nullptr;
    if (data.found()) {
        mdk::log3dsPrintf("data: %s", data.dir().c_str());
        if (mdk::LevelData::load(&level, &data, flow.state().level)) {
            const int32_t start = level.dti().startArena();
            if (start >= 0 && start < (int32_t)level.dti().arenas().size()) {
                arena = level.arenaNamed(level.dti().arenas()[start].name);
            }
            if (!arena && !level.arenas().empty()) {
                arena = level.arenas()[0];
            }
            if (arena) {
                mdk::log3dsPrintf("arena: %s (%d arenas)", arena->name().c_str(),
                                  (int)level.arenas().size());
                if (view.load(renderer, &level, arena, &surfaces)) {
                    mdk::log3dsPrintf("view: %d batches, %d vertices, %d triangles",
                                      view.batchCount(), view.vertexCount(),
                                      arena->triangleCount());
                } else {
                    mdk::log3dsPrintf("view: FAILED");
                    arena = nullptr;
                }
            }
            // Kurt's frames are loaded whether the arena view worked or not: the
            // sprite is his, not the arena's, and a level with no arena geometry
            // still has him.
            std::vector<uint8_t> spriteBytes;
            if (!data.read("TRAVERSE/TRAVSPRT.BNI", &spriteBytes) ||
                !mdk::formats::Bni::parse(&sprites, std::move(spriteBytes))) {
                mdk::log3dsPrintf("sprites: TRAVSPRT.BNI did not load -- Kurt is invisible");
            } else if (kurtSprite.load(renderer, sprites, level.dti().palette())) {
                kurtSpriteReady = true;
                mdk::log3dsPrintf("sprites: Kurt's animations loaded");
            } else {
                mdk::log3dsPrintf("sprites: FAILED to decode Kurt's animations");
            }
        } else {
            mdk::log3dsPrintf("level %d: FAILED to load", (int)flow.state().level);
        }
    } else {
        mdk::log3dsPrintf("data: NOT FOUND");
        // The diagnostics, because "not found" has too many causes.
        const mdk::formats::FileSystem* fs = mdk::formats::fileSystem();
        mdk::log3dsPrintf("fs: %s", fs ? "installed" : "NULL");
        if (fs) {
            const std::string marker =
                mdk::formats::caseInsensitiveResolve(fs, "romfs:/", "TRAVERSE/TRAVSPRT.BNI");
            mdk::log3dsPrintf("marker: %s", marker.c_str());
            std::vector<std::string> names;
            const bool listed = fs->listDirectory(fs->user, "romfs:/", &names);
            mdk::log3dsPrintf("list romfs:/ -> %d, %d entries", (int)listed, (int)names.size());
            for (size_t i = 0; i < names.size() && i < 8; i++) {
                mdk::log3dsPrintf("  %s", names[i].c_str());
            }
            std::vector<uint8_t> bytes;
            const bool read = fs->readFile(fs->user, "romfs:/manifest.txt", &bytes);
            mdk::log3dsPrintf("manifest: %d, %d bytes", (int)read, (int)bytes.size());
        }
    }

    // ---------------------------------------------------------------------
    // The sounds: the level's SNI archives, the mixer, and the ndsp sink.
    //
    // THE BANK IS NOT PRELOADED, AND THAT IS THE MEMORY ANSWER. Measured, not
    // estimated, with `tick1 mixsize <level>`, which reads the element size out of
    // the store rather than restating a comment: level 3's three archives are
    // 4,390,060 bytes on disk and decode to 3,234,976 samples = 6,469,952 bytes in
    // the int16 store (12,939,904 when the store was float); level 4 is 7,878,536
    // and level 8 is 10,002,084. So the Linux frontend's
    // `bank.preload()` is NOT called here: SoundBank::get decodes a sound the first
    // time something asks for it, so the console pays for the sounds the level
    // actually plays and nothing else. What that does NOT do is bound the total: a
    // level whose scripts eventually play everything still grows without limit, and
    // halving the store did not change that, because the SNI archives stay
    // resident too (4.39 / 5.90 / 5.82 MB for these same three levels), so a fully
    // decoded level 3 is 10.86 MB and level 8 is 15.82 MB before a single triangle.
    //
    // THE CEILING THIS COMMENT USED TO INVENT, AND WHAT REPLACED IT. It said "16 MB
    // of FCRAM", which is not a number this console has (review-1 F3). The sourced
    // figures are in mdk_core/src/audio/sound.h's memory section and are not
    // restated at length here: the build asks for a 24 MiB C++ heap
    // (`__heap_size = 0x01800000`, read out of mdk.elf's crt0 header, a devkitARM
    // default this Makefile does not override), the console maps 128 MiB of FCRAM on
    // an Old 3DS (`OS_OLD_FCRAM_SIZE`, `3ds/os.h:49`), and the region the kernel
    // actually grants — the only one of the three that is a real ceiling, from
    // `osGetMemRegionSize(MEMREGION_APPLICATION)` — has never been printed by this
    // port on hardware or in the emulator. That last gap is the honest reason the
    // policy below is conservative, not a fit that was ever computed.
    //
    // Budget-and-evict stays open, named, in docs/CONTINUATION.md, and the two
    // halves of that decision are linked: freeing an archive's bytes is only
    // possible once every sound in it has decoded, which means preloading, which is
    // the opposite of the lazy policy that keeps the peak low today. It is not
    // solved by this batch; it is no longer hidden either, because the game is now
    // audible without it.
    //
    // THE MIXER IS STANDALONE HERE, and that is the C#'s shape too: Game.cs:206
    // builds the AudioDevice, Viewer.cs:194 builds the SoundMixer over the bank,
    // and Kurt gets the mixer. This frontend has no ScriptRuntime (the scripts are
    // not wired on the console yet), so the mixer lives here instead of inside one,
    // and what it can play is Kurt's own set — footsteps, the chute, the guns,
    // landing — not the scripts' ambient and object sounds. Those arrive with the
    // runtime, not before it.
    // ---------------------------------------------------------------------
    mdk::audio::SoundBank bank;
    bool bankReady = false;
    if (data.found() &&
        mdk::audio::SoundBank::forLevel(&bank, data, flow.state().level)) {
        bankReady = true;
        mdk::log3dsPrintf("audio: bank ready, %d archives, decoded on demand",
                          (int)bank.archiveCount());
    } else {
        mdk::log3dsPrintf("audio: the level's SNI archives did not load -- no sounds");
    }
    mdk::audio::SoundMixer mixer(bankReady ? &bank : nullptr);
    // The sink gets no queue query (audio_3ds.h: ndsp has no queued-frames
    // query, only a position inside the buffer now playing), so the device keeps
    // the frame's own accounting and the wave-buffer queue absorbs the jitter.
    const bool audioReady = mdk::audioOpen();
    if (audioReady) {
        mixer.device().setSink(mdk::audioSink, nullptr);
    }

    // The BSP world Kurt moves through: the arenas the level says are reachable
    // (the C# Viewer adds only IsReachable ones -- a hidden arena whose bounds
    // overlap the spawn column would otherwise steal the arena selection).
    mdk::ArenaSpace space;
    for (const mdk::formats::Arena* a : level.arenas()) {
        if (level.isReachable(a->name())) {
            space.add(a);
        }
    }

    // ---------------------------------------------------------------------
    // Kurt, and the camera that follows him.
    //
    // THE CAMERA IS THE GAME'S, not a framing stand-in: FollowCamera owns the
    // pitch smoothing, the clearance that shoves Kurt off a wall, the air tilt
    // and the climb, and it is placed by the simulation step that just ran. The
    // old frontend framed the arena's bounds because nothing else was ported;
    // that camera is gone, and with it the level's start position used as a
    // camera position.
    //
    // The spawn is the C# Viewer's: the DTI's start position RAISED by `StartDrop`
    // (3 units) above the pad, facing the start angle -- the original lands Kurt by
    // chute, so the start position is below him, not above (Viewer.cs:220, and the
    // Linux frontend's `start[2] + 3.0f`). A respawn after a death rebuilds the
    // same fresh state, because the C# reloads the level.
    // ---------------------------------------------------------------------
    mdk::Player player;
    mdk::FollowCamera follow;
    bool playerReady = false;
    // THE ONE HEALTH, taken once. The C#'s fresh Viewer seeds Kurt from the fall's
    // carry and CONSUMES it (Viewer.cs:246 -> CarryFromFall, :347-361, which does
    // `_kurt.SetHealth(carry.Health)` and then `state.Carry = null`), so the carry
    // belongs to the level that takes it and a second fresh level in the same
    // session starts whole. `play()` has already consumed it into the flow's level
    // result by the time this frontend has a Kurt to seed, which is why the value
    // is read from `levelHealth()` and not from `state().carry` (review-8 F3).
    const int32_t entryHealth = flow.levelHealth();
    bool firstSpawn = true;
    auto spawnPlayer = [&]() {
        const float* start = level.dti().startPosition();
        player = mdk::Player();
        player.feet = mdk::Vec3{start[0], start[1], start[2] + 3.0f};
        player.yaw = level.dti().startAngle();
        if (firstSpawn) {
            player.setHealth(entryHealth);
            firstSpawn = false;
        }
        player.setSpace(&space);
        player.setMixer(&mixer);  // a fresh Player has a null mixer (C# Kurt's ctor arg)
        if (kurtSpriteReady) {
            player.setFrameCounts(kurtSprite.frameCounts());
        }
        follow = mdk::FollowCamera();
        // Pin Kurt's arena to the one around the spawn, as the fresh level's start.
        // The step loop refreshes it from here on (the C# refreshes it every step,
        // Viewer.cs:533); what the spawn pin is FOR is the case the loop CANNOT
        // supply, a Kurt standing outside every arena: with no pin at all, bottom()
        // resolves nothing, the fall-out death never fires and he falls forever.
        const std::string* here = space.arenaAt(mdk::Vec{player.feet.x, player.feet.y, player.feet.z});
        if (here) {
            space.setSolid(*here);
        }
        mdk::log3dsPrintf("spawn: feet=(%.1f,%.1f,%.1f) yaw=%.1f health=%d arenas=%d",
                          player.feet.x, player.feet.y, player.feet.z, player.yaw,
                          player.health(), (int)level.arenas().size());
    };
    if (arena) {
        spawnPlayer();
        playerReady = true;
    }

    // The camera is built once up front so the very first frame has a valid
    // matrix even if no simulation step fits in it (a zero camera would collapse
    // every vertex to the clip origin and draw nothing). The NON-clearing build:
    // the C# places no camera before its first step, so seeding with the cleared
    // one would shove Kurt mid-air at spawn before any physics has run, and the
    // roll is 0 because this Player has not stepped yet (C# CameraRoll starts
    // level).
    mdk::Camera camera;
    if (playerReady) {
        mdk::buildFollowCamera(&camera, player.feet, player.facing(),
                               mdk::follow_camera::kDefaultPitch,
                               mdk::follow_camera::kDefaultPivot, kAspect, 0.0f);
    }

    // ---------------------------------------------------------------------
    // The main loop.
    //
    // aptMainLoop() MUST be serviced regularly: libctru uses it to process
    // sleep and HOME behaviour and to notice the application being closed.
    // Blocking it indefinitely on a worker is a shutdown bug (P1).
    //
    // THE STEP IS THE SIMULATION's, not the frame rate: GameFlow::stepFor is the
    // C#'s choice (the level's own 1/60 s here) and elapsedFor is the C#'s elapsed
    // for a live run (the wall seconds since the last frame). Frames faster than
    // the step run no step and redraw the last one's camera; a frame slower runs
    // several, and the pending clamp is what stops a stall becoming an unbounded
    // catch-up (AGENTS P1: a long stall -- a sleep, a load -- must not be handed to
    // the simulation as one enormous step).
    // ---------------------------------------------------------------------
    double last = nowSeconds();
    // What the AUDIO has been handed, in seconds, and not the wall clock: the
    // device's `owed` grows by rate*elapsed and `elapsed` is the CLAMPED delta,
    // so a 40 s sleep the loop clamped to 0.25 s owes 0.25 s of audio and this
    // clock says so. It is the denominator the sink's rate test needs, because
    // that test is a rate against the time the mixer was given and not against
    // a frame count (review F3 of run-20261106-062909: frame x 735/882 is the
    // same number only at exactly 60 fps). Accumulated in double for the same
    // reason `last` and `delta` are: this is a running time sum, not a hot-path
    // accumulator, and it is read once every 60 frames.
    double audioClock = 0.0;
    const float step = mdk::flow::GameFlow::stepFor(false, false, 0.0f);
    const float maxPending = step * 10.0f;  // the C#'s ten-step ceiling
    float pending = 0.0f;
    uint32_t frame = 0;
    uint32_t steps = 0;
    const char* exitReason = "aptMainLoop returned false";

    while (aptMainLoop()) {
        // The frame's clock FIRST: the look integrates over the time since the
        // previous frame, so the input poll needs the same delta the simulation is
        // about to get. Clamp it here -- the platform's floor under the game's own
        // policy -- because a 40-second sleep is not 40 seconds of walking.
        const double now = nowSeconds();
        double delta = now - last;
        last = now;
        if (delta < 0.0) delta = 0.0;
        if (delta > 0.25) delta = 0.25;
        const float elapsed = mdk::flow::GameFlow::elapsedFor(false, step, (float)delta);

        // The controls. ONE scan of HID for the whole frame, inside the poll:
        // libctru recomputes its edge bits on every hidScanInput(), so a second
        // scan anywhere in this loop would discard the first one's presses. The
        // held mask comes back from the poll for exactly that reason. The Input is
        // the frame's own, and that is correct rather than careless: the poll ADDS
        // the circle pad to its mouse and the game DRAINS it below with
        // player.mouseLook / follow.mouseLook, which run every frame and not only
        // on frames that fit a step -- so nothing accumulates across a frame that
        // runs zero steps.
        mdk::Input input;
        const u32 held = mdk::input3dsPoll(&input, elapsed);

        // The flow's events. START is the C# pad's Escape, and Escape opens the
        // menu, which is cut: the honest answer here is to stop, and the flow says
        // so (Quit -> Screen::None) rather than the frontend deciding on its own.
        // SELECT stays as the second quit chord because it is unbound in the game
        // and it is the only other way out of a build that has no menu.
        mdk::flow::Event next = mdk::flow::Event::None;
        if (held & (KEY_START | KEY_SELECT)) {
            next = mdk::flow::Event::Quit;
        }
        if (next != mdk::flow::Event::None) {
            screen = flow.handle(next);
            if (screen == mdk::flow::Screen::None) {
                exitReason = (held & KEY_SELECT) ? "SELECT pressed" : "START pressed";
                break;
            }
            mdk::log3dsPrintf("flow: screen=%d", (int)screen);
            if (screen != mdk::flow::Screen::Level) {
                exitReason = "the flow asked for a screen this frontend cannot show";
                break;
            }
        }

        if (playerReady) {
            // The C# Viewer.Frame order: the mouse look is PER FRAME (Kurt's yaw
            // and the camera's look offset), then the simulation runs in fixed
            // steps, and EACH step updates Kurt and then the camera (UpdateKurt
            // then UpdateCamera) -- so the pitch smoothing and the clearance run
            // once per step, not once per frame.
            player.mouseLook(input);
            follow.mouseLook(input);

            pending += elapsed;
            if (pending > maxPending) pending = maxPending;
            while (pending >= step) {
                player.update(input, step);
                // The pitch the camera eases to (Viewer.cs:746-747). The live
                // camera_track request and the scripts' current arena are the
                // oracle's sources and both need the script runtime, so this uses
                // the geometric arena under Kurt, which is what the runtime-less
                // Linux frontend uses for the same reason.
                float target = mdk::follow_camera::kDefaultPitch;
                const std::string* here =
                    space.arenaAt(mdk::Vec{player.feet.x, player.feet.y, player.feet.z});
                if (here) {
                    target = level.arenaPitch(*here);
                }
                follow.update(&camera, &space, player, target, step, kAspect);
                // The collision world follows him. The C# refreshes it EVERY STEP
                // (`_space.SetSolid(_scripts.SolidArenas)`, Viewer.cs:533, after
                // UpdateCamera), and the pin is sticky: while `_solid` is set,
                // move(), floorCrossing(), crosses() and sight() use it and ignore
                // the arenas around the feet (arena_space.cpp:118 and :174-181).
                // So a pin left at the spawn would keep resolving Kurt against the
                // arena he was born in after he walked into its neighbour: the walk
                // probes would find no floor there, he would fall through it, and
                // bottom() -- which DOES consider any arena holding his feet --
                // would report that neighbour's bottom and fire the fall-out death.
                // Without the scripts the geometric arena under his feet is the
                // stand-in for `SolidArenas`. It is NOT what the Linux frontend
                // does: that one refreshes from the runtime's current arena and
                // only when a runtime exists (mdk_linux/src/main.cpp:531-533), and
                // it pins once at the spawn otherwise (:366-369). The oracle's own
                // rule is neither: the scripts' pin moves only on a doorway
                // connection or a teleport (ScriptRuntime.cs:917-931, :791), so it
                // does NOT follow him down a fall column. Connections are not
                // ported and this frontend has no ScriptRuntime, so the geometric
                // refresh stays the closest available stand-in for the walking case
                // -- the one review-8 F1 measured -- at the cost of a fall-out that
                // can re-pin on 4 of the 1681 columns the review probed. Narrowing
                // it to the supported steps is a separate batch with its own walk-
                // out-of-the-arena golden, not a one-liner inside this one. The
                // pin is deliberately NOT cleared when he is outside every arena:
                // that is the fall-out case, and bottom() still has to resolve an
                // arena for the death to fire at all. It is probed AGAIN rather
                // than reusing the pitch's `here`, because the C# evaluates SetSolid
                // after UpdateCamera and the camera's clearance can shove Kurt into
                // a different arena than the one his step started in.
                const std::string* solid =
                    space.arenaAt(mdk::Vec{player.feet.x, player.feet.y, player.feet.z});
                if (solid) {
                    space.setSolid(*solid);
                }
                pending -= step;
                steps++;
            }

            // Death. The C#'s Kurt.Died writes LASTGAME and leaves for the menu
            // (Game.cs:448-456); the save and the menu are cut, so the frontend
            // respawns him on the same event, exactly as the Linux frontend does.
            // The beat itself is the oracle's -- player.died() is the step the
            // skull crosses SkullEnd, not a wall-clock timer of this frontend's.
            if (player.died()) {
                spawnPlayer();
                mdk::buildFollowCamera(&camera, player.feet, player.facing(),
                                       mdk::follow_camera::kDefaultPitch,
                                       mdk::follow_camera::kDefaultPivot, kAspect, 0.0f);
            }
        }

        // THE MIX, once per frame, after the steps that moved the listener. The C#
        // splits this across two files: the mixer's Update runs in DrawScene right
        // after the listener is taken from the camera (Viewer.cs:686-696) and the
        // device's Update runs after the screen's frame (Game.cs:334-337). Both
        // take the frame's elapsed, not the step's. It runs with no sink too, as
        // the muted C# does: the voice lifecycle has to advance or a one-shot would
        // play forever to a silent console.
        mixer.listenFromCamera(camera);
        mixer.update(elapsed);
        mixer.device().update(elapsed);
        // AFTER the device, so the clock is exactly the sum of what the device
        // was given and not of what the frame happened to measure.
        audioClock += elapsed;

        MdkFrameParams params;
        params.elapsed = elapsed;
        params.frame_index = frame;
        params.clear_colour = 0xFF102030u;
        params.present = true;
        params.screenshot_path = nullptr;

        // P2: the camera is handed to the renderer every frame, whether or not a
        // step ran, and every GPU state the frame needs is set inside
        // begin/end_frame by the backend.
        mdk_render_begin_frame(renderer, &params);
        mdk_render_set_camera(renderer, &camera);

        if (arena) {
            view.draw();
        }
        if (kurtSpriteReady && playerReady) {
            // C# Viewer.DrawScene draws Kurt after the level and before the HUD.
            kurtSprite.draw(renderer, player, player.feet, camera.position, camera.forward,
                            camera.up, mdk::follow_camera::kFieldOfView, &space);
        }
        // The HUD would be drawn here (hud_view.cpp is ported); see the header for
        // why it is not called yet: the canvas seam on this backend discards every
        // quad, so a call here would be a stub presented as a port.
        mdk_render_end_frame(renderer);
        frame++;

        if (frame == 1u) {
            mdk::log3dsPrintf("input: held=%08lX mx=%.4f my=%.4f", (unsigned long)held,
                              (double)input.mouseX, (double)input.mouseY);
            // A number the guest log and the host golden agree on. A stick hard
            // right for one 60 Hz step is 3 degrees of look, and 3 degrees at 0.15
            // degrees a count is EXACTLY 20 counts (180.0f * (1/60f) is
            // 3.0000001, and that divided by 0.15f rounds back to 20.0), which is
            // a pinned row of tests/scripts/input_3ds_tests.cpp -- pinned IN BITS,
            // 0x41A00000, so the log prints the bits and not a rounded decimal:
            // 20.000002f (0x41A00001) and 20.0f both print 20.0000, and a 1-ULP
            // VFP-vs-SSE divergence would hide behind that (review-7 F2).
            mdk::HidFrame probe;
            probe.held = mdk::kHidDUp | mdk::kHidR;
            probe.padX = 155;
            probe.padY = 0;
            mdk::Input selftest;
            mdk::input3dsApply(probe, 1.0f / 60.0f, &selftest);
            uint32_t lookBits = 0;
            memcpy(&lookBits, &selftest.mouseX, sizeof(lookBits));
            uint32_t pitchBits = 0;
            memcpy(&pitchBits, &selftest.mouseY, sizeof(pitchBits));
            mdk::log3dsPrintf("input selftest: fwd=%d fire=%d lookbits=%08X pitchbits=%08X",
                              selftest.isDown(mdk::kKeyForward) ? 1 : 0,
                              selftest.isDown(mdk::kKeyFire) ? 1 : 0,
                              (unsigned)lookBits, (unsigned)pitchBits);
            mdk::log3dsPrintf("game: player=%d sprite=%d arenas=%d step=%.6f",
                              playerReady ? 1 : 0, kurtSpriteReady ? 1 : 0,
                              (int)level.arenas().size(), (double)step);
            mdk::log3dsPrintf("audio: sink=%d bank=%d voices=%d queue=%d",
                              audioReady ? 1 : 0, bankReady ? 1 : 0,
                              (int)mixer.voices().size(), (int)mixer.device().queueFrames());
        }
        if (frame <= 5u || (frame % 60u) == 0u) {
            mdk::log3dsPrintf("frame %lu elapsed=%.4f steps=%u feet=(%.1f,%.1f,%.1f) "
                              "yaw=%.1f health=%d eye=(%.1f,%.1f,%.1f) voices=%d plays=%d",
                              (unsigned long)frame, elapsed, (unsigned)steps,
                              player.feet.x, player.feet.y, player.feet.z, player.yaw,
                              player.health(), camera.position.x, camera.position.y,
                              camera.position.z, (int)mixer.voices().size(),
                              (int)mixer.device().nextId() - 1);
        }
        if (audioReady && (frame % 60u) == 0u) {
            // The sink's own ledger, on the same cadence as the frame line. The
            // frame number is repeated HERE on purpose (review F1 of
            // run-20261106-062909): the only reading that discriminates a healthy
            // sink from a wrong-nsamples one is `submitted` against what the clock
            // owes, so the two numbers have to be on the same line to be divided.
            // `done` cannot carry that test: submitted - done == inDsp <= 4 by
            // construction. See audio_3ds.h.
            //
            // AND THE CLOCK IS ON THE LINE TOO, as seconds (review F3). Dividing
            // `submitted` by frame x 735/882 was the frame count standing in for
            // the clock, and it is only the same number at exactly 60 fps; at 30
            // fps a half-rate sink scores 1.00 that way. `secs` is the sum of the
            // elapsed values the mixer actually received, so the rate test is
            // submitted/(secs x 50) and is frame-rate independent by construction.
            //
            // THREE DECIMAL PLACES, not two (review-10 F1). This line is also the
            // instrument for the sink's own frame counter, and that test is a
            // SLOPE between two rows: dpf is an integer, so the width of the
            // result is set entirely by the rounding of `secs`. At 2 dp a 47 s
            // window carries +/-0.005 s at each end, which is +/-0.043/s — wider
            // than the residual the slope exists to resolve, and the reason three
            // batches read a fixed 20 ms start offset as instrument noise. At 3 dp
            // the same band is +/-0.004/s, which is finer than the clock pair's own
            // scatter and so is no longer the limiting error.
            const mdk::AudioStats st = mdk::audioStats();
            mdk::log3dsPrintf("audio: frame=%lu secs=%.3f submitted=%u done=%u dropped=%u "
                              "requeued=%u inDsp=%d dspframes=%u dspdropped=%u",
                              (unsigned long)frame, audioClock, (unsigned)st.submitted,
                              (unsigned)st.done,
                              (unsigned)st.dropped, (unsigned)st.requeued, st.inDsp,
                              (unsigned)st.dspFrames, (unsigned)st.dspDropped);
        }

        if (g_frameBudget != 0 && frame >= g_frameBudget) {
            exitReason = "frame budget reached";
            break;
        }
    }

    printf("[3ds] exiting after %lu frames\n", (unsigned long)frame);

    mdk::log3dsPrintf("exiting after %lu frames (%u steps): %s", (unsigned long)frame,
                      (unsigned)steps, exitReason);
    mdk_render_destroy(renderer);
    mdk::audioClose();
    mdk::romfsFileSystemExit();
    // The log is closed BEFORE gfxExit(): libctru's gfxExit() tears the SD
    // card's filesystem down, and a FILE* opened on it cannot be flushed
    // afterwards -- the final line would be silently lost.
    mdk::log3dsClose();
    gfxExit();
    return 0;
}

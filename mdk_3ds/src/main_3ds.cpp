// ============================================================================
// main_3ds.cpp — the 3DS frontend.
//
// The 3DS is one of MDK's two frontends. It owns the platform lifecycle
// (apt, the display, the input) and drives the shared game through the
// render.h seam. It never touches a C3D type itself: every GPU call goes
// through the seam, in renderer_c3d.cpp.
//
// The lifecycle rules are AGENTS.md P1.
// ============================================================================

#include <3ds.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "formats/case_insensitive_path.h"
#include "formats/file_system.h"
#include "formats/mdk_data.h"
#include "game/level_data.h"
#include "game/level_view.h"
#include "render/camera_builder.h"
#include "render/render.h"
#include "util/math_lut.h"
#include "platform/filesystem_romfs.h"
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
    // The level.
    //
    // The data is found the way the game finds it, through the file-system
    // seam -- which on the 3DS is the ROMFS tree, so the case-insensitive
    // resolution works against it (AGENTS.md P5).
    // ---------------------------------------------------------------------
    mdk::formats::MdkData data = mdk::formats::MdkData::find(mdk::formats::fileSystem());
    mdk::LevelData level;
    mdk::LevelView view;
    const mdk::formats::Arena* arena = nullptr;
    if (data.found()) {
        mdk::log3dsPrintf("data: %s", data.dir().c_str());
        if (mdk::LevelData::load(&level, &data, 3)) {
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
                if (view.load(renderer, &level, arena)) {
                    mdk::log3dsPrintf("view: %d batches, %d vertices, %d triangles",
                                      view.batchCount(), view.vertexCount(),
                                      arena->triangleCount());
                } else {
                    mdk::log3dsPrintf("view: FAILED");
                    arena = nullptr;
                }
            }
        } else {
            mdk::log3dsPrintf("level 3: FAILED to load");
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
    // The main loop.
    //
    // aptMainLoop() MUST be serviced regularly: libctru uses it to process
    // sleep and HOME behaviour and to notice the application being closed.
    // Blocking it indefinitely on a worker is a shutdown bug (P1).
    // ---------------------------------------------------------------------
    double last = nowSeconds();
    float elapsed = 0.0f;
    uint32_t frame = 0;
    const char* exitReason = "aptMainLoop returned false";

    while (aptMainLoop()) {
        hidScanInput();
        const u32 held = hidKeysHeld();
        if (held & KEY_START) {
            exitReason = "START pressed";
            break;
        }
        if (held & KEY_SELECT) {
            exitReason = "SELECT pressed";
            break;
        }

        const double now = nowSeconds();
        double delta = now - last;
        last = now;
        // Clamp the delta: a long stall (a sleep, a load) must not be
        // handed to the simulation as one enormous step. MDK's own frame
        // policy is in the shared game code; this is the platform's floor.
        if (delta < 0.0) delta = 0.0;
        if (delta > 0.25) delta = 0.25;
        elapsed = (float)delta;

        MdkFrameParams params;
        params.elapsed = elapsed;
        params.frame_index = frame;
        params.clear_colour = 0xFF102030u;
        params.present = true;
        params.screenshot_path = nullptr;

        mdk_render_begin_frame(renderer, &params);

        if (arena) {
            // THE CAMERA. The level's start position is the game's
            // KURT's start, not a camera: the game's camera follows Kurt,
            // and the game logic is not ported yet. So the camera is placed
            // to FRAME the arena's bounds, looking along the level's start
            // heading.
            //
            // The aspect is the LANDSCAPE one (400x240 on the top screen):
            // the projection is built for the landscape picture and the 90
            // degree rotation turns it into the portrait framebuffer.
            mdk::Vec3 min;
            mdk::Vec3 max;
            mdk::Camera camera;
            if (view.bounds(&min, &max)) {
                const mdk::Vec3 centre{(min.x + max.x) * 0.5f,
                                       (min.y + max.y) * 0.5f,
                                       (min.z + max.z) * 0.5f};
                const float span = (max.x - min.x) + (max.y - min.y) + (max.z - min.z);
                const float distance = span * 0.75f + 16.0f;
                const float heading = level.dti().startAngle() * 3.14159265f / 180.0f;
                const mdk::Vec3 eye{centre.x - mdk::fastCos(heading) * distance,
                                    centre.y - mdk::fastSin(heading) * distance,
                                    centre.z + distance * 0.45f};
                mdk::buildLookAtCamera(&camera, eye, centre, mdk::Vec3{0.0f, 0.0f, 1.0f},
                                       60.0f, 400.0f / 240.0f, 1.0f, distance * 8.0f);
            } else {
                const float* start = level.dti().startPosition();
                mdk::buildHeadingCamera(&camera, mdk::Vec3{start[0], start[1], start[2]},
                                        level.dti().startAngle(), 0.0f,
                                        60.0f, 400.0f / 240.0f, 1.0f, 4000.0f);
            }
            if (frame < 3) {
                mdk::log3dsPrintf("camera: eye=(%.1f,%.1f,%.1f) fwd=(%.2f,%.2f,%.2f)",
                                  camera.position.x, camera.position.y, camera.position.z,
                                  camera.forward.x, camera.forward.y, camera.forward.z);
            }
            mdk_render_set_camera(renderer, &camera);
            view.draw();
        }

        mdk_render_end_frame(renderer);
        frame++;

        if (g_frameBudget != 0 && frame >= g_frameBudget) {
            exitReason = "frame budget reached";
            break;
        }
        if (frame == 3u) {
            int32_t draws = 0;
            mdk_render_stats(renderer, &draws, nullptr);
            mdk::log3dsPrintf("draws issued: %d", (int)draws);
        }
        if (frame <= 5u || (frame % 60u) == 0u) {
            mdk::log3dsPrintf("frame %lu elapsed=%.4f", (unsigned long)frame, elapsed);
        }
    }

    printf("[3ds] exiting after %lu frames\n", (unsigned long)frame);

    mdk::log3dsPrintf("exiting after %lu frames: %s", (unsigned long)frame, exitReason);
    mdk_render_destroy(renderer);
    mdk::romfsFileSystemExit();
    // The log is closed BEFORE gfxExit(): libctru's gfxExit() tears the SD
    // card's filesystem down, and a FILE* opened on it cannot be flushed
    // afterwards -- the final line would be silently lost.
    mdk::log3dsClose();
    gfxExit();
    return 0;
}

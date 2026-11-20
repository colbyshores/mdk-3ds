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
//   * The HUD is HERE, AND IT IS NOT YET ON SCREEN. hud_view.cpp was ported long
//     before this backend could draw it, because mdk_render_canvas_quad was a
//     no-op on the Citro3D backend. The canvas is now a record in the frame's
//     queue (renderer_c3d.cpp's canvas seams), so the HUD LOADS and BLINKS like
//     the Linux frontend's and its quads are QUEUED and EMITTED — and no HUD pixel
//     has yet been measured on the panel. "Emitted" and "painted" are different
//     claims and only the first is true; the second is the open item in
//     docs/OPEN_ITEMS.md (`3ds.hud-canvas`), with the measurements that narrowed
//     it. What the HUD would show is what the port has: health, the skull and its
//     fade, the flashes, and an empty inventory -- the pickup and target-bar
//     writers land with their own subsystems and the HUD must not invent them.
//   * The ropes and the sniper scope — `_ropes.Draw` and `_sniper.Draw`
//     between the objects and Kurt in C# Viewer.DrawScene (Viewer.cs:705-706).
//     Neither is ported. The effects (`_effects.Draw`, Viewer.cs:707) ARE:
//     pieces and sprite effects draw after the objects, before Kurt.
//
// THE OBJECT PASS IS HERE. The runtime's objects are drawn between the arena and
// Kurt, which is the C#'s order (Viewer.cs:702-708), at the blended transform, and
// the one 3DS question it carries — how big the linear stream mesh has to be — is
// answered in the frame log and in docs/CONTINUATION.md rather than assumed.
//
// The lifecycle rules are AGENTS.md P1; the frame rules are P2.
// ============================================================================

#include <3ds.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <memory>
#include <new>
#include <string>

#include "audio/audio_3ds.h"
#include "audio/audio_dsp_3ds.h"
#include "audio/level_music.h"
#include "audio/sound_bank.h"
#include "audio/sound_mixer.h"
#include "formats/bni.h"
#include "formats/case_insensitive_path.h"
#include "formats/cmi.h"
#include "formats/file_system.h"
#include "formats/kurt_pack.h"
#include "formats/mdk_data.h"
#include "formats/font.h"
#include "game/arena_space.h"
#include "game/arena_views.h"
#include "game/enhanced_config.h"
#include "game/enhanced_menu.h"
#include "platform/controls_3ds.h"
#include "game/font_view.h"
#include "game/follow_camera.h"
#include "game/game_flow.h"
#include "game/hud_view.h"
#include "game/input.h"
#include "game/kurt_sprite.h"
#include "game/level_data.h"
#include "game/level_light.h"
#include "game/mod_images.h"
#include "game/level_view.h"
#include "game/effects_draw.h"
#include "game/effects_view.h"
#include "game/object_draw.h"
#include "game/object_view.h"
#include "game/player.h"
#include "render/material_resolver.h"
#include "render/render.h"
#include "render/render_c3d_stats.h"
#include "scripts/script_runtime.h"
#include "util/math_lut.h"
#include "platform/filesystem_romfs.h"
#include "platform/harness_3ds.h"
#include "render/sky_dome.h"
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

// Set by the APT lifecycle hook when the console wakes (lid opened /
// resumed): the main loop rebases its frame clock on the next frame
// (P1, AGENTS.md 11) instead of spending the suspended interval. The
// hook itself does nothing but set this; all work stays on the loop's
// own thread.
static volatile bool g_resumePending = false;

// The picture's aspect. The top screen is 400x240 LANDSCAPE; the framebuffer is
// 240x400 because the hardware rotates the picture 90 degrees on the way to the
// panel, which is why this is width/height of the LANDSCAPE mode and not of the
// framebuffer axes the renderer names (see mdk_render_canvas_width's note for the
// same inversion). The projection built with the portrait ratio would squash the
// level sideways.
constexpr float kAspect = 400.0f / 240.0f;

// The Enhanced Mode toggles' persistence (SPEC.md 7.9): the whole
// config.ini, rewritten whenever a toggle changes. The directory is
// the log's (log_3ds.cpp makes sdmc:/3ds/mdk before this can run).
bool saveEnhancedConfig(const mdk::EnhancedToggles& toggles) {
#if defined(MDK_AUTOQUIT) && defined(MDK_HW_TOGGLES)
    // A TOGGLES harness build runs a config the card does not hold: writing it
    // back would leave the override in sdmc:/3ds/mdk/config.ini for every later
    // run (review, 2026-11-18). Nothing is saved; the caller logs saved=0.
    (void)toggles;
    return false;
#endif
    FILE* config = fopen("sdmc:/3ds/mdk/config.ini", "wb");
    if (!config) {
        return false;
    }
    const std::string text = mdk::writeEnhancedConfig(toggles);
    const bool written = fwrite(text.c_str(), 1, text.size(), config) == text.size();
    fclose(config);
    return written;
}

// PHASE 0 HEAP PROBE (arena streaming). The number that matters is the
// newlib heap's, not the application region's: libctru allocates the whole
// application region at boot (heap_split.cpp explains the split), so
// osGetMemRegionFree(APPLICATION) reads 0 at every point in the run. That is
// a statement about the reservation, not about headroom. mallinfo() reports
// the allocator itself:
//   arena    bytes sbrk'd from the heap reservation so far
//   used     bytes currently handed out (uordblks)
//   free     free space inside that arena (fordblks)
// `peak` is the run's high-water mark of `used`, which is what an arena
// budget has to fit under. `lin` is the GPU heap's free bytes
// (linearSpaceFree), the other half of the split: a texture allocation fails
// with invisible geometry, not an abort, so it is logged beside the malloc
// numbers on every probe. No probe on this path allocates (log3dsPrintf
// writes through a static FILE buffer, not newlib's heap).
static uint32_t g_heapPeak = 0;
static void heapProbe(const char* tag) {
    struct mallinfo mi = mallinfo();
    const uint32_t used = (uint32_t)mi.uordblks;
    if (used > g_heapPeak) {
        g_heapPeak = used;
    }
    mdk::log3dsPrintf("mem[%s]: cap=%u arena=%u used=%u free=%u peak=%u lin=%u",
                      tag, (unsigned)envGetHeapSize(), (unsigned)mi.arena, (unsigned)used,
                      (unsigned)mi.fordblks, (unsigned)g_heapPeak, (unsigned)linearSpaceFree());
}

// THE HD PACK (docs/HD_TEXTURES_3DS_SPEC.md): built on the host by
// mdk_3ds/tools/build_hd_pack.sh and copied to the SD card. Its manifest lists
// every .t3x by key, so the pack is indexed by ModImages' own rule
// (ModImages::fromList) without scanning the card; the surface cache then hands
// each Lit surface's match to the backend (mdk_render_replace_compressed), which
// streams it under the HD budget.
const char kHdPackRoot[] = "sdmc:/3ds/mdk/hd";

// KURT'S COMPRESSED FRAMES (EnhancedToggles::kurtCompressed): his ORIGINAL
// TRAVSPRT.BNI frames as ETC1A4, made on the host by
// mdk_3ds/tools/kurt_etc1_convert.py, packed into ONE file by kurt_pack.py
// (formats/kurt_pack.h) and staged into the RomFS by stage_romfs.sh (the set is
// derived from the game data the RomFS already carries, so it ships in every
// .3dsx and .cia and the hardware loop needs no separate SD copy).
const char kKurtPackPath[] = "romfs:/kurt/kurt.pak";

// The sound bank's decoded store, bounded (see where it is set).
const size_t kDecodedSoundBudgetBytes = (size_t)6 << 20;

// The HD pack's manifest, indexed for `level` by ModImages' own rule. False
// when there is none.
bool loadCompressedManifest(const char* root, int32_t level, mdk::ModImages* out,
                            int32_t* count) {
    const std::string manifest = std::string(root) + "/manifest.txt";
    FILE* file = fopen(manifest.c_str(), "r");
    if (!file) {
        return false;
    }
    std::vector<std::string> files;
    char line[512];
    bool tooBig = false;
    while (fgets(line, sizeof(line), file)) {
        const size_t length = strlen(line);
        if (length > 0 && line[length - 1] != '\n' && !feof(file)) {
            // Longer than the buffer: no pack line is, so skip the rest of it
            // rather than parse its tail as a line of its own (review F10).
            int c;
            while ((c = fgetc(file)) != EOF && c != '\n') {
            }
            continue;
        }
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }
        // Column 8 is the size cap the pack was built with
        // (hd_pack_convert.py): the backend's budget gate assumes 512
        // (kHdLargestBytes), so a larger cap is refused, not drawn over budget.
        {
            int column = 0;
            const char* field = line;
            for (const char* p = line; *p && column < 8; p++) {
                if (*p == '\t') {
                    column++;
                    field = p + 1;
                }
            }
            if (column == 8 && atoi(field) > 512) {
                tooBig = true;
            }
        }
        char* tab = strchr(line, '\t');
        if (!tab) {
            continue;
        }
        *tab = '\0';
        files.push_back(std::string(line) + ".t3x");
    }
    fclose(file);
    if (tooBig) {
        mdk::log3dsPrintf("hd: pack built above the 512 cap the 3DS budgets for; not used");
        return false;
    }
    *out = mdk::ModImages::fromList(root, files, true, level);
    *count = (int32_t)files.size();
    return !files.empty();
}

bool loadHdPack(int32_t level, mdk::ModImages* out) {
    int32_t count = 0;
    if (!loadCompressedManifest(kHdPackRoot, level, out, &count)) {
        return false;
    }
    mdk::log3dsPrintf("hd: pack manifest %d textures (sdmc:/3ds/mdk/hd)", (int)count);
    return true;
}

// KURT'S PACK AT LEVEL LOAD (cold; AGENTS.md P5). ONE open of romfs:/kurt/kurt.pak,
// the 32-byte header, then the index and the start set in ONE contiguous read
// (kurt_pack.py lays the start set out right after the index), and the file is
// closed: the other animations are read later, a range at a time, by the
// backend's reader thread (KurtSprite's stream), never inside a frame. Every
// read is checked: no file, a short read, a bad magic/version/layout or a file
// shorter than its header says is logged ONCE here and Kurt's frames take the
// 16-bit path for the level -- nothing is retried. A bad index ENTRY costs only
// its own frame (KurtSprite::useCompressedPack) and is named here.
struct KurtPackLoad {
    mdk::formats::KurtPack pack;
    // The pack's bytes [kHeaderBytes, startEnd): the index, then the start set.
    // A staging buffer: freed as soon as the start set is imported.
    std::vector<uint8_t> front;
    int32_t opened = 0;
    int32_t bytesRead = 0;
};

bool loadKurtPack(const char* path, KurtPackLoad* out) {
    FILE* file = fopen(path, "rb");
    if (!file) {
        mdk::log3dsPrintf("sprites: no pack at %s; Kurt's frames are 16-bit", path);
        return false;
    }
    out->opened = 1;
    long length = -1;
    if (fseek(file, 0, SEEK_END) == 0) {
        length = ftell(file);
    }
    uint8_t header[mdk::formats::KurtPack::kHeaderBytes];
    bool ok = length >= 0 && fseek(file, 0, SEEK_SET) == 0 &&
              fread(header, 1, sizeof(header), file) == sizeof(header);
    if (ok) {
        out->bytesRead += (int32_t)sizeof(header);
    }
    mdk::formats::KurtPack::Header parsed;
    const mdk::formats::KurtPack::Status status =
        ok ? mdk::formats::KurtPack::parseHeader(header, sizeof(header),
                                                 length > 0x7FFFFFFFL ? 0x7FFFFFFFu
                                                                      : (uint32_t)length,
                                                 &parsed)
           : mdk::formats::KurtPack::Status::TooSmall;
    if (status != mdk::formats::KurtPack::Status::Ok) {
        fclose(file);
        mdk::log3dsPrintf("sprites: pack %s refused (%s, %ld bytes); Kurt's frames are 16-bit",
                          path, mdk::formats::KurtPack::statusName(status), length);
        return false;
    }
    // The header's bounds (parseHeader) keep this under the file's size.
    const uint32_t frontBytes = parsed.startEnd - mdk::formats::KurtPack::kHeaderBytes;
    out->front.resize(frontBytes);
    const size_t got = frontBytes > 0 ? fread(out->front.data(), 1, frontBytes, file) : 0;
    fclose(file);
    if (got != frontBytes) {
        mdk::log3dsPrintf("sprites: pack %s: short read of the index and start set (%u of %u "
                          "bytes); Kurt's frames are 16-bit",
                          path, (unsigned)got, (unsigned)frontBytes);
        std::vector<uint8_t>().swap(out->front);
        return false;
    }
    out->bytesRead += (int32_t)frontBytes;
    const uint32_t indexBytes = parsed.indexEnd() - mdk::formats::KurtPack::kHeaderBytes;
    if (mdk::formats::KurtPack::parseIndex(parsed, out->front.data(), indexBytes, &out->pack) !=
        mdk::formats::KurtPack::Status::Ok) {
        mdk::log3dsPrintf("sprites: pack %s: index refused; Kurt's frames are 16-bit", path);
        std::vector<uint8_t>().swap(out->front);
        return false;
    }
    if (out->pack.badEntries() > 0) {
        const int32_t first = out->pack.firstBadEntry();
        mdk::log3dsPrintf("sprites: pack %s: %d bad index entries (first #%d %s: %s); those "
                          "frames are 16-bit",
                          path, (int)out->pack.badEntries(), (int)first,
                          out->pack.entries()[(size_t)first].name.c_str(),
                          out->pack.firstBadReason());
    }
    return true;
}

// The surface cache's lookup into the HD pack: a Lit surface's .t3x path.
struct HdPackSource {
    mdk::ModImages* pack = nullptr;
    std::string path;
};

bool hdPackLookup(void* user, const mdk::Surface& surface, const mdk::formats::Palette* palette,
                  mdk::ColourReplacement* out) {
    HdPackSource* source = static_cast<HdPackSource*>(user);
    if (!source || !source->pack || !surface.texture || !palette) {
        return false;
    }
    const std::string& name = surface.name.empty() ? surface.texture->name() : surface.name;
    if (!source->pack->resolve(mdk::ModImages::Kind::Textures, name, *surface.texture, *palette,
                               &source->path)) {
        return false;
    }
    out->compressedPath = source->path.c_str();
    return true;
}

// With -fno-exceptions libstdc++'s operator new calls this and then aborts,
// so the handler is the only place that can name the allocation that ran
// the heap dry. Without it an OOM is a silent exit with no last line.
static void onHeapExhausted() {
    heapProbe("OOM");
    mdk::log3dsPrintf("FATAL: operator new failed -- the 24 MiB heap is exhausted");
    mdk::log3dsClose();
    abort();
}

} // namespace

int main(int argc, char** argv) {
    // P1 lifecycle (AGENTS.md 11): know when the console sleeps and
    // wakes. The hook only records the wake; the loop rebases its clock
    // and logs it there. Unregistered before return, below.
    static aptHookCookie s_lifecycleCookie;
    aptHook(&s_lifecycleCookie,
            [](APT_HookType hook, void*) {
                if (hook == APTHOOK_ONWAKEUP || hook == APTHOOK_ONRESTORE) {
                    g_resumePending = true;
                }
            },
            nullptr);
#ifdef MDK_AUTOQUIT
    // The autonomous hardware loop (harness_3ds.h): on exit, chain-load
    // the netload stub so the next 3dslink push has somewhere to land.
    // Registered first so every exit route -- including the R1 startup
    // failure's immediate return -- hands off the same way. Harness
    // builds only: a release build never sees this.
    std::atexit(mdk::harnessChainloadStub);
#endif
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
    std::set_new_handler(onHeapExhausted);
    heapProbe("boot");
    // P6 evidence: the split heap_split.cpp asks for, printed before any
    // level is loaded. `cap` is what the allocator granted main; `lin` is the
    // GPU heap's size before the renderer touches it.
    mdk::log3dsPrintf("main: heap split main=%u linear=%u (avail=%u)", (unsigned)envGetHeapSize(),
                      (unsigned)envGetLinearHeapSize(),
                      (unsigned)(envGetHeapSize() + envGetLinearHeapSize()));
    mdk::log3dsPrintf("main: FZ/DN ok (fpscr=0x%08lx)",
                      (unsigned long)mdk::vfpReadFpscr());
#ifdef MDK_AUTOQUIT
    // WHICH PAYLOAD THIS IS, in the log itself. A build without MDK_AUTOQUIT
    // links harness_3ds.cpp's no-op answers: harnessAutoquitDue() is a
    // constant false, so the run never ends and no `--- session end ---` can
    // ever appear -- and the loop then reports a console that "ran 600 s
    // without finishing" for a payload that cannot finish. That happened
    // (measured 2026-11-17: the pushed .3dsx contained this file's other
    // autoquit STRING but not netload_stub, i.e. the string without the
    // macro). The line below is the proof that reads back from a pull.
    mdk::log3dsPrintf("main: harness build, autoquit=%ds chainload=netload_stub", (int)(MDK_AUTOQUIT));
#endif

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

    // The Enhanced Mode toggles (SPEC.md 7.9): persisted on the SD card
    // so they can be flipped between runs (and, when the settings menu
    // lands, live, through mdk_render_set_enhanced_toggles + this same
    // write). A missing file is written with the defaults (all on) so
    // the knobs are discoverable; a file that does not parse keeps the
    // defaults too, and says so in the boot log.
    mdk::EnhancedToggles enhancedToggles;
    {
        char configText[1024];
        bool read = false;
        if (FILE* config = fopen("sdmc:/3ds/mdk/config.ini", "rb")) {
            const size_t got = fread(configText, 1, sizeof(configText) - 1, config);
            configText[got] = '\0';
            fclose(config);
            read = true;
        }
        if (read && mdk::parseEnhancedConfig(configText, &enhancedToggles)) {
            mdk::log3dsPrintf("config: sdmc:/3ds/mdk/config.ini loaded");
        } else {
            if (read) {
                mdk::log3dsPrintf("config: config.ini did not parse; defaults kept");
            }
            if (saveEnhancedConfig(enhancedToggles)) {
                mdk::log3dsPrintf("config: wrote default sdmc:/3ds/mdk/config.ini");
            } else {
                mdk::log3dsPrintf("config: could not write sdmc:/3ds/mdk/config.ini");
            }
        }
    }
    // THE CONTROLS (owner, 2026-11-19): sdmc:/3ds/mdk/controls.ini, one line
    // per action (platform/controls_3ds.h has the format and the defaults).
    // Cold, once, here: a missing file is the defaults and is written out once
    // so the owner can edit it; a bad line keeps that action's default and an
    // unknown key is ignored, both logged; the resolved table is one line.
    static mdk::ControlBindings controls = mdk::defaultControlBindings();
    {
        static char controlsText[4096];
        size_t got = 0;
        bool read = false;
        if (FILE* file = fopen("sdmc:/3ds/mdk/controls.ini", "rb")) {
            got = fread(controlsText, 1, sizeof(controlsText) - 1, file);
            controlsText[got] = '\0';
            fclose(file);
            read = true;
        }
        if (read) {
            mdk::ControlsReport report;
            mdk::parseControls(controlsText, got, &controls, &report);
            mdk::log3dsPrintf("controls: sdmc:/3ds/mdk/controls.ini loaded: %d actions set, %d "
                              "bad (default kept), %d unknown (ignored)%s%s",
                              (int)report.applied, (int)report.bad, (int)report.unknown,
                              report.firstProblem[0] ? "; first: " : "", report.firstProblem);
        } else {
            const std::string text = mdk::writeControls(controls);
            bool written = false;
            if (FILE* file = fopen("sdmc:/3ds/mdk/controls.ini", "wb")) {
                written = fwrite(text.c_str(), 1, text.size(), file) == text.size();
                written = fclose(file) == 0 && written;
            }
            mdk::log3dsPrintf(written ? "controls: wrote default sdmc:/3ds/mdk/controls.ini"
                                      : "controls: could not write sdmc:/3ds/mdk/controls.ini; "
                                        "defaults in use");
        }
        mdk::log3dsPrintf("controls: %s", mdk::describeControls(controls).c_str());
        if (controls.buttons[mdk::kKeySniper] != 0) {
            mdk::log3dsPrintf("controls: sniper is bound but not implemented (the sniper view "
                              "is cut); its button does nothing yet");
        }
    }

#if defined(MDK_AUTOQUIT) && defined(MDK_HW_TOGGLES)
    // THE HARNESS'S CONFIG OVERRIDE (`make AUTOQUIT=.. TOGGLES='k=v;k=v'`), applied
    // over whatever sdmc:/3ds/mdk/config.ini held and never written back (the
    // menu's save is off in such a build: saveEnhancedConfig). The value must not
    // hold ' # or , (make, the shell, the macro), nor a word that is a macro. The
    // per-toggle cost runs need one console config each and the appliance's SD
    // has no write path from the loop. A harness build only.
    {
#define MDK_HW_STR2(x) #x
#define MDK_HW_STR(x) MDK_HW_STR2(x)
        // Unknown keys are dropped by the parser, so the RESOLVED set is logged
        // after it: a typo (`enableGlow=0`) shows as a run with the glow still on,
        // and a stale -D (the Makefile does not track it) shows as the old set.
        char overrides[512];
        const int length = snprintf(overrides, sizeof(overrides), "%s", MDK_HW_STR(MDK_HW_TOGGLES));
        if (length < 0 || length >= (int)sizeof(overrides)) {
            mdk::log3dsPrintf("main: harness config override TOGGLES is too long (%d bytes); "
                              "NOT applied", length);
        } else {
            for (char* c = overrides; *c != '\0'; c++) {
                if (*c == ';') {
                    *c = '\n';
                }
            }
            mdk::parseEnhancedConfig(overrides, &enhancedToggles);
            mdk::log3dsPrintf("main: harness config override TOGGLES=%s", MDK_HW_STR(MDK_HW_TOGGLES));
            mdk::log3dsPrintf("main: resolved config shadows=%d lights=%d haze=%d mirrors=%d glow=%d "
                              "filtering=%d mipmaps=%d hd=%d lighting=%d smoothing=%d "
                              "smoothing2x2=%d glowBloom=%d flip=%d shadowMapSize=%d shadowMapRefresh=%d "
                              "texture16=%d kurtCompressed=%d",
                              (int)enhancedToggles.enableSkyShadows,
                              (int)enhancedToggles.enableFragmentLights,
                              (int)enhancedToggles.enableHaze, (int)enhancedToggles.enableMirrors,
                              (int)enhancedToggles.enableGlowBillboards,
                              (int)enhancedToggles.enableFiltering,
                              (int)enhancedToggles.enableMipmaps,
                              (int)enhancedToggles.enableHdTextures,
                              (int)enhancedToggles.enableLighting,
                              (int)enhancedToggles.enableEdgeSmoothing,
                              (int)enhancedToggles.edgeSmoothing2x2,
                              (int)enhancedToggles.glowBloom,
                              (int)enhancedToggles.shadowMapFlipBytes,
                              (int)enhancedToggles.shadowMapSize,
                              (int)enhancedToggles.shadowMapRefresh,
                              (int)enhancedToggles.texture16,
                              (int)enhancedToggles.kurtCompressed);
        }
#undef MDK_HW_STR
#undef MDK_HW_STR2
    }
#endif

#ifdef MDK_TOGGLE_WALK
    // The toggle walk (a harness aid, `make TOGGLEWALK=1`, never a release):
    // start in the ORIGINAL look and switch the configured enhancements on one
    // at a time, logging each BEFORE the frame that first draws it, so a
    // console that hangs names its culprit in the last line of mdk.log.
    const mdk::EnhancedToggles walkTarget = enhancedToggles;
    enhancedToggles.enableSkyShadows = false;
    enhancedToggles.enableFragmentLights = false;
    enhancedToggles.enableHaze = false;
    enhancedToggles.enableMirrors = false;
    enhancedToggles.enableGlowBillboards = false;
    enhancedToggles.enableFiltering = false;
    enhancedToggles.enableMipmaps = false;
    enhancedToggles.enableHdTextures = false;
    enhancedToggles.enableLighting = false;
    enhancedToggles.enableEdgeSmoothing = false;
    mdk::log3dsPrintf("walk: toggle walk build -- every enhancement off, then one at a time");
#endif

    MdkRenderDesc desc;
    desc.width = 240;
    desc.height = 400;
    desc.surface = nullptr;   // the 3DS owns its own surface
    desc.vsync = true;
    desc.enhanced_look = false;  // the 3DS shows the ORIGINAL look
    desc.enhanced_toggles = enhancedToggles;

    MdkRenderer renderer = mdk_render_create(&desc);
    if (!renderer) {
        mdk::log3dsPrintf("renderer creation FAILED");
        mdk::log3dsClose();
        gfxExit();
        return 1;
    }
    mdk::log3dsPrintf("backend: %s", mdk_render_backend_name(renderer));
    heapProbe("renderer");

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
#if defined(MDK_AUTOQUIT) && defined(MDK_HW_LEVEL)
    // The hardware loop's level choice (`make AUTOQUIT=90 LEVEL=<n>`): the
    // console has no command line, so the arena-streaming spec's Phase 0
    // "baseline numbers for every level on hardware" needs the first level
    // chosen at build time. Harness builds only; the flow validates it.
    options.level = (int32_t)(MDK_HW_LEVEL);
    mdk::log3dsPrintf("main: harness level override LEVEL=%d", (int)(MDK_HW_LEVEL));
#endif
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
    // The HD pack, when the SD card has one (loadHdPack). Its lookup is wired
    // whatever the toggle says: the backend decides per draw (enableHdTextures)
    // whether the HD copy is used, so the toggle stays live.
    mdk::ModImages hdPack;
    HdPackSource hdSource;
    if (loadHdPack(options.level, &hdPack)) {
        hdSource.pack = &hdPack;
        surfaces.setColourReplacementLookup(hdPackLookup, &hdSource);
    }
    // The arenas on the GPU: a LevelView per arena the scripts have loaded,
    // following the arena stream (game/arena_views.h). Until 2026-11-18 this was
    // ONE view of the start arena, loaded once: nothing past the first door was
    // ever drawn.
    mdk::ArenaViews arenaViews;
    std::string startArenaName;
    // THE SKY, as geometry (render/sky_dome.h): the PICA200 has no fragment
    // shader for the oracle's per-pixel panorama pass, and the panorama is wider
    // than a PICA texture, so it is a sphere around the camera in slices.
    // Made once at level load; queued first every frame, DoubleSided, so solid
    // geometry (drawn in an earlier pass bucket) occludes it through depth.
    struct SkyRange {
        int32_t texture = -1;
        int32_t first = 0;
        int32_t count = 0;
    };
    MdkMeshId skyMesh = -1;
    SkyRange skyRanges[8];
    int32_t skyRangeCount = 0;
    // Kurt's sprites. TRAVERSE/TRAVSPRT.BNI is the same archive the HUD and the
    // scripts read (C# Viewer loads it once and passes it to all three), and the
    // level's palette indexes it, exactly as there.
    mdk::formats::Bni sprites;
    mdk::KurtSprite kurtSprite;
    bool kurtSpriteReady = false;
    // The sprites ARCHIVE parsed, which is not the same fact as kurtSpriteReady:
    // the runtime borrows the archive (its built-in behaviours play H150_R and
    // H150_I out of it) and a level whose Kurt-sprite decode failed still has a
    // usable archive for the scripts.
    bool spritesReady = false;
    // The HUD, off the same archive and the same palette (C# Viewer hands
    // TRAVERSE/TRAVSPRT.BNI to HudView and KurtSprite alike). Its own readiness is
    // a separate fact from Kurt's: the HUD's sprites are four images out of the
    // archive and either half can fail to decode without the other.
    mdk::HudView hud;
    bool hudReady = false;
    // The Video/Graphics settings menu (SPEC.md 7.9): its fonts, and
    // its readiness. SELECT opens it when ready; without the fonts
    // there is no menu and SELECT keeps its old quit meaning. STATIC,
    // not stack: a FontView carries a whole Font (256 glyph records,
    // ~11 KB), and two of them as main() locals overflowed the main
    // thread's stack -- the first printf of the run wrote below it
    // and the guest died before its first log line (measured in
    // Mandarine, 2026-11-14; AGENTS.md section 10's stack rule).
    static mdk::EnhancedMenu menu;
    static mdk::FontView menuBig;
    static mdk::FontView menuSmall;
    bool menuReady = false;
    // The level's script bytecode. The C# Viewer loads it next to the geometry
    // and the runtime cannot exist without it.
    mdk::formats::Cmi cmi;
    bool cmiReady = false;
    const mdk::formats::Arena* arena = nullptr;
    if (data.found()) {
        mdk::log3dsPrintf("data: %s", data.dir().c_str());
        if (mdk::LevelData::load(&level, &data, flow.state().level)) {
            heapProbe("level-data");
            if (level.corridorReadFailures() != 0) {
                mdk::log3dsPrintf("level: %d corridor read(s) FAILED -- the level has holes "
                                  "where those corridors are",
                                  (int)level.corridorReadFailures());
            }
            // The scripts' bytecode is a level asset, found the same way the C#
            // finds it. A level with no CMI is a level with no scripts, and this
            // frontend says so in the log rather than running a silent half-world.
            char cmiPath[64];
            snprintf(cmiPath, sizeof(cmiPath), "TRAVERSE/LEVEL%d/LEVEL%d.CMI",
                     (int)flow.state().level, (int)flow.state().level);
            std::vector<uint8_t> cmiBytes;
            if (!data.read(cmiPath, &cmiBytes)) {
                mdk::log3dsPrintf("scripts: %s did not read -- no scripts", cmiPath);
            } else if (!mdk::formats::Cmi::parse(&cmi, cmiBytes.data(),
                                                 (int32_t)cmiBytes.size())) {
                mdk::log3dsPrintf("scripts: %s did not parse -- no scripts", cmiPath);
            } else {
                cmiReady = true;
                mdk::log3dsPrintf("scripts: CMI loaded (%d bytes)", (int)cmiBytes.size());
                heapProbe("cmi");
            }
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
                // The enhanced look's SHADING marks which draws its features may
                // touch (the C#'s Lit/Sprite, as the Linux build sets them); each
                // feature is then gated by its own EnhancedToggles switch inside the
                // backend (SPEC.md 7.9). With every switch off the backend draws a
                // Lit surface in the same colours as an Original one. The mark is not
                // quite free: ObjectView splits a shiny part into its own (Sprite)
                // batch under Lit, which can reorder same-depth parts -- measured, 7
                // pixels of level 3 frame 300 (docs/evidence/2026-11-17-3ds-haze.txt).
                startArenaName = arena->name();
                arenaViews.start(renderer, &level, &surfaces, mdk::Shading::Lit);
                // The level's start is a SHOW of Kurt's arena (the original's
                // BSPShow at 0x41ba68): its textures are read in whole, now, and
                // its view built. Every later arena arrives through the frame's
                // stream step (see the loop).
                level.stream().update(startArenaName, std::string(), false);
                arenaViews.sync(startArenaName, std::string());
                if (const mdk::LevelView* startView = arenaViews.find(startArenaName)) {
                    mdk::log3dsPrintf("view: %d batches, %d vertices, %d triangles",
                                      (int)startView->batchCount(), (int)startView->vertexCount(),
                                      (int)arena->triangleCount());
                    mdk::log3dsPrintf("stream: start arena %s, %d bytes read (%d arenas hold "
                                      "pixels, %d bytes)",
                                      startArenaName.c_str(),
                                      (int)level.stream().stats().bytesRead,
                                      (int)level.stream().residentArenas(),
                                      (int)level.stream().residentBytes());
                    {
                        const mdk::formats::Dti& dti = level.dti();
                        const uint8_t* pal = dti.palette().rgba();
                        mdk::SkyDome dome;
                        if (mdk::buildSkyDome(dti.sky(), pal, dti.skyWrapWidth(), dti.skyHorizonRow(),
                                              dti.skyOffset(), dti.skyTopColor(),
                                              dti.skyBottomColor(), &dome)) {
                            skyMesh = mdk_render_create_mesh(renderer, dome.vertices.data(),
                                                             (int32_t)dome.vertices.size());
                            for (const mdk::SkySlice& slice : dome.slices) {
                                if (skyRangeCount >= 6) break;
                                SkyRange& r = skyRanges[skyRangeCount++];
                                r.texture = mdk_render_create_index_texture(
                                    renderer, slice.storedWidth, dome.height, slice.indices.data(), pal);
                                r.first = slice.first;
                                r.count = slice.count;
                            }
                            const uint8_t top[4] = {dome.topRgb[0], dome.topRgb[1], dome.topRgb[2], 255};
                            const uint8_t bottom[4] = {dome.bottomRgb[0], dome.bottomRgb[1],
                                                       dome.bottomRgb[2], 255};
                            SkyRange& t = skyRanges[skyRangeCount++];
                            t.texture = mdk_render_create_rgba_texture(renderer, 1, 1, top);
                            t.first = dome.topFirst;
                            t.count = dome.topCount;
                            SkyRange& b = skyRanges[skyRangeCount++];
                            b.texture = mdk_render_create_rgba_texture(renderer, 1, 1, bottom);
                            b.first = dome.bottomFirst;
                            b.count = dome.bottomCount;
                            mdk::log3dsPrintf("sky: %dx%d wrap=%d horizon=%d offset=%d -> %d slices, "
                                              "%d vertices, mesh=%d, index0 remapped %d texels to %d (d2=%d)",
                                              (int)dti.sky().width(), (int)dti.sky().height(),
                                              (int)dti.skyWrapWidth(), (int)dti.skyHorizonRow(),
                                              (int)dti.skyOffset(), (int)dome.slices.size(),
                                              (int)dome.vertices.size(), (int)skyMesh,
                                              (int)dome.remappedTexels, (int)dome.remapTarget,
                                              (int)dome.remapDistance);
                        } else {
                            mdk::log3dsPrintf("sky: none (the level's panorama cannot be built)");
                        }
                        // The dome's CPU copies die here: the renderer keeps its own.
                    }
                } else {
                    mdk::log3dsPrintf("view: FAILED");
                    arena = nullptr;
                }
                heapProbe("arena-view");
            }
            // Kurt's frames are loaded whether the arena view worked or not: the
            // sprite is his, not the arena's, and a level with no arena geometry
            // still has him.
            std::vector<uint8_t> spriteBytes;
            if (!data.read("TRAVERSE/TRAVSPRT.BNI", &spriteBytes) ||
                !mdk::formats::Bni::parse(&sprites, std::move(spriteBytes))) {
                mdk::log3dsPrintf("sprites: TRAVSPRT.BNI did not load -- Kurt is invisible");
            } else {
                // The archive PARSED, which is the runtime's requirement and not
                // Kurt's: the archive is what the built-in behaviours play out of,
                // the decode below is Kurt's own animations.
                spritesReady = true;
                if (kurtSprite.load(renderer, sprites, level.dti().palette())) {
                    kurtSpriteReady = true;
                    kurtSprite.setShading(mdk::Shading::Sprite);  // see view.setShading
                    // Kurt's frames from the HD pack (K_IDLE_0..., in the level's
                    // folder), made as each frame is first made.
                    if (hdSource.pack) {
                        kurtSprite.setMods(&hdPack);
                    }
                    mdk::log3dsPrintf("sprites: Kurt's animations loaded");
                    // KURT'S COMPRESSED FRAMES and the start set, at level load
                    // (cold; AGENTS.md P5: no SD read inside a frame). One open of
                    // the pack, its index and the start set in one read, the start
                    // set imported, the read's buffer freed; the other animations
                    // stream later (pumpStream, below). One line: files opened,
                    // bytes read, ms, start-set frames imported, and the .t3x bytes
                    // still held on the heap after it (0: the stream starts empty).
                    // Off (kurtCompressed=0, the A/B): every frame is the
                    // expansion, in the texture16 store (RGBA5551, or RGBA8 under
                    // texture16=0).
                    {
                        const double started = nowSeconds();
                        mdk::KurtSprite::CompressedReport report;
                        KurtPackLoad packLoad;
                        const bool have = enhancedToggles.kurtCompressed &&
                                          loadKurtPack(kKurtPackPath, &packLoad);
                        kurtSprite.useCompressedPack(have ? &packLoad.pack : nullptr,
                                                     flow.state().level, kKurtPackPath, &report);
                        const double read = nowSeconds();
                        if (have) {
                            const mdk::formats::KurtPack::Header& header =
                                packLoad.pack.header();
                            const uint32_t skip =
                                header.dataOffset - mdk::formats::KurtPack::kHeaderBytes;
                            kurtSprite.preloadStartSet(packLoad.front.data() + skip,
                                                       header.dataOffset,
                                                       header.startEnd - header.dataOffset,
                                                       &report);
                        }
                        // The staging buffer goes now: the start set's GPU copies
                        // are made (and the backend keeps each one's .t3x as its
                        // re-make source, startFileBytes).
                        std::vector<uint8_t>().swap(packLoad.front);
                        const double done = nowSeconds();
                        mdk::log3dsPrintf(
                            "sprites: kurtCompressed=%d texture16=%d pack=%s: %d file opened, "
                            "%d bytes read in %d ms; %d/%d frames ETC1A4 (%d bad), %d "
                            "animations to stream; start set %d/%d frames imported, %d GPU "
                            "bytes of %d, from %d bytes of .t3x, in %d ms; %u bytes of .t3x "
                            "kept on the heap (stream bound %u)",
                            (int)enhancedToggles.kurtCompressed, (int)enhancedToggles.texture16,
                            have ? kKurtPackPath : "none", (int)packLoad.opened,
                            (int)packLoad.bytesRead, (int)((read - started) * 1000.0),
                            (int)report.compressed, (int)report.frames, (int)report.badFrames,
                            (int)report.streamable, (int)report.preloaded,
                            (int)report.startFrames, (int)report.preloadBytes,
                            (int)report.preloadBudget, (int)report.startFileBytes,
                            (int)((done - read) * 1000.0),
                            (unsigned)kurtSprite.streamStats().staged,
                            (unsigned)mdk::KurtSprite::kStagedCacheBytes);
                    }
                } else {
                    mdk::log3dsPrintf("sprites: FAILED to decode Kurt's animations");
                }
                if (hud.load(renderer, sprites, level.dti().palette(),
                             hdSource.pack ? &hdPack : nullptr)) {
                    hudReady = true;
                    mdk::log3dsPrintf("hud: loaded");
                } else {
                    mdk::log3dsPrintf("hud: FAILED to decode the HUD sprites");
                }
                // The settings menu's fonts: MDKFONT.FTI's FONTBIG and
                // FONTSML in its SYS_PAL colours, the oracle's Fonts
                // pair (Ui.cs). A failure leaves menuReady false and
                // the boot log says why; the game runs unchanged.
                {
                    std::vector<uint8_t> ftiBytes;
                    mdk::formats::Fti menuFti;
                    std::vector<uint8_t> bigBytes;
                    std::vector<uint8_t> smallBytes;
                    std::vector<uint8_t> sysPal;
                    // Heap, not stack: each Font is ~11 KB (see the
                    // menu objects above).
                    std::unique_ptr<mdk::formats::Font> bigFont(new mdk::formats::Font());
                    std::unique_ptr<mdk::formats::Font> smallFont(new mdk::formats::Font());
                    if (!data.read("MISC/MDKFONT.FTI", &ftiBytes) ||
                        !mdk::formats::Fti::parse(&menuFti, ftiBytes.data(),
                                                  (int32_t)ftiBytes.size()) ||
                        !menuFti.getBytes("FONTBIG", &bigBytes) ||
                        !menuFti.getBytes("FONTSML", &smallBytes) ||
                        !menuFti.getBytes("SYS_PAL", &sysPal) ||
                        !mdk::formats::Font::parse(bigBytes.data(), (int32_t)bigBytes.size(),
                                                   6 /* FONTBIG space, Font.cs */, bigFont.get()) ||
                        !mdk::formats::Font::parse(smallBytes.data(), (int32_t)smallBytes.size(),
                                                   4 /* FONTSML space, Font.cs */,
                                                   smallFont.get())) {
                        mdk::log3dsPrintf("menu: MDKFONT.FTI fonts did not load -- no menu");
                    } else {
                        const mdk::formats::Palette sysPalette = mdk::formats::Palette::fromRgb(
                            sysPal.data(), (int32_t)sysPal.size());
                        const int32_t palette =
                            mdk_render_create_palette(renderer, sysPalette.rgba());
                        if (palette >= 0 && menuBig.load(renderer, *bigFont, palette, sysPalette.rgba()) &&
                            menuSmall.load(renderer, *smallFont, palette, sysPalette.rgba())) {
                            menuReady = true;
                            mdk::log3dsPrintf("menu: pause menu ready (START or SELECT)");
                        } else {
                            mdk::log3dsPrintf("menu: font atlases did not upload -- no menu");
                        }
                    }
                }
            }
            heapProbe("assets");
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
    // SINCE ARENA STREAMING PHASE 5 (2026-11-18) the two paragraphs above describe
    // the past in one respect: the music archive (LEVELnO.SNI, 3.3-4.5 MB) is no
    // longer resident -- the bank opens it by its directory and reads a track when
    // an arena loads -- and the decoded store IS bounded (the budget set below,
    // and releaseMusicExcept in the frame loop). What stays resident is
    // TRAVERSE.SNI and LEVELnS.SNI, the effects: 0.83-0.98 MB on levels 3, 7, 8.
    // The figures are in docs/evidence/2026-11-18-arena-streaming-phase5.txt and
    // they are EMULATOR figures; the console's are still owed.
    //
    // THE MIXER LIVES IN THE RUNTIME WHEN THERE IS ONE -- which is the PORT's shape,
    // not the C#'s. The oracle shares ONE mixer: Game.cs:206 builds the AudioDevice,
    // Viewer.cs:194 builds the SoundMixer over the bank, and the SAME object goes to
    // Kurt (Kurt.cs takes it as a constructor argument) and to the ScriptRuntime
    // (ScriptRuntime.cs:323 takes `SoundMixer mixer`, it does not own one). The port
    // gives the runtime its own mixer BY VALUE, so a rebuilt runtime is a new device
    // and the sink has to be re-attached; the C# cannot have that failure because its
    // mixer outlives the runtime. What is the same on both is the ONE-mixer property
    // that matters: the frame mixes the mixer the scripts write into, never a second
    // one. So the mixer the frame mixes
    // and the sink is attached to is `mix` below: the runtime's when the scripts
    // run, and the standalone one built here when they cannot (no CMI, no bank or
    // no sprites archive). What the standalone one can play is Kurt's own set —
    // footsteps, the chute, the guns, landing — and what the runtime's adds is the
    // scripts' ambient and object sounds, which is why this was the audio item.
    // ---------------------------------------------------------------------
    mdk::audio::SoundBank bank;
    bool bankReady = false;
    mdk::log3dsPrintf("mem: app region size=%llu free=%llu bytes",
                      (unsigned long long)osGetMemRegionSize(MEMREGION_APPLICATION),
                      (unsigned long long)osGetMemRegionFree(MEMREGION_APPLICATION));
    heapProbe("pre-audio");
    mdk::log3dsPrintf("audio: loading SNI archives (level %d)", (int)flow.state().level);
    if (data.found() &&
        mdk::audio::SoundBank::forLevel(&bank, data, flow.state().level)) {
        bankReady = true;
        // THE DECODED STORE IS BOUNDED (SoundBank::setDecodedBudgetBytes: least
        // recently played first, never a sound a voice is playing). Until Phase 5
        // nothing played music, so the store only ever held effects; a level's
        // music is 5-9 tracks that decode to about a megabyte each, and a level
        // played end to end would otherwise keep them all. 6 MiB is every effect
        // of the largest level decoded at once (level 4: about 2.5 MB) plus three
        // tracks -- the one playing, the one fading out and the one prefetched for
        // the next arena -- so nothing that can be needed without warning is ever
        // the victim. A hardware number, like the rest: it has to be confirmed on
        // the console.
        bank.setDecodedBudgetBytes(kDecodedSoundBudgetBytes);
        mdk::log3dsPrintf("audio: bank ready, %d archives (%d bytes resident; the music "
                          "archive is read by entry), decoded on demand, budget %d bytes",
                          (int)bank.archiveCount(), (int)bank.archiveBytes(),
                          (int)bank.decodedBudgetBytes());
    } else {
        mdk::log3dsPrintf("audio: the level's SNI archives did not load -- no sounds");
    }
    heapProbe("post-audio");
    mdk::audio::SoundMixer mixer(bankReady ? &bank : nullptr);
    // THE OUTPUT (SPEC.md 8). Release builds mix on the DSP (audio_dsp_3ds.h):
    // every audible voice on its own ndsp channel, the device's sample plane OFF
    // so the ARM11 sums nothing and only advances the voices' lifecycle. A harness
    // build made with DSPAUDIO=0 keeps the software mix into audio_3ds.cpp's one
    // channel, for the console's A/B. Either way the device keeps the frame's own
    // `owed` accounting (no queue query), so the voices end on the same frames.
    const bool audioReady = mdk::kDspAudio ? mdk::audioDspOpen(bankReady ? &bank : nullptr)
                                           : mdk::audioOpen();
    if (!mdk::kDspAudio) {
        mdk::log3dsPrintf("audio: software mix (harness DSPAUDIO=0): 64 voices summed on the "
                          "ARM11 into ndsp chn0, limiter on; the `audio: frame=` ledger is "
                          "this sink's");
    }
    mixer.device().setSampleMix(!mdk::kDspAudio);
    if (audioReady) {
        mixer.device().setSink(mdk::kDspAudio ? nullptr : mdk::audioSink, nullptr);
    }

    // The three assets the scripts need, all of them loaded above: the CMI is the
    // bytecode, the bank is what decides whether a script's play_sound names a
    // sound this level has, and the sprites archive is what the built-in
    // behaviours play their animations out of. The C# has no run without them, and
    // a runtime built with a hole in one of them is not a degraded oracle run but a
    // DIFFERENT game: the Runner draws its idle chance only while idle or done, so
    // a missing archive consumes the random stream differently from tick one. So
    // the runtime is not constructed at all when one is missing, and the level runs
    // without scripts, which the log says out loud.
    const bool scriptAssetsReady = cmiReady && bankReady && spritesReady;

    // The BSP world Kurt moves through: the arenas the level says are reachable
    // (the C# Viewer adds only IsReachable ones -- a hidden arena whose bounds
    // overlap the spawn column would otherwise steal the arena selection).
    mdk::ArenaSpace space;
    for (const mdk::formats::Arena* a : level.arenas()) {
        if (level.isReachable(a->name())) {
            space.add(a);
        }
    }
    heapProbe("arena-space");

    // ---------------------------------------------------------------------
    // The object pass: the C#'s Viewer._objects, Viewer._looks and DrawObjects.
    //
    // WHY IT SITS HERE, BEFORE KURT AND BEFORE spawnPlayer. Two things own parts
    // of it and they have different lifetimes:
    //
    //   * The RESOLVER and the LAYOUT CACHE are keyed by the level's DTI and by
    //     the MODEL objects, and the models are owned by the RUNTIME
    //     (ScriptRuntime::modelCache_, script_runtime.h:580 — the CMI's global
    //     models, parsed once and held by value). A respawn rebuilds the runtime,
    //     so every one of those Model addresses is freed and the next level run
    //     gets new ones. Both caches key on that address: ObjectView::layouts_ on
    //     (Model*, Palette*) and the shared pose caches on Model* (see
    //     scripts/pose_cache.h, whose own comment names this exact failure — "a new
    //     level's model can land on a freed address and inherit the old one's
    //     pose"). The C# cannot have that failure: a respawn builds a fresh Viewer,
    //     whose constructor calls MdkObject.ForgetPoses() (Viewer.cs:185) and builds
    //     a fresh ObjectView, so both caches die with the runtime that owned the
    //     models. So the drop has to happen HERE, in spawnPlayer, and that is why
    //     these declarations are above it and not beside the draw below.
    //
    //   * The LOOKS are keyed by arena NAME and point into LevelData, which is not
    //     rebuilt, so they survive a respawn and are not cleared by one.
    //
    // The shading is Lit (Sprite for the effects), as LevelView's is: the mark of
    // the surfaces the Enhanced Mode features may touch, each behind its own
    // toggle in the backend (SPEC.md 7.9; see view.setShading above).
    // ---------------------------------------------------------------------
    mdk::MaterialResolver objectResolver(&level.dti(), mdk::Shading::Lit);  // see view.setShading
    mdk::ObjectView objectView(objectResolver);
    mdk::ObjectDraw objectDraw;
    mdk::EffectsView effectsView(objectResolver, &level, mdk::Shading::Sprite);
    mdk::EffectsDraw effectsDraw;
    mdk::EffectsFrame effectsFrame;
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
    // THE SCRIPTS. The C# Viewer owns one ScriptRuntime per level and builds a
    // fresh one when the level reloads, and a respawn here IS a level reload, so
    // the runtime is rebuilt in spawnPlayer with Kurt: its object list, its tick
    // count, its arena state and its random stream all start over (Viewer.cs:232).
    // It is a pointer because it is rebuilt, and it is gated on its three assets
    // rather than constructed with a hole in one of them (scriptAssetsReady).
    std::unique_ptr<mdk::scripts::ScriptRuntime> runtime;
    // THE LEVEL'S MUSIC (audio/level_music.h, the C#'s LevelMusic): the track of
    // Kurt's arena, played through the runtime's mixer -- so it lives and dies
    // with the runtime, like everything else that holds a voice of that mixer.
    std::unique_ptr<mdk::audio::LevelMusic> music;
    // The arena whose track the bank was last asked to prefetch (below).
    std::string musicPrefetchedFor;
    // THE BANK'S BLOCKING READS, ALL OF THEM SAID (spec Phase 3: "log every
    // stall"). A streamed sound that is asked for while it is not in memory is
    // read whole, wherever the asker is -- the music's enter(), a script's play
    // in the middle of its tick, a track the budget evicted. The ones the
    // music's enter() makes are timed and said there; the frame says the rest.
    int32_t audioStallsSaid = 0;
    int32_t audioDemandSaid = 0;
    int64_t audioDemandBytesSaid = 0;
    // The prefetch in flight: its steps' worst time, for the line that ends it.
    double prefetchWorstMs = 0.0;
    // The arena whose light rig the renderer holds (see the frame's light below).
    const mdk::formats::Arena* litArena = nullptr;
    // THE SCRIPT OPTION (C# ScriptRuntime.Option). It is a USER SETTING, not a
    // constant: `Option = ui.Settings.Gore ? 1 : 0` when the runtime is built
    // (Viewer.cs:234) and `Option = data["option"]` when a save is loaded
    // (ScriptRuntime.Snapshot.cs:142) — NOT Viewer.cs:874, which review-7 F4 named:
    // that line is the `TOOSCARYFORME` gore CHEAT (Viewer.cs:859-876, whose own doc
    // comment says "not saved"). `Settings.Gore` DEFAULTS TO true
    // (Flow/Settings.cs:75), so a
    // default oracle run has Option == 1 and this port ships the same default.
    // The port has no settings UI, so the value is fixed here rather than read.
    // Two ported behaviours read it: opcode 232 `if_option` branches on equality,
    // and opcode 129 `blow_off_parts` mode 2 is skipped while it is 0
    // (script_vm.cpp:858-867). What is NOT ported is the gore DEBRIS -- the slime
    // drops of ScriptRuntime.cs:1754-1764 -- so setting 1 here does NOT add the
    // 32x4 RNG draws that half consumes, and the random stream is unchanged either
    // way. Review-6 F2: the port used to write 0 and call it "the oracle's Option",
    // which is the opposite of the oracle's default.
    static const int32_t kScriptOption = 1;  // 1 = gore on, the Settings.Gore default
    // THE ONE MIXER, and which one it is depends on the runtime: the C# gives the
    // runtime and Kurt the same SoundMixer, and ScriptRuntime OWNS its mixer, so
    // when there is a runtime the frontend mixes that one and the sink is attached
    // to it. With no runtime the standalone mixer built above stays in use and what
    // can play is Kurt's own set. Re-pointed in spawnPlayer, never left dangling:
    // it is reset to the standalone mixer before every rebuild, and the runtime's
    // address is stable because the mixer is a member of the object pointed at.
    mdk::audio::SoundMixer* mix = &mixer;
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
#if defined(MDK_AUTOQUIT) && defined(MDK_HW_AT)
        // THE HARNESS'S START OVERRIDE (`make AUTOQUIT=.. AT=x,y,z,yaw`): Kurt's
        // feet and yaw exactly as given, as the Linux build's --at sets them (no
        // +3 lift: the override is the feet). A harness build only: the console
        // has no command line, and a release must start where the level says.
        {
            const float at[4] = {MDK_HW_AT};
            player.feet = mdk::Vec3{at[0], at[1], at[2]};
            player.yaw = at[3];
            mdk::log3dsPrintf("main: harness start override AT=%.1f,%.1f,%.1f yaw %.1f",
                              at[0], at[1], at[2], at[3]);
        }
#endif
        if (firstSpawn) {
            player.setHealth(entryHealth);
            firstSpawn = false;
        }
        player.setSpace(&space);
        player.setMixer(&mixer);  // a fresh Player has a null mixer (C# Kurt's ctor arg)
        // The scripts restart with him. The runtime is destroyed and rebuilt rather
        // than reset, because that is what the C# does (a fresh Viewer, a fresh
        // ScriptRuntime) and because the runtime has no reset: its arena states,
        // object deque, tick count and RNG are the level's history.
        // The DSP channels follow the device being replaced: silence and forget
        // them (and give back the music stream's bank pins) before it goes.
        mdk::audioDspDetach();
        music.reset();   // its voices are the runtime's mixer's
        musicPrefetchedFor.clear();
        runtime.reset();
        mix = &mixer;
        // THE CACHES THAT KEY ON A MODEL ADDRESS DIE WITH THE RUNTIME, and this is
        // the moment they have to be dropped. `runtime.reset()` above freed
        // ScriptRuntime::modelCache_, so every Model* the layout cache and the pose
        // caches are keyed by is now a freed address, and the new runtime will parse
        // its models into whatever the allocator hands back — which, same level,
        // same parse order, is very likely the SAME addresses holding DIFFERENT
        // models. A layout cache kept across this line hands a recycled address the
        // previous model's geometry; a pose cache kept across it is the failure
        // pose_cache.h was written for. The C# drops both here: a respawn is a fresh
        // Viewer, whose constructor calls MdkObject.ForgetPoses() (Viewer.cs:185)
        // and constructs a fresh ObjectView. Dropping derived data is never wrong —
        // the next layout and the next pose are rebuilt from the model — so this is
        // safe even on the first spawn, where there is nothing cached yet.
        mdk::scripts::MdkObject::forget_poses();
        objectView.clear();
        effectsView.clear();
        if (scriptAssetsReady) {
            // The seed. The oracle seeds its RNG with `new Random()` when no seed is
            // passed (ScriptRuntime.cs:323), which is the clock: an oracle run is
            // NOT reproducible here either, so a fixed seed would be the divergence,
            // not the fidelity. The console's clock is the ARM11 cycle counter, and
            // the value is logged at spawn so a run can be replayed by hand.
#if defined(MDK_AUTOQUIT) && defined(MDK_HW_WALK)
            // A WALK harness build replays one scene: the same script, the same
            // seed (the Linux build's --seed 1 for the same route), so two
            // profiles of two builds measure the same enemies and effects.
            const int32_t seed = 1;
#else
            const int32_t seed = (int32_t)(uint32_t)svcGetSystemTick();
#endif
            runtime.reset(new mdk::scripts::ScriptRuntime(&level, &cmi, seed, &bank, &player,
                                                          &sprites));
            runtime->option = kScriptOption;
            // Kurt's sounds go through the runtime's mixer, because in the C# they
            // are the same mixer.
            player.setMixer(&runtime->mixer());
            mix = &runtime->mixer();
            // The new mixer owns a new SoundDevice, so the output has to be handed
            // to it: a respawn without this would be a silent level (and, on the
            // DSP path, a device mixing samples nobody hears).
            mix->device().setSampleMix(!mdk::kDspAudio);
            if (audioReady) {
                mix->device().setSink(mdk::kDspAudio ? nullptr : mdk::audioSink, nullptr);
            }
            mdk::log3dsPrintf("scripts: runtime ready (seed %d, option %d)", (int)seed,
                              (int)kScriptOption);
            heapProbe("runtime");
        }
        if (kurtSpriteReady) {
            player.setFrameCounts(kurtSprite.frameCounts());
        }
        follow = mdk::FollowCamera();
        // Pin Kurt's arena to the one around the spawn, as the fresh level's start.
        // The step loop refreshes it from here on (from the runtime's DRAWN LIST --
        // current arena plus the active second -- when there is one, geometrically
        // when there is not: the C# refreshes it every step, Viewer.cs:533); what the
        // spawn pin is FOR is the case the loop CANNOT supply, a Kurt standing outside
        // every arena and a runtime that has not completed its first tick, when the
        // drawn list is the empty name that resolves to no arena at all: with no pin,
        // bottom() resolves nothing, the fall-out death never fires and he falls
        // forever.
        const std::string* here = space.arenaAt(mdk::Vec{player.feet.x, player.feet.y, player.feet.z});
        if (here) {
            space.setSolid({*here});
        }
        // And it is the arena the frame shows until the scripts' first tick names
        // one (the stream step's fallback): Kurt's own, which is the level's start
        // arena unless he was placed elsewhere (--at, a respawn).
        if (here) {
            startArenaName = *here;
        }
        // The level starts in Kurt's arena, its music at once (C# Viewer.cs:225-228,
        // the original's 0x41ba68): the DTI's start arena, or where he was put.
        if (runtime) {
            music.reset(new mdk::audio::LevelMusic(&runtime->mixer(), &cmi.arenaMusic(),
                                                   &cmi.arenaAmbience()));
            const double music0 = nowSeconds();
            music->enter(startArenaName);
            mdk::log3dsPrintf("music: %s for %s (read and decoded in %.1f ms)",
                              music->playing().c_str(), startArenaName.c_str(),
                              (nowSeconds() - music0) * 1000.0);
            // That read is said (the line above): the frame's ledger starts here.
            audioStallsSaid = bank.stats().prefetchStalls;
            audioDemandSaid = bank.stats().demandReads;
            audioDemandBytesSaid = bank.stats().demandBytes;
        }
        mdk::log3dsPrintf("spawn: feet=(%.1f,%.1f,%.1f) yaw=%.1f health=%d arenas=%d",
                          player.feet.x, player.feet.y, player.feet.z, player.yaw,
                          player.health(), (int)level.arenas().size());
    };
    if (arena) {
        spawnPlayer();
        playerReady = true;
    }
    if (playerReady && !runtime) {
        // Said out loud, because a silent half-world is the failure mode: the level
        // plays, Kurt walks, and nothing the scripts own exists. The three assets
        // are named so the cause is readable from the guest log alone.
        mdk::log3dsPrintf("scripts: NOT started (cmi=%d bank=%d sprites=%d) -- no script "
                          "sounds and no script objects", (int)cmiReady, (int)bankReady,
                          (int)spritesReady);
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
    // The HUD's blink runs on the C#'s 30 Hz clock and not on the frame's, so it
    // needs its own accumulator beside `pending` (Viewer.cs:535-538, and the same
    // pair the Linux frontend keeps).
    const float kHudStep = 1.0f / 30.0f;
    float hudPending = 0.0f;
    uint32_t frame = 0;
    uint32_t steps = 0;
    // The stream mesh's capacity the frame log last reported, so a growth is said
    // once instead of every frame. Zero until the first frame with corners.
    int32_t streamCapacityLogged = -1;
    // Printed on the first refused attempt after a success, and re-armed by a
    // success (see the print site). Like the capacity line above it it is
    // edge-triggered: a level under linear pressure would otherwise repeat the
    // same line 60 times a second, and a log that repeats is a log nobody reads
    // (review-10 F3). A latch that could never re-arm would instead hide every
    // refusal after the first (review-11 F7).
    bool streamFailureLogged = false;
    bool effectsFailureLogged = false;
    // This frame's object-pass cost, and the peak over the run. The peak is the
    // number that says what a level costs the stream mesh: the capacity is rounded
    // up to a power of two, so the capacity alone cannot tell a 480-corner level
    // from one that nearly fills its 4096.
    int32_t objectCorners = 0;
    int32_t objectDraws = 0;
    // The arena stream's log state (see the frame's stream step).
    int32_t lastViewsReleased = 0;
    int32_t lastViewsFailed = 0;
    int32_t lastDemandLoads = 0;
    std::vector<std::string> startDrawn;
    const std::vector<std::string>* frameDrawn = nullptr;   // this frame's drawn list
    int32_t objectPeakCorners = 0;
    int32_t objectPeakDraws = 0;
    const char* exitReason = "aptMainLoop returned false";
    u32 prevHeld = 0;  // last frame's held mask; the menu reads edges

    heapProbe("loop");
#if defined(MDK_AUTOQUIT) && defined(MDK_HW_WALK)
    // The scripted walk (harness_3ds.h), parsed and logged before the first frame.
    const bool walkReady = mdk::harnessWalkInit();
#endif
    mdk::log3dsPrintf("loop: entering main loop");
    // THE PAUSE MENU'S FALLBACK. Without the menu's fonts there is no menu to
    // open, and so no Quit row: holding START and SELECT together is then the
    // way out (one press of either alone does nothing), and the log says so.
    if (!menuReady) {
        mdk::log3dsPrintf("menu: no pause menu (its fonts did not load); hold START+SELECT "
                          "together to quit");
    }
    // The hint, for the first seconds of the level: the owner did not know the
    // effects menu existed. Drawn with the menu's own small font on the canvas,
    // only while the menu is ready and closed.
    const double hintUntil = nowSeconds() + 5.0;
    while (aptMainLoop()) {
        // The harness deadline (harness builds only; a no-op question in
        // any other build): quit through the ordinary teardown so the
        // atexit chain-load hands the console back to the stub.
        // The OG profile's per-frame spans (PROFILE builds; all zero otherwise).
        uint64_t spanSim0 = 0, spanSim = 0;
        uint64_t spanAudio = 0;
        uint32_t spanSteps0 = steps;
        if (mdk::harnessAutoquitDue()) {
            exitReason = "harness autoquit (MDK_AUTOQUIT)";
            break;
        }
#ifdef MDK_TOGGLE_WALK
        // One step every kWalkFrames: the next toggle the config asked for.
        {
            const uint32_t kWalkFrames = 120;
            static const char* const kWalkNames[] = {
                "filtering", "mipmaps", "haze", "lighting", "fragmentLights",
                "mirrors", "hdTextures", "edgeSmoothing", "glow", "skyShadows"};
            const uint32_t kWalkSteps = sizeof(kWalkNames) / sizeof(kWalkNames[0]);
            static uint32_t walkStep = 0;
            static uint32_t walkNext = kWalkFrames;
            if (walkStep > 0u && frame + kWalkFrames / 2u == walkNext) {
                mdk::log3dsPrintf("walk: %s survived %u frames", kWalkNames[walkStep - 1u],
                                  (unsigned)(kWalkFrames / 2u));
            }
            if (frame == walkNext && walkStep < kWalkSteps) {
                if (walkStep == 0u) {
                    mdk::log3dsPrintf("walk: the original look survived %u frames",
                                      (unsigned)frame);
                }
                bool* slot = nullptr;
                bool want = false;
                switch (walkStep) {
                case 0: slot = &enhancedToggles.enableFiltering; want = walkTarget.enableFiltering; break;
                case 1: slot = &enhancedToggles.enableMipmaps; want = walkTarget.enableMipmaps; break;
                case 2: slot = &enhancedToggles.enableHaze; want = walkTarget.enableHaze; break;
                case 3: slot = &enhancedToggles.enableLighting; want = walkTarget.enableLighting; break;
                case 4: slot = &enhancedToggles.enableFragmentLights; want = walkTarget.enableFragmentLights; break;
                case 5: slot = &enhancedToggles.enableMirrors; want = walkTarget.enableMirrors; break;
                case 6: slot = &enhancedToggles.enableHdTextures; want = walkTarget.enableHdTextures; break;
                case 7: slot = &enhancedToggles.enableEdgeSmoothing; want = walkTarget.enableEdgeSmoothing; break;
                case 8: slot = &enhancedToggles.enableGlowBillboards; want = walkTarget.enableGlowBillboards; break;
                default: slot = &enhancedToggles.enableSkyShadows; want = walkTarget.enableSkyShadows; break;
                }
                mdk::log3dsPrintf("walk: step %u: %s %s at frame %u", (unsigned)(walkStep + 1u),
                                  kWalkNames[walkStep], want ? "ON" : "(left off by config.ini)",
                                  (unsigned)frame);
                if (want) {
                    *slot = true;
                    mdk_render_set_enhanced_toggles(renderer, &enhancedToggles);
                }
                walkStep++;
                walkNext += kWalkFrames;
                if (walkStep == kWalkSteps) {
                    mdk::log3dsPrintf("walk: every step taken; the config's full set is on");
                }
            }
        }
#endif

        // The frame's clock FIRST: the look integrates over the time since the
        // previous frame, so the input poll needs the same delta the simulation is
        // about to get. Clamp it here -- the platform's floor under the game's own
        // policy -- because a 40-second sleep is not 40 seconds of walking.
        const double now = nowSeconds();
        if (g_resumePending) {
            // Woke from sleep/suspend: the interval is not gameplay.
            // Rebase explicitly (the clamp below stays as the backstop
            // for every other long frame).
            g_resumePending = false;
            last = now;
            mdk::log3dsPrintf("lifecycle: woke; frame clock rebased");
        }
        double delta = now - last;
        last = now;
        // The OG profile (harness PROFILE builds only; a no-op otherwise): the
        // raw period, before the gameplay clamp, and never the first frame,
        // which carries the time since init.
        if (frame > 0) {
            mdk::harnessProfileFrame(delta);
        }
        if (delta < 0.0) delta = 0.0;
        if (delta > 0.25) delta = 0.25;
#if defined(MDK_AUTOQUIT) && defined(MDK_HW_FIXED)
        // The harness's deterministic clock (`make AUTOQUIT=90 FIXED=1`): every
        // frame advances exactly one 1/60 s step, as the Linux build's
        // --fixed-step does, so `CAPTURE=<n>` writes the SAME simulation frame
        // the Linux capture writes at --capture-frame=<n>, and the two can be
        // pixel-diffed (mdk_3ds/tools/compare_linux.sh). Never in a play or
        // profile build: it is not real time.
        delta = 1.0 / 60.0;
#endif
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
        const u32 held = mdk::input3dsPoll(&input, elapsed, controls);
#if defined(MDK_AUTOQUIT) && defined(MDK_HW_WALK)
        // The scripted walk owns the actions, after the poll and before the game
        // reads them (harness_3ds.h). A refused script stops the run at once.
        if (!walkReady) {
            exitReason = "harness walk refused (MDK_HW_WALK)";
            break;
        }
        mdk::harnessWalkApply(frame, &input, player.feet.x, player.feet.y, player.feet.z,
                              player.yaw, runtime ? runtime->currentArena().c_str() : "");
#endif

        const u32 pressed = held & ~prevHeld;
        prevHeld = held;

        // THE PAUSE MENU (owner, 2026-11-19; SPEC.md 7.9's Video/Graphics
        // settings are its rows). START opens it -- it used to quit the game
        // on one press -- and SELECT opens the same menu. It opens on RESUME;
        // while it is open the simulation below is paused and the frame's
        // edges drive the menu instead: the D-pad moves (and wraps), A flips a
        // toggle or takes RESUME / QUIT, and START, B or SELECT close it. A
        // flip applies at once (mdk_render_set_enhanced_toggles) and is written
        // back to config.ini in the same frame, so a hardware A/B is one press.
        // QUIT is a row, confirmed with A: never a single press.
        bool menuQuit = false;
        if (menuReady) {
            if (!menu.open() && (pressed & (KEY_START | KEY_SELECT))) {
                menu.setOpen(true);
                mdk::log3dsPrintf("menu: paused (%s)",
                                  (pressed & KEY_START) ? "START" : "SELECT");
            } else if (menu.open()) {
                mdk::EnhancedMenu::Input edges;
                edges.up = (pressed & KEY_DUP) != 0;
                edges.down = (pressed & KEY_DDOWN) != 0;
                edges.left = (pressed & KEY_DLEFT) != 0;
                edges.right = (pressed & KEY_DRIGHT) != 0;
                edges.confirm = (pressed & KEY_A) != 0;
                edges.close = (pressed & (KEY_START | KEY_B | KEY_SELECT)) != 0;
                const mdk::EnhancedMenu::Action action = menu.handle(edges, &enhancedToggles);
                if (action == mdk::EnhancedMenu::Action::Toggled) {
                    mdk_render_set_enhanced_toggles(renderer, &enhancedToggles);
                    const bool saved = saveEnhancedConfig(enhancedToggles);
                    mdk::log3dsPrintf("menu: shadows=%d lights=%d haze=%d mirrors=%d "
                                      "glow=%d filtering=%d mipmaps=%d hd=%d lighting=%d smoothing=%d "
                                      "(saved=%d)",
                                      (int)enhancedToggles.enableSkyShadows,
                                      (int)enhancedToggles.enableFragmentLights,
                                      (int)enhancedToggles.enableHaze,
                                      (int)enhancedToggles.enableMirrors,
                                      (int)enhancedToggles.enableGlowBillboards,
                                      (int)enhancedToggles.enableFiltering,
                                      (int)enhancedToggles.enableMipmaps,
                                      (int)enhancedToggles.enableHdTextures,
                                      (int)enhancedToggles.enableLighting,
                                      (int)enhancedToggles.enableEdgeSmoothing, (int)saved);
                } else if (action == mdk::EnhancedMenu::Action::Quit) {
                    menuQuit = true;
                    mdk::log3dsPrintf("menu: QUIT chosen");
                }
                if (!menu.open()) {
                    mdk::log3dsPrintf("menu: closed (resumed)");
                }
            }
            if (menu.open()) {
                // The poll accumulated this frame's look into the
                // mouse; the paused game must not spend it on resume.
                input.takeMouseX();
                input.takeMouseY();
            }
        }

        // The flow's events. QUIT is the C# pad's Escape answer here: the
        // menu that Escape opens in the C# is cut, so the honest end is to
        // stop, and the flow says so (Quit -> Screen::None) rather than the
        // frontend deciding on its own. It comes from the pause menu's QUIT
        // row -- or, only when the menu's fonts did not load, from START and
        // SELECT held together.
        mdk::flow::Event next = mdk::flow::Event::None;
        const char* quitReason = "menu Quit";
        if (menuQuit) {
            next = mdk::flow::Event::Quit;
        } else if (!menuReady && (held & KEY_START) && (held & KEY_SELECT)) {
            next = mdk::flow::Event::Quit;
            quitReason = "START+SELECT held (no pause menu)";
        }
        if (next != mdk::flow::Event::None) {
            screen = flow.handle(next);
            if (screen == mdk::flow::Screen::None) {
                exitReason = quitReason;
                break;
            }
            mdk::log3dsPrintf("flow: screen=%d", (int)screen);
            if (screen != mdk::flow::Screen::Level) {
                exitReason = "the flow asked for a screen this frontend cannot show";
                break;
            }
        }

        if (playerReady && !menu.open()) {
            // The C# Viewer.Frame order: the mouse look is PER FRAME (Kurt's yaw
            // and the camera's look offset), then the simulation runs in fixed
            // steps, and EACH step updates Kurt and then the camera (UpdateKurt
            // then UpdateCamera) -- so the pitch smoothing and the clearance run
            // once per step, not once per frame.
            player.mouseLook(input);
            follow.mouseLook(input);

            pending += elapsed;
            if (pending > maxPending) pending = maxPending;
            spanSim0 = mdk::harnessProfileTick();
            spanSteps0 = steps;
            while (pending >= step) {
                const uint64_t tp0 = mdk::harnessProfileTick();
                player.update(input, step);
                const uint64_t tp1 = mdk::harnessProfileTick();
                // The C# Viewer's step order is Kurt, then the scripts, then the
                // camera (Viewer.cs:505-535). ScriptRuntime keeps its own 1/30 s
                // clock, so a 1/60 s call completes a script tick every second step
                // while Kurt's physics stay at 60 Hz.
                if (runtime) {
                    runtime->update(step);
                }
                const uint64_t tp2 = mdk::harnessProfileTick();
                // The pitch the camera eases to (Viewer.cs:746-747). The oracle's
                // sources are a live camera_track request first and the scripts'
                // current arena's rest pitch after it.
                //
                // The two halves are ported, but not by the same batch, and saying
                // so is the point. camera_track is real script output: opcode 203
                // writes it and it decays per tick, so the runtime is the only
                // possible source and this is the oracle's own rule. The ARENA half
                // is the crossing: `currentArena()` used to be written once and then
                // never move off the spawn arena, because `CrossedArena` was not
                // ported, and this line therefore held the SPAWN arena's rest pitch
                // for a whole level. Batch 134 ported the DTI doorway crossing (the
                // `crossedArena` call where `currentArena_` is set in runTick), so the
                // input moves when a move crosses a connection and the formula here is
                // the oracle's on a live input, not a frozen one.
                // No capture yet shows a crossing on this frontend: the emulator runs
                // idle a level, so the pitch change is unmeasured (docs/PROGRESS_LOG.md).
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
                follow.update(&camera, &space, player, target, step, kAspect);
                const uint64_t tp3 = mdk::harnessProfileTick();
                // The music follows Kurt's arena, which the scripts set (C#
                // Viewer.cs:521-525: Enter, then Update(Step), every step, after
                // the camera). A track that is not in memory yet is read HERE,
                // inside enter(): the prefetch below is what makes that rare, and
                // when it happens anyway it is said, with its time.
                if (music) {
                    const mdk::audio::SoundBank::Stats audioBefore = bank.stats();
                    const double music0 = nowSeconds();
                    music->enter(runtime->currentArena());
                    const mdk::audio::SoundBank::Stats& audioAfter = bank.stats();
                    if (audioAfter.prefetchStalls != audioBefore.prefetchStalls) {
                        mdk::log3dsPrintf("music: STALL %s wanted before its prefetch was done: "
                                          "%d bytes in %.1f ms",
                                          music->playing().c_str(),
                                          (int)(audioAfter.prefetchStallBytes -
                                                audioBefore.prefetchStallBytes),
                                          (nowSeconds() - music0) * 1000.0);
                    } else if (audioAfter.demandReads != audioBefore.demandReads) {
                        mdk::log3dsPrintf("music: STALL %s was not prefetched: %d bytes in "
                                          "%.1f ms",
                                          music->playing().c_str(),
                                          (int)(audioAfter.demandBytes - audioBefore.demandBytes),
                                          (nowSeconds() - music0) * 1000.0);
                    }
                    // Said here, with its time: the frame's own ledger (below)
                    // reports only the blocking reads nobody else timed.
                    audioStallsSaid += audioAfter.prefetchStalls - audioBefore.prefetchStalls;
                    audioDemandSaid += audioAfter.demandReads - audioBefore.demandReads;
                    music->update(step);
                }
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
                //
                // WITH A RUNTIME THIS IS THE ORACLE'S OWN VALUE, not a stand-in:
                // `ScriptRuntime::solidArenas()` is the scripts' DRAWN list -- the
                // current arena plus the active second (ScriptRuntime.cs:265-282,
                // UpdateArenas :658-664) -- which is exactly what Viewer.cs:533 hands
                // `SetSolid`. It moves when a move crosses a doorway CONNECTION
                // (CrossedArena, :791-803, over the DTI type-6 records) and it does
                // NOT follow the feet, so it holds while Kurt falls out of the world
                // instead of re-pinning below the arena he left (review-8 F1's four
                // columns of 1681). It is read AFTER `follow.update` because that is
                // where the C# evaluates SetSolid and the camera's clearance can move
                // Kurt into another arena.
                //
                // What it deliberately is NOT is `currentArena()` alone. One name
                // where the oracle passes two drops the ACTIVE SECOND -- the room
                // behind the door Kurt just came through -- and `move()` sweeps only
                // the pinned arenas, so he finds no floor in the room he left, drops
                // through it, and `bottom()` fires the fall-out death into a respawn
                // loop. That is review-6 F1's defect class (batch 99 shipped it) and
                // both `tests/scripts/main_3ds_flow_test.sh` and
                // `tests/scripts/frontend_solid_pin_test.sh` keep it red.
                //
                // The branch with NO runtime is the asset gate: a level whose CMI,
                // bank or sprites archive is missing runs without scripts, so there
                // is no drawn list to read and the geometric arena under his feet is
                // the stand-in. It is a stand-in in both directions -- it can re-pin
                // during a fall-out, and it carries one name -- and it is kept because
                // it still beats a pin left at the spawn. It is probed AGAIN rather
                // than reusing the pitch's `here` for the same reason the C# evaluates
                // SetSolid after UpdateCamera. It is NOT cleared when he is outside
                // every arena: that is the fall-out case, and `bottom()` still has to
                // resolve an arena for the death to fire at all.
                if (runtime) {
                    space.setSolid(runtime->solidArenas());
                } else {
                    const std::string* solid =
                        space.arenaAt(mdk::Vec{player.feet.x, player.feet.y, player.feet.z});
                    if (solid) {
                        space.setSolid({*solid});
                    }
                }
                // The C# advances the HUD blink once per 30 Hz boundary
                // (Viewer.cs:535-538), not once per 60 Hz frame.
                mdk::harnessProfileStepSpans(tp1 - tp0, tp2 - tp1, tp3 - tp2,
                                             mdk::harnessProfileTick() - tp3);
                hudPending += step;
                if (hudPending >= kHudStep) {
                    hud.tick();
                    hudPending -= kHudStep;
                }
                pending -= step;
                steps++;
            }
            spanSim = mdk::harnessProfileTick() - spanSim0;

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
        // play forever to a silent console. `mix` is the runtime's mixer when the
        // scripts are running, which is the C#'s one-mixer shape and the only way a
        // script's play_sound reaches this device at all.
        //
        // THE DSP PATH'S FRAME sits between the two updates (voice_binding.h):
        // after the mixer, so this frame's gain, pitch and pan are set and stopped
        // voices are gone; before the device, so a voice started this frame is
        // bound at its first sample. A no-op on the software path.
        // The `audio` profile span is all of it, in both modes (PROFILE builds).
        const uint64_t spanAudio0 = mdk::harnessProfileTick();
        mix->listenFromCamera(camera);
        mix->update(elapsed);
        mdk::audioDspFrame(*mix);
        mix->device().update(elapsed);
        spanAudio = mdk::harnessProfileTick() - spanAudio0;
        // AFTER the device, so the clock is exactly the sum of what the device
        // was given and not of what the frame happened to measure.
        audioClock += elapsed;

        // The screenshot arming file (the hardware loop's picture, ledger
        // 3ds.screenshot): dropping sdmc:/3ds/mdk/capture.txt beside the
        // log asks for one framebuffer BMP at the next frame; the file is
        // consumed so the ask fires once. Checked every 30 frames -- a
        // stat a second, never frame-critical work.
        // NOT in release or PROFILE builds: the fopen of a file that is not
        // there costs ~85 ms on the console's SD (measured 2026-11-19: a stall
        // every 30 frames, the 30-frame spikes). Release screenshots are
        // ATM's / Rosalina's; the harness's fixed-frame capture is below.
        static char capturePath[128] = {};
        if (mdk::log3dsPeriodic() && capturePath[0] == '\0' && (frame % 30) == 0) {
            if (FILE* armed = std::fopen("sdmc:/3ds/mdk/capture.txt", "rb")) {
                char named[sizeof(capturePath)] = {};
                size_t got = std::fread(named, 1, sizeof(named) - 1, armed);
                std::fclose(armed);
                while (got > 0 && (named[got - 1] == '\n' || named[got - 1] == '\r' ||
                                   named[got - 1] == ' ')) {
                    named[--got] = '\0';
                }
                std::snprintf(capturePath, sizeof(capturePath), "%s",
                              named[0] ? named : "sdmc:/3ds/mdk/capture.bmp");
                std::remove("sdmc:/3ds/mdk/capture.txt");
            }
        }
        // The harness's fixed-frame capture (harness_3ds.h; hw.pixels-rgba8):
        // no file to drop on a console nobody touches, so the frame is a build
        // flag. nullptr in every build without MDK_HW_CAPTURE.
        if (capturePath[0] == '\0') {
            if (const char* hw = mdk::harnessCapturePath(frame)) {
                std::snprintf(capturePath, sizeof(capturePath), "%s", hw);
            }
        }

        // THE ARENAS IN MEMORY AND ON THE GPU FOLLOW THE SCRIPTS, once per frame,
        // after the steps and before anything is drawn (game/arena_stream.h,
        // game/arena_views.h): the stream first, so an arena that is drawn this
        // frame has its textures' pixels before its view is asked for a texture;
        // then the views. Outside begin/end_frame: both make and release GPU
        // resources. A stall (an arena SHOWN before its stream was done, as the
        // original's BSPShow blocks) is logged with its time: spec Phase 3.
#ifdef MDK_HW_PROFILE
        // This frame's stream and view work, for the arena-entry table (harness_3ds.h).
        float hwStreamMs = 0.0f;
        float hwViewsMs = 0.0f;
        int32_t hwViewsBuilt = 0;
#endif
        if (arena) {
            const std::string& streamCurrent =
                (runtime && !runtime->currentArena().empty()) ? runtime->currentArena()
                                                              : startArenaName;
            const std::string streamSecond = runtime ? runtime->secondArena() : std::string();
            const bool streamActive = runtime && runtime->secondActive();
            // WHAT THIS FRAME DRAWS: the scripts' drawn list (C# Viewer.cs:702,
            // LevelView.Draw(DrawnArenas)), or Kurt's arena until their first
            // tick has named one. ONE list for the stream, the views and the
            // draw below: the list is the middle of the scripts' tick and the
            // pair above is its end, and after a scripted teleport they differ
            // for a frame (review, 2026-11-18).
            const bool scriptsDrew = runtime && !runtime->drawnArenas().empty() &&
                                     !runtime->drawnArenas()[0].empty();
            if (!scriptsDrew) {
                startDrawn.assign(1, startArenaName);
            }
            frameDrawn = scriptsDrew ? &runtime->drawnArenas() : &startDrawn;
            mdk::ArenaStream& stream = level.stream();
            const mdk::ArenaStream::Stats before = stream.stats();
            // A reader that found pixels away during the last frame's draws (the
            // stream's backstop): correct, but a read nobody planned, so it is
            // said. Level 6's OLYM_9 is the one retail arena expected to.
            if (before.demandLoads != lastDemandLoads) {
                mdk::log3dsPrintf("stream: %d demand load(s) last frame, %d bytes in all",
                                  (int)(before.demandLoads - lastDemandLoads),
                                  (int)before.demandBytes);
                lastDemandLoads = before.demandLoads;
            }
            const std::string wasStreaming = stream.streaming();
            const double stream0 = nowSeconds();
            stream.update(streamCurrent, streamSecond, streamActive, *frameDrawn);
            const double stream1 = nowSeconds();
            const int32_t builtViews = arenaViews.sync(streamCurrent, streamSecond, *frameDrawn);
            const double stream2 = nowSeconds();
#ifdef MDK_HW_PROFILE
            hwStreamMs = (float)((stream1 - stream0) * 1000.0);
            hwViewsMs = (float)((stream2 - stream1) * 1000.0);
            hwViewsBuilt = builtViews;
#endif
            const mdk::ArenaStream::Stats& after = stream.stats();
            if (after.switches != before.switches) {
                const mdk::formats::Arena* target = level.arenaNamed(stream.resident());
                mdk::log3dsPrintf("stream: switch to %s (%d bytes; cur=%s second=%s%s)",
                                  stream.resident().c_str(),
                                  target ? (int)target->texturePrefixBytes() : 0,
                                  streamCurrent.c_str(), streamSecond.c_str(),
                                  streamActive ? " active" : "");
            }
            if (after.stalls != before.stalls) {
                mdk::log3dsPrintf("stream: STALL %s shown before its stream was done: %d bytes "
                                  "in %.1f ms",
                                  stream.resident().c_str(),
                                  (int)(after.stallBytes - before.stallBytes),
                                  (stream1 - stream0) * 1000.0);
            } else if (!wasStreaming.empty() && stream.streaming().empty() &&
                       after.readFailures == before.readFailures) {
                mdk::log3dsPrintf("stream: %s in (%d steps so far, %d arenas hold pixels, "
                                  "%d bytes)",
                                  wasStreaming.c_str(), (int)after.steps,
                                  (int)stream.residentArenas(), (int)stream.residentBytes());
                heapProbe("stream-in");
            }
            if (after.readFailures != before.readFailures) {
                mdk::log3dsPrintf("stream: read FAILED for %s (%d failures)",
                                  stream.resident().c_str(), (int)after.readFailures);
            }
            if (arenaViews.stats().failed != lastViewsFailed) {
                mdk::log3dsPrintf("views: build FAILED (%d so far); it is tried again",
                                  (int)arenaViews.stats().failed);
                lastViewsFailed = arenaViews.stats().failed;
            }
            if (builtViews > 0 || arenaViews.stats().released != lastViewsReleased) {
                mdk::log3dsPrintf("views: %d built (last %s, %.1f ms), %d released, %d kept",
                                  (int)builtViews, arenaViews.lastBuilt().c_str(),
                                  (stream2 - stream1) * 1000.0,
                                  (int)(arenaViews.stats().released - lastViewsReleased),
                                  (int)arenaViews.count());
                lastViewsReleased = arenaViews.stats().released;
            }

            // THE MUSIC OF THE ARENA THAT IS LOADING (docs/ARENA_STREAMING_SPEC.md
            // Phase 5). The original starts an arena's music preload when the
            // arena loads (arena_load -> 0x4192c4) and reads a step of it a frame
            // (game_frame -> 0x4193a4), so the track is in memory when Kurt walks
            // in. Here the arena that is loading is the pair's OTHER one: the
            // second arena, which is where a door or a corridor leads. Asked once
            // per change of that arena; the bank reads one step per frame.
            if (music) {
                if (streamSecond != musicPrefetchedFor) {
                    musicPrefetchedFor = streamSecond;
                    if (!streamSecond.empty()) {
                        const std::string track = music->trackOf(streamSecond);
                        if (bank.prefetch(track)) {
                            mdk::log3dsPrintf("music: prefetch %s for %s", track.c_str(),
                                              streamSecond.c_str());
                        }
                    }
                }
                if (!bank.prefetching().empty()) {
                    const std::string reading = bank.prefetching();
                    const int32_t failuresBefore = bank.stats().readFailures;
                    // Every step is timed: a step is an fopen, a seek and a read
                    // of up to 64 KiB, and the LAST one also decodes the whole
                    // track (about half a million samples) -- the one frame of
                    // the prefetch that is not small, so its time is on the line.
                    const double step0 = nowSeconds();
                    const bool more = bank.prefetchStep();
                    const double stepMs = (nowSeconds() - step0) * 1000.0;
                    if (more && stepMs > prefetchWorstMs) {
                        prefetchWorstMs = stepMs;
                    }
                    if (!more && bank.prefetching().empty()) {
                        if (bank.stats().readFailures != failuresBefore) {
                            mdk::log3dsPrintf("music: prefetch of %s FAILED (%d read failures); "
                                              "it is read when it is played",
                                              reading.c_str(), (int)bank.stats().readFailures);
                        } else {
                            mdk::log3dsPrintf("music: %s in (%d steps so far, worst read step "
                                              "%.1f ms, last step with the decode %.1f ms; %d "
                                              "bytes decoded in the bank, %d evicted)",
                                              reading.c_str(), (int)bank.stats().prefetchSteps,
                                              prefetchWorstMs, stepMs,
                                              (int)bank.decodedBytes(),
                                              (int)bank.stats().evictions);
                            heapProbe("music-in");
                        }
                        prefetchWorstMs = 0.0;
                    }
                }
                // AND THE MUSIC OF ARENAS THAT ARE GONE IS GIVEN BACK (the spec's
                // "free the decoded samples of an arena when it is deactivated"):
                // once a second, every decoded track but the one playing, the one
                // fading out (both pinned by their voices) and the loading arena's.
                // A level played end to end then holds three tracks, not all nine.
                if (mdk::log3dsPeriodic() && (frame % 60u) == 0u) {
                    const std::string keep[3] = {
                        music->playing(), music->fading(),
                        streamSecond.empty() ? std::string() : music->trackOf(streamSecond)};
                    const size_t freed = bank.releaseMusicExcept(keep, 3);
                    if (freed != 0) {
                        mdk::log3dsPrintf("music: released %d bytes of tracks no arena in "
                                          "memory plays (%d decoded bytes left)",
                                          (int)freed, (int)bank.decodedBytes());
                    }
                    // And the loading arena's track is asked for AGAIN: nothing if
                    // it is decoded or being read, a new prefetch if the budget
                    // evicted it since (the budget does not know which track is
                    // waiting for Kurt; this does).
                    if (!keep[2].empty() && bank.prefetch(keep[2])) {
                        mdk::log3dsPrintf("music: prefetch %s for %s again (it was evicted)",
                                          keep[2].c_str(), streamSecond.c_str());
                    }
                }
            }
            // The blocking reads of this frame that nothing above timed (a
            // script's play of a streamed sound, a track read back after it was
            // released): counted by the bank, said here.
            if (bankReady) {
                const mdk::audio::SoundBank::Stats& audio = bank.stats();
                if (audio.prefetchStalls != audioStallsSaid ||
                    audio.demandReads != audioDemandSaid) {
                    mdk::log3dsPrintf("audio: STALL %d streamed sound(s) read whole inside "
                                      "the frame (%d bytes), %d prefetch(es) finished early",
                                      (int)(audio.demandReads - audioDemandSaid),
                                      (int)(audio.demandBytes - audioDemandBytesSaid),
                                      (int)(audio.prefetchStalls - audioStallsSaid));
                }
                audioStallsSaid = audio.prefetchStalls;
                audioDemandSaid = audio.demandReads;
                audioDemandBytesSaid = audio.demandBytes;
            }
        }

        MdkFrameParams params;
        params.elapsed = elapsed;
        params.frame_index = frame;
        params.clear_colour = 0xFF102030u;
        params.present = true;
        params.screenshot_path = capturePath[0] ? capturePath : nullptr;

        // P2: the camera is handed to the renderer every frame, whether or not a
        // step ran, and every GPU state the frame needs is set inside
        // begin/end_frame by the backend.
        const uint64_t spanBegin0 = mdk::harnessProfileTick();
        mdk_render_begin_frame(renderer, &params);
        const uint64_t spanDraw0 = mdk::harnessProfileTick();
        if (capturePath[0]) {
            capturePath[0] = '\0';  // one frame, one picture (after the copy above)
        }
        mdk_render_set_camera(renderer, &camera);

        // The enhanced look's light (LevelLight.Update): the scripts' current
        // arena's rig, or the start arena's before the runtime exists. The Linux
        // build rebuilds it every frame; here it is rebuilt only when the arena
        // CHANGES, because enhancedLighting walks the sky panorama and every
        // triangle of the arena -- level-load work, not frame work, on an ARM11.
        // The rig carries the haze's colour and density (SPEC.md 7.9).
        {
            const mdk::formats::Arena* lightArena = arena;
            if (runtime) {
                if (const mdk::formats::Arena* current =
                        level.arenaNamed(runtime->currentArena())) {
                    lightArena = current;
                }
            }
            if (lightArena && lightArena != litArena) {
                litArena = lightArena;
                const mdk::Lighting rig = mdk::enhancedLighting(level.dti(), *lightArena);
                mdk_render_set_lighting(renderer, &rig);
                mdk::log3dsPrintf("light: arena %s haze rgb=(%.3f,%.3f,%.3f) density=%.5f",
                                  lightArena->name().c_str(), rig.haze_colour.r,
                                  rig.haze_colour.g, rig.haze_colour.b, rig.haze_density);
            }
        }

        // The sky first (see SkyRange): centred on the camera, DoubleSided.
        if (skyMesh >= 0) {
            for (int32_t i = 0; i < skyRangeCount; i++) {
                const SkyRange& range = skyRanges[i];
                if (range.texture < 0 || range.count <= 0) {
                    continue;
                }
                mdk::MeshDraw sky;
                sky.mesh = skyMesh;
                sky.first_vertex = range.first;
                sky.vertex_count = range.count;
                sky.material.texture = range.texture;
                sky.material.pass = mdk::Pass::DoubleSided;
                sky.world[0] = sky.world[5] = sky.world[10] = sky.world[15] = 1.0f;
                sky.world[12] = camera.position.x;
                sky.world[13] = camera.position.y;
                sky.world[14] = camera.position.z;
                mdk_render_draw_mesh(renderer, &sky);
            }
        }

        // The arenas the scripts say are drawn: Kurt's and the active second
        // (C# Viewer.cs:702, LevelView.Draw(DrawnArenas)); the start arena until
        // the runtime's first tick has named one.
        if (arena && frameDrawn) {
            arenaViews.draw(*frameDrawn);
        }
        if (runtime) {
            // C# Viewer.DrawScene draws the objects after the arena and before
            // Kurt (Viewer.cs:702-708): every object the scripts hold, in its
            // list's order, skipping the invisible ones. The runtime keeps removed
            // objects in its deque for pointer stability, so they are skipped here
            // too — the C#'s list has already taken them out.
            //
            // The transform is the BLENDED one. The scripts tick at 1/30 s and this
            // loop presents faster, so most frames draw an object part-way from its
            // last tick to its current one: the C# hands ObjectView.Draw
            // `blend.TransformOf(obj)` (Viewer.cs:959), whose alpha is
            // ScriptRuntime.TickBlend of the step loop's leftover — which is
            // exactly what `pending` holds here, after the loop above took every
            // whole step out of it.
            //
            // What is NOT here is the other half of TransformOf: `obj == _driven`
            // draws the object shifted by the step's lag, which needs Rides (the
            // ridden object) and FrameBlend's Kurt pose, neither of which this port
            // has. ScriptRuntime::ridesRidden_ is null for every tick of every
            // shipped level, so the oracle does not take that branch either here.
            //
            // And the ropes and sniper scope that the C# draws between the
            // objects and Kurt (Viewer.cs:705-706) are not ported; see the
            // header. The effects (Viewer.cs:707) draw below.
            const float tickAlpha = runtime->tick_blend(pending);
            objectStream.reset();
            for (const mdk::scripts::MdkObject& obj : runtime->objects()) {
                if (!obj.visible || runtime->isRemoved(&obj)) {
                    continue;
                }
                objectView.addObject(obj, objectLook(obj.arena), obj.transform_at(tickAlpha),
                                     &objectStream);
            }
            objectDraw.draw(renderer, &surfaces, objectStream);
            // The frame's own cost, kept for the log line below. The peak over a
            // run is what a level costs the stream mesh; one frame's count is not.
            objectCorners = (int32_t)objectStream.vertices.size();
            objectDraws = (int32_t)objectStream.draws.size();
            if (objectCorners > objectPeakCorners) {
                objectPeakCorners = objectCorners;
            }
            if (objectDraws > objectPeakDraws) {
                objectPeakDraws = objectDraws;
            }
            // THE LINEAR-MEMORY ANSWER, printed rather than assumed. The C# sizes
            // its stream mesh at Renderer.StreamVertices (1 << 18 vertices = 5.2
            // MiB) and keeps it for the process; on this target that is 5.2 MiB out
            // of a linear heap measured in single megabytes, and a failed
            // linearAlloc is a silently missing mesh. So the mesh starts at 4096
            // corners and doubles to what the frames reach — and what it reaches is
            // what a level costs, at 5 floats (x, y, z, u, v) a corner. Logged on
            // every change, so the growth and the steady state are both readable.
            //
            // NOT LOGGED BEFORE THERE IS A MESH (review-9 F6). `ObjectDraw::draw`
            // returns without creating anything while the frame has no corners, so a
            // frame with a runtime and nothing to draw used to print `stream mesh 0
            // corners = 0 KB linear (0 corners, 0 draws this frame)` -- the same shape
            // as renderer_c3d's real `linearAlloc(...) FAILED` line, so a future run
            // whose mesh genuinely fails to allocate would read exactly like this
            // benign first frame. A capacity of 0 means "no mesh yet", and there is
            // nothing to report until the pass makes one.
            //
            // BUT A REFUSED ALLOCATION IS NOT A BENEFIT OF THE DOUBT (review-10 F3).
            // Suppressing the 0 line also suppressed the only line that could ever
            // have reported a failure, because a failed `ensure` leaves the capacity
            // at 0 too: the objects would vanish from the frame and the guest log
            // would say nothing at all. "Capacity 0" therefore cannot tell the two
            // cases apart, so the pass RECORDS the refusal (object_draw.h
            // allocationFailed) and the refusal is what is printed here, not an
            // inference from a size.
            if (objectDraw.allocationFailed()) {
                // Edge-triggered, but RE-ARMED: the flag clears when a later
                // attempt succeeds, so a second refusal after a recovery — a grow
                // to 8192 corners after a first success at 4096 — still prints
                // (review-11 F7). A one-shot latch for the whole run would print
                // the first, smaller refusal and be silent about the bigger one.
                if (!streamFailureLogged) {
                    streamFailureLogged = true;
                    // THE CAPACITY THAT WAS REFUSED, not the frame's corner count.
                    // `ensure` rounds up to a power of two and it is that rounded
                    // capacity the backend was asked for, so printing the frame's
                    // 480 corners here reported a 9 KB refusal for an 80 KB one and
                    // disagreed with renderer_c3d's own `linearAlloc(N) FAILED`
                    // line by up to 8x (review-11 F1). This number and that byte
                    // count are now the same request.
                    const int32_t attempted = objectDraw.attemptedCapacity();
                    mdk::log3dsPrintf("objects: stream mesh of %d corners = %d KB "
                                      "linear — linearAlloc FAILED, no objects drawn",
                                      (int)attempted,
                                      (int)((int64_t)attempted * 5 * sizeof(float) /
                                            1024));
                }
            } else {
                streamFailureLogged = false;
            }
            if (objectDraw.capacity() > 0 && objectDraw.capacity() != streamCapacityLogged) {
                streamCapacityLogged = objectDraw.capacity();
                mdk::log3dsPrintf("objects: stream mesh %d corners = %d KB linear "
                                  "(%d corners, %d draws this frame)",
                                  (int)streamCapacityLogged,
                                  (int)((int64_t)streamCapacityLogged * 5 * sizeof(float) /
                                        1024),
                                  (int)objectCorners, (int)objectDraws);
            }
        }
        if (runtime) {
            // The pieces and sprite effects, where the C# draws them:
            // after the objects, before Kurt (Viewer.cs:707). The ropes
            // and sniper scope the C# draws alongside are not ported.
            // A refusal is reported like the object pass's, edge-triggered
            // and re-armed, but on this pass's own latch.
            effectsView.build(*runtime, camera, &effectsFrame);
#if defined(MDK_AUTOQUIT) && defined(MDK_HW_GLOWPROBE)
            // THE HARNESS'S GLOW PROBE (`make AUTOQUIT=.. GLOWPROBE=1`): one halo,
            // 30 units in front of the camera, 12 across, warm white -- the
            // billboard path drawn on hardware without waiting for a script to
            // explode something. A harness build only.
            {
                mdk::Mat4 view;
                mdk::Mat4 projection;
                memcpy(view.m, camera.view, sizeof(view.m));
                memcpy(projection.m, camera.projection, sizeof(projection.m));
                mdk::Vec3 right;
                mdk::Vec3 up;
                mdk::Vec3 forward;
                mdk::effectsBillboardBasis(mdk::multiply(view, projection), &right, &up, &forward);
                const mdk::Vec3 at{camera.position.x + forward.x * 30.0f,
                                   camera.position.y + forward.y * 30.0f,
                                   camera.position.z + forward.z * 30.0f};
                mdk::GlowQuad probe;
                mdk::effectsQuadVertices(at, right, up, 12, 12, probe.corners);
                probe.colour[0] = 1.0f;
                probe.colour[1] = 0.8f;
                probe.colour[2] = 0.5f;
                effectsFrame.glows.push_back(probe);
            }
#endif
            effectsDraw.draw(renderer, &surfaces, effectsFrame);
            if (mdk::log3dsPeriodic() && (frame % 300u) == 0u) {
                mdk::log3dsPrintf("effects: frame %d: %d sprite effects, %d with a halo, %d not "
                                  "yet measured (pixels away)",
                                  (int)frame, (int)effectsView.spriteEffects(),
                                  (int)effectsView.glowingEffects(),
                                  (int)effectsView.unmeasuredGlows());
            }
            if (effectsDraw.allocationFailed()) {
                if (!effectsFailureLogged) {
                    effectsFailureLogged = true;
                    const int32_t attempted = effectsDraw.attemptedCapacity();
                    mdk::log3dsPrintf("effects: mesh of %d corners = %d KB "
                                      "linear — linearAlloc FAILED, no pieces or effects drawn",
                                      (int)attempted,
                                      (int)((int64_t)attempted * 5 * sizeof(float) / 1024));
                }
            } else {
                effectsFailureLogged = false;
            }
        }
        if (kurtSpriteReady && playerReady) {
            // THE PACK STREAM'S PUMP (KurtSprite::pumpStream): the read the
            // reader thread finished, if any, staged -- no I/O on this thread. A
            // failed read is logged once and its frames stay 16-bit.
            kurtSprite.pumpStream();
            if (const char* failure = kurtSprite.takeStreamFailure()) {
                mdk::log3dsPrintf("%s", failure);
            }
            // C# Viewer.DrawScene draws Kurt after the level and before the HUD.
            kurtSprite.draw(renderer, player, player.feet, camera.position, camera.forward,
                            camera.up, mdk::follow_camera::kFieldOfView, &space);
        }
        if (runtime) {
            // Viewer.cs:699: the scripts' Eye is the camera (an explosion faces it).
            runtime->eye = camera.position;
        }
        const bool showHint = menuReady && !menu.open() && now < hintUntil;
        if (hudReady || menu.open() || showHint) {
            // The HUD is the last thing drawn before the frame ends, which is the
            // C#'s order (Viewer.DrawScene: the canvas is the Overlay pass, and
            // the queue draws Overlay after every other pass). The canvas seams
            // queue it rather than drawing it: see renderer_c3d.cpp's canvas
            // section for why a 3DS canvas cannot flush at its own boundary.
            //
            // What the state carries is what this port has: health, death and the
            // two combat flashes, all read off Kurt himself exactly as
            // Viewer.HudStateOf passes them (Viewer.cs:913), and the chain gun's target
            // bar off the scripts. The inventory stays empty until the pickup subsystem
            // lands, because the C# starts it empty and nothing here may invent it.
            mdk_render_canvas_begin(renderer);
            if (hudReady) {
                mdk::HudState state;
                state.health = player.health();
                state.dead = player.dead;
                state.hurtFlash = player.combat().hurtFlash();
                state.whiteFlash = player.combat().whiteFlash();
                // The inventory (Viewer.HudStateOf, Viewer.cs:905-914): the slots as (item, count),
                // the selected slot and the super chain gun's ticks left.
                const mdk::Kurt& inventory = player.combat();
                for (int32_t i = 0; i < inventory.slotCount() && i < mdk::HudState::kMaxSlots; i++) {
                    state.slots[i] = std::make_pair((int32_t)inventory.slots()[i].item,
                                                    inventory.slots()[i].count);
                    state.slotCount = i + 1;
                }
                state.selected = inventory.selected();
                state.superChainGun = inventory.superChainGun();
                // The target's health bar while the chain gun hits it (Viewer.cs:912, GetBar).
                if (runtime) {
                    const mdk::scripts::ScriptRuntime::BarValues bar = runtime->bar();
                    state.barHealth = bar.health;
                    state.barMax = bar.max;
                }
                hud.draw(renderer, state);
            }
            if (menu.open()) {
                menu.draw(renderer, menuBig, menuSmall, mdk_render_canvas_width(renderer),
                          enhancedToggles);
            }
            if (showHint) {
                const char* hint = "START: PAUSE / EFFECTS";
                const float hintX = (mdk_render_canvas_width(renderer) -
                                     (float)menuSmall.width(hint) * 1.5f) * 0.5f;
                menuSmall.draw(renderer, hint, hintX, 24.0f, 1.5f,
                               mdk::Colour{1.0f, 0.85f, 0.4f, 1.0f});
            }
            mdk_render_canvas_end(renderer);
        }
        const uint64_t spanEnd0 = mdk::harnessProfileTick();
        mdk_render_end_frame(renderer);
        if (frame > 0) {
            mdk::harnessProfileSpans(spanSim, steps - spanSteps0, spanDraw0 - spanBegin0,
                                     spanEnd0 - spanDraw0,
                                     mdk::harnessProfileTick() - spanEnd0, spanAudio);
        }
#ifdef MDK_HW_PROFILE
        // The arena-entry table's sample (harness_3ds.h): counters and the arena
        // pair, copied into RAM; nothing reaches the SD card before exit.
        {
            mdk::HarnessArenaFrame sample;
            sample.arena = (runtime && !runtime->currentArena().empty())
                               ? runtime->currentArena().c_str()
                               : startArenaName.c_str();
            sample.second = runtime ? runtime->secondArena().c_str() : "";
            sample.secondActive = runtime && runtime->secondActive();
            const mdk::ArenaStream::Stats& streamStats = level.stream().stats();
            sample.streamSwitches = streamStats.switches;
            sample.streamBytes = streamStats.bytesRead;
            sample.streamStalls = streamStats.stalls;
            sample.streamMs = hwStreamMs;
            sample.viewsBuilt = hwViewsBuilt;
            sample.viewsMs = hwViewsMs;
            MdkC3dFirstUse made;
            mdk_render_c3d_first_use(renderer, &made);
            sample.texturesMade = made.texturesMade;
            sample.hdImported = made.hdImported;
            if (kurtSpriteReady) {
                sample.kurtReads = kurtSprite.streamStats().reads;
                sample.kurtStandIns = kurtSprite.streamStats().standIns;
            }
            sample.logLines = mdk::log3dsLinesWritten();
            mdk::harnessArenaFrame(frame, sample);
        }
#endif
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
            // Forward and fire through the RESOLVED bindings (controls.ini may
            // have moved them): their first buttons, held together.
            probe.held = (controls.buttons[mdk::kKeyForward] &
                          (0u - controls.buttons[mdk::kKeyForward])) |
                         (controls.buttons[mdk::kKeyFire] & (0u - controls.buttons[mdk::kKeyFire]));
            probe.padX = 155;
            probe.padY = 0;
            mdk::Input selftest;
            mdk::input3dsApply(probe, 1.0f / 60.0f, controls, &selftest);
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
                              (int)mix->voices().size(), (int)mix->device().queueFrames());
            // The scripts' own state, on the frame the input selftest reports on.
            // `objects` is the live count (the runtime keeps removed objects in its
            // deque for pointer stability, so objectCount subtracts them) and
            // `arena` is the pin the step loop is using.
            mdk::log3dsPrintf("scripts: runtime=%d objects=%d arena=%s mix=%s",
                              runtime ? 1 : 0, runtime ? (int)runtime->objectCount() : 0,
                              runtime && !runtime->currentArena().empty()
                                  ? runtime->currentArena().c_str()
                                  : "-",
                              runtime ? "runtime" : "standalone");
        }
        if (frame <= 5u || (mdk::log3dsPeriodic() && (frame % 60u) == 0u)) {
            mdk::log3dsPrintf("frame %lu elapsed=%.4f steps=%u feet=(%.1f,%.1f,%.1f) "
                              "yaw=%.1f health=%d eye=(%.1f,%.1f,%.1f) voices=%d plays=%d "
                              "objects=%d obj=%dc/%dd peak=%dc/%dd",
                              (unsigned long)frame, elapsed, (unsigned)steps,
                              player.feet.x, player.feet.y, player.feet.z, player.yaw,
                              player.health(), camera.position.x, camera.position.y,
                              camera.position.z, (int)mix->voices().size(),
                              (int)mix->device().nextId() - 1,
                              runtime ? (int)runtime->objectCount() : 0,
                              (int)objectCorners, (int)objectDraws,
                              (int)objectPeakCorners, (int)objectPeakDraws);
        }
        if (audioReady && mdk::log3dsPeriodic() && (frame % 60u) == 0u) {
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
            if (mdk::kDspAudio) {
                // THE DSP PATH'S LEDGER (audio_dsp_3ds.h). The line above counts the
                // software sink and reads 0 here; this one says what the DSP path did:
                // voices bound, evicted, rebound, cut and left to finish; the sample
                // arena; the music stream (buffers queued, drained-channel restarts =
                // underruns); and ndsp's own frame counters.
                const mdk::AudioDspStats ds = mdk::audioDspStats();
                // `frame` goes in as %u and not as the sink line's %lu: that line's
                // `(unsigned long)frame, audioClock` argument pair is the one
                // tests/scripts/ndsp_sink_test.sh pins, and a second spelling of it
                // here would let a mutant of the first pass unseen.
                mdk::log3dsPrintf("audio: dsp frame=%u secs=%.3f bound=%d binds=%u evicted=%u "
                                  "rebound=%u cut=%u drained=%u arena=%u/%u fills=%u "
                                  "arenaevict=%u nospace=%u music=%d queued=%u restarts=%u "
                                  "dspframes=%u dspdropped=%u",
                                  (unsigned)frame, audioClock, (int)ds.bound, (unsigned)ds.binds,
                                  (unsigned)ds.evictions, (unsigned)ds.rebinds,
                                  (unsigned)ds.cuts, (unsigned)ds.drains,
                                  (unsigned)ds.arenaUsed, (unsigned)ds.arenaBytes,
                                  (unsigned)ds.cacheFills, (unsigned)ds.cacheEvictions,
                                  (unsigned)ds.cacheMisses, (int)ds.musicActive,
                                  (unsigned)ds.musicQueued, (unsigned)ds.musicRestarts,
                                  (unsigned)ds.dspFrames, (unsigned)ds.dspDropped);
            }
        }

        if (g_frameBudget != 0 && frame >= g_frameBudget) {
            exitReason = "frame budget reached";
            break;
        }
    }

    printf("[3ds] exiting after %lu frames\n", (unsigned long)frame);

    mdk::harnessProfileReport();
    mdk::log3dsPrintf("exiting after %lu frames (%u steps): %s", (unsigned long)frame,
                      (unsigned)steps, exitReason);
    if (kurtSpriteReady) {
        // The pack stream's record for the run, once, at exit (never inside play).
        const mdk::KurtSprite::StreamStats& stream = kurtSprite.streamStats();
        // `held`: draws that showed Kurt's previous frame while the wanted one's
        // bytes were in flight (never a 16-bit stand-in made inside the frame);
        // `unheld`: draws with nothing to hold, Kurt not drawn; `HD lookups
        // skipped`: compressed frames the HD pack was not asked for.
        mdk::log3dsPrintf("sprites: stream: %d animations read (%d bytes), %d failed; %d draws "
                          "held, %d unheld, %d refused, %d HD lookups skipped; %u bytes staged, "
                          "peak %u of %u, %d dropped",
                          (int)stream.reads, (int)stream.bytesRead, (int)stream.failures,
                          (int)stream.holds, (int)stream.holdMisses, (int)stream.importFailures,
                          (int)stream.hdSkipped,
                          (unsigned)stream.staged, (unsigned)stream.stagedPeak,
                          (unsigned)mdk::KurtSprite::kStagedCacheBytes, (int)stream.dropped);
    }
    aptUnhook(&s_lifecycleCookie);
    // The arenas' GPU objects go while the renderer is still there.
    arenaViews.clear();
    mdk_render_destroy(renderer);
    mdk::audioClose();
    // The DSP path: join its worker and give back its bank pins while the bank
    // (a local of this function) is still alive.
    mdk::audioDspClose();
    mdk::romfsFileSystemExit();
    // The log is closed BEFORE gfxExit(): libctru's gfxExit() tears the SD
    // card's filesystem down, and a FILE* opened on it cannot be flushed
    // afterwards -- the final line would be silently lost.
    mdk::log3dsClose();
    gfxExit();
    return 0;
}

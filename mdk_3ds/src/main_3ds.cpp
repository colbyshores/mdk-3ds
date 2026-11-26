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
//   * The screens the flow can ask for other than the level, the menu and the
//     briefing (statistics, stream, fall, end movies): skipped the C#'s way
//     (GameFlow::skipUnported). The main menu and the briefing are package #20a's
//     (menu_screens_3ds.h): the game boots into the menu.
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
#include "formats/cmi.h"
#include "formats/file_system.h"
#include "formats/mdk_data.h"
#include "game/arena_space.h"
#include "game/arena_views.h"
#include "game/enhanced_config.h"
#include "game/enhanced_menu.h"
#include "game/cheats_menu.h"
#include "platform/controls_3ds.h"
#include "game/font_view.h"
#include "game/cutscene_camera.h"
#include "game/strike_view.h"
#include "game/follow_camera.h"
#include "fall/fall_sim.h"
#include "game/game_flow.h"
#include "game/game_settings.h"
#include "game/pause_menu.h"
#include "game/screen_view.h"
#include "game/save_games.h"
#include "game/save_prompt.h"
#include "game/viewer_saves.h"
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
#include "game/loading_screen.h"
#include "game/sniper_view.h"
#include "render/material_resolver.h"
#include "render/render.h"
#include "formats/ascii_case.h"
#include "scripts/game_stats.h"
#include "scripts/script_runtime.h"
#include "util/math_lut.h"
#include "platform/filesystem_romfs.h"
#include "game/sky_modes.h"
#include "platform/input_3ds.h"
#include "platform/vfp_fpscr.h"
#include "platform/log_3ds.h"
#include "config_3ds.h"
#include "level_load_3ds.h"
#include "menu_screens_3ds.h"
#include "packs_3ds.h"
#include "platform/clock_3ds.h"
#include "platform/heap_probe_3ds.h"
#include "stream_3ds.h"

namespace {

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
// The top screen's picture per eye, in pixels: the size the draw culls measure
// on screen against (projectedPixelSize).
constexpr int32_t kTopWidth = 400;
constexpr int32_t kTopHeight = 240;

// The sound bank's decoded store, bounded (see where it is set).
const size_t kDecodedSoundBudgetBytes = (size_t)4 << 20;

// THE TINY-ENEMY CULL (EnhancedToggles::tinyEnemyPixels), its per-object state:
// one byte per slot of the runtime's object deque (slots never move: removed
// objects keep theirs). Known: the type was looked at; Candidate: the type is
// one the cull may hide; Hidden: hidden last frame (ObjectView's hysteresis
// byte, kept apart so its pointer can be handed over).
const uint8_t kTinyKnown = 1;
const uint8_t kTinyCandidate = 2;

// Whether an object's TYPE may be hidden when tiny: the original's enemy types
// (GameStats.IsEnemy), but not its missiles and bombs -- projectiles stay drawn
// -- and never a boss: the XB family (XB1 is the first level's DANT_10 boss,
// max health 9000; XB, XB2, XB3, XBSHARK, XBSHIP and XBT are kept with it) and
// XGUNTA (Gunter). The boss the health bar shows is excluded per frame by the
// caller too. Asked once per object: it upper-cases a string.
bool tinyEnemyType(const mdk::scripts::MdkObject& obj) {
    if (!mdk::scripts::isEnemyType(obj.type_name)) {
        return false;
    }
    const std::string upper = mdk::formats::toUpperAscii(obj.type_name);
    if (upper == "XG_MISS" || upper == "XG_BOMB1") {
        return false;
    }
    if (upper.compare(0, 2, "XB") == 0 || upper == "XGUNTA") {
        return false;
    }
    return true;
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
    // STATE, so each worker sets its own (see audio_dsp_3ds.cpp).
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
    std::set_new_handler(mdk::onHeapExhausted);
    mdk::heapProbe("boot");
    // P6 evidence: the split heap_split.cpp asks for, printed before any
    // level is loaded. `cap` is what the allocator granted main; `lin` is the
    // GPU heap's size before the renderer touches it.
    mdk::log3dsPrintf("main: heap split main=%u linear=%u (avail=%u)", (unsigned)envGetHeapSize(),
                      (unsigned)envGetLinearHeapSize(),
                      (unsigned)(envGetHeapSize() + envGetLinearHeapSize()));
    mdk::log3dsPrintf("main: FZ/DN ok (fpscr=0x%08lx)",
                      (unsigned long)mdk::vfpReadFpscr());

    // NEW 3DS: ask for the 804 MHz clock and the L2 cache (one ptm:sysm call;
    // libctru re-applies it each time the app wakes from HOME or an applet).
    // A .3dsx launch starts at 268 MHz with L2 off, and so does the CIA
    // (cia/mdk.rsf), so nothing gets the fast mode without asking. The Old 3DS
    // is not asked. svcGetSystemTick keeps its 268 MHz rate in either mode, so
    // the frame clock (clock_3ds.h) is unaffected.
    bool isNew3ds = false;
    APT_CheckNew3DS(&isNew3ds);
    if (isNew3ds) {
        osSetSpeedupEnable(true);
    }
    mdk::log3dsPrintf("main: %s 3DS, %s", isNew3ds ? "New" : "Old",
                      isNew3ds ? "804 MHz + L2 requested" : "268 MHz (no speedup to request)");

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
    mdk::loadConfig3ds(&enhancedToggles);
    // THE CONTROLS (owner, 2026-11-19): sdmc:/3ds/mdk/controls.ini, one line
    // per action (platform/controls_3ds.h has the format and the defaults).
    // Cold, once, here: a missing file is the defaults and is written out once
    // so the owner can edit it; a bad line keeps that action's default and an
    // unknown key is ignored, both logged; the resolved table is one line.
    static mdk::ControlBindings controls = mdk::defaultControlBindings();
    mdk::loadControls3ds(&controls);

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
    mdk::heapProbe("renderer");

    // ---------------------------------------------------------------------
    // The flow, BEFORE the level: the flow is what decides which level plays
    // (C# Game owns _state.Level and hands it to the Viewer), so the frontend
    // asks it rather than naming a level of its own. It starts at the main menu
    // (package #20a).
    // ---------------------------------------------------------------------
    // THE ENTRY SCREEN (package #20a, the owner un-cut 2026-11-20): the main menu,
    // as the C#'s game starts (Start.Menu).
    mdk::g_toggles = &enhancedToggles;
    mdk::flow::FlowOptions options;
    options.start = mdk::flow::Start::Menu;
    // THE INTRO1A SPLASH at boot (package #20c): the C#'s default (Program.cs:241,
    // no --menu).
    options.splash = true;
    mdk::flow::GameFlow flow(options);
    // THE SAVES (package #19): sdmc:/3ds/mdk/saves, an explicit path (AGENTS.md P5),
    // the C#'s `<user folder>/saves`. LASTGAME goes at start, as the C#'s Game
    // constructor deletes it. Every write is whole-to-temporary-then-rename and every
    // read is size-checked (game/save_games.h): a full or missing card is a logged
    // `false`, never a retry.
    const mdk::flow::SaveGames saves("sdmc:/3ds/mdk/saves");
    saves.remove(mdk::flow::SaveGames::kLastGame);
    flow.setSaves(saves);
    mdk::flow::Screen screen = flow.handle(flow.firstEvent());
    mdk::log3dsPrintf("flow: screen=%d level=%d", (int)screen, (int)flow.state().level);
    if (screen != mdk::flow::Screen::Level && screen != mdk::flow::Screen::Menu &&
        screen != mdk::flow::Screen::Briefing && screen != mdk::flow::Screen::Stream &&
        screen != mdk::flow::Screen::Fall && screen != mdk::flow::Screen::EndMovie) {
        mdk::log3dsPrintf("flow asked for screen %d, which this frontend cannot show; exiting",
                          (int)screen);
        mdk_render_destroy(renderer);
        mdk::romfsFileSystemExit();
        mdk::log3dsClose();
        gfxExit();
        return 0;
    }

    // The run's frame and step counts and its end: run-scoped, so the frame
    // budget and the log count across every level the run plays.
    uint32_t frame = 0;
    uint32_t steps = 0;
    const char* exitReason = "aptMainLoop returned false";
    mdk::heapProbe("run-start");
    // The menus' screens (menu_screens_3ds.h).
    mdk::FrontScreens3ds front;
    front.renderer = renderer;
    front.flow = &flow;
    front.settings = &mdk::g_gameSettings;
    front.save = mdk::saveConfigNow;
    front.controls = &controls;
    front.toggles = &enhancedToggles;
    front.now = mdk::nowSeconds;
    front.resumePending = &g_resumePending;
    front.frame = &frame;
    front.frameBudget = g_frameBudget;
    // The main menu's Continue and Saved Game page read the saves (package #19).
    front.saves = &saves;
    // The START menu's OPTIONS row opens the C# PauseMenu's options pages
    // (pause_menu.h's openOptions). Static: it outlives the levels, as the menu
    // and its fonts do.
    static mdk::menu::PauseMenu pauseOptions;
    // THE CHEATS (gap package #21): the START menu's CHEATS row opens this page of
    // toggles, because this target has no keyboard for TOOSCARYFORME and no console for
    // god and noclip. The switches are the session's, as the C#'s DevSession holds god
    // and noclip for every level (Viewer.cs:286-288); gore is the settings' own flag,
    // mirrored when the page opens.
    static mdk::CheatsMenu cheatsPage;
    static mdk::CheatsMenu::Switches cheatSwitches;

    // =====================================================================
    // THE LEVEL LOOP (package 2, the next-level transition). One pass per level:
    // load, play, and -- when the level ends and the flow names another level
    // (GameFlow::skipUnported: the screens this port lacks, skipped the way the
    // C# skips them; the stream, statistics, fall and save prompt are shown) -- tear the level down and go
    // round again. The C#'s Game.Show does the same between two screens: the old
    // screen disposed, every voice stopped, the renderer's scope released
    // (Game.cs:539-547), then the next Viewer built.
    //
    // THE ORDER OF THE TEARDOWN is at the level block's end (marked "END OF THE
    // LEVEL"): Kurt's pack read settled, the music, the DSP path (its worker joined
    // and its bank pins given back while the bank lives), the runtime, the pose
    // caches, the arena views; then the block's brace destroys the rest in reverse
    // order of declaration (the passes, the HUD, Kurt's sprite, the bank and its SNI
    // archives, the CMI, the level data, the HD pack); then the renderer's scope
    // releases what the level made through it -- textures, palettes, meshes, newest
    // first, so linear memory goes back in the reverse order it was taken -- and two
    // empty frames give the GPU its completion boundary (C3D_FrameBegin waits for the
    // last queue; the deferred releases are freed there, AGENTS.md P3) BEFORE the
    // next level allocates anything.
    //
    // The body is NOT re-indented (a 1900-line whitespace diff in a file other
    // packages edit).
    // =====================================================================
    for (;;) {
    // THE STREAM (package #18b), when the flow shows one: after a level's end (the
    // level-end hook goes round the loop for it, after the level's teardown). Its
    // own renderer scope and the same two
    // empty frames a level's teardown gets; then its Event is handled, the screens
    // this port still lacks are skipped, and the level that follows loads below.
    if (flow.screen() == mdk::flow::Screen::Stream) {
        const MdkScope streamScope = mdk_render_mark(renderer);
        // C# ShowStream's seed: DefaultSeed for a direct stream, else
        // Environment.TickCount -- the console's tick counter.
        const uint32_t clockSeed = (uint32_t)svcGetSystemTick();
        const int32_t streamLevel = flow.state().level;
        int32_t streamEndHealth = flow.streamHealth();
        const char* streamExit = exitReason;
        mdk::flow::Event streamEvent = mdk::flow::Event::Quit;
        {
            const mdk::formats::MdkData streamData =
                mdk::formats::MdkData::find(mdk::formats::fileSystem());
            mdk::Stream3dsFrontend streamFrontend;
            streamFrontend.renderer = renderer;
            streamFrontend.controls = &controls;
            streamFrontend.frame = &frame;
            streamFrontend.frameBudget = g_frameBudget;
            streamFrontend.resumePending = &g_resumePending;
            streamFrontend.heapProbe = mdk::heapProbe;
            streamFrontend.difficulty = (mdk::Kurt::Difficulty)mdk::g_gameSettings.difficulty;
            streamEvent = mdk::runStream3ds(streamFrontend, streamData, streamLevel,
                                            flow.streamHealth(), flow.streamSeed(clockSeed),
                                            &streamEndHealth, &streamExit);
        }
        mdk_render_release(renderer, streamScope);
        mdk::streamBlankFrames3ds(renderer, frame, 2);
        mdk::heapProbe("stream-teardown");
        if (streamEvent == mdk::flow::Event::Quit) {
            exitReason = streamExit;
            break;
        }
        // C# AfterStream reads the stream screen's Health before it shows the next.
        flow.setStreamHealth(streamEndHealth);
        screen = flow.handle(streamEvent);
        mdk::log3dsPrintf("flow: the stream after level %d ended (health %d); the flow shows %s",
                          (int)streamLevel, (int)streamEndHealth,
                          mdk::flow::GameFlow::screenName(screen));
        mdk::flow::GameFlow::SkipTrail trail;
        screen = flow.skipUnported(&trail);
        for (int32_t i = 0; i < trail.count; i++) {
            mdk::log3dsPrintf("flow: skipped %s (package #%d not ported)",
                              mdk::flow::GameFlow::screenName(trail.skipped[i]),
                              (int)mdk::flow::GameFlow::screenPackage(trail.skipped[i]));
        }
        if (screen == mdk::flow::Screen::Level) {
            mdk::log3dsPrintf("flow: next level %d at frame %lu, entered at health %d",
                              (int)flow.state().level, (unsigned long)frame,
                              (int)flow.levelHealth());
        } else if (screen == mdk::flow::Screen::Menu || screen == mdk::flow::Screen::Fall ||
                   screen == mdk::flow::Screen::Statistics ||
                   screen == mdk::flow::Screen::SavePrompt) {
            // Kurt died in the Gunter stream: the main menu (package #20a), below. Or
            // the statistics (#20b), then the next level's fall (#17b); or the save
            // prompt after the Gunter stream (C# SavePromptScreen), then LEVEL5: the
            // front screens show them, then the level.
            mdk::log3dsPrintf("flow: the %s follow%s",
                              screen == mdk::flow::Screen::Fall         ? "fall"
                              : screen == mdk::flow::Screen::Statistics ? "statistics"
                              : screen == mdk::flow::Screen::SavePrompt ? "save prompt"
                                                                        : "menu",
                              screen == mdk::flow::Screen::Statistics ? "" : "s");
        } else {
            mdk::log3dsPrintf("flow: %s follows, which this frontend cannot show; exiting",
                              mdk::flow::GameFlow::screenName(screen));
            exitReason = "stream ended";
            break;
        }
    }
    // THE MENU AND THE BRIEFING (package #20a), until the flow names a level.
    if (screen != mdk::flow::Screen::Level) {
        screen = mdk::runFrontScreens3ds(&front);
        if (screen != mdk::flow::Screen::Level) {
            exitReason = front.exitReason;
            break;
        }
        mdk::heapProbe("menus-done");
    }
    // Set when the level is left for the main menu (the START menu's MAIN MENU,
    // or a level that ends into the menu).
    bool toMenu = false;
    const MdkScope levelScope = mdk_render_mark(renderer);
    // C# Play: the loading screen, then the level (package #20c). The C# draws it
    // once, the bar empty; the 3DS load takes seconds, so the bar moves at the
    // load's steps below and through the HD set's hold, then the screen is freed
    // before play. The fractions are the steps' share of a measured Old 3DS load
    // (docs/WORK_LEDGER.md, the loading bar).
    std::unique_ptr<mdk::LoadingScreen> loading(new mdk::LoadingScreen());
    mdk::showLoadingScreen3ds(renderer, flow.state().level, frame, loading.get());
    const double loadStart = mdk::nowSeconds();
    auto loadingBar = [&](float progress) {
        mdk::drawLoadingScreen3ds(renderer, *loading, progress, frame);
        mdk::log3dsPrintf("loading: bar %d%% at %.0f ms", (int)(progress * 100.0f + 0.5f),
                          (mdk::nowSeconds() - loadStart) * 1000.0);
    };
    mdk::log3dsPrintf("level loop: level %d", (int)flow.state().level);
    bool nextLevel = false;
    {
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
    // The compressed packs, when they are there: the HD pack on the SD card
    // (loadHdPack) and the orig pack in the RomFS (loadOrigPack). The lookup is
    // wired whatever the toggle says: the backend decides per draw
    // (enableHdTextures) whether a pack copy is used, so the toggle stays live.
    // With neither pack staged nothing is wired and the level behaves exactly as
    // before the packs existed.
    mdk::ModImages hdPack;
    mdk::ModImages origPack;
    mdk::OrigByName origByName;
    mdk::HdPackSource hdSource;
    // The two packs' side tables for the name index (loadOrigByName): the HD
    // files by content key, the orig files' original sizes. Level-load only.
    std::map<std::string, std::string> hdByKey;
    std::map<std::string, uint32_t> origDims;
    // Every HD file's .t3x size and format (the level's set is sized from it).
    std::map<std::string, uint32_t> hdShapes;
    // THE LEVEL'S RESIDENT HD SET (spec 3.2): the cap its levels are read at, 0
    // when no set was chosen (pinHdSet3ds, below).
    int32_t hdSetCap = 0;
    if (mdk::loadHdPack(flow.state().level, &hdPack, &hdByKey, &hdShapes)) {
        hdSource.pack = &hdPack;
    }
    if (mdk::loadOrigPack(flow.state().level, &origPack, &origDims)) {
        hdSource.orig = &origPack;
    }
    if (hdSource.pack || hdSource.orig) {
        surfaces.setColourReplacementLookup(mdk::hdPackLookup, &hdSource);
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
    MdkMeshId skyMesh = -1;
    mdk::SkyRange skyRanges[8];
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
    // The rig per arena, computed once (cachedLighting). enhancedLighting scans
    // the arena's faces (mean up, mean facing) -- ~111 ms per arena change on
    // the Old 3DS -- and its answer depends on nothing but (dti, arena), both
    // fixed for the level's life. Two arenas of level 7 log byte-identical
    // haze; walking a door twice must not pay the scan twice. Residency fills
    // it for every arena at load.
    std::vector<std::pair<const mdk::formats::Arena*, mdk::Lighting>> lightCache;
    if (data.found()) {
        mdk::log3dsPrintf("data: %s", data.dir().c_str());
        if (mdk::LevelData::load(&level, &data, flow.state().level)) {
            mdk::heapProbe("level-data");
            loadingBar(0.15f);
            // The name-keyed orig index needs the loaded level (arena name ->
            // palette), so it is built here, not with the pack itself.
            if (hdSource.orig &&
                mdk::loadOrigByName(flow.state().level, level, hdByKey, origDims, &origByName)) {
                hdSource.byname = &origByName;
                mdk::log3dsPrintf("orig: byname index %d textures, %d with an HD copy",
                                  (int)origByName.paths.size(), (int)origByName.hdPaths.size());
            }
            // NO NAME INDEX, NO HD (spec 3.1): without byname.txt an HD file could
            // only be found by hashing every texture in play, which the HD rule
            // forbids. Refused for the level, loudly; the textures take the orig
            // pack (or their expansion) as with no HD pack at all.
            if (hdSource.pack && !hdSource.byname) {
                mdk::log3dsPrintf("hd: REFUSED for level %d: no name index (%s/byname.txt) "
                                  "-- the HD pack is not used without it",
                                  (int)flow.state().level, mdk::kOrigPackRoot);
                hdSource.pack = nullptr;
            }
            std::map<std::string, std::string>().swap(hdByKey);
            std::map<std::string, uint32_t>().swap(origDims);
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
                mdk::heapProbe("cmi");
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
                // BSPShow at 0x41ba68): its view and its textures are made now,
                // under the loading screen. The rest of the level arrives the
                // original's way -- the pair and its corridors kept, the second
                // arena's mesh built hundreds of frames before its door and
                // its textures pumped from the resident MTO (arena_views.h) --
                // with no SD read anywhere in play (game/level_data.cpp).
                const int32_t viewsBuilt = arenaViews.sync(startArenaName, std::string());
                // The start arena's textures are NOT made here: linear must
                // stay free for Kurt's animation pack (which streams in as
                // animations are used), the fonts, and the audio sink -- making
                // 171 of them at load left 779 KB and cost the menu fonts and
                // the audio worker (measured 2026-11-22). The pump makes them
                // over the first frames, which are the load-in frames the
                // original also spent inside BSPShow (0x41ba68).
                // The light rigs too: the scan is ~113 ms an arena and no door
                // frame must pay it.
                for (const mdk::formats::Arena* a : level.arenas()) {
                    mdk::cachedLighting(lightCache, level, a);
                }
                mdk::log3dsPrintf("start: %d views, %d light rigs precomputed",
                                  (int)viewsBuilt, (int)lightCache.size());
                if (const mdk::LevelView* startView = arenaViews.find(startArenaName)) {
                    mdk::log3dsPrintf("view: %d batches, %d vertices, %d triangles",
                                      (int)startView->batchCount(), (int)startView->vertexCount(),
                                      (int)arena->triangleCount());
                    skyRangeCount = mdk::buildSky3ds(renderer, level.dti(), flow.state().level,
                                                     enhancedToggles.enableHdTextures,
                                                     &skyMesh, skyRanges);
                } else {
                    mdk::log3dsPrintf("view: FAILED");
                    arena = nullptr;
                }
                mdk::heapProbe("arena-view");
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
                    // NO HD FOR KURT on the 3DS: his frames are kurt.pak's ETC1A4
                    // copies of the originals (loadKurtFrames3ds, after his level
                    // animations below), as large as the HD pack's 128 px frames
                    // (owner 2026-11-19); the HD pack's K_* files go unused.
                    mdk::log3dsPrintf("sprites: Kurt's animations loaded");
                } else {
                    mdk::log3dsPrintf("sprites: FAILED to decode Kurt's animations");
                }
                // Sniper mode's screen in pieces, every second texel (the 3DS's
                // memory budget, game/sniper_overlay.h): its two full-screen images
                // would otherwise cost 2 MiB of linear memory each.
                hud.sniper().budget.tiles = true;
                hud.sniper().budget.shrink = 2;
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
                menuReady = mdk::loadMenuFonts3ds(renderer, data, &menuBig, &menuSmall, &hud);
            }
            mdk::heapProbe("assets");
            loadingBar(0.35f);
        } else {
            mdk::log3dsPrintf("level %d: FAILED to load", (int)flow.state().level);
        }
    } else {
        mdk::log3dsPrintf("data: NOT FOUND");
        mdk::logDataNotFound3ds();
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
    // longer resident -- the bank opens it by its directory -- and the decoded
    // store IS bounded (the budget set below). Since 2026-11-25 the bank reads no
    // music at all: the tracks are DSP-ADPCM files the audio worker streams
    // (audio_dsp_3ds.h, mdk_3ds/music). What stays resident is
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
    mdk::heapProbe("pre-audio");
    mdk::log3dsPrintf("audio: loading SNI archives (level %d)", (int)flow.state().level);
    if (data.found() &&
        mdk::audio::SoundBank::forLevel(&bank, data, flow.state().level)) {
        bankReady = true;
        // THE DECODED STORE IS BOUNDED (SoundBank::setDecodedBudgetBytes: least
        // recently played first, never a sound a voice is playing). It holds
        // EFFECTS only: the level's music is streamed by the audio worker from its
        // own DSP-ADPCM files (audio_dsp_3ds.h), registered here, and the bank
        // never reads or decodes a track. 4 MiB is every effect of the largest
        // level decoded at once (level 4: about 2.5 MB) with room to spare, so no
        // effect is ever the victim.
        bank.setDecodedBudgetBytes(kDecodedSoundBudgetBytes);
        mdk::audioDspMusicRegister(&bank, flow.state().level);
        mdk::log3dsPrintf("audio: bank ready, %d archives (%d bytes resident; the music "
                          "archive is read by entry), decoded on demand, budget %d bytes",
                          (int)bank.archiveCount(), (int)bank.archiveBytes(),
                          (int)bank.decodedBudgetBytes());
    } else {
        mdk::log3dsPrintf("audio: the level's SNI archives did not load -- no sounds");
    }
    mdk::heapProbe("post-audio");
    loadingBar(0.45f);
    // KURT'S LEVEL ANIMATIONS: the level's SNI archives carry the wind tunnels' slide
    // (K_SLIP, K_SLIDE, K_FSLIDE, K_BSLIDE) and the snowboard's frames (C# Viewer:
    // KurtSprite.Add over SoundBank.Animation; the Linux frontend's same loop). Without
    // them a sliding Kurt has no frame to draw, and -- because Player's frame counts come
    // from this sprite -- K_SLIP would last one step instead of its animation (package 8,
    // Kurt.Slide.cs). Cold, at level load, before the Player takes the frame counts.
    if (kurtSpriteReady && bankReady) {
        int added = 0;
        int frames = 0;
        for (int i = 0; i < mdk::KurtSprite::levelAnimationCount(); i++) {
            const char* name = mdk::KurtSprite::levelAnimationName(i);
            mdk::formats::SpriteAnimation animation;
            if (bank.animation(name, &animation) && kurtSprite.add(renderer, name, animation)) {
                added++;
                frames += (int)animation.frameCount();
            }
        }
        mdk::log3dsPrintf("sprites: %d level animations (%d frames) from the SNI archives",
                          added, frames);
        mdk::heapProbe("post-level-animations");
    }
    // Kurt's compressed frames and the start set (packs_3ds.cpp): AFTER his level
    // animations, so the slides' and the board's frames find their kurt.pak copies
    // too (textures/LEVELn/ or the shared folder, kurt_frames_export.cpp) instead of
    // being made 16-bit.
    if (kurtSpriteReady) {
        mdk::loadKurtFrames3ds(&kurtSprite, enhancedToggles, flow.state().level);
        loadingBar(0.55f);
    }
    mdk::audio::SoundMixer mixer(bankReady ? &bank : nullptr);
    // THE OUTPUT (SPEC.md 8). The level mixes on the DSP (audio_dsp_3ds.h): every
    // audible voice on its own ndsp channel, the device's sample plane OFF so the
    // ARM11 sums nothing and only advances the voices' lifecycle. The device keeps
    // the frame's own `owed` accounting (no queue query). When the DSP path does
    // not open the level runs muted, its voices' lifecycle still advancing.
    const bool audioReady = mdk::audioDspOpen();
    mixer.device().setSampleMix(false);

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
    mdk::heapProbe("arena-space");

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
    // Sniper mode's view besides the world through the scope: the overlay's state
    // and the clip's inset (game/sniper_view.h; no round-camera insets here).
    mdk::SniperView sniperView;
    bool sniperClipFailureLogged = false;
    mdk::EffectsView effectsView(objectResolver, &level, mdk::Shading::Sprite);
    mdk::EffectsDraw effectsDraw;
    mdk::EffectsFrame effectsFrame;
    mdk::ObjectStream objectStream;
    // The tiny-enemy cull's per-object state (kTinyKnown...) and hysteresis
    // byte (ObjectView::addObject's tinyHidden), by deque slot.
    std::vector<uint8_t> tinyEnemyState;
    std::vector<uint8_t> tinyEnemyHidden;
    // Whether the scope in 2D has the parallax barrier off (EnhancedToggles::scopeMono).
    bool scopeMonoOn = false;
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
    // The scripts' cutscene camera (package 13, game/cutscene_camera.h): the C# Viewer's
    // _cutscene (Viewer.cs:142), the view instead of `follow` while a cutscene holds it.
    mdk::CutsceneCamera cutsceneCamera;
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
    // The loading (second) arena whose track was last primed (below).
    std::string musicPrefetchedFor;
    // The bubble arena whose track was last primed.
    std::string musicBubbleFor;
    // The second arenas whose first effects-warm step was already
    // deferred once (per name; see the frame's warm).
    std::vector<std::string> warmDeferred;
    // The object warm's place per arena (warmObjectStep's cursor), and the
    // surface cache's palette-release count when the cursors were last checked
    // against it (see the frame's object warm).
    std::map<std::string, size_t> objectWarmAt;
    int32_t objectWarmReleases = 0;
    mdk::WarmAsk warmAsk;
    warmAsk.surfaces = &surfaces;
    warmAsk.source = &hdSource;
    // The door bubble's hints for this frame (rebuilt every frame).
    std::vector<std::string> bubbleHints;
    // Last frame's camera position, for the bubble's speed scaling.
    mdk::Vec3 bubblePrevEye;
    bool bubbleHavePrevEye = false;
    double bubblePrevTime = 0.0;
    float bubbleSpeed = 0.0f;
    // Hinted arenas still inside the wider hysteresis radius.
    std::vector<std::string> bubbleSticky;
    // THE BANK'S BLOCKING READS, ALL OF THEM SAID (spec Phase 3: "log every
    // stall"). A streamed sound that is asked for while it is not in memory is
    // read whole, wherever the asker is -- a script's play of a LEVELnO.SNI
    // entry in the middle of its tick, an effect the budget evicted. (The music
    // never: it is streamed by the audio worker.) The frame says them.
    int32_t audioStallsSaid = 0;
    int32_t audioDemandSaid = 0;
    int64_t audioDemandBytesSaid = 0;
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
    // Since package #20a the menus' Gore option (C# Viewer.cs:234, `Option =
    // ui.Settings.Gore ? 1 : 0`), read as each runtime is built; on by default. The
    // gore cheat (package #21) flips the setting mid-level, so it is read there.
    // THE ONE MIXER, and which one it is depends on the runtime: the C# gives the
    // runtime and Kurt the same SoundMixer, and ScriptRuntime OWNS its mixer, so
    // when there is a runtime the frontend mixes that one and the sink is attached
    // to it. With no runtime the standalone mixer built above stays in use and what
    // can play is Kurt's own set. Re-pointed in spawnPlayer, never left dangling:
    // it is reset to the standalone mixer before every rebuild, and the runtime's
    // address is stable because the mixer is a member of the object pointed at.
    mdk::audio::SoundMixer* mix = &mixer;
    // The volumes and the music filter (C# Settings.Apply onto its AudioDevice).
    mdk::g_gameSettings.applyAudio(&mixer.device());
    // THE ONE HEALTH, taken once. The C#'s fresh Viewer seeds Kurt from the fall's
    // carry and CONSUMES it (Viewer.cs:246 -> CarryFromFall, :347-361, which does
    // `_kurt.SetHealth(carry.Health)` and then `state.Carry = null`), so the carry
    // belongs to the level that takes it and a second fresh level in the same
    // session starts whole. `play()` has already consumed it into the flow's level
    // result by the time this frontend has a Kurt to seed, which is why the value
    // is read from `levelHealth()` and not from `state().carry` (review-8 F3).
    const int32_t entryHealth = flow.levelHealth();
    // ...and the pickups the fall carried (package #17b, Viewer.CarryFromFall: the
    // pickups taken again without their messages, then the health).
    const mdk::flow::FallCarry entryCarry = flow.levelCarry();
    bool firstSpawn = true;
    auto spawnPlayer = [&]() {
        const float* start = level.dti().startPosition();
        player = mdk::Player();
        // The difficulty decides the town's timer: before the scripts (Viewer.cs:231).
        player.combat().setDifficulty((mdk::Kurt::Difficulty)mdk::g_gameSettings.difficulty);
        player.feet = mdk::Vec3{start[0], start[1], start[2] + 3.0f};
        player.yaw = level.dti().startAngle();
        // The session's cheats hold in every level and every respawn (Viewer.cs:286-288).
        player.setMortality(cheatSwitches.god ? mdk::Kurt::God : mdk::Kurt::Mortal);
        player.setClipping(cheatSwitches.noclip ? mdk::Player::ClipOff : mdk::Player::ClipOn);
        if (cheatSwitches.god || cheatSwitches.noclip) {
            mdk::log3dsPrintf("cheats: god %s, noclip %s", cheatSwitches.god ? "on" : "off",
                              cheatSwitches.noclip ? "on" : "off");
        }
        // Pickups show their names (Viewer.cs:249). A respawn is a fresh C# Viewer,
        // so its HUD's messages start empty.
        hud.messages().reset();
        {
            mdk::Kurt::PickupHook pickupHook;
            pickupHook.context = &hud.messages();
            pickupHook.pickedUp = mdk::Messages::pickedUp;
            player.combat().setPickupHook(pickupHook);
        }
        if (firstSpawn) {
            mdk::fall::carryIntoLevel(entryCarry, &player.combat());
            if (!entryCarry.pickups.empty()) {
                const mdk::Kurt& kurt = player.combat();
                mdk::log3dsPrintf("level: the fall's carry: health %d, %d pickups; %d slots, "
                                  "ammo %d:%d:%d:%d:%d, sgun %d",
                                  (int)entryHealth, (int)entryCarry.pickups.size(),
                                  (int)kurt.slotCount(), (int)kurt.ammo(1), (int)kurt.ammo(2),
                                  (int)kurt.ammo(3), (int)kurt.ammo(4), (int)kurt.ammo(5),
                                  (int)kurt.superChainGun());
            }
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
        musicBubbleFor.clear();
        warmDeferred.clear();
        objectWarmAt.clear();
        // A fresh camera with the fresh level (its clip view's context is the runtime
        // going now). It is reset HERE, before the runtime below binds its clip view, its
        // shake and its seed to it: reset after them, as it was until package #15, it
        // dropped the clip view package #3 bound.
        follow = mdk::FollowCamera();
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
        tinyEnemyState.clear();
        tinyEnemyHidden.clear();
        if (scriptAssetsReady) {
            // The seed. The oracle seeds its RNG with `new Random()` when no seed is
            // passed (ScriptRuntime.cs:323), which is the clock: an oracle run is
            // NOT reproducible here either, so a fixed seed would be the divergence,
            // not the fidelity. The console's clock is the ARM11 cycle counter, and
            // the value is logged at spawn so a run can be replayed by hand.
            const int32_t seed = (int32_t)(uint32_t)svcGetSystemTick();
            runtime.reset(new mdk::scripts::ScriptRuntime(&level, &cmi, seed, &bank, &player,
                                                          &sprites));
            // The setting as it is now: the gore cheat flips it mid-level (not saved), and
            // a respawn is a fresh C# Viewer reading `ui.Settings.Gore` again.
            runtime->option = mdk::g_gameSettings.gore ? 1 : 0;
            // The one air strike of the last two levels survives a death (Viewer.cs:236).
            runtime->airStrike().usedUp = flow.state().strikeUsed;
            // hud_message reaches the HUD (Viewer.cs:245, `_scripts.Messages`).
            runtime->hudMessages = &hud.messages();
            // Kurt's sounds go through the runtime's mixer, because in the C# they
            // are the same mixer.
            // The camera's line of sight against the objects (Viewer.cs:237: ClipView =
            // _scripts.ClipView, package #3).
            follow.clipView.context = runtime.get();
            follow.clipView.clip = &mdk::scripts::ScriptRuntime::clipViewTrampoline;
            // The scripts shake the camera (Viewer.cs:238, `ShakeRaised +=
            // _follow.RaiseShake`; package #15), whose offsets take the scripts' seed:
            // the C#'s camera Random is time-seeded.
            runtime->setShakeRaisedHook(&mdk::FollowCamera::raiseShakeHook, &follow);
            follow.seedShake(seed);
            player.setMixer(&runtime->mixer());
            mix = &runtime->mixer();
            mdk::g_gameSettings.applyAudio(&mix->device());
            // The new mixer owns a new SoundDevice: its sample plane goes off too, or
            // it would mix samples nobody hears (the DSP plays the voices).
            mix->device().setSampleMix(false);
            mdk::log3dsPrintf("scripts: runtime ready (seed %d, option %d)", (int)seed,
                              (int)runtime->option);
            mdk::heapProbe("runtime");
        }
        if (kurtSpriteReady) {
            player.setFrameCounts(kurtSprite.frameCounts());
        }
        // (The follow camera was reset above, before the runtime bound itself to it.)
        cutsceneCamera = mdk::CutsceneCamera();
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
            music->enter(startArenaName);
            mdk::log3dsPrintf("music: %s for %s", music->playing().c_str(),
                              startArenaName.c_str());
            // The frame's ledger of the bank's blocking reads starts here.
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
        // C# Viewer's constructor: RestoreSnapshot (a full save being loaded, F9's),
        // then ShowNotice ("Game loaded"). The save's arenas go into Kurt's space too
        // (the C#'s ONE ArenaSpace; ScriptRuntime::enteredArenas), and his arena is
        // pinned again where he now stands.
        bool restored = false;
        std::string restoreLog;
        if (!mdk::flow::restoreSnapshot(&flow.state(), runtime.get(), &restored, &restoreLog)) {
            mdk::log3dsPrintf("save: %s; the level starts plain", restoreLog.c_str());
            spawnPlayer();
        } else if (restored) {
            mdk::log3dsPrintf("save: %s", restoreLog.c_str());
            const std::vector<std::string> entered = runtime->enteredArenas();
            for (size_t i = 0; i < entered.size(); i++) {
                const mdk::formats::Arena* into = level.arenaNamed(entered[i]);
                if (into != nullptr) {
                    space.add(into);
                }
            }
            const std::string* here =
                space.arenaAt(mdk::Vec{player.feet.x, player.feet.y, player.feet.z});
            if (here) {
                space.setSolid({*here});
                startArenaName = *here;
            }
            mdk::log3dsPrintf("save: Kurt at (%.1f,%.1f,%.1f) in %s, health %d",
                              player.feet.x, player.feet.y, player.feet.z,
                              runtime->currentArena().c_str(), player.health());
        }
        const std::string notice = mdk::flow::takeNotice(&flow.state());
        if (!notice.empty()) {
            hud.messages().pushText((const uint8_t*)notice.data(), (int32_t)notice.size(),
                                    mdk::Messages::kFlagZoom | mdk::Messages::kFlagFront,
                                    mdk::flow::kNoticeSeconds);
            mdk::log3dsPrintf("%s", notice.c_str());
        }
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
    // THE FRAME'S VIEW (C# Viewer.SceneView): the follow camera's, or in sniper mode
    // the scope's from Kurt's eye (FollowCamera.SniperView). `camera` stays the
    // follow camera's state, which follow.update places every step; this is what
    // the frame is drawn and heard through.
    mdk::Camera sceneCamera;
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
    double last = mdk::nowSeconds();
    const float step = mdk::flow::GameFlow::stepFor(false, false, 0.0f);
    const float maxPending = step * 10.0f;  // the C#'s ten-step ceiling
    float pending = 0.0f;
    // The HUD's blink runs on the C#'s 30 Hz clock and not on the frame's, so it
    // needs its own accumulator beside `pending` (Viewer.cs:535-538, and the same
    // pair the Linux frontend keeps).
    const float kHudStep = 1.0f / 30.0f;
    float hudPending = 0.0f;
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
    int32_t lastSyncFinishes = 0;
    std::vector<std::string> startDrawn;
    const std::vector<std::string>* frameDrawn = nullptr;   // this frame's drawn list
    int32_t objectPeakCorners = 0;
    int32_t objectPeakDraws = 0;
    u32 prevHeld = 0;  // last frame's held mask; the menu reads edges
    // THE END OF A LEVEL (package 1). The C#'s Viewer subscribes to the scripts'
    // LevelEnded (Viewer.cs:240) and leaves EndDelay (0.5 s, :112) later through
    // Game.cs's Event.LevelEnded (:442, AfterLevel). The runtime's levelEnded() is
    // that event, latched; `endingDelay` is the C#'s _ending countdown, started on the
    // frame it is first seen and spent by the same frame's elapsed (Viewer.cs:450-457),
    // and `levelEndedExit` is its `_next` -- acted on after the frame is drawn.
    const float kEndDelay = 0.5f;
    float endingDelay = 0.0f;
    bool endingStarted = false;
    bool levelEndedExit = false;
    // C# Event.GameFinished (Viewer.cs:241): special_event 81 latched it (package 13).
    bool gameFinishedExit = false;
    bool endSeen = false;
    bool tearRefusedLogged = false;
    // A death or a quick load leaves the level for the flow's next screen, as the
    // level's end does (C# Viewer._next = KurtDied / Play; package #19).
    bool leaveLevel = false;
    mdk::flow::Event leaveEvent = mdk::flow::Event::None;
    // C# Viewer.Notify: the port's own HUD message, also logged.
    auto notify = [&hud](const char* text) {
        hud.messages().pushText((const uint8_t*)text, (int32_t)strlen(text),
                                mdk::Messages::kFlagZoom | mdk::Messages::kFlagFront,
                                mdk::flow::kNoticeSeconds);
        mdk::log3dsPrintf("%s", text);
    };
    // F2 then Enter (C# Viewer.OpenSnapshot, then the prompt's Save with the name it
    // offers): the 3DS has no keyboard, so the pause menu's X saves at once under the
    // level's number. SYNCHRONOUS, and said: a save is 13-100 KB of text (the goldens'
    // files: 13 KB on LEVEL4's start, 98 KB with 46 objects on LEVEL3), one fwrite
    // and a rename while the game is paused in the menu, so no frame waits on it; the
    // log line carries the measured milliseconds for the hardware run to confirm.
    auto quickSave = [&]() {
        if (!runtime || !runtime->canSnapshot() || player.sniping()) {
            notify("Can't save now");
            return;
        }
        const double t0 = mdk::nowSeconds();
        const std::string json = runtime->capture();
        mdk::log3dsPrintf("snapshot hash %s, objects %d",
                          mdk::scripts::ScriptRuntime::snapshotHash(json).c_str(),
                          (int)runtime->objectCount());
        const int32_t number = flow.state().level;
        mdk::flow::SavePrompt prompt(saves,
                                     mdk::flow::snapshotSave(&flow.state(), number,
                                                             player.health(), json),
                                     mdk::flow::snapshotName(number));
        mdk::flow::SavePrompt::Input enter;
        enter.accept = true;
        prompt.update(enter, 0.0f);
        const double t1 = mdk::nowSeconds();
        if (prompt.saved()) {
            flow.state().quickSlot = prompt.name();
            mdk::log3dsPrintf("Saved game %s: level %d (%d bytes, %.1f ms)",
                              prompt.name().c_str(), (int)number, (int)json.size(),
                              (t1 - t0) * 1000.0);
            notify("Game saved");
        } else {
            mdk::log3dsPrintf("save: not written (%s)", prompt.error().c_str());
            notify("Can't save now");
        }
    };
    // F9 (C# Viewer.QuickLoad): the session's save, else the newest; the level reloads.
    auto quickLoad = [&]() {
        mdk::flow::Event next = mdk::flow::Event::None;
        std::string log;
        if (!mdk::flow::quickLoad(saves, &flow.state(), &next, &log)) {
            notify("Nothing to load");
            return;
        }
        mdk::log3dsPrintf("%s", log.c_str());
        leaveLevel = true;
        leaveEvent = next;
    };

    // THE LEVEL'S HD SET, RESIDENT BEFORE THE LOADING SCREEN GOES (owner
    // 2026-11-24: "I dont want surface pop in"; docs/HD_TEXTURES_3DS_SPEC.md 3.2).
    // Streaming per surface cannot promise a copy before its surface is seen: the
    // Old 3DS reads ~13 HD files a second in play. So the level's whole covered
    // set -- every HD file the name index pairs with this level's rows -- is
    // chosen here by arithmetic against the free linear heap, read on the loading
    // screen, and kept for the level: no HD read in play, nothing evicted.
    //   tier 512: the files as built, if they fit;
    //   tier 256: else the same files with their levels over 256 texels dropped
    //             at the read (a quarter of the bytes for each level dropped);
    //   streamed: else per surface, as before the set (pop-in possible), said.
    // kHdPlayReserve is what play still takes from the linear heap after this
    // point (Kurt's animation stream, the uncovered textures, effects, meshes):
    // measured on the Old 3DS level-7 walk, 6.27 MB between this line and the
    // walk's lowest free (12.50 MB free here, 4.18 MB of set, 2.05 MB left at
    // the low point), so 7 MiB.
    // The last level's copies are given back first, so the heap read below is
    // this level's (mdk_render_pin_hd_set frees what nothing links any more).
    loadingBar(0.6f);
    mdk_render_pin_hd_set(renderer, nullptr, nullptr, 0);
    if (hdSource.pack && hdSource.byname) {
        hdSetCap = mdk::pinHdSet3ds(renderer, flow.state().level, origByName, hdShapes);
    }
    std::map<std::string, uint32_t>().swap(hdShapes);
    // The first view's surfaces asked for now (its batches, hidden ones too, and
    // the textures of every model the arena or its scripts can show), so their
    // ids exist and their packed originals -- the uncovered surfaces' -- are read
    // in the same hold.
    if (enhancedToggles.enableHdTextures && hdSource.pack && hdSource.byname &&
        !startArenaName.empty()) {
        arenaViews.pumpSurfaces(1 << 20);
        if (const mdk::formats::Arena* owner = level.arenaNamed(startArenaName)) {
            size_t& cursor = objectWarmAt[startArenaName];
            while (!mdk::warmObjectStep(*owner, cmiReady ? &cmi : nullptr,
                                        objectLook(startArenaName), objectResolver,
                                        mdk::requestPackedSurface, &warmAsk, &cursor)) {
            }
        }
        mdk::holdHdSet3ds(renderer, "loading screen", loading.get(), 0.6f, frame);
    }
    loadingBar(1.0f);
    loading->release(renderer);
    loading.reset();
    mdk::heapProbe("loop");
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
    const double hintUntil = mdk::nowSeconds() + 5.0;
    while (aptMainLoop()) {
        // The frame's clock FIRST: the look integrates over the time since the
        // previous frame, so the input poll needs the same delta the simulation is
        // about to get. Clamp it here -- the platform's floor under the game's own
        // policy -- because a 40-second sleep is not 40 seconds of walking.
        const double now = mdk::nowSeconds();
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
        const u32 held = mdk::input3dsPoll(&input, elapsed, controls);
        // In sniper mode X and B zoom instead of tilting (input_3ds.h).
        if (playerReady && player.sniping()) {
            mdk::input3dsScopeButtons(held, controls, &input);
        }

        const u32 pressed = held & ~prevHeld;
        prevHeld = held;

        // THE PAUSE MENU (owner, 2026-11-19; SPEC.md 7.9's Video/Graphics
        // settings are its rows). START opens it -- it used to quit the game
        // on one press -- and SELECT opens the same menu. It opens on RESUME;
        // while it is open the simulation below is paused and the frame's
        // edges drive the menu instead: the D-pad moves (and wraps), A flips a
        // toggle or takes RESUME / QUIT, left/right step a slider or cycle a
        // choice, and START, B or SELECT close it. A
        // change applies at once (mdk_render_set_enhanced_toggles) and is written
        // back to config.ini in the same frame, so a hardware A/B is one press.
        // QUIT is a row, confirmed with A: never a single press.
        bool menuQuit = false;
        bool menuMainMenu = false;
        // THE FULL-SCREEN STRIKE (package 14, game/strike_view.h): while it plays, START or
        // SELECT skips it (the C#'s Esc) instead of pausing (Viewer.cs:389-396).
        const bool striking = runtime && runtime->strikeScene() != nullptr &&
                              runtime->strikeScene()->active();
        if (menuReady) {
            if (!menu.open() && !striking && (pressed & (KEY_START | KEY_SELECT))) {
                menu.setOpen(true);
                cheatsPage.setOpen(false);
                mdk::log3dsPrintf("menu: paused (%s)",
                                  (pressed & KEY_START) ? "START" : "SELECT");
            } else if (menu.open() && pauseOptions.open()) {
                // THE OPTIONS PAGES (the C# PauseMenu's Options, package #20a):
                // the D-pad moves and steps, A chooses, B or START goes up a page
                // (the C#'s Esc), and Back on the options returns here.
                mdk::menu::Keys keys;
                keys.up = (pressed & KEY_DUP) != 0;
                keys.down = (pressed & KEY_DDOWN) != 0;
                keys.left = (pressed & KEY_DLEFT) != 0;
                keys.right = (pressed & KEY_DRIGHT) != 0;
                keys.accept = (pressed & KEY_A) != 0;
                keys.back = (pressed & (KEY_B | KEY_START)) != 0;
                keys.leftHeld = (held & KEY_DLEFT) != 0;
                keys.rightHeld = (held & KEY_DRIGHT) != 0;
                keys.any = pressed != 0;
                pauseOptions.update(keys, elapsed);
                if (!pauseOptions.open()) {
                    mdk::log3dsPrintf("menu: options closed");
                }
            } else if (menu.open() && cheatsPage.open()) {
                // THE CHEATS PAGE (package #21): the D-pad moves, A or left/right flips,
                // B, START or SELECT (or A on BACK) returns to the START menu.
                mdk::EnhancedMenu::Input edges;
                edges.up = (pressed & KEY_DUP) != 0;
                edges.down = (pressed & KEY_DDOWN) != 0;
                edges.left = (pressed & KEY_DLEFT) != 0;
                edges.right = (pressed & KEY_DRIGHT) != 0;
                edges.confirm = (pressed & KEY_A) != 0;
                edges.close = (pressed & (KEY_START | KEY_B | KEY_SELECT)) != 0;
                const mdk::CheatsMenu::Action action = cheatsPage.handle(edges, &cheatSwitches);
                if (action == mdk::CheatsMenu::Action::Changed) {
                    // TOOSCARYFORME (Viewer.cs:872-876): the settings' gore, not saved, and
                    // the running level's script Option. God and noclip: the console's
                    // SetGod/SetNoclip on this level's Kurt (LevelCommands.cs:37-39).
                    mdk::g_gameSettings.gore = cheatSwitches.gore;
                    if (runtime) {
                        runtime->option = mdk::g_gameSettings.gore ? 1 : 0;
                    }
                    player.setMortality(cheatSwitches.god ? mdk::Kurt::God : mdk::Kurt::Mortal);
                    player.setClipping(cheatSwitches.noclip ? mdk::Player::ClipOff
                                                            : mdk::Player::ClipOn);
                    mdk::log3dsPrintf("menu: cheats: gore %s, god %s, noclip %s",
                                      cheatSwitches.gore ? "on" : "off",
                                      cheatSwitches.god ? "on" : "off",
                                      cheatSwitches.noclip ? "on" : "off");
                } else if (action == mdk::CheatsMenu::Action::Closed) {
                    mdk::log3dsPrintf("menu: cheats closed");
                }
            } else if (menu.open() && (pressed & (KEY_X | KEY_Y))) {
                // THE SAVES (package #19): X saves the level (F2 + Enter), Y loads the
                // last save (F9) -- the C#'s quick keys, which the pad has no room for
                // in play. A load closes the menu: the level reloads.
                if (pressed & KEY_X) {
                    quickSave();
                } else {
                    quickLoad();
                    if (leaveLevel) {
                        menu.setOpen(false);
                        mdk::log3dsPrintf("menu: closed (load)");
                    }
                }
            } else if (menu.open()) {
                mdk::EnhancedMenu::Input edges;
                edges.up = (pressed & KEY_DUP) != 0;
                edges.down = (pressed & KEY_DDOWN) != 0;
                edges.left = (pressed & KEY_DLEFT) != 0;
                edges.right = (pressed & KEY_DRIGHT) != 0;
                edges.confirm = (pressed & KEY_A) != 0;
                edges.close = (pressed & (KEY_START | KEY_B | KEY_SELECT)) != 0;
                const bool hdMenuWas = enhancedToggles.enableHdTextures;
                const mdk::EnhancedMenu::Action action = menu.handle(edges, &enhancedToggles);
                if (action == mdk::EnhancedMenu::Action::Changed) {
                    const bool hdWas = hdMenuWas;
                    mdk_render_set_enhanced_toggles(renderer, &enhancedToggles);
                    // THE HD SET BACK ON: held for, like the loading screen
                    // (spec 3.2) -- the menu stays up while the set is read.
                    if (!hdWas && enhancedToggles.enableHdTextures && hdSetCap > 0) {
                        mdk::holdHdSet3ds(renderer, "toggled on", nullptr, 0.0f, frame);
                    }
                    // THE SKY FOLLOWS THE HD TOGGLE: its ETC1 slices with it on,
                    // the 16-bit expansion with it off, never both (buildSky3ds).
                    if (hdWas != enhancedToggles.enableHdTextures && skyMesh >= 0) {
                        mdk::releaseSky3ds(renderer, &skyMesh, skyRanges, &skyRangeCount);
                        skyRangeCount = mdk::buildSky3ds(renderer, level.dti(), flow.state().level,
                                                         enhancedToggles.enableHdTextures,
                                                         &skyMesh, skyRanges);
                    }
                    mdk::saveToggles3ds(enhancedToggles);
                } else if (action == mdk::EnhancedMenu::Action::Quit) {
                    menuQuit = true;
                    mdk::log3dsPrintf("menu: QUIT chosen");
                } else if (action == mdk::EnhancedMenu::Action::MainMenu) {
                    menuMainMenu = true;
                    mdk::log3dsPrintf("menu: MAIN MENU chosen");
                } else if (action == mdk::EnhancedMenu::Action::Cheats) {
                    cheatSwitches.gore = mdk::g_gameSettings.gore;
                    cheatsPage.setOpen(true);
                    mdk::log3dsPrintf("menu: cheats opened");
                } else if (action == mdk::EnhancedMenu::Action::Options) {
                    // The C# options pages, over the paused game, in the fonts the
                    // START menu draws with; their changes apply and save at once.
                    mdk::menu::MenuItems& items = pauseOptions.items();
                    items.setFonts(&menuBig.font(), &menuSmall.font());
                    items.setHost(mdk::menu::Host::Console);
                    items.setSettings(&mdk::g_gameSettings, mdk::optionsChanged3ds, &mix);
                    items.setPrint(mdk::printLine3ds, nullptr);
                    pauseOptions.openOptions();
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
        } else if (menuMainMenu) {
            next = mdk::flow::Event::Menu;
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
            if (screen == mdk::flow::Screen::Menu) {
                // C# Game.Handle(Event.Menu): the level is left for a new main menu.
                menu.setOpen(false);
                toMenu = true;
                break;
            }
            if (screen != mdk::flow::Screen::Level) {
                exitReason = "the flow asked for a screen this frontend cannot show";
                break;
            }
        }

        // The strike advances (or is skipped) and, while it plays, the game waits: no step
        // runs and the frame is the sky and its model alone (SniperView.DrawStrike).
        const bool strikeFrame =
            !menu.open() &&
            mdk::advanceStrikeScene(runtime.get(),
                                    striking && (pressed & (KEY_START | KEY_SELECT)) != 0,
                                    elapsed);
        if (playerReady && !menu.open() && !strikeFrame) {
            // The C# Viewer.Frame order: the mouse look is PER FRAME (Kurt's yaw
            // and the camera's look offset), then the simulation runs in fixed
            // steps, and EACH step updates Kurt and then the camera (UpdateKurt
            // then UpdateCamera) -- so the pitch smoothing and the clearance run
            // once per step, not once per frame.
            player.mouseLook(input);
            follow.mouseLook(player, input);

            pending += elapsed;
            if (pending > maxPending) pending = maxPending;
            while (pending >= step) {
                player.update(input, step);
                // The C# Viewer's step order is Kurt, then the scripts, then the
                // camera (Viewer.cs:505-535). ScriptRuntime keeps its own 1/30 s
                // clock, so a 1/60 s call completes a script tick every second step
                // while Kurt's physics stay at 60 Hz.
                if (runtime) {
                    runtime->update(step);
                }
                if (runtime) {
                    // The bullet holes the step stamped (special_130, package #16): the C#'s
                    // TextureStamped -> MaterialResolver.Refresh (Viewer.cs:263), each changed
                    // texture's GPU copies given its pixels again. The backend takes the
                    // pixels at once and expands the texture again at a frame boundary,
                    // one a frame (05_resources.inc, mdk_render_update_texture).
                    const std::vector<const mdk::formats::Texture*>& stamped =
                        runtime->stampedTextures();
                    for (size_t i = 0; i < stamped.size(); i++) {
                        surfaces.refresh(stamped[i]);
                    }
                    runtime->clearStampedTextures();
                }
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
                // The music follows Kurt's arena, which the scripts set (C#
                // Viewer.cs:521-525: Enter, then Update(Step), every step, after
                // the camera). A new track is a request to the audio worker,
                // which starts it from its primed read-ahead (below): nothing is
                // read or decoded here.
                if (music) {
                    const std::string was = music->playing();
                    music->enter(runtime->currentArena());
                    if (music->playing() != was) {
                        mdk::log3dsPrintf("music: %s for %s (was %s, fading %s)",
                                          music->playing().empty() ? "-" : music->playing().c_str(),
                                          runtime->currentArena().c_str(),
                                          was.empty() ? "-" : was.c_str(),
                                          music->fading().empty() ? "-" : music->fading().c_str());
                    }
                    music->update(step);
                }
                // The cutscene's shot of this step (Viewer.cs:528-531, after the music).
                if (runtime && mdk::CutsceneCamera::active(*runtime)) {
                    cutsceneCamera.update(*runtime);
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
                hudPending += step;
                if (hudPending >= kHudStep) {
                    hud.tick();
                    hudPending -= kHudStep;
                }
                pending -= step;
                steps++;
            }

            // Death. The C#'s Kurt.Died (Viewer.OnDied) counts it and leaves for
            // Game.Handle(KurtDied), which writes LASTGAME and opens the main menu
            // (package #20a's), whose first item "Continue" reloads the level from
            // its start (package #19's saves hooks). The beat is the oracle's --
            // player.died() is the step the skull crosses SkullEnd.
            if (player.died() && !leaveLevel) {
                mdk::log3dsPrintf("Kurt died at frame %lu", (unsigned long)frame);
                mdk::flow::countDeath(&flow.state(), flow.state().level,
                                      runtime ? runtime->airStrike().usedUp
                                              : flow.state().strikeUsed);
                leaveLevel = true;
                leaveEvent = mdk::flow::Event::KurtDied;
            }

            // The end of a level: special_event <= 50 started it (the log says where),
            // EndLevel finished it (levelEnded), and the frontend leaves EndDelay later.
            if (runtime && runtime->endLevel() && !endSeen) {
                endSeen = true;
                mdk::log3dsPrintf("level: special_event ended the level in %s at frame %lu; "
                                  "the arena breaks up and Kurt rises",
                                  runtime->endLevel()->arena().c_str(), (unsigned long)frame);
            }
            if (runtime && runtime->levelEnded() && !endingStarted) {
                endingStarted = true;
                endingDelay = kEndDelay;
                mdk::log3dsPrintf("level: ended (LevelEnded) at frame %lu; leaving in %.1f s",
                                  (unsigned long)frame, (double)kEndDelay);
            }
            if (endingStarted && !levelEndedExit) {
                endingDelay -= elapsed;
                levelEndedExit = endingDelay <= 0.0f;
            }
            // The end of the game (special_event 81): the C#'s GameFinished, at once
            // (Viewer.cs:241, no delay), acted on after the frame like the level's end.
            if (runtime && runtime->gameFinished() && !gameFinishedExit) {
                gameFinishedExit = true;
                mdk::log3dsPrintf("game: finished (special_event 81, GameFinished) at frame %lu",
                                  (unsigned long)frame);
            }
        }

        // A CUTSCENE HOLDS THE CAMERA (package 13): the C# Viewer draws from its
        // CutsceneCamera instead of the follow camera while CutsceneCamera.Active
        // (Viewer.SceneView/Eye, :728/:739), blended between the scripts' ticks
        // (Viewer.BlendFrame, :614-617). The follow camera keeps its own state and
        // rewrites `camera` on the next step, so overwriting it here for the frame
        // leaves nothing behind when the cutscene ends.
        if (runtime && mdk::CutsceneCamera::active(*runtime)) {
            cutsceneCamera.blend(runtime->tick_blend(pending));
            cutsceneCamera.build(&camera, kAspect);
        }
        // The strike's own camera while it holds the frame (SniperView.DrawStrike's view);
        // the follow camera rewrites `camera` on the first step after it.
        if (strikeFrame) {
            mdk::buildStrikeCamera(*runtime->strikeScene(), kAspect, &camera);
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
        // The scope's view in sniper mode (C# Viewer.SceneView), but never over the
        // strike's frame: DrawStrike presents its own view before SceneView is asked
        // (Viewer.cs:396-410), and Bones' strike is called from the scope.
        sceneCamera = camera;
        if (playerReady && player.sniping() && !strikeFrame) {
            mdk::buildSniperCamera(&sceneCamera, player, kAspect);
        }
        // C# Viewer.DrawScene (Viewer.cs:686-693): the listener stands at the frame's
        // view with the follow camera's right; in sniper mode the sounds are heard
        // through the scope (ScopeZoom, Kurt's line of sight and its up).
        mix->listenFromCamera(camera);
        mix->listenerPosition = sceneCamera.position;
        mix->scopeZoom = playerReady && player.sniping() ? player.scope().zoom : 0.0f;
        mix->listenerForward = player.sniperForward();
        mix->listenerUp = mdk::followCameraUpOf(mix->listenerForward);
        mix->update(elapsed);
        mdk::audioDspFrame(*mix);
        mix->device().update(elapsed);

        // THE ARENAS ON THE GPU FOLLOW THE SCRIPTS (arena_views.h): the pair and
        // its corridors are kept, the second arena's mesh is built long before
        // its door, and its textures are pumped from the resident MTO -- no SD
        // read and no unplanned build lands on the frame Kurt crosses a door.
        // Outside begin/end_frame: it can make and release GPU resources.
        if (arena) {
            const std::string& streamCurrent =
                (runtime && !runtime->currentArena().empty()) ? runtime->currentArena()
                                                              : startArenaName;
            const std::string streamSecond = runtime ? runtime->secondArena() : std::string();
            // WHAT THIS FRAME DRAWS: the scripts' drawn list (C# Viewer.cs:702,
            // LevelView.Draw(DrawnArenas)), or Kurt's arena until their first
            // tick has named one. After a scripted teleport the list and the
            // pair differ for a frame (review, 2026-11-18).
            const bool scriptsDrew = runtime && !runtime->drawnArenas().empty() &&
                                     !runtime->drawnArenas()[0].empty();
            if (!scriptsDrew) {
                startDrawn.assign(1, startArenaName);
            }
            frameDrawn = scriptsDrew ? &runtime->drawnArenas() : &startDrawn;
            // THE DOOR BUBBLE: the arenas through doorways within this
            // radius of the camera join the build list NOW, so their
            // views, effect textures and (below) music are ready long
            // before the scripts make them the second arena. At Kurt's
            // turbo speed (~0.66 units a frame) 120 units is ~180 frames
            // of lead. Residency: the pair plus at most three hinted
            // names kept, the nearest built ahead first (up to the
            // three kept can build over time as the nearest changes) --
            // never the level.
            // The radius scales with the camera's speed IN UNITS PER
            // SECOND (time-normalised and smoothed, so a single long
            // frame cannot inflate it -- the feedback the alternative
            // invites), keeping the LEAD TIME constant at ~2.5 s of
            // travel: sprinting closes the base radius in a third of
            // the time walking does, and the fixed radius ran out. The
            // clamp is a plain scalar bound, not a simulation branch.
            const float kBubbleBaseRadius = 120.0f;
            const float kBubbleLeadSeconds = 2.5f;
            const float kBubbleMaxRadius = 420.0f;
            const float kBubbleHysteresis = 40.0f;
            float bubbleRadius = kBubbleBaseRadius;
            if (runtime && bubbleHavePrevEye) {
                const double now = mdk::nowSeconds();
                const float dt = (float)(now - bubblePrevTime);
                if (dt > 0.001f) {
                    const float dx = runtime->eye.x - bubblePrevEye.x;
                    const float dy = runtime->eye.y - bubblePrevEye.y;
                    const float dz = runtime->eye.z - bubblePrevEye.z;
                    const float perSecond = sqrtf(dx * dx + dy * dy + dz * dz) / dt;
                    bubbleSpeed += (perSecond - bubbleSpeed) * 0.15f;
                    bubblePrevTime = now;
                }
                bubbleRadius = kBubbleBaseRadius + bubbleSpeed * kBubbleLeadSeconds;
                if (bubbleRadius > kBubbleMaxRadius) {
                    bubbleRadius = kBubbleMaxRadius;
                }
            }
            if (runtime) {
                if (!bubbleHavePrevEye) {
                    bubblePrevTime = mdk::nowSeconds();
                }
                bubblePrevEye = runtime->eye;
                bubbleHavePrevEye = true;
            }
            // Hysteresis: a hinted arena stays hinted until it is 40
            // units past the radius, so idling at the bubble's edge does
            // not release and rebuild its view repeatedly. At most two
            // hinted arenas (nearest first, per the query's order):
            // residency is the current room plus the approached one(s),
            // never the level.
            bubbleHints.clear();
            if (runtime) {
                std::vector<std::string> fresh;
                runtime->doorBubbleArenas(runtime->eye, bubbleRadius, &fresh);
                std::vector<std::string> wide;
                runtime->doorBubbleArenas(runtime->eye, bubbleRadius + kBubbleHysteresis, &wide);
                // Fresh names first, nearest first (the query's order):
                // sync builds only the first hinted name ahead, so that
                // is the build priority. Sticky names follow for KEEP
                // only (hysteresis: released at the wider radius, never
                // outranking the nearest door for the build slot).
                // Already-resident names (the pair, the drawn list)
                // never consume a slot. Residency bound: the pair, the
                // drawn corridors, and at most three hinted names kept
                // (one of them built ahead; the rest kept unbuilt until
                // the scripts make them the pair).
                for (size_t i = 0; i < fresh.size(); i++) {
                    bubbleHints.push_back(fresh[i]);
                }
                for (size_t i = 0; i < bubbleSticky.size(); i++) {
                    bool stillNear = false;
                    for (size_t k = 0; k < wide.size(); k++) {
                        if (wide[k] == bubbleSticky[i]) {
                            stillNear = true;
                            break;
                        }
                    }
                    bool seen = false;
                    for (size_t k = 0; k < bubbleHints.size(); k++) {
                        if (bubbleHints[k] == bubbleSticky[i]) {
                            seen = true;
                            break;
                        }
                    }
                    if (stillNear && !seen) {
                        bubbleHints.push_back(bubbleSticky[i]);
                    }
                }
                for (size_t i = bubbleHints.size(); i > 0; i--) {
                    const std::string& name = bubbleHints[i - 1];
                    bool resident = name == streamCurrent || name == streamSecond;
                    for (size_t k = 0; !resident && k < frameDrawn->size(); k++) {
                        resident = (*frameDrawn)[k] == name;
                    }
                    if (resident) {
                        bubbleHints.erase(bubbleHints.begin() + (int32_t)(i - 1));
                    }
                }
                if (bubbleHints.size() > 3) {
                    bubbleHints.resize(3);
                }
                bubbleSticky = bubbleHints;
            }
            const double stream1 = mdk::nowSeconds();
            const int32_t builtViews =
                arenaViews.sync(streamCurrent, streamSecond, *frameDrawn, bubbleHints);
            const double stream2 = mdk::nowSeconds();
            // Spread the texture makes over the frames before anyone sees them
            // (arena_views.h): from the resident MTO, so a make is decode and
            // upload with no SD read. Two a frame; the entry stall was 442 ms
            // for nine at once.
            arenaViews.pumpSurfaces(2);
            // The script effects' textures for the resident arenas, one
            // texture a frame per arena (EffectsView::warmStep): an
            // effect's first frame is its spawn frame, so without this its
            // texture is made in its first draw (measured: TRAIL, 64x704
            // stacked, 77 ms at the DANT_1 -> CDANT_1 crossing) -- and
            // resolving all six in the arena-change frame was most of that
            // frame's remaining stall, so the warm is spread like the
            // builds above it.
            effectsView.warmStep(streamCurrent, mdk::requestPackedSurface, mdk::startEffectSurface,
                                 &warmAsk);
            // The bubble's arenas warm too: they are approached, not yet
            // resident, and their lead is the whole point (no deferral --
            // the bubble IS the early start).
            for (size_t i = 0; i < bubbleHints.size(); i++) {
                if (bubbleHints[i] != streamCurrent && bubbleHints[i] != streamSecond) {
                    effectsView.warmStep(bubbleHints[i], mdk::requestPackedSurface,
                                         mdk::startEffectSurface, &warmAsk);
                }
            }
            if (!streamSecond.empty() && streamSecond != streamCurrent) {
                // Not the change frame either (see the music step below):
                // each name's first warm step is deferred exactly once
                // (recorded per name), then it progresses every frame --
                // alternating names each pay one frame, never all of them.
                bool deferred = false;
                for (size_t i = 0; i < warmDeferred.size(); i++) {
                    if (warmDeferred[i] == streamSecond) {
                        deferred = true;
                        break;
                    }
                }
                if (!deferred) {
                    warmDeferred.push_back(streamSecond);
                } else {
                    effectsView.warmStep(streamSecond, mdk::requestPackedSurface,
                                         mdk::startEffectSurface, &warmAsk);
                }
            }
            // THE OBJECTS' TEXTURES for the pair (warmObjectStep): one model a
            // frame per arena -- its own models, then every type the scripts
            // can spawn -- each Lit surface the orig pack holds asked for as a
            // pending id, read off the frame as a hint. Measured before it, on
            // the Old 3DS level-7 walk: every in-play draw-time pack read was an
            // object texture (doors, machinery, aliens) made the frame the
            // object first showed. A finished arena costs one map lookup.
            //
            // A RELEASED PALETTE TAKES ITS ASKS WITH IT (SurfaceCache::
            // releasePalette, an arena leaving the residency): an arena whose
            // warm had finished would come back to the pair asking for nothing.
            // So on any release, every arena NOT in the pair starts its warm
            // over when it is next in it (the pair's own palettes are kept, so
            // their cursors stand). Releases are rare; the check is one compare.
            if (surfaces.paletteReleases() != objectWarmReleases) {
                objectWarmReleases = surfaces.paletteReleases();
                for (std::map<std::string, size_t>::iterator it = objectWarmAt.begin();
                     it != objectWarmAt.end();) {
                    if (it->first != streamCurrent && it->first != streamSecond) {
                        objectWarmAt.erase(it++);
                    } else {
                        ++it;
                    }
                }
            }
            for (int k = 0; k < 2; k++) {
                const std::string& name = k == 0 ? streamCurrent : streamSecond;
                if (name.empty() || (k == 1 && name == streamCurrent)) {
                    continue;
                }
                const mdk::formats::Arena* owner = level.arenaNamed(name);
                if (owner != nullptr) {
                    size_t& cursor = objectWarmAt[name];
                    mdk::warmObjectStep(*owner, cmiReady ? &cmi : nullptr, objectLook(name),
                                        objectResolver, mdk::requestPackedSurface, &warmAsk,
                                        &cursor);
                }
            }
            if (arenaViews.stats().failed != lastViewsFailed) {
                mdk::log3dsPrintf("views: build FAILED (%d so far); it is tried again",
                                  (int)arenaViews.stats().failed);
                lastViewsFailed = arenaViews.stats().failed;
            }
            if (arenaViews.stats().syncFinishes != lastSyncFinishes) {
                // The bubble's report card: how often a drawn view still
                // needed the synchronous finish (the guarantee firing).
                mdk::log3dsPrintf("views: %d drawn view(s) finished synchronously at sync "
                                  "(%d sliced steps so far)",
                                  (int)arenaViews.stats().syncFinishes,
                                  (int)arenaViews.stats().buildSteps);
                lastSyncFinishes = arenaViews.stats().syncFinishes;
            }
            if (builtViews > 0 || arenaViews.stats().released != lastViewsReleased) {
                mdk::log3dsPrintf("views: %d built (last %s, %.1f ms), %d released, %d kept",
                                  (int)builtViews, arenaViews.lastBuilt().c_str(),
                                  (stream2 - stream1) * 1000.0,
                                  (int)(arenaViews.stats().released - lastViewsReleased),
                                  (int)arenaViews.count());
                lastViewsReleased = arenaViews.stats().released;
            }
            // The groups the steps retextured (group_set_texture, opcode 140; package
            // #15): the C#'s TriangleGroups.Changed -> LevelView.Rebuild, each arena's
            // view built again from its groups' materials, here between frames as sync
            // builds them (the old mesh goes at the backend's completion boundary).
            if (runtime) {
                const std::vector<std::string>& retextured = runtime->retexturedArenas();
                for (size_t i = 0; i < retextured.size(); i++) {
                    const bool rebuilt = arenaViews.refresh(retextured[i]);
                    mdk::log3dsPrintf("views: %s retextured, %s", retextured[i].c_str(),
                                      rebuilt ? "rebuilt" : "its mesh was REFUSED");
                }
                runtime->clearRetexturedArenas();
            }

            // THE MUSIC OF THE ARENA THAT IS LOADING (docs/ARENA_STREAMING_SPEC.md
            // Phase 5). The original starts an arena's music preload when the
            // arena loads (arena_load -> 0x4192c4), so the track is in memory when
            // Kurt walks in. Here the arena that is loading is the pair's OTHER
            // one (the second arena, where a door or a corridor leads), and the
            // door bubble's nearest arena is a second hint: each one's track is
            // PRIMED by the audio worker (its file opened and its first two
            // seconds read, off this thread), so the track starts at the door
            // with no read. Asked once per change of either arena.
            if (music) {
                if (!bubbleHints.empty() && bubbleHints[0] != streamSecond &&
                    bubbleHints[0] != musicBubbleFor) {
                    musicBubbleFor = bubbleHints[0];
                    const std::string track = music->trackOf(bubbleHints[0]);
                    if (track != music->playing() && track != music->fading()) {
                        mdk::audioDspPrimeMusic(track);
                        mdk::log3dsPrintf("music: prime %s for %s (bubble)", track.c_str(),
                                          bubbleHints[0].c_str());
                    }
                }
                if (streamSecond != musicPrefetchedFor) {
                    musicPrefetchedFor = streamSecond;
                    if (!streamSecond.empty()) {
                        const std::string track = music->trackOf(streamSecond);
                        if (track != music->playing() && track != music->fading()) {
                            mdk::audioDspPrimeMusic(track);
                            mdk::log3dsPrintf("music: prime %s for %s", track.c_str(),
                                              streamSecond.c_str());
                        }
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
        // The background the scripts want (C# Viewer.Background; package #15): the sky
        // mode (opcode 202) picks the backdrop -- the dome below is drawn only under Sky,
        // mode 1 clears to black and any other keeps the last frame (no colour clear).
        // Under Sky the clear stays this frontend's 0xFF102030 rather than the C#'s DTI
        // sky colour: the dome's caps cover the panel, and tools/panel_geom.py's census
        // is defined on this exact colour.
        const int32_t frameSkyMode = runtime ? runtime->skyMode : mdk::sky_modes::kShown;
        params.backdrop = mdk::sky_modes::backdropOf(frameSkyMode);
        if (frameSkyMode == mdk::sky_modes::kBlack) {
            params.clear_colour = mdk::sky_modes::clearOf(frameSkyMode, params.clear_colour);
        }
        // Said when it changes (the C#'s `sky {mode} ({backdrop})`, Viewer.cs:543): the
        // retail arenas that black or hide the sky are closed rooms, so the log is where a
        // run shows it.
        {
            static int32_t loggedSkyMode = mdk::sky_modes::kShown;
            if (frameSkyMode != loggedSkyMode) {
                static const char* const kBackdropNames[] = {"Sky", "Clear", "Keep"};
                mdk::log3dsPrintf("sky: mode %d (%s) at frame %lu", (int)frameSkyMode,
                                  kBackdropNames[(int)params.backdrop], (unsigned long)frame);
                loggedSkyMode = frameSkyMode;
            }
        }
        params.present = true;
        params.screenshot_path = nullptr;

        // P2: the camera is handed to the renderer every frame, whether or not a
        // step ran, and every GPU state the frame needs is set inside
        // begin/end_frame by the backend.
        // THE SCOPE IN 2D (EnhancedToggles::scopeMono): while the scope is up the
        // parallax barrier goes off, so the renderer plans one eye (it reads
        // gfxIs3D) and the panel shows it to both; back on when the scope drops.
        {
            const bool wantMono =
                enhancedToggles.scopeMono && playerReady && player.sniping() && !strikeFrame;
            if (wantMono != scopeMonoOn) {
                gfxSet3D(!wantMono);
                scopeMonoOn = wantMono;
            }
        }
        mdk_render_begin_frame(renderer, &params);
        mdk_render_set_camera(renderer, &sceneCamera);

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
                const mdk::Lighting rig = mdk::cachedLighting(lightCache, level, lightArena);
                mdk_render_set_lighting(renderer, &rig);
                mdk::log3dsPrintf("light: arena %s haze rgb=(%.3f,%.3f,%.3f) density=%.5f",
                                  lightArena->name().c_str(), rig.haze_colour.r,
                                  rig.haze_colour.g, rig.haze_colour.b, rig.haze_density);
            }
        }

        // The sky first (see SkyRange): centred on the camera, DoubleSided -- only when
        // the scripts' sky mode shows it (C# `Backdrop == Backdrop.Sky`).
        if (skyMesh >= 0 && params.backdrop == mdk::Backdrop::Sky) {
            for (int32_t i = 0; i < skyRangeCount; i++) {
                const mdk::SkyRange& range = skyRanges[i];
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
                sky.world[12] = sceneCamera.position.x;
                sky.world[13] = sceneCamera.position.y;
                sky.world[14] = sceneCamera.position.z;
                mdk_render_draw_mesh(renderer, &sky);
            }
        }

        // The arenas the scripts say are drawn: Kurt's and the active second
        // (C# Viewer.cs:702, LevelView.Draw(DrawnArenas)); the start arena until
        // the runtime's first tick has named one.
        if (arena && frameDrawn && !strikeFrame) {
            // The end of a level's torn triangles leave the arena's mesh first (the C#'s
            // LevelView.DrawEnd, Viewer.cs:703): its view swaps to a dynamic mesh and
            // rewrites it in this frame, which is the slot C3D_FrameBegin waited for
            // (AGENTS.md P3). The flying pieces are drawn by the effects pass below.
            if (runtime && runtime->endLevel() &&
                !arenaViews.tear(runtime->endLevel()->arena(), runtime->endLevel()->torn())) {
                if (!tearRefusedLogged) {
                    tearRefusedLogged = true;
                    mdk::log3dsPrintf("level: the torn arena's dynamic mesh was REFUSED (%s); "
                                      "it is drawn whole, the pieces still fly",
                                      runtime->endLevel()->arena().c_str());
                }
            }
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
            // The other half of TransformOf: `obj == _driven` (the XD2 Kurt rides,
            // package #9) is drawn at its own transform shifted by Kurt's lag between
            // two steps. This port draws Kurt at Player::feet (FrameBlend's Kurt pose is
            // not ported), so his lag is zero and the driven object is drawn at its
            // current transform -- alpha 1 -- where Rides::follow put it after the
            // step, under the camera that follows the same feet. The XE Kurt flies
            // (package #11) is the C#'s Carrier (Viewer.cs:588): the oracle draws Kurt
            // and its camera blended with the XE, and here Rides::follow puts his feet
            // -- and so LookDown's camera -- on the XE's current place each step, so the
            // XE is drawn there too (alpha 1), or it would lag under the camera.
            //
            // And the ropes and sniper scope that the C# draws between the
            // objects and Kurt (Viewer.cs:705-706) are not ported; see the
            // header. The effects (Viewer.cs:707) draw below.
            const float tickAlpha = runtime->tick_blend(pending);
            // The object cull for this frame: the centre frustum, with the
            // sphere margin covering both stereo eyes (half the configured
            // separation; the eyes are this frustum translated along the
            // camera's right axis).
            mdk::Frustum objectFrustum;
            if (mdk::buildFrustum(sceneCamera, &objectFrustum)) {
                objectView.setCullFrustum(&objectFrustum,
                                          mdk::g_gameSettings.stereoSeparation * 0.5f);
            } else {
                objectView.setCullFrustum(nullptr, 0.0f);
            }
            objectStream.reset();
            // THE TINY-ENEMY CULL (EnhancedToggles::tinyEnemyPixels): an enemy whose
            // bounding sphere covers that many top-screen pixels or fewer across, at
            // the scene camera (the scope's zoom included; one decision for both
            // eyes), is not drawn until it covers more than 1.25 times as many --
            // ObjectView tests it after the frustum cull, on the same sphere.
            // Drawing only: the runtime has already run it this frame.
            const float tinyHide = (float)enhancedToggles.tinyEnemyPixels;
            objectView.setTinyCull(&sceneCamera, tinyHide, tinyHide * 1.25f, kTopWidth,
                                   kTopHeight);
            size_t slot = 0;
            for (const mdk::scripts::MdkObject& obj : runtime->objects()) {
                const size_t objectSlot = slot++;
                if (strikeFrame) {
                    break;  // the strike's frame shows its model alone (below)
                }
                // `dead`, not isRemoved(): remove() is the only writer of
                // dead=true in a running level and sets it as it appends to
                // removed_, so the two agree here -- and isRemoved scans every
                // removal so far, per visible slot, per presentation.
                if (!obj.visible || obj.dead) {
                    continue;
                }
                const bool carried = runtime->rides().bomber() && &obj == runtime->rides().ridden();
                const float alpha =
                    &obj == runtime->rides().driven() || carried ? 1.0f : tickAlpha;
                uint8_t* tinyHidden = nullptr;
                if (tinyHide > 0.0f) {
                    if (tinyEnemyState.size() <= objectSlot) {
                        tinyEnemyState.resize(objectSlot + 1, 0);
                        tinyEnemyHidden.resize(objectSlot + 1, 0);
                    }
                    uint8_t& state = tinyEnemyState[objectSlot];
                    if ((state & kTinyKnown) == 0) {
                        state = (uint8_t)(kTinyKnown | (tinyEnemyType(obj) ? kTinyCandidate : 0));
                    }
                    if ((state & kTinyCandidate) != 0 && &obj != runtime->barObject) {
                        tinyHidden = &tinyEnemyHidden[objectSlot];
                    } else {
                        tinyEnemyHidden[objectSlot] = 0;
                    }
                }
                objectView.addObject(obj, objectLook(obj.arena), obj.transform_at(alpha),
                                     &objectStream, tinyHidden);
            }
            if (strikeFrame) {
                // THE FULL-SCREEN STRIKE (package 14): the scene's model alone over the sky
                // (SniperView.DrawStrike: objects.Draw of its Subject, posed by its frame and
                // turned by its yaw), through this same pass -- which sets every GPU state
                // it needs per draw.
                const mdk::scripts::MdkObject& subject = runtime->strikeScene()->subject();
                // The strike's subject is the frame: never culled.
                objectView.setCullFrustum(nullptr, 0.0f);
                objectView.addObject(subject, objectLook(subject.arena), subject.transform(),
                                     &objectStream);
            }
            // C# SniperView.Draw's first loop (Viewer.cs:706; not in the strike's
            // frame, which DrawStrike draws instead): the sniper rounds in
            // flight, each in its arena's look, at its own transform.
            for (int32_t i = 0; i < mdk::scripts::SniperRounds::kSlots && !strikeFrame; i++) {
                if (runtime->sniperRounds().visualFlying(i)) {
                    const mdk::scripts::MdkObject& round = runtime->sniperRounds().visual(i);
                    objectView.addObject(round, objectLook(round.arena), round.transform(),
                                         &objectStream);
                }
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
        if (runtime && !strikeFrame) {
            // The pieces and sprite effects, where the C# draws them:
            // after the objects, before Kurt (Viewer.cs:707). The ropes
            // and sniper scope the C# draws alongside are not ported.
            // A refusal is reported like the object pass's, edge-triggered
            // and re-armed, but on this pass's own latch.
            // The effects cull (EffectsView::setCullFrustum): the object cull's
            // centre frustum and stereo margin, so a piece or sprite behind the
            // camera or off to the side is not built (the Linux build culls none).
            mdk::Frustum effectsFrustum;
            if (mdk::buildFrustum(sceneCamera, &effectsFrustum)) {
                effectsView.setCullFrustum(&effectsFrustum,
                                           mdk::g_gameSettings.stereoSeparation * 0.5f);
            } else {
                effectsView.setCullFrustum(nullptr, 0.0f);
            }
            // Debris this small on the top screen is not drawn (EnhancedToggles::
            // tinyDebrisPixels, 0 = off): one decision for both eyes, at the
            // scene camera's zoom.
            effectsView.setTinyPieceCull((float)enhancedToggles.tinyDebrisPixels, kTopWidth,
                                         kTopHeight);
            effectsView.build(*runtime, sceneCamera, &effectsFrame);
            effectsDraw.draw(renderer, &surfaces, effectsFrame);
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
        if (playerReady && !strikeFrame) {
            // The rest of C# SniperView.Draw (Viewer.cs:706): the overlay's state and
            // the loaded rounds' inset. NO round-camera insets on this target
            // (sniper_view.h): a watching camera's window is filled as an empty one.
            sniperView.draw(renderer, hud.sniper(), player, runtime.get(), elapsed, false,
                            objectView, &surfaces,
                            objectLook(runtime ? runtime->currentArena() : std::string()));
            if (sniperView.allocationFailed()) {
                if (!sniperClipFailureLogged) {
                    sniperClipFailureLogged = true;
                    mdk::log3dsPrintf("sniper: the clip's stream mesh — linearAlloc FAILED, "
                                      "the loaded rounds are not drawn");
                }
            } else {
                sniperClipFailureLogged = false;
            }
        }
        if (kurtSpriteReady && playerReady && !strikeFrame) {
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
        if (runtime && !strikeFrame) {
            // Viewer.cs:699: the scripts' Eye is the camera (an explosion faces it) --
            // the frame's view, the scope's in sniper mode; the strike's frames are
            // not DrawScene's, so they leave it alone.
            runtime->eye = sceneCamera.position;
        }
        const bool showHint = menuReady && !menu.open() && now < hintUntil;
        if (!strikeFrame && (hudReady || menu.open() || showHint)) {
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
                    // The XE bomber's sight (Viewer.cs:914, Rides.Bomber?.Shown; package
                    // #11), drawn by HudView through the canvas path like the rest.
                    mdk::scripts::Bomber::Sight sight;
                    const mdk::scripts::Bomber* bomber = runtime->rides().bomberRide();
                    if (bomber != nullptr && bomber->shown(&sight)) {
                        state.bomber = true;
                        state.bomberCursorX = sight.cursorX;
                        state.bomberCursorY = sight.cursorY;
                        state.bomberBombs = sight.bombs;
                    }
                }
                // Viewer.DrawScene: the messages advance by the frame's elapsed time
                // (paused frames too), then the HUD draws them last.
                hud.messages().update(elapsed);
                hud.draw(renderer, state);
                // THE SCOPE CLIP (EnhancedToggles::scopeClip): the scope's frame,
                // just queued, covers everything outside its 600 x 360 view, so the
                // scene is drawn inside the view only (the same picture).
                if (enhancedToggles.scopeClip && hud.sniper().visible()) {
                    float scale = 0.0f;
                    float left = 0.0f;
                    mdk::SniperOverlay::placement(mdk_render_canvas_width(renderer), &scale,
                                                  &left);
                    const float window[4] = {left + mdk::SniperOverlay::kViewX * scale,
                                             mdk::SniperOverlay::kViewY * scale,
                                             mdk::SniperOverlay::kViewWidth * scale,
                                             mdk::SniperOverlay::kViewHeight * scale};
                    mdk_render_set_scene_window(renderer, window);
                }
            }
            if (menu.open() && pauseOptions.open()) {
                // The options pages over the dimmed game (C# PauseMenu.Update's
                // drawing), on the 600 x 360 view: the top screen's canvas.
                mdk::ScreenView view;
                view.layoutInside(mdk_render_canvas_width(renderer), mdk::kCanvasHeight);
                pauseOptions.draw(renderer, view, menuBig, menuSmall);
            } else if (menu.open() && cheatsPage.open()) {
                cheatsPage.draw(renderer, menuBig, menuSmall, mdk_render_canvas_width(renderer),
                                cheatSwitches);
            } else if (menu.open()) {
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
        if (frame <= 5u) {
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

        if (g_frameBudget != 0 && frame >= g_frameBudget) {
            exitReason = "frame budget reached";
            break;
        }

        // THE LEVEL-END HOOK (package 2). Game.cs takes Event.LevelEnded to AfterLevel
        // (:442, :499-512): the level's counts and Kurt's health are kept (the C# reads
        // them off `_viewer`), and the flow says what follows. The stream is played
        // (package #18b: the loop's top, after this level's teardown); the
        // statistics, the fall and the save prompt are not ported, so they are skipped
        // the way the C# skips them (GameFlow::skipScreen, pinned against the C# by
        // golden/flow_next_level.txt), each one logged. A level at the end of that
        // walk is loaded by the level loop; anything else (the menu) ends the run.
        if (leaveLevel) {
            // C# Game.Handle(KurtDied) -- LASTGAME written, then the main menu with
            // its splash, whose first item is "Continue" (Game.cs:448-456,
            // MainMenu.ShowMain) -- or the Play/Briefing a quick load set. The menu
            // and the briefing are the menus' screens (package #20a); the screens
            // this port still lacks are skipped the C#'s way, each one logged.
            screen = flow.handle(leaveEvent);
            std::string report = flow.takeReport();
            mdk::flow::GameFlow::SkipTrail trail;
            if (screen != mdk::flow::Screen::Level && screen != mdk::flow::Screen::Menu &&
                screen != mdk::flow::Screen::Briefing) {
                screen = flow.skipUnported(&trail);
                const std::string more = flow.takeReport();
                report += report.empty() || more.empty() ? more : "; " + more;
            }
            if (!report.empty()) {
                mdk::log3dsPrintf("flow: %s", report.c_str());
            }
            for (int32_t i = 0; i < trail.count; i++) {
                mdk::log3dsPrintf("flow: skipped %s (package #%d not ported)",
                                  mdk::flow::GameFlow::screenName(trail.skipped[i]),
                                  (int)mdk::flow::GameFlow::screenPackage(trail.skipped[i]));
            }
            if (screen == mdk::flow::Screen::Level) {
                nextLevel = true;
                mdk::log3dsPrintf("flow: level %d again at frame %lu, deaths %d",
                                  (int)flow.state().level, (unsigned long)frame,
                                  (int)flow.state().deaths);
            } else if (screen == mdk::flow::Screen::Menu ||
                       screen == mdk::flow::Screen::Briefing) {
                mdk::log3dsPrintf("flow: the %s follows at frame %lu (deaths %d)",
                                  screen == mdk::flow::Screen::Menu ? "menu" : "briefing",
                                  (unsigned long)frame, (int)flow.state().deaths);
                toMenu = true;
            } else {
                mdk::log3dsPrintf("flow: %s follows, which this frontend cannot show; exiting",
                                  mdk::flow::GameFlow::screenName(screen));
                exitReason = leaveEvent == mdk::flow::Event::KurtDied ? "Kurt died" : "load";
            }
            break;
        }
        if (levelEndedExit) {
            mdk::flow::LevelResult result;
            if (runtime) {
                result.stats = runtime->stats;
            }
            result.health = player.health();
            flow.reportLevel(result);
            screen = flow.handle(mdk::flow::Event::LevelEnded);
            mdk::log3dsPrintf("flow: level %d ended (health %d); the flow shows %s",
                              (int)flow.state().level, (int)result.health,
                              mdk::flow::GameFlow::screenName(screen));
            if (screen == mdk::flow::Screen::Stream) {
                // The stream is shown (package #18b): the level is torn down first,
                // and the level loop's top plays it.
                nextLevel = true;
                break;
            }
            mdk::flow::GameFlow::SkipTrail trail;
            screen = flow.skipUnported(&trail);
            for (int32_t i = 0; i < trail.count; i++) {
                mdk::log3dsPrintf("flow: skipped %s (package #%d not ported)",
                                  mdk::flow::GameFlow::screenName(trail.skipped[i]),
                                  (int)mdk::flow::GameFlow::screenPackage(trail.skipped[i]));
            }
            if (screen == mdk::flow::Screen::Level) {
                nextLevel = true;
                mdk::log3dsPrintf("flow: next level %d at frame %lu, entered at health %d",
                                  (int)flow.state().level, (unsigned long)frame,
                                  (int)flow.levelHealth());
            } else if (screen == mdk::flow::Screen::Menu || screen == mdk::flow::Screen::Fall) {
                // The main menu follows (package #20a): after LEVEL5, a game over.
                // Or the next level's fall (#17b): the front screens show it.
                mdk::log3dsPrintf("flow: the %s follows",
                                  screen == mdk::flow::Screen::Fall ? "fall" : "menu");
                menu.setOpen(false);
                toMenu = true;
            } else if (screen == mdk::flow::Screen::Statistics) {
                // The statistics follow (package #20b): the menus' screens show them
                // (menu_screens_3ds.h), then the briefing of the next level.
                mdk::log3dsPrintf("flow: the statistics follow at frame %lu",
                                  (unsigned long)frame);
                menu.setOpen(false);
                toMenu = true;
            } else {
                mdk::log3dsPrintf("flow: %s follows, which this frontend cannot show; exiting",
                                  mdk::flow::GameFlow::screenName(screen));
                exitReason = "level ended";
            }
            break;
        }
        // THE GAME-FINISHED HOOK (beside package 2's): the C#'s Game takes
        // Event.GameFinished to the end movie (Game.cs:458, MDKEND.FLC and MDKBZK.MVE,
        // then the menu with the splash), which the front screens show (package #20c).
        if (gameFinishedExit) {
            screen = flow.handle(mdk::flow::Event::GameFinished);
            mdk::log3dsPrintf("game: finished; the flow shows %s",
                              mdk::flow::GameFlow::screenName(screen));
            if (screen == mdk::flow::Screen::EndMovie) {
                toMenu = true;
            } else {
                exitReason = "game finished";
            }
            break;
        }
    }

    if (kurtSpriteReady) {
        // The pack stream's record for the level, once, at its end (never inside play).
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

    // THE LEVEL'S TEARDOWN, between frames: the last frame has ended, the next
    // level's first has not begun (see the level loop's header for the order).
    // 1. Kurt's pack read in flight, collected and freed, so the next level's sprite
    //    (the same generation) cannot take it for one of its own. Bounded: the
    //    reader thread finishes a read in milliseconds; two seconds is a hang.
    if (kurtSpriteReady) {
        const double settleStart = mdk::nowSeconds();
        while (!kurtSprite.settleStream()) {
            if (mdk::nowSeconds() - settleStart > 2.0) {
                mdk::log3dsPrintf("level: Kurt's pack read did not land in 2 s; left to the "
                                  "reader");
                break;
            }
            svcSleepThread(1000000LL);
        }
    }
    // 2. The sound: the music's voices, then the output (the DSP worker joined and
    //    its bank pins given back WHILE the bank is alive; the software sink's ndsp
    //    channel), then the runtime and its mixer. The next level opens them again.
    // The scope in 2D does not outlive the level: the barrier back on.
    if (scopeMonoOn) {
        gfxSet3D(true);
        scopeMonoOn = false;
    }
    music.reset();
    mdk::audioDspClose();
    // The level's HD set goes with the level (its copies are nobody's from here
    // and the pump frees them).
    mdk_render_pin_hd_set(renderer, nullptr, nullptr, 0);
    runtime.reset();
    // 3. The pose caches key on the runtime's Model addresses, which the reset just
    //    freed (scripts/pose_cache.h; the C#'s next Viewer calls ForgetPoses).
    mdk::scripts::MdkObject::forget_poses();
    // 4. The arenas' GPU objects, while their views still know them.
    arenaViews.clear();
    }   // END OF THE LEVEL: its objects are destroyed here, in reverse order
    // 5. What the level made through the renderer and no object released, newest
    //    first (the deferred release: freed at the next frame boundary).
    mdk_render_release(renderer, levelScope);
    if (nextLevel || toMenu) {
        // 6. THE LOADING SCREEN'S FRAMES (C# Play: Show(null), LoadingScreen.Show,
        //    Show(null), Game.cs:480-483): two empty frames, the clear colour, not
        //    game frames (`frame` does not count them). C3D_FrameBegin waits for the
        //    previous queue and the deferred releases are freed there (AGENTS.md P3),
        //    so the linear memory is back BEFORE the next level asks for any.
        for (int i = 0; i < 2; i++) {
            MdkFrameParams blank;
            blank.elapsed = 0.0f;
            blank.frame_index = frame;
            blank.clear_colour = 0xFF000000u;
            blank.present = true;
            blank.screenshot_path = nullptr;
            mdk_render_begin_frame(renderer, &blank);
            mdk_render_end_frame(renderer);
        }
        // The heap and linear figures with no level loaded: a level-to-level run
        // returns to the same figures here (mem[run-start] is the first).
        mdk::heapProbe("level-teardown");
        continue;
    }
    break;
    }   // END OF THE LEVEL LOOP

    printf("[3ds] exiting after %lu frames\n", (unsigned long)frame);

    mdk::log3dsPrintf("exiting after %lu frames (%u steps): %s", (unsigned long)frame,
                      (unsigned)steps, exitReason);
    aptUnhook(&s_lifecycleCookie);
    // The arenas' GPU objects, the DSP path (its worker joined, its bank pins given
    // back while the bank was alive) and the software sink went with the last level
    // (the level loop's teardown); both closes are no-ops on what is already closed.
    mdk_render_destroy(renderer);
    mdk::audioClose();
    mdk::audioDspClose();
    mdk::romfsFileSystemExit();
    // The log is closed BEFORE gfxExit(): libctru's gfxExit() tears the SD
    // card's filesystem down, and a FILE* opened on it cannot be flushed
    // afterwards -- the final line would be silently lost.
    mdk::log3dsClose();
    gfxExit();
    return 0;
}

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
//   * The ropes, the sniper scope and the effects — `_ropes.Draw`, `_sniper.Draw`
//     and `_effects.Draw` between the objects and Kurt in C# Viewer.DrawScene
//     (Viewer.cs:705-707). None of the three is ported, so the frame here goes
//     arena, objects, Kurt, and stops where the port's list stops.
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
#include <string>

#include "audio/audio_3ds.h"
#include "audio/sound_bank.h"
#include "audio/sound_mixer.h"
#include "formats/bni.h"
#include "formats/case_insensitive_path.h"
#include "formats/cmi.h"
#include "formats/file_system.h"
#include "formats/mdk_data.h"
#include "formats/font.h"
#include "game/arena_space.h"
#include "game/enhanced_config.h"
#include "game/enhanced_menu.h"
#include "game/font_view.h"
#include "game/follow_camera.h"
#include "game/game_flow.h"
#include "game/hud_view.h"
#include "game/input.h"
#include "game/kurt_sprite.h"
#include "game/level_data.h"
#include "game/level_view.h"
#include "game/object_draw.h"
#include "game/object_view.h"
#include "game/player.h"
#include "render/material_resolver.h"
#include "render/render.h"
#include "scripts/script_runtime.h"
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

// The Enhanced Mode toggles' persistence (SPEC.md 7.9): the whole
// config.ini, rewritten whenever a toggle changes. The directory is
// the log's (log_3ds.cpp makes sdmc:/3ds/mdk before this can run).
bool saveEnhancedConfig(const mdk::EnhancedToggles& toggles) {
    FILE* config = fopen("sdmc:/3ds/mdk/config.ini", "wb");
    if (!config) {
        return false;
    }
    const std::string text = mdk::writeEnhancedConfig(toggles);
    const bool written = fwrite(text.c_str(), 1, text.size(), config) == text.size();
    fclose(config);
    return written;
}

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
                if (view.load(renderer, &level, arena, &surfaces)) {
                    mdk::log3dsPrintf("view: %d batches, %d vertices, %d triangles",
                                      (int)view.batchCount(), (int)view.vertexCount(),
                                      (int)arena->triangleCount());
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
            } else {
                // The archive PARSED, which is the runtime's requirement and not
                // Kurt's: the archive is what the built-in behaviours play out of,
                // the decode below is Kurt's own animations.
                spritesReady = true;
                if (kurtSprite.load(renderer, sprites, level.dti().palette())) {
                    kurtSpriteReady = true;
                    mdk::log3dsPrintf("sprites: Kurt's animations loaded");
                } else {
                    mdk::log3dsPrintf("sprites: FAILED to decode Kurt's animations");
                }
                if (hud.load(renderer, sprites, level.dti().palette())) {
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
                            mdk::log3dsPrintf("menu: video/graphics ready (SELECT)");
                        } else {
                            mdk::log3dsPrintf("menu: font atlases did not upload -- no menu");
                        }
                    }
                }
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
        if (scriptAssetsReady) {
            // The seed. The oracle seeds its RNG with `new Random()` when no seed is
            // passed (ScriptRuntime.cs:323), which is the clock: an oracle run is
            // NOT reproducible here either, so a fixed seed would be the divergence,
            // not the fidelity. The console's clock is the ARM11 cycle counter, and
            // the value is logged at spawn so a run can be replayed by hand.
            const int32_t seed = (int32_t)(uint32_t)svcGetSystemTick();
            runtime.reset(new mdk::scripts::ScriptRuntime(&level, &cmi, seed, &bank, &player,
                                                          &sprites));
            runtime->option = kScriptOption;
            // Kurt's sounds go through the runtime's mixer, because in the C# they
            // are the same mixer.
            player.setMixer(&runtime->mixer());
            mix = &runtime->mixer();
            if (audioReady) {
                // The new mixer owns a new SoundDevice, so the sink has to be handed
                // to it: a respawn without this would be a silent level.
                mix->device().setSink(mdk::audioSink, nullptr);
            }
            mdk::log3dsPrintf("scripts: runtime ready (seed %d, option %d)", (int)seed,
                              (int)kScriptOption);
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
    // This frame's object-pass cost, and the peak over the run. The peak is the
    // number that says what a level costs the stream mesh: the capacity is rounded
    // up to a power of two, so the capacity alone cannot tell a 480-corner level
    // from one that nearly fills its 4096.
    int32_t objectCorners = 0;
    int32_t objectDraws = 0;
    int32_t objectPeakCorners = 0;
    int32_t objectPeakDraws = 0;
    const char* exitReason = "aptMainLoop returned false";
    u32 prevHeld = 0;  // last frame's held mask; the menu reads edges

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
        const u32 pressed = held & ~prevHeld;
        prevHeld = held;

        // The Video/Graphics settings menu (SPEC.md 7.9). SELECT is
        // unbound in the game, so it opens and closes the menu; while
        // the menu is open the simulation below is paused and the
        // frame's edges drive the menu instead. A flip applies at once
        // (mdk_render_set_enhanced_toggles) and is written back to
        // config.ini in the same frame, so a hardware A/B is one
        // button press. START remains the way out of the build.
        if (menuReady) {
            if (!menu.open() && (pressed & KEY_SELECT)) {
                menu.setOpen(true);
                mdk::log3dsPrintf("menu: video/graphics opened");
            } else if (menu.open()) {
                mdk::EnhancedMenu::Input edges;
                edges.up = (pressed & KEY_DUP) != 0;
                edges.down = (pressed & KEY_DDOWN) != 0;
                edges.left = (pressed & KEY_DLEFT) != 0;
                edges.right = (pressed & KEY_DRIGHT) != 0;
                edges.confirm = (pressed & KEY_A) != 0;
                edges.close = (pressed & KEY_SELECT) != 0;
                if (menu.handle(edges, &enhancedToggles)) {
                    mdk_render_set_enhanced_toggles(renderer, &enhancedToggles);
                    const bool saved = saveEnhancedConfig(enhancedToggles);
                    mdk::log3dsPrintf("menu: shadows=%d lights=%d haze=%d mirrors=%d "
                                      "glow=%d (saved=%d)",
                                      (int)enhancedToggles.enableSkyShadows,
                                      (int)enhancedToggles.enableFragmentLights,
                                      (int)enhancedToggles.enableHaze,
                                      (int)enhancedToggles.enableMirrors,
                                      (int)enhancedToggles.enableGlowBillboards, (int)saved);
                }
                if (!menu.open()) {
                    mdk::log3dsPrintf("menu: closed");
                }
            }
            if (menu.open()) {
                // The poll accumulated this frame's look into the
                // mouse; the paused game must not spend it on resume.
                input.takeMouseX();
                input.takeMouseY();
            }
        }

        // The flow's events. START is the C# pad's Escape, and Escape opens
        // the (cut) main menu in the C#: the honest answer here is to stop,
        // and the flow says so (Quit -> Screen::None) rather than the
        // frontend deciding on its own. SELECT was the second quit chord
        // while there was no menu; with the settings menu ready it opens
        // that instead (above), and this chord only remains when the
        // menu's fonts did not load.
        mdk::flow::Event next = mdk::flow::Event::None;
        if ((held & KEY_START) || (!menuReady && (held & KEY_SELECT))) {
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
            while (pending >= step) {
                player.update(input, step);
                // The C# Viewer's step order is Kurt, then the scripts, then the
                // camera (Viewer.cs:505-535). ScriptRuntime keeps its own 1/30 s
                // clock, so a 1/60 s call completes a script tick every second step
                // while Kurt's physics stay at 60 Hz.
                if (runtime) {
                    runtime->update(step);
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
        mix->listenFromCamera(camera);
        mix->update(elapsed);
        mix->device().update(elapsed);
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
            // And the ropes, the sniper scope and the effects that the C# draws
            // between the objects and Kurt (Viewer.cs:705-707) are not ported; see
            // the header.
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
        if (kurtSpriteReady && playerReady) {
            // C# Viewer.DrawScene draws Kurt after the level and before the HUD.
            kurtSprite.draw(renderer, player, player.feet, camera.position, camera.forward,
                            camera.up, mdk::follow_camera::kFieldOfView, &space);
        }
        if (hudReady || menu.open()) {
            // The HUD is the last thing drawn before the frame ends, which is the
            // C#'s order (Viewer.DrawScene: the canvas is the Overlay pass, and
            // the queue draws Overlay after every other pass). The canvas seams
            // queue it rather than drawing it: see renderer_c3d.cpp's canvas
            // section for why a 3DS canvas cannot flush at its own boundary.
            //
            // What the state carries is what this port has: health, death and the
            // two combat flashes, all read off Kurt himself exactly as
            // Viewer.HudStateOf passes them (Viewer.cs:913). The inventory and the
            // target bar stay empty until the pickup and shooting subsystems land,
            // because the C# starts them empty and nothing here may invent them.
            mdk_render_canvas_begin(renderer);
            if (hudReady) {
                mdk::HudState state;
                state.health = player.health();
                state.dead = player.dead;
                state.hurtFlash = player.combat().hurtFlash();
                state.whiteFlash = player.combat().whiteFlash();
                hud.draw(renderer, state);
            }
            if (menu.open()) {
                menu.draw(renderer, menuBig, menuSmall, mdk_render_canvas_width(renderer),
                          enhancedToggles);
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
        if (frame <= 5u || (frame % 60u) == 0u) {
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

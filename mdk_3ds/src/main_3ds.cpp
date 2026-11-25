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
//     (menu_screens_3ds.h): a release build boots into the menu, a harness LEVEL=
//     straight into its level.
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

#include <algorithm>
#include <map>
#include <set>
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
#include "game/harness_aids.h"
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
#include "game/sniper_view.h"
#include "render/material_resolver.h"
#include "render/render.h"
#include "render/render_c3d_stats.h"
#include "scripts/script_runtime.h"
#include "util/math_lut.h"
#include "platform/filesystem_romfs.h"
#include "platform/harness_3ds.h"
#include "render/sky_dome.h"
#include "game/sky_modes.h"
#include "platform/input_3ds.h"
#include "platform/vfp_fpscr.h"
#include "platform/log_3ds.h"
#include "menu_screens_3ds.h"
#include "stream_3ds.h"

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
// THE GAME'S SETTINGS (C# Flow/Settings.cs, package #20a): the volumes, the music
// filter, the difficulty and gore, kept in the same config.ini after the toggles
// (game/game_settings.h's formatConsole; each parser skips the other's keys).
// File-scope because the menus' option changes save the file from the screens
// (menu_screens_3ds.cpp) as well as from the level's START menu.
static mdk::GameSettings g_gameSettings;
// The toggles the file is written with (main's own), for saveConfigNow.
static const mdk::EnhancedToggles* g_toggles = nullptr;

bool saveEnhancedConfig(const mdk::EnhancedToggles& toggles) {
    FILE* config = fopen("sdmc:/3ds/mdk/config.ini", "wb");
    if (!config) {
        return false;
    }
    const std::string text = mdk::writeEnhancedConfig(toggles) + g_gameSettings.formatConsole();
    const bool written = fwrite(text.c_str(), 1, text.size(), config) == text.size();
    fclose(config);
    return written;
}

// config.ini with the current toggles and settings (the options' save, C#
// Ui.ApplySettings).
bool saveConfigNow() {
    return g_toggles != nullptr && saveEnhancedConfig(*g_toggles);
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
// (ModImages::fromList) without scanning the card. In play a surface's HD file is
// found BY NAME through the orig pack's name index (loadOrigByName, spec 3.1:
// no content hash on the console), and the pair is made from its two files
// (mdk_render_create_compressed_file): the backend streams the HD copy under the
// HD budget while the toggle is on and never reads the original for it.
const char kHdPackRoot[] = "sdmc:/3ds/mdk/hd";

// KURT'S COMPRESSED FRAMES (EnhancedToggles::kurtCompressed): his ORIGINAL
// TRAVSPRT.BNI frames as ETC1A4, made on the host by
// mdk_3ds/tools/kurt_etc1_convert.py, packed into ONE file by kurt_pack.py
// (formats/kurt_pack.h) and staged into the RomFS by stage_romfs.sh (the set is
// derived from the game data the RomFS already carries, so it ships in every
// .3dsx and .cia and the hardware loop needs no separate SD copy).
const char kKurtPackPath[] = "romfs:/kurt/kurt.pak";

// THE ORIG PACK: the ORIGINAL-LOOK arena textures pre-encoded on the host --
// the same manifest, the same hdKeyOf keys, the same .t3x import as the HD
// pack -- staged into the RomFS by stage_romfs.sh, so it ships in every .3dsx
// and .cia and the hardware loop needs no separate SD copy. Its point is the
// level-load / arena-stream-in hitches: an arena index texture expanded on the
// ARM11 (palette -> RGBA8 -> 16-bit pack -> mip chain) costs 600-1600 ms per
// arena. A surface the pack holds is ASKED FOR from its .t3x at first use
// (surface_cache.h's pre-creation consult -> mdk_render_create_compressed_file):
// the renderer's reader thread reads the file off the frame, a frame boundary
// imports it (a copy: the containers are uncompressed), and only a surface in
// view that is still unread is read on the main thread. The expansion never
// runs.
// With no pack staged (the tool did not run) the manifest open fails silently
// and the expansion is today's, exactly.
const char kOrigPackRoot[] = "romfs:/orig";

// The sound bank's decoded store, bounded (see where it is set).
const size_t kDecodedSoundBudgetBytes = (size_t)6 << 20;

// THE MUSIC PREFETCH'S READS, LENT (audio/sound_bank.h's setReader, game/
// async_read.h): the renderer's file thread on its music channel (render.h's
// mdk_render_read_music_async, platform/async_file_reader.h's kChannelMusic). On
// the main thread a step of a track was an fopen, a seek and a read of up to 64
// KiB -- 11-17 ms a frame for eight frames of level 7's corridor crossing on the
// Old 3DS -- and the thread reads it while the main one waits for the frame end.
bool musicReadRequest(void* user, int32_t tag, const char* path, uint32_t offset,
                      uint32_t length) {
    return mdk_render_read_music_async(static_cast<MdkRenderer>(user), tag, path, offset, length);
}

bool musicReadPoll(void* user, int32_t tag, uint8_t** bytes, uint32_t* size, bool* ok) {
    return mdk_render_poll_music_read(static_cast<MdkRenderer>(user), tag, bytes, size, ok);
}

void musicReadCancel(void* user, int32_t tag) {
    mdk_render_cancel_music_read(static_cast<MdkRenderer>(user), tag);
}

// A compressed pack's manifest, indexed for `level` by ModImages' own rule.
// False when there is none. `tag` names the pack in its log lines ("hd",
// "orig"); `maxCap` is the largest build cap the backend budgets for it (see
// the column-8 gate below).
//
// `dims` (may be null) gets each file's SOURCE size, columns 3 and 4, keyed by
// the file's key (column 1, no .t3x): for the orig pack that is the original
// texture's width and stacked height (arena_etc1_convert.py), which the name
// index checks a texture against (loadOrigByName). `files` (may be null) gets
// the manifest's file list.
bool loadCompressedManifest(const char* root, int32_t level, mdk::ModImages* out,
                            int32_t* count, const char* tag, int32_t maxCap,
                            std::map<std::string, uint32_t>* dims = nullptr,
                            std::vector<std::string>* filesOut = nullptr,
                            std::map<std::string, uint32_t>* shapes = nullptr) {
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
        // (hd_pack_convert.py): the backend budgets the largest pack texture
        // (kHdLargestBytes, sized for the worst file of the largest cap it
        // accepts), so a pack built above `maxCap` is refused, not drawn over
        // budget. The HD pack is capped at 512; the orig pack's arena textures
        // pad up to 1024x512 and is capped at 1024 -- kHdLargestBytes covers
        // that worst case (01_types.inc).
        {
            int column = 0;
            const char* field = line;
            for (const char* p = line; *p && column < 8; p++) {
                if (*p == '\t') {
                    column++;
                    field = p + 1;
                }
            }
            if (column == 8 && atoi(field) > maxCap) {
                tooBig = true;
            }
        }
        char* tab = strchr(line, '\t');
        if (!tab) {
            continue;
        }
        *tab = '\0';
        if (shapes) {
            // key \t format \t src_w \t src_h \t w \t h ...: the .t3x's own size and
            // format (an HD file's exact bytes, hdChainBytes), keyed by its path.
            const char* format = tab + 1;
            const char* c2 = strchr(format, '\t');
            const char* c3 = c2 ? strchr(c2 + 1, '\t') : nullptr;
            const char* c4 = c3 ? strchr(c3 + 1, '\t') : nullptr;
            const char* c5 = c4 ? strchr(c4 + 1, '\t') : nullptr;
            if (c4 && c5) {
                const uint32_t w = (uint32_t)atoi(c4 + 1);
                const uint32_t h = (uint32_t)atoi(c5 + 1);
                const bool a4 = strncmp(format, "etc1a4", 6) == 0;
                (*shapes)[std::string(root) + "/" + std::string(line) + ".t3x"] =
                    (w << 16) | (h & 0xFFFFu) | (a4 ? 0x80000000u : 0u);
            }
        }
        if (dims) {
            // key \t format \t src_w \t src_h ...
            const char* format = tab + 1;
            const char* srcW = strchr(format, '\t');
            const char* srcH = srcW ? strchr(srcW + 1, '\t') : nullptr;
            if (srcW && srcH) {
                const uint32_t w = (uint32_t)atoi(srcW + 1);
                const uint32_t h = (uint32_t)atoi(srcH + 1);
                (*dims)[std::string(line)] = (w << 16) | (h & 0xFFFFu);
            }
        }
        files.push_back(std::string(line) + ".t3x");
    }
    fclose(file);
    if (tooBig) {
        mdk::log3dsPrintf("%s: pack built above the %d cap the 3DS budgets for; not used", tag,
                          (int)maxCap);
        return false;
    }
    *out = mdk::ModImages::fromList(root, files, true, level);
    *count = (int32_t)files.size();
    if (filesOut) {
        filesOut->swap(files);
        return !filesOut->empty();
    }
    return !files.empty();
}

static std::string lowerName3ds(const std::string& s);

// THE HD PACK BY CONTENT KEY, for the name index (loadOrigByName): every
// texture file of the pack, keyed by the hdKeyOf in its name
// (textures/LEVELn/NAME@<key>.t3x -- hd_pack_convert.py keeps the PC mod's
// names). The key is the texture seen through a palette (game/hd_key.h), the
// same key the orig pack's file names carry, so an orig file and an HD file with
// one key are the same texture through the same palette -- matched here by
// STRING, with no hash computed on the console. Only exact-view files (`@`):
// the pack has no other kind (hd_pack_convert.py writes the PC mod's tree, and
// the C# port's --upscale-textures names every texture by its view).
bool loadHdPack(int32_t level, mdk::ModImages* out,
                std::map<std::string, std::string>* byKey,
                std::map<std::string, uint32_t>* shapes) {
    int32_t count = 0;
    std::vector<std::string> files;
    if (!loadCompressedManifest(kHdPackRoot, level, out, &count, "hd", 512, nullptr, &files,
                                shapes)) {
        return false;
    }
    for (size_t i = 0; i < files.size(); i++) {
        const std::string& file = files[i];
        if (file.compare(0, 9, "textures/") != 0) {
            continue;
        }
        const size_t slash = file.rfind('/');
        const size_t mark = file.find('@', slash == std::string::npos ? 0 : slash);
        const size_t dot = file.rfind('.');
        if (mark == std::string::npos || dot == std::string::npos || dot <= mark + 1) {
            continue;
        }
        (*byKey)[lowerName3ds(file.substr(mark + 1, dot - mark - 1))] =
            std::string(kHdPackRoot) + "/" + file;
    }
    mdk::log3dsPrintf("hd: pack manifest %d textures, %d keyed (%s)", (int)count,
                      (int)byKey->size(), kHdPackRoot);
    return true;
}

// THE BYTES AN HD FILE HOLDS IMPORTED (hdShapes' w, h, ETC1A4 bit) with its
// levels wider or taller than `cap` dropped (0: none) -- the backend's own drop
// rule (04_textures.inc hdUnpack) and tex3ds' chain, levels down to 8 texels on
// the shorter side: ETC1 4 bits a texel, ETC1A4 8.
static uint32_t hdChainBytes(uint32_t shape, int32_t cap) {
    uint32_t w = (shape >> 16) & 0x7FFFu;
    uint32_t h = shape & 0xFFFFu;
    const uint32_t bits = (shape & 0x80000000u) ? 8u : 4u;
    while (cap > 0 && (w > (uint32_t)cap || h > (uint32_t)cap) && w >= 16 && h >= 16) {
        w >>= 1;
        h >>= 1;
    }
    uint32_t bytes = 0;
    while (w >= 8 && h >= 8) {
        bytes += w * h * bits / 8u;
        if (w == 8 || h == 8) {
            break;
        }
        w >>= 1;
        h >>= 1;
    }
    return bytes;
}

// The orig pack, at the same level-load moment and through the same machinery
// as the HD pack (loadCompressedManifest). Its textures are the ORIGINAL look
// pre-encoded -- original resolution, a full gaussian mip chain the sampler
// only descends under the Enhanced Mode filtering+mipmaps pair (06_draw.inc)
// -- so with the pack staged the ARM11 expands nothing for the arenas that
// have a key, and the original look is the ETC1 copy sampled NEAREST at level 0
// rather than the palette expansion.
bool loadOrigPack(int32_t level, mdk::ModImages* out, std::map<std::string, uint32_t>* dims) {
    int32_t count = 0;
    if (!loadCompressedManifest(kOrigPackRoot, level, out, &count, "orig", 1024, dims)) {
        return false;
    }
    mdk::log3dsPrintf("orig: pack manifest %d textures (%s)", (int)count, kOrigPackRoot);
    return true;
}

// THE NAME-KEYED ORIG INDEX (romfs:/orig/byname.txt). manifest.txt is keyed by
// the CONTENT hash, so a ModImages lookup re-hashes the texture's whole index
// array -- 60-150 ms per texture on the ARM11, measured, which is the entire
// cost of the texture pumps. The build also knows every texture by name
// (arena_etc1_convert.py writes byname.txt from the exporter's index.tsv), so
// the pump and the draw can find the file by (arena, name) for free. A miss
// falls through to the hash path: a stale or absent byname.txt costs time,
// never correctness. Keyed by the arena's PALETTE because that is what the
// lookup is handed -- and the palette is what the .t3x's pixels were encoded
// through. A corridor shares its arena's palette OBJECT (LevelData::paletteOf),
// so the two collide here on purpose: their rows name the same file (checked
// for all 3156 corridor rows of the shipped pack, 2026-11-23), and a row that
// ever disagrees is logged rather than silently kept.
//
// AND THE HD PACK BY NAME, from the same rows (docs/HD_TEXTURES_3DS_SPEC.md
// 3.1): a row's file is named by its content key (textures/name@<key>), and the
// HD pack's file of that key (loadHdPack's byKey) is the upscale of exactly that
// texture through exactly that palette -- so the HD file is found by (palette,
// name) too, and play never hashes a texture to find it. No pack has to be
// rebuilt: the owner's SD pack is keyed this way already. `dims` is the original
// texture's width and stacked height from the orig manifest: a texture asked for
// under a row's name but of another size is not that row's (no file is answered,
// and the texture keeps the path a miss takes).
struct OrigByName {
    std::map<std::pair<const void*, std::string>, std::string> paths;
    std::map<std::pair<const void*, std::string>, std::string> hdPaths;
    std::map<std::pair<const void*, std::string>, uint32_t> dims;
};

static std::string lowerName3ds(const std::string& s) {
    std::string out = s;
    for (size_t i = 0; i < out.size(); i++) {
        if (out[i] >= 'A' && out[i] <= 'Z') {
            out[i] = (char)(out[i] - 'A' + 'a');
        }
    }
    return out;
}

bool loadOrigByName(int32_t level, mdk::LevelData& levelData,
                    const std::map<std::string, std::string>& hdByKey,
                    const std::map<std::string, uint32_t>& origDims, OrigByName* out) {
    FILE* file = fopen((std::string(kOrigPackRoot) + "/byname.txt").c_str(), "r");
    if (!file) {
        return false;
    }
    char line[512];
    while (fgets(line, sizeof(line), file)) {
        const size_t length = strlen(line);
        if (length > 0 && line[length - 1] != '\n' && !feof(file)) {
            int c;
            while ((c = fgetc(file)) != EOF && c != '\n') {
            }
            continue;
        }
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }
        // level \t arena \t name \t rel   (arena_etc1_convert.py)
        std::string fields[4];
        size_t field = 0;
        for (const char* p = line; *p && *p != '\n' && field < 4; p++) {
            if (*p == '\t') {
                field++;
            } else {
                fields[field] += *p;
            }
        }
        if (field != 3 || atoi(fields[0].c_str()) != (int)level) {
            continue;
        }
        const mdk::formats::Arena* arena = levelData.arenaNamed(fields[1]);
        if (arena == nullptr) {
            continue;
        }
        const mdk::formats::Palette& palette = levelData.paletteOf(arena);
        const std::pair<const void*, std::string> key(
            (const void*)&palette, lowerName3ds(fields[2]));
        const std::string path = std::string(kOrigPackRoot) + "/" + fields[3] + ".t3x";
        const std::map<std::pair<const void*, std::string>, std::string>::iterator known =
            out->paths.find(key);
        if (known != out->paths.end() && known->second != path) {
            // Only reachable if the exporter ships a corridor and its arena
            // different files for one name. The arena with colours of its own
            // is the owner (a corridor borrows, so it has none): it wins.
            const bool ownerRow = arena->paletteRgbBytes() != 0;
            mdk::log3dsPrintf("orig: byname collision arena=%s name=%s keeping=%s ignoring=%s",
                              fields[1].c_str(), fields[2].c_str(),
                              ownerRow ? path.c_str() : known->second.c_str(),
                              ownerRow ? known->second.c_str() : path.c_str());
            if (!ownerRow) {
                continue;
            }
        }
        out->paths[key] = path;
        const std::map<std::string, uint32_t>::const_iterator dims = origDims.find(fields[3]);
        if (dims != origDims.end()) {
            out->dims[key] = dims->second;
        } else {
            out->dims.erase(key);
        }
        const size_t mark = fields[3].find('@');
        const std::map<std::string, std::string>::const_iterator hd =
            mark == std::string::npos ? hdByKey.end()
                                      : hdByKey.find(lowerName3ds(fields[3].substr(mark + 1)));
        if (hd != hdByKey.end()) {
            out->hdPaths[key] = hd->second;
        } else {
            out->hdPaths.erase(key);
        }
    }
    fclose(file);
    return !out->paths.empty();
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

// The surface cache's lookup into the compressed packs: a Lit or Sprite
// surface's .t3x path (a Sprite takes only an orig-pack answer: SurfaceCache
// surfaceId), answered by the pair and its names, never by the shading. `pack`
// is the HD pack (the SD card's upscale), `orig` the orig pack (the RomFS's
// original-resolution pre-encode); either may be absent.
// THE CONTENT HASHES the lookup ran (ModImages::resolve's hdKeyOf over the
// texture's whole index array, after any MTO read-back of its pixels): how many,
// their time, and the first texture's name. Said once per level (hdrule:, at
// teardown) -- the HD rule's proof that play never hashes a covered surface.
static int32_t g_lookupHashes = 0;
static double g_lookupHashMs = 0.0;
static char g_lookupHashFirst[40];
// The name index's other outcomes: misses with an HD pack installed (never
// hashed: expanded), rows refused for their size, and HD files not taken for an
// animated texture.
static int32_t g_lookupNameMisses = 0;
static char g_lookupNameMissFirst[48];
static int32_t g_lookupSizeRefused = 0;
static int32_t g_lookupHdAnimated = 0;

struct HdPackSource {
    mdk::ModImages* pack = nullptr;
    mdk::ModImages* orig = nullptr;
    OrigByName* byname = nullptr;
    std::string path;
    std::string hdPath;
    // Set by the warms around their ask (requestPackedSurface): the ask is
    // a HINT, and a hint never brings a texture's pixels back (below).
    bool hint = false;
};

bool hdPackLookup(void* user, const mdk::Surface& surface, const mdk::formats::Palette* palette,
                  mdk::ColourReplacement* out) {
    HdPackSource* source = static_cast<HdPackSource*>(user);
    if (!source || (!source->pack && !source->orig) || !surface.texture || !palette) {
        return false;
    }
    const std::string& name = surface.name.empty() ? surface.texture->name() : surface.name;
    // THE NAME INDEX FIRST, with or without an HD pack: the build knows this
    // texture by name, so the content hash is the fallback, not the rule. Both
    // names it could be keyed by are tried -- the material's and the texture's
    // own -- because an archive can hold a texture under a different name than
    // its own. A row whose original size is not this texture's is not this
    // texture's file: it is skipped (counted), never answered.
    //
    // THE HD RULE (docs/HD_TEXTURES_3DS_SPEC.md 3.1): a row with an HD file
    // answers BOTH files -- the packed original (compressedPath) and the HD copy
    // (hdPath) -- and the backend makes the texture from them alone
    // (mdk_render_create_compressed_file): with enableHdTextures on only the HD
    // file is read, with it off only the original, and the toggle stays live per
    // draw. No standard-definition pixel is read, expanded or hashed for it. An
    // ANIMATED texture (stacked frames) takes no HD copy: the pack's strip is
    // resampled to a power of two (hd_pack_convert.py), which squeezes its
    // frames -- EXPLODE's 26 frames of 128 rows become 19.7 rows each -- and the
    // backend never draws an HD copy for a stacked texture (06_draw.inc).
    if (source->byname && !source->byname->paths.empty()) {
        std::string keys[2] = {lowerName3ds(name), lowerName3ds(surface.texture->name())};
        const uint32_t size =
            ((uint32_t)surface.texture->width() << 16) |
            ((uint32_t)(surface.texture->height() * surface.texture->frameCount()) & 0xFFFFu);
        for (int k = 0; k < 2; k++) {
            const std::pair<const void*, std::string> key((const void*)palette, keys[k]);
            std::map<std::pair<const void*, std::string>, std::string>::const_iterator hit =
                source->byname->paths.find(key);
            if (hit == source->byname->paths.end()) {
                continue;
            }
            std::map<std::pair<const void*, std::string>, uint32_t>::const_iterator dims =
                source->byname->dims.find(key);
            if (dims != source->byname->dims.end() && dims->second != size) {
                g_lookupSizeRefused++;
                continue;
            }
            source->path = hit->second;
            out->compressedPath = source->path.c_str();
            out->compressedOriginal = true;
            std::map<std::pair<const void*, std::string>, std::string>::const_iterator hd =
                source->byname->hdPaths.find(key);
            if (hd != source->byname->hdPaths.end()) {
                if (surface.texture->frameCount() <= 1) {
                    source->hdPath = hd->second;
                    out->hdPath = source->hdPath.c_str();
                } else {
                    g_lookupHdAnimated++;
                }
            }
            return true;
        }
    }
    // A HINT ANSWERS FROM THE NAME INDEX ONLY. The content hash below reads
    // and hashes the texture's whole index array (and, for an arena texture
    // whose pixels are away, first brings back its arena's whole texture block,
    // LevelData::loadPixelsFromMto). Measured on the Old 3DS: the object warm's
    // step for EXPLODE -- its 128x3328 strip is over the PICA's 1024, so the
    // pack never holds it and the name index misses -- spent 104.0 ms of frame 1
    // in that hash. A hint exists to take work OFF the frame, so it never pays
    // for a hash: `true` with no path is SurfaceCache::requestPacked's "not a
    // pack answer" -- nothing made, nothing marked -- and the texture's first
    // draw asks the full lookup, exactly as without the warm.
    if (source->hint) {
        return true;
    }
    // WITH AN HD PACK, A NAME MISS IS NOT HASHED. The hash is what finds a
    // texture in either pack by content, and it is the cost the HD rule forbids
    // in play: 60-150 ms per texture on the Old 3DS, plus the MTO read-back of
    // its pixels. A texture the name index does not know -- one over the PICA's
    // 1024 (refused from both packs by the builds), a model's texture outside
    // the arenas' archives -- is drawn the way it would be with no pack at all:
    // expanded from its indices. Counted and named (hdrule:, at teardown).
    if (source->pack && source->byname) {
        if (g_lookupNameMisses == 0) {
            snprintf(g_lookupNameMissFirst, sizeof(g_lookupNameMissFirst), "%s %dx%d",
                     name.c_str(), (int)surface.texture->width(),
                     (int)(surface.texture->height() * surface.texture->frameCount()));
        }
        g_lookupNameMisses++;
        return false;
    }
    // NOTHING OVER THE PICA'S 1024 IS IN THE ORIG PACK (arena_etc1_convert.py
    // refuses such a strip by name: sl_big 64x1920, fire 64x2628, explode
    // 128x3328), so with no HD pack the content hash below can never match one
    // -- and hashing EXPLODE's 128x3328 strip costs ~100 ms on the Old 3DS. A
    // Sprite-shaded object part drawing it asks here at its first draw (frame
    // 650 of the L7 walk: obj 105.5 ms once Sprites asked the pack).
    if (!source->pack && (surface.texture->width() > 1024 ||
                          surface.texture->height() * surface.texture->frameCount() > 1024)) {
        return false;
    }
    // THE HD PACK FIRST, THE ORIG PACK SECOND: where both hold the key the HD
    // copy (the upscale) wins, and the orig pack fills the rest -- an arena the
    // HD pack does not cover still skips the ARM11 expansion. Both resolve by
    // the same hdKeyOf key, so a texture the HD pack rekeys (a stamped texture,
    // 05_resources.inc) matches neither pack and takes the index path.
    if (g_lookupHashes == 0) {
        snprintf(g_lookupHashFirst, sizeof(g_lookupHashFirst), "%s", name.c_str());
    }
    g_lookupHashes++;
    const double hashStart = nowSeconds();
    if (source->pack && source->pack->resolve(mdk::ModImages::Kind::Textures, name,
                                              *surface.texture, *palette, &source->path)) {
        g_lookupHashMs += (nowSeconds() - hashStart) * 1000.0;
        out->compressedPath = source->path.c_str();
        return true;
    }
    const bool origHit =
        source->orig && source->orig->resolve(mdk::ModImages::Kind::Textures, name,
                                              *surface.texture, *palette, &source->path);
    g_lookupHashMs += (nowSeconds() - hashStart) * 1000.0;
    if (origHit) {
        out->compressedPath = source->path.c_str();
        // THE ORIG PACK'S HIT IS ASKED FOR UP FRONT, not attached after an
        // expansion: surfaceId's pre-creation consult (surface_cache.h) turns
        // this into mdk_render_create_compressed_file, so the ARM11 expands
        // nothing for this texture -- the arena's 600-1600 ms of palette
        // expansion, filter and mip chain becomes a RomFS read on the reader
        // thread and an import at a frame boundary.
        out->compressedOriginal = true;
        return true;
    }
    return false;
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

// The light rig for one arena, computed once and kept in `cache` (main's
// lightCache). enhancedLighting scans the arena's faces -- ~113 ms per arena
// on the Old 3DS -- and its answer depends on nothing but (dti, arena), both
// fixed for the level's life. The frame's light code and the stream pump both
// come through here, so the scan runs at most once per arena.
static mdk::Lighting cachedLighting(
    std::vector<std::pair<const mdk::formats::Arena*, mdk::Lighting>>& cache,
    const mdk::LevelData& level, const mdk::formats::Arena* arena) {
    for (const std::pair<const mdk::formats::Arena*, mdk::Lighting>& e : cache) {
        if (e.first == arena) {
            return e.second;
        }
    }
    cache.push_back(std::make_pair(arena, mdk::enhancedLighting(level.dti(), *arena)));
    return cache.back().second;
}

// The warms' ask (game/object_view.h warmObjectStep, EffectsView::warmStep): a
// hint only, and only for a surface the orig pack holds
// (SurfaceCache::requestPacked) -- any other texture is still made by its first
// draw, as before. The lookup is told it is a hint (HdPackSource::hint: no pixel
// fetch for it).
struct WarmAsk {
    mdk::SurfaceCache* surfaces = nullptr;
    HdPackSource* source = nullptr;
};



static void requestPackedSurface(void* user, const mdk::Surface& surface,
                                 const mdk::formats::Palette* palette) {
    WarmAsk* ask = static_cast<WarmAsk*>(user);
    ask->source->hint = true;
    ask->surfaces->requestPacked(surface, palette);
    ask->source->hint = false;
}

// The effects warm's start for a large sprite (EffectsView::warmStep): ask the
// shared cache for its texture, starting its make -- the pending id when the
// pack holds it, else a sliced expansion.
static void startEffectSurface(void* user, const mdk::Surface& surface,
                               const mdk::formats::Palette* palette) {
    static_cast<WarmAsk*>(user)->surfaces->prepare(surface, palette);
}

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
        // Static, and 2 KB: the toggles' lines and the game's (package #20a).
        static char configText[2048];
        bool read = false;
        if (FILE* config = fopen("sdmc:/3ds/mdk/config.ini", "rb")) {
            const size_t got = fread(configText, 1, sizeof(configText) - 1, config);
            configText[got] = '\0';
            fclose(config);
            read = true;
        }
        if (read) {
            g_gameSettings.parseOver(configText);
        }
        if (read && mdk::parseEnhancedConfig(configText, &enhancedToggles)) {
            mdk::log3dsPrintf("config: sdmc:/3ds/mdk/config.ini loaded (game: master=%d music=%d "
                              "effects=%d filter=%d difficulty=%s gore=%d)",
                              (int)g_gameSettings.masterVolume, (int)g_gameSettings.musicVolume,
                              (int)g_gameSettings.effectsVolume, (int)g_gameSettings.musicFilter,
                              mdk::GameSettings::difficultyName(g_gameSettings.difficulty),
                              (int)g_gameSettings.gore);
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
        if (controls.buttons[mdk::kKeyZoomIn] == 0 || controls.buttons[mdk::kKeyZoomOut] == 0) {
            mdk::log3dsPrintf("controls: the scope's zoom_in/zoom_out are unbound; sniper mode "
                              "stays at its entry zoom");
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
    heapProbe("renderer");

    // ---------------------------------------------------------------------
    // The flow, BEFORE the level: the flow is what decides which level plays
    // (C# Game owns _state.Level and hands it to the Viewer), so the frontend
    // asks it rather than naming a level of its own. It starts at the main menu
    // (package #20a), or in the harness's LEVEL=.
    // ---------------------------------------------------------------------
    // THE ENTRY SCREEN (package #20a, the owner un-cut 2026-11-20): the main menu,
    // as the C#'s game starts (Start.Menu). A harness LEVEL= still starts in that
    // level (Start.Level, the C#'s --level), so every hardware and emulator run
    // that names one is unchanged (menu_screens_3ds.h).
    g_toggles = &enhancedToggles;
    mdk::flow::FlowOptions options;
    options.start = mdk::flow::Start::Menu;
    // THE INTRO1A SPLASH at boot (package #20c): the C#'s default (Program.cs:241,
    // no --menu). A harness build's `SPLASH=0` is the C#'s --menu: the menu alone,
    // so MENU= presses reach it at once.
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

    // The run's frame and step counts and its end: run-scoped, so CAPTURE=, WALK=,
    // the frame budget and the log count across every level the run plays.
    uint32_t frame = 0;
    uint32_t steps = 0;
    const char* exitReason = "aptMainLoop returned false";
    // C# Game._firstLevel: the first level gets the test options (the harness's
    // EVENT= and AT=), later ones do not (Game.cs:487) -- unless EVENT_EVERY=1, the
    // harness's level-to-level chain (package 2).
    bool firstLevel = true;
    heapProbe("run-start");
    // The menus' screens (menu_screens_3ds.h). A run that starts there hands no
    // test option to a level (C# Handle(Menu) clears _firstLevel first).
    mdk::FrontScreens3ds front;
    front.renderer = renderer;
    front.flow = &flow;
    front.settings = &g_gameSettings;
    front.save = saveConfigNow;
    front.controls = &controls;
    front.toggles = &enhancedToggles;
    front.now = nowSeconds;
    front.resumePending = &g_resumePending;
    front.frame = &frame;
    front.frameBudget = g_frameBudget;
    // The main menu's Continue and Saved Game page read the saves (package #19).
    front.saves = &saves;
    if (screen == mdk::flow::Screen::Menu || screen == mdk::flow::Screen::Fall) {
        firstLevel = false;
    }
    // The START menu's OPTIONS row opens the C# PauseMenu's options pages
    // (pause_menu.h's openOptions). Static: it outlives the levels, as the menu
    // and its fonts do.
    static mdk::menu::PauseMenu pauseOptions;
    // THE CHEATS (gap package #21): the START menu's CHEATS row opens this page of
    // toggles, because this target has no keyboard for TOOSCARYFORME and no console for
    // god and noclip. The switches are the session's, as the C#'s DevSession holds god
    // and noclip for every level (Viewer.cs:286-288); gore is the settings' own flag,
    // mirrored when the page opens. Harness builds can start with them on:
    // `make AUTOQUIT=.. GOD=1 NOCLIP=1`.
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
    // level-end hook goes round the loop for it, after the level's teardown) or
    // first, from the harness's STREAM=1. Its own renderer scope and the same two
    // empty frames a level's teardown gets; then its Event is handled, the screens
    // this port still lacks are skipped, and the level that follows loads below.
    if (flow.screen() == mdk::flow::Screen::Stream) {
        const MdkScope streamScope = mdk_render_mark(renderer);
        // C# ShowStream's seed: DefaultSeed for a direct stream, else
        // Environment.TickCount -- the console's tick counter, except in the
        // harness's FIXED=1 runs, whose every stream takes the default seed.
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
            streamFrontend.heapProbe = heapProbe;
            streamFrontend.difficulty = (mdk::Kurt::Difficulty)g_gameSettings.difficulty;
            streamEvent = mdk::runStream3ds(streamFrontend, streamData, streamLevel,
                                            flow.streamHealth(), flow.streamSeed(clockSeed),
                                            &streamEndHealth, &streamExit);
        }
        mdk_render_release(renderer, streamScope);
        mdk::streamBlankFrames3ds(renderer, frame, 2);
        heapProbe("stream-teardown");
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
        // C# Handle clears _firstLevel after the stream's event.
        firstLevel = false;
    }
    // THE MENU AND THE BRIEFING (package #20a), until the flow names a level.
    if (screen != mdk::flow::Screen::Level) {
        screen = mdk::runFrontScreens3ds(&front);
        if (screen != mdk::flow::Screen::Level) {
            exitReason = front.exitReason;
            break;
        }
        heapProbe("menus-done");
    }
    // C# Play: the loading screen's one frame, then the level (package #20c).
    mdk::showLoadingScreen3ds(renderer, flow.state().level, frame);
    // Set when the level is left for the main menu (the START menu's MAIN MENU,
    // or a level that ends into the menu).
    bool toMenu = false;
    mdk::log3dsPrintf("level loop: level %d%s", (int)flow.state().level,
                      firstLevel ? " (the run's first: the harness's test options apply)" : "");
    const MdkScope levelScope = mdk_render_mark(renderer);
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
    OrigByName origByName;
    HdPackSource hdSource;
    // The two packs' side tables for the name index (loadOrigByName): the HD
    // files by content key, the orig files' original sizes. Level-load only.
    std::map<std::string, std::string> hdByKey;
    std::map<std::string, uint32_t> origDims;
    // Every HD file's .t3x size and format (the level's set is sized from it).
    std::map<std::string, uint32_t> hdShapes;
    // THE LEVEL'S RESIDENT HD SET (spec 3.2): its files, the cap their levels are
    // read at, and whether one was chosen (holdHdSet, below).
    std::vector<std::string> hdSetPaths;
    int32_t hdSetCap = 0;
    if (loadHdPack(flow.state().level, &hdPack, &hdByKey, &hdShapes)) {
        hdSource.pack = &hdPack;
    }
    if (loadOrigPack(flow.state().level, &origPack, &origDims)) {
        hdSource.orig = &origPack;
    }
    if (hdSource.pack || hdSource.orig) {
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
            heapProbe("level-data");
            // The name-keyed orig index needs the loaded level (arena name ->
            // palette), so it is built here, not with the pack itself.
            if (hdSource.orig &&
                loadOrigByName(flow.state().level, level, hdByKey, origDims, &origByName)) {
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
                                  (int)flow.state().level, kOrigPackRoot);
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
                    cachedLighting(lightCache, level, a);
                }
                mdk::log3dsPrintf("start: %d views, %d light rigs precomputed",
                                  (int)viewsBuilt, (int)lightCache.size());
                if (const mdk::LevelView* startView = arenaViews.find(startArenaName)) {
                    mdk::log3dsPrintf("view: %d batches, %d vertices, %d triangles",
                                      (int)startView->batchCount(), (int)startView->vertexCount(),
                                      (int)arena->triangleCount());
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
                            // The HUD's messages draw in the same pair (the C#
                            // HudView's fonts are these: FONTBIG, FONTSML, SYS_PAL),
                            // so they share the menu's atlases rather than upload
                            // a second copy; the texts are copied out of the FTI
                            // (a few KB) and the 160 KB file goes with this block.
                            hud.setFonts(&menuBig, &menuSmall);
                            hud.messages().setTexts(menuFti);
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
        // The prefetch's reads go to the renderer's file thread (above). The
        // renderer outlives every level's bank, and the bank cancels its read in
        // flight when it goes (the level's teardown). No thread (it failed to
        // start): the bank reads its steps on the frame, as before.
        if (mdk_render_has_file_reader(renderer)) {
            mdk::AsyncRead musicReader;
            musicReader.requestRange = musicReadRequest;
            musicReader.poll = musicReadPoll;
            musicReader.cancel = musicReadCancel;
            musicReader.user = renderer;
            bank.setReader(musicReader);
        }
        mdk::log3dsPrintf("audio: bank ready, %d archives (%d bytes resident; the music "
                          "archive is read by entry), decoded on demand, budget %d bytes",
                          (int)bank.archiveCount(), (int)bank.archiveBytes(),
                          (int)bank.decodedBudgetBytes());
    } else {
        mdk::log3dsPrintf("audio: the level's SNI archives did not load -- no sounds");
    }
    heapProbe("post-audio");
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
        heapProbe("post-level-animations");
    }
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
    // Sniper mode's view besides the world through the scope: the overlay's state
    // and the clip's inset (game/sniper_view.h; no round-camera insets here).
    mdk::SniperView sniperView;
    bool sniperClipFailureLogged = false;
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
    // The arena whose track the bank was last asked to prefetch (below).
    std::string musicPrefetchedFor;
    // The bubble arena whose track was last prefetched ahead.
    std::string musicBubbleFor;
    // The second arenas whose first effects-warm step was already
    // deferred once (per name; see the frame's warm).
    std::vector<std::string> warmDeferred;
    // The object warm's place per arena (warmObjectStep's cursor), and the
    // surface cache's palette-release count when the cursors were last checked
    // against it (see the frame's object warm).
    std::map<std::string, size_t> objectWarmAt;
    int32_t objectWarmReleases = 0;
    WarmAsk warmAsk;
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
    // read whole, wherever the asker is -- the music's enter(), a script's play
    // in the middle of its tick, a track the budget evicted. The ones the
    // music's enter() makes are timed and said there; the frame says the rest.
    int32_t audioStallsSaid = 0;
    int32_t audioDemandSaid = 0;
    int64_t audioDemandBytesSaid = 0;
    // The prefetch in flight: its steps' worst time, for the line that ends it.
    double prefetchWorstMs = 0.0;
    // The bank's counters when it began, for the same line.
    mdk::audio::SoundBank::Stats prefetchStart;
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
    g_gameSettings.applyAudio(&mixer.device());
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
        player.combat().setDifficulty((mdk::Kurt::Difficulty)g_gameSettings.difficulty);
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
            runtime->option = g_gameSettings.gore ? 1 : 0;
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
            // the C#'s camera Random is time-seeded, and a WALK harness build replays
            // its fixed seed.
            runtime->setShakeRaisedHook(&mdk::FollowCamera::raiseShakeHook, &follow);
            follow.seedShake(seed);
            player.setMixer(&runtime->mixer());
            mix = &runtime->mixer();
            g_gameSettings.applyAudio(&mix->device());
            // The new mixer owns a new SoundDevice, so the output has to be handed
            // to it: a respawn without this would be a silent level (and, on the
            // DSP path, a device mixing samples nobody hears).
            mix->device().setSampleMix(!mdk::kDspAudio);
            if (audioReady) {
                mix->device().setSink(mdk::kDspAudio ? nullptr : mdk::audioSink, nullptr);
            }
            mdk::log3dsPrintf("scripts: runtime ready (seed %d, option %d)", (int)seed,
                              (int)runtime->option);
            heapProbe("runtime");
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
        const double t0 = nowSeconds();
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
        const double t1 = nowSeconds();
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
    std::vector<const char*> hdSetArgs;
    auto holdHdSet = [&](const char* why) {
        // The hold: the backend reads and imports while this thread waits; the
        // applet loop is served throughout (HOME, sleep, power).
        const double hold0 = nowSeconds();
        int32_t made = 0;
        int32_t pending = mdk_render_hd_set_pending(renderer);
        // A wait that makes nothing for a second (no file read or imported, the
        // count unchanged: what is left cannot be read) ends the hold, as does a
        // minute in all, or the applet loop saying stop.
        int32_t idle = 0;
        while (pending > 0 && idle < 20 && nowSeconds() - hold0 < 60.0) {
            if (!aptMainLoop()) {
                break;
            }
            const int32_t slice = mdk_render_settle_streams(renderer, 50);
            const int32_t before = pending;
            made += slice;
            pending = mdk_render_hd_set_pending(renderer);
            if (slice == 0 && pending == before) {
                idle++;
                svcSleepThread(50000000LL);
            } else {
                idle = 0;
            }
        }
        made += mdk_render_settle_streams(renderer, 2000);   // the packed originals asked
        MdkC3dFirstUse state;
        mdk_render_c3d_first_use(renderer, &state);
        mdk::log3dsPrintf("hd: set %s: %d of %d resident (%u bytes, cap %d), %d given up, "
                          "%d files made in %.0f ms; linear free %u",
                          why, (int)state.hdPinnedResident, (int)state.hdPinned,
                          (unsigned)state.hdPinnedBytes, (int)state.hdTopCap,
                          (int)state.hdPinFailed, (int)made, (nowSeconds() - hold0) * 1000.0,
                          (unsigned)linearSpaceFree());
    };
    // The last level's copies are given back first, so the heap read below is
    // this level's (mdk_render_pin_hd_set frees what nothing links any more).
    mdk_render_pin_hd_set(renderer, nullptr, nullptr, 0);
    if (hdSource.pack && hdSource.byname) {
        static const uint32_t kHdPlayReserve = 7u * 1024u * 1024u;
        std::set<std::string> unique;
        for (std::map<std::pair<const void*, std::string>, std::string>::const_iterator it =
                 origByName.hdPaths.begin();
             it != origByName.hdPaths.end(); ++it) {
            unique.insert(it->second);
        }
        // Each file's bytes at the three caps, in the set's (sorted) order.
        hdSetPaths.assign(unique.begin(), unique.end());
        std::vector<uint32_t> at512(hdSetPaths.size()), at256(hdSetPaths.size()),
            at128(hdSetPaths.size());
        uint32_t bytes512 = 0;
        uint32_t bytes256 = 0;
        uint32_t bytes128 = 0;
        int32_t unsized = 0;
        for (size_t f = 0; f < hdSetPaths.size(); f++) {
            const std::map<std::string, uint32_t>::const_iterator shape =
                hdShapes.find(hdSetPaths[f]);
            if (shape == hdShapes.end()) {
                unsized++;
                continue;
            }
            at512[f] = hdChainBytes(shape->second, 0);
            at256[f] = hdChainBytes(shape->second, 256);
            at128[f] = hdChainBytes(shape->second, 128);
            bytes512 += at512[f];
            bytes256 += at256[f];
            bytes128 += at128[f];
        }
        const uint32_t freeNow = (uint32_t)linearSpaceFree();
        const uint32_t room = freeNow > kHdPlayReserve ? freeNow - kHdPlayReserve : 0u;
        // The tier. 512 or 256 for the whole set when it fits; else the set at
        // 256 with the files that save the most taken down to 128, one at a
        // time, until it fits (the level's largest textures lose a level, the
        // rest keep 256); else the stream per surface.
        std::vector<int32_t> caps(hdSetPaths.size(), 0);
        int32_t at128Count = 0;
        uint32_t total = bytes512;
        if (unsized == 0 && bytes512 <= room) {
            hdSetCap = 512;
        } else if (unsized == 0 && bytes256 <= room) {
            hdSetCap = 256;
            std::fill(caps.begin(), caps.end(), 256);
            total = bytes256;
        } else if (unsized == 0 && bytes128 <= room) {
            hdSetCap = 128;
            std::fill(caps.begin(), caps.end(), 256);
            total = bytes256;
            std::vector<std::pair<uint32_t, size_t> > savings;
            for (size_t f = 0; f < hdSetPaths.size(); f++) {
                savings.push_back(std::make_pair(at256[f] - at128[f], f));
            }
            std::sort(savings.begin(), savings.end());
            for (size_t k = savings.size(); k > 0 && total > room; k--) {
                const size_t f = savings[k - 1].second;
                total -= savings[k - 1].first;
                caps[f] = 128;
                at128Count++;
            }
        } else {
            hdSetCap = 0;
            hdSetPaths.clear();
        }
        mdk::log3dsPrintf("hd: level %d set %d files: %u bytes at 512, %u at 256, %u at 128 "
                          "(%d unsized); linear free %u, room %u above the %u reserve: %s, "
                          "%u bytes, %d files at 128",
                          (int)flow.state().level, (int)unique.size(), (unsigned)bytes512,
                          (unsigned)bytes256, (unsigned)bytes128, (int)unsized,
                          (unsigned)freeNow, (unsigned)room, (unsigned)kHdPlayReserve,
                          hdSetCap == 512   ? "tier 512, the whole set"
                          : hdSetCap == 256 ? "tier 256, the whole set, levels over 256 dropped"
                          : hdSetCap == 128 ? "tier 256/128, the largest files down to 128"
                                            : "DOES NOT FIT: streamed per surface",
                          (unsigned)(hdSetCap > 0 ? total : 0u), (int)at128Count);
        if (hdSetCap > 0) {
            for (size_t p = 0; p < hdSetPaths.size(); p++) {
                hdSetArgs.push_back(hdSetPaths[p].c_str());
            }
            mdk_render_pin_hd_set(renderer, hdSetArgs.data(), caps.data(),
                                  (int32_t)hdSetArgs.size());
        }
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
                                        requestPackedSurface, &warmAsk, &cursor)) {
            }
        }
        holdHdSet("loading screen");
    }
    heapProbe("loop");
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
                    g_gameSettings.gore = cheatSwitches.gore;
                    if (runtime) {
                        runtime->option = g_gameSettings.gore ? 1 : 0;
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
                        holdHdSet("toggled on");
                    }
                    const bool saved = saveEnhancedConfig(enhancedToggles);
                    mdk::log3dsPrintf("menu: shadows=%d lights=%d haze=%d mirrors=%d "
                                      "glow=%d filtering=%d mipmaps=%d hd=%d lighting=%d smoothing=%d "
                                      "cap30=%d hazeDensity=%d glowStrength=%d shadowDarkness=%d "
                                      "exposure=%d tintSaturation=%d shadowMapSize=%d "
                                      "shadowMapRefresh=%d (saved=%d)",
                                      (int)enhancedToggles.enableSkyShadows,
                                      (int)enhancedToggles.enableFragmentLights,
                                      (int)enhancedToggles.enableHaze,
                                      (int)enhancedToggles.enableMirrors,
                                      (int)enhancedToggles.enableGlowBillboards,
                                      (int)enhancedToggles.enableFiltering,
                                      (int)enhancedToggles.enableMipmaps,
                                      (int)enhancedToggles.enableHdTextures,
                                      (int)enhancedToggles.enableLighting,
                                      (int)enhancedToggles.enableEdgeSmoothing,
                                      (int)enhancedToggles.cap30Fps,
                                      (int)enhancedToggles.hazeDensity,
                                      (int)enhancedToggles.glowStrength,
                                      (int)enhancedToggles.shadowDarkness,
                                      (int)enhancedToggles.exposure,
                                      (int)enhancedToggles.tintSaturation,
                                      (int)enhancedToggles.shadowMapSize,
                                      (int)enhancedToggles.shadowMapRefresh, (int)saved);
                } else if (action == mdk::EnhancedMenu::Action::Quit) {
                    menuQuit = true;
                    mdk::log3dsPrintf("menu: QUIT chosen");
                } else if (action == mdk::EnhancedMenu::Action::MainMenu) {
                    menuMainMenu = true;
                    mdk::log3dsPrintf("menu: MAIN MENU chosen");
                } else if (action == mdk::EnhancedMenu::Action::Cheats) {
                    cheatSwitches.gore = g_gameSettings.gore;
                    cheatsPage.setOpen(true);
                    mdk::log3dsPrintf("menu: cheats opened");
                } else if (action == mdk::EnhancedMenu::Action::Options) {
                    // The C# options pages, over the paused game, in the fonts the
                    // START menu draws with; their changes apply and save at once.
                    mdk::menu::MenuItems& items = pauseOptions.items();
                    items.setFonts(&menuBig.font(), &menuSmall.font());
                    items.setHost(mdk::menu::Host::Console);
                    struct Applied {
                        static void changed(void* user) {
                            mdk::audio::SoundMixer* const* live =
                                static_cast<mdk::audio::SoundMixer* const*>(user);
                            if (*live) {
                                g_gameSettings.applyAudio(&(*live)->device());
                            }
                            const bool saved = saveConfigNow();
                            mdk::log3dsPrintf("menu: options changed: master=%d music=%d "
                                              "effects=%d filter=%d difficulty=%s gore=%d "
                                              "(saved=%d)",
                                              (int)g_gameSettings.masterVolume,
                                              (int)g_gameSettings.musicVolume,
                                              (int)g_gameSettings.effectsVolume,
                                              (int)g_gameSettings.musicFilter,
                                              mdk::GameSettings::difficultyName(
                                                  g_gameSettings.difficulty),
                                              (int)g_gameSettings.gore, (int)saved);
                        }
                        static void print(void*, const char* line) {
                            mdk::log3dsPrintf("%s", line);
                        }
                    };
                    items.setSettings(&g_gameSettings, Applied::changed, &mix);
                    items.setPrint(Applied::print, nullptr);
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
                                          "%d bytes left to read, read and the whole track "
                                          "decoded in %.1f ms",
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
        // The `audio` profile span is all of it, in both modes (PROFILE builds).
        const uint64_t spanAudio0 = mdk::harnessProfileTick();
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
                const double now = nowSeconds();
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
                    bubblePrevTime = nowSeconds();
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
            const double stream1 = nowSeconds();
            const int32_t builtViews =
                arenaViews.sync(streamCurrent, streamSecond, *frameDrawn, bubbleHints);
            const double stream2 = nowSeconds();
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
            effectsView.warmStep(streamCurrent, requestPackedSurface, startEffectSurface,
                                 &warmAsk);
            // The bubble's arenas warm too: they are approached, not yet
            // resident, and their lead is the whole point (no deferral --
            // the bubble IS the early start).
            for (size_t i = 0; i < bubbleHints.size(); i++) {
                if (bubbleHints[i] != streamCurrent && bubbleHints[i] != streamSecond) {
                    effectsView.warmStep(bubbleHints[i], requestPackedSurface,
                                         startEffectSurface, &warmAsk);
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
                    effectsView.warmStep(streamSecond, requestPackedSurface,
                                         startEffectSurface, &warmAsk);
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
                                        objectResolver, requestPackedSurface, &warmAsk,
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
            // arena loads (arena_load -> 0x4192c4) and reads a step of it a frame
            // (game_frame -> 0x4193a4), so the track is in memory when Kurt walks
            // in. Here the arena that is loading is the pair's OTHER one: the
            // second arena, which is where a door or a corridor leads. Asked once
            // per change of that arena; the bank steps it once per frame: a read
            // step asked of the file thread or taken back from it (the frame only
            // appends the landed bytes), then the decode a bounded slice a frame
            // (SoundBank::kDecodeStepSamples). No frame reads, and none decodes a
            // whole track: those are the last resort, a track wanted before its
            // prefetch is in (counted, and said below).
            bool musicStartedHere = false;
            if (music) {
                // The bubble's nearest arena's track too, when no
                // prefetch is in flight: it reads a step a frame like
                // the second's, so the doorway's music is resident with
                // its view. The second arena's own start below drops a
                // bubble staging it no longer needs (prefetch replaces
                // the staging), so there is no double read.
                if (!bubbleHints.empty() && bank.prefetching().empty() &&
                    bubbleHints[0] != streamSecond && bubbleHints[0] != musicBubbleFor) {
                    const std::string track = music->trackOf(bubbleHints[0]);
                    prefetchStart = bank.stats();
                    if (bank.prefetch(track)) {
                        prefetchWorstMs = 0.0;
                        musicBubbleFor = bubbleHints[0];
                        musicStartedHere = true;
                        mdk::log3dsPrintf("music: prefetch %s for %s (bubble)", track.c_str(),
                                          bubbleHints[0].c_str());
                    }
                }
                if (streamSecond != musicPrefetchedFor) {
                    musicPrefetchedFor = streamSecond;
                    if (!streamSecond.empty()) {
                        const std::string track = music->trackOf(streamSecond);
                        const mdk::audio::SoundBank::Stats before = bank.stats();
                        if (bank.prefetch(track)) {
                            prefetchStart = before;
                            prefetchWorstMs = 0.0;
                            musicStartedHere = true;
                            // The second's prefetch replaces the staging:
                            // a bubble prefetch it superseded may be
                            // issued again while its arena stays nearest.
                            if (musicBubbleFor != streamSecond) {
                                musicBubbleFor.clear();
                            }
                            mdk::log3dsPrintf("music: prefetch %s for %s", track.c_str(),
                                              streamSecond.c_str());
                        }
                    }
                }
                // Not the change frame's own step: the arena-activation frame
                // already carries the view build and the warm, and the track is
                // wanted at the door, frames later -- registering here and
                // stepping from the next frame loses nothing.
                if (!bank.prefetching().empty() && !musicStartedHere) {
                    const std::string reading = bank.prefetching();
                    const int32_t failuresBefore = bank.stats().readFailures;
                    // Every step is timed: a landed read's append (up to 64 KiB
                    // copied) or a decode slice (32768 samples) -- both bounded,
                    // and the line that ends the prefetch says the worst.
                    const double step0 = nowSeconds();
                    const bool more = bank.prefetchStep();
                    const double stepMs = (nowSeconds() - step0) * 1000.0;
                    if (stepMs > prefetchWorstMs) {
                        prefetchWorstMs = stepMs;
                    }
                    if (!more && bank.prefetching().empty()) {
                        const mdk::audio::SoundBank::Stats& now = bank.stats();
                        if (now.readFailures != failuresBefore) {
                            mdk::log3dsPrintf("music: prefetch of %s FAILED (%d read failures); "
                                              "it is read when it is played",
                                              reading.c_str(), (int)now.readFailures);
                        } else {
                            mdk::log3dsPrintf("music: %s in (%d read steps, %d of them on the "
                                              "reader thread, %d decode slices; worst step "
                                              "%.1f ms; %d bytes decoded in the bank, %d "
                                              "evicted)",
                                              reading.c_str(),
                                              (int)(now.prefetchSteps - prefetchStart.prefetchSteps),
                                              (int)(now.prefetchReads - prefetchStart.prefetchReads),
                                              (int)(now.decodeSteps - prefetchStart.decodeSteps),
                                              prefetchWorstMs, (int)bank.decodedBytes(),
                                              (int)now.evictions);
                        }
                        prefetchWorstMs = 0.0;
                    }
                }
                // AND THE MUSIC OF ARENAS THAT ARE GONE IS GIVEN BACK (the spec's
                // "free the decoded samples of an arena when it is deactivated"):
                // once a second, every decoded track but the one playing, the one
                // fading out (both pinned by their voices) and the loading arena's.
                // A level played end to end then holds three tracks, not all nine.
                // HOUSEKEEPING, NOT A LOG LINE: it runs in every build (it was
                // under log3dsPeriodic(), so release and PROFILE builds never gave
                // a track back nor re-asked an evicted one). Its cost is a walk of
                // the decoded names (tens) against three; its lines are events
                // (something was freed or re-asked), not periodic.
                if ((frame % 60u) == 0u) {
                    // The bubble's prefetched track is kept too: it is the
                    // door ahead's music, and freeing it within the second
                    // made the door read and decode it again (review M2).
                    const std::string keep[4] = {
                        music->playing(), music->fading(),
                        streamSecond.empty() ? std::string() : music->trackOf(streamSecond),
                        musicBubbleFor.empty() ? std::string() : music->trackOf(musicBubbleFor)};
                    const size_t freed = bank.releaseMusicExcept(keep, 4);
                    if (freed != 0) {
                        mdk::log3dsPrintf("music: released %d bytes of tracks no arena in "
                                          "memory plays (%d decoded bytes left)",
                                          (int)freed, (int)bank.decodedBytes());
                    }
                    // And the loading arena's track is asked for AGAIN: nothing if
                    // it is decoded or being read, a new prefetch if the budget
                    // evicted it since (the budget does not know which track is
                    // waiting for Kurt; this does).
                    const mdk::audio::SoundBank::Stats before = bank.stats();
                    if (!keep[2].empty() && bank.prefetch(keep[2])) {
                        prefetchStart = before;
                        prefetchWorstMs = 0.0;
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
        mdk_render_set_camera(renderer, &sceneCamera);

        // The enhanced look's light (LevelLight.Update): the scripts' current
        // arena's rig, or the start arena's before the runtime exists. The Linux
        // build rebuilds it every frame; here it is rebuilt only when the arena
        // CHANGES, because enhancedLighting walks the sky panorama and every
        // triangle of the arena -- level-load work, not frame work, on an ARM11.
        // The rig carries the haze's colour and density (SPEC.md 7.9).
        uint64_t tRig0 = 0, tRig1 = 0;
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
                tRig0 = mdk::harnessProfileTick();
                const mdk::Lighting rig = cachedLighting(lightCache, level, lightArena);
                tRig1 = mdk::harnessProfileTick();
                mdk_render_set_lighting(renderer, &rig);
                mdk::log3dsPrintf("light: arena %s haze rgb=(%.3f,%.3f,%.3f) density=%.5f",
                                  lightArena->name().c_str(), rig.haze_colour.r,
                                  rig.haze_colour.g, rig.haze_colour.b, rig.haze_density);
            }
        }
        const uint64_t tLight = mdk::harnessProfileTick();

        // The sky first (see SkyRange): centred on the camera, DoubleSided -- only when
        // the scripts' sky mode shows it (C# `Backdrop == Backdrop.Sky`).
        if (skyMesh >= 0 && params.backdrop == mdk::Backdrop::Sky) {
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
                sky.world[12] = sceneCamera.position.x;
                sky.world[13] = sceneCamera.position.y;
                sky.world[14] = sceneCamera.position.z;
                mdk_render_draw_mesh(renderer, &sky);
            }
        }
        const uint64_t tSky = mdk::harnessProfileTick();

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
        const uint64_t tArena = mdk::harnessProfileTick();
        uint64_t tLay = tArena;
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
                                          g_gameSettings.stereoSeparation * 0.5f);
            } else {
                objectView.setCullFrustum(nullptr, 0.0f);
            }
            objectStream.reset();
            for (const mdk::scripts::MdkObject& obj : runtime->objects()) {
                if (strikeFrame) {
                    break;  // the strike's frame shows its model alone (below)
                }
                if (!obj.visible || runtime->isRemoved(&obj)) {
                    continue;
                }
                const bool carried = runtime->rides().bomber() && &obj == runtime->rides().ridden();
                const float alpha =
                    &obj == runtime->rides().driven() || carried ? 1.0f : tickAlpha;
                objectView.addObject(obj, objectLook(obj.arena), obj.transform_at(alpha),
                                     &objectStream);
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
            const uint64_t tLay = mdk::harnessProfileTick();
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
        const uint64_t tObj = mdk::harnessProfileTick();
        if (runtime && !strikeFrame) {
            // The pieces and sprite effects, where the C# draws them:
            // after the objects, before Kurt (Viewer.cs:707). The ropes
            // and sniper scope the C# draws alongside are not ported.
            // A refusal is reported like the object pass's, edge-triggered
            // and re-armed, but on this pass's own latch.
            effectsView.build(*runtime, sceneCamera, &effectsFrame);
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
        const uint64_t tFx = mdk::harnessProfileTick();
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
        const uint64_t tKurt = mdk::harnessProfileTick();
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
        const uint64_t spanEnd0 = mdk::harnessProfileTick();
        mdk_render_end_frame(renderer);
        if (frame > 0) {
            mdk::harnessProfileDrawSub(tLight - spanDraw0, tRig1 - tRig0, tSky - tLight,
                                       tArena - tSky, tLay - tArena, tObj - tLay,
                                       tFx - tObj, tKurt - tFx, spanEnd0 - tKurt);
            mdk::harnessProfileSpans(spanSim, steps - spanSteps0, spanDraw0 - spanBegin0,
                                     spanEnd0 - spanDraw0,
                                     mdk::harnessProfileTick() - spanEnd0, spanAudio);
        }
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
        const double settleStart = nowSeconds();
        while (!kurtSprite.settleStream()) {
            if (nowSeconds() - settleStart > 2.0) {
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
    music.reset();
    if (mdk::kDspAudio) {
        mdk::audioDspClose();
    } else {
        mdk::audioClose();
    }
    // THE HD RULE'S PROOF, once per level (docs/HD_TEXTURES_3DS_SPEC.md 3.1):
    // the lookup's content hashes, the HD answers made from SD anyway, the SD
    // copies made for a texture with a linked HD copy while the toggle was on
    // (all three must be 0 with the toggle on), and the HD copies that were late.
    {
        MdkC3dFirstUse made;
        mdk_render_c3d_first_use(renderer, &made);
        mdk::log3dsPrintf("hdrule: level %d hd=%d pack=%d nameMisses=%d (first %s) "
                          "sizeRefused=%d hdAnimated=%d",
                          (int)flow.state().level, (int)enhancedToggles.enableHdTextures,
                          hdSource.pack ? 1 : 0, (int)g_lookupNameMisses,
                          g_lookupNameMisses > 0 ? g_lookupNameMissFirst : "-",
                          (int)g_lookupSizeRefused, (int)g_lookupHdAnimated);
        mdk::log3dsPrintf("hdrule: level %d hd=%d pack=%d hashes=%d (%.1f ms, first %s) "
                          "hdAnsweredSd=%d coveredSdMade=%d lateTextures=%d lateDraws=%d "
                          "blankDraws=%d hdImported=%d hdEvicted=%d residentPeak=%u "
                          "(backend counts since boot)",
                          (int)flow.state().level, (int)enhancedToggles.enableHdTextures,
                          hdSource.pack ? 1 : 0, (int)g_lookupHashes, g_lookupHashMs,
                          g_lookupHashes > 0 ? g_lookupHashFirst : "-",
                          (int)surfaces.sourceStats().hdAnsweredSd, (int)made.hdCoveredSdMade,
                          (int)made.hdLateTextures, (int)made.hdLateDraws,
                          (int)made.hdBlankDraws, (int)made.hdImported, (int)made.hdEvicted,
                          (unsigned)made.hdResidentPeak);
        // The late copies: how many landed after a draw wanted them, their frames
        // late (sum, worst with its file and wanted frame), the first eight.
        char rows[8 * 16];
        size_t at = 0;
        rows[0] = '\0';
        for (int32_t i = 0; i < made.hdLateLanded && i < 8 && at < sizeof(rows); i++) {
            at += (size_t)snprintf(rows + at, sizeof(rows) - at, "%s%d+%d", i ? "," : "",
                                   (int)made.hdLateRows[i][0], (int)made.hdLateRows[i][1]);
        }
        mdk::log3dsPrintf("hdrule: late landed=%d frames=%d worst=%d (wanted frame %d, %s) "
                          "first=%s",
                          (int)made.hdLateLanded, (int)made.hdLateFramesSum,
                          (int)made.hdLateFramesMax, (int)made.hdLateWorstFrame,
                          made.hdLateWorst[0] ? made.hdLateWorst : "-", rows[0] ? rows : "-");
        mdk::log3dsPrintf("hdrule: set cap=%d pinned=%d resident=%d bytes=%u givenUp=%d "
                          "stampsSkipped=%d linkedAfterSd=%d",
                          (int)made.hdTopCap, (int)made.hdPinned, (int)made.hdPinnedResident,
                          (unsigned)made.hdPinnedBytes, (int)made.hdPinFailed,
                          (int)made.hdStampsSkipped, (int)made.hdLinkedAfterSd);
        mdk::log3dsPrintf("hdrule: margin firsts=%d min=%d frames (at frame %d, %s) under30=%d",
                          (int)made.hdSlackFirsts, (int)made.hdSlackMin, (int)made.hdSlackMinFrame,
                          made.hdSlackWorst[0] ? made.hdSlackWorst : "-",
                          (int)made.hdSlackUnder30);
        g_lookupHashes = 0;
        g_lookupHashMs = 0.0;
        g_lookupNameMisses = 0;
        g_lookupSizeRefused = 0;
        g_lookupHdAnimated = 0;
    }
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
        heapProbe("level-teardown");
        firstLevel = false;
        continue;
    }
    break;
    }   // END OF THE LEVEL LOOP

    printf("[3ds] exiting after %lu frames\n", (unsigned long)frame);

    mdk::harnessProfileReport();
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

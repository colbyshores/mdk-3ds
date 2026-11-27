// ============================================================================
// level_load_3ds.cpp -- see level_load_3ds.h.
// ============================================================================

#include "level_load_3ds.h"

#include <3ds.h>
#include <stdio.h>

#include <memory>

#include "formats/case_insensitive_path.h"
#include "formats/file_system.h"
#include "formats/font.h"
#include "formats/fti.h"
#include "platform/log_3ds.h"
#include "render/mirror_cube.h"
#include "render/render_c3d_mirror.h"
#include "render/sky_dome.h"

namespace mdk {

// The light rig for one arena, computed once and kept in `cache` (main's
// lightCache). enhancedLighting scans the arena's faces -- ~113 ms per arena
// on the Old 3DS -- and its answer depends on nothing but (dti, arena), both
// fixed for the level's life. The frame's light code and the stream pump both
// come through here, so the scan runs at most once per arena.
mdk::Lighting cachedLighting(
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

// THE SKY'S ETC1 SLICE (HD on): the build's copy of slice `k`
// (mdk_3ds/tools/arena_etc1_convert.py convert_skies: the panorama de-dithered,
// cut exactly as buildSkyDome cuts it, ETC1 with no mips), read whole at level
// load. -1 when it is not there or does not import: the caller expands the
// slice's indices as with HD off. 256 KiB of linear a slice, against 1 MiB for
// the 16-bit expansion, and no 8-bit dither in the gradients.
static MdkTextureId skySliceEtc1(MdkRenderer renderer, int32_t level, int32_t k, int32_t w,
                                 int32_t h) {
    const formats::FileSystem* fs = formats::fileSystem();
    if (!fs || !fs->readFile) {
        return -1;
    }
    char path[64];
    snprintf(path, sizeof(path), "romfs:/orig/sky/level%d_%d.t3x", (int)level, (int)k);
    std::vector<uint8_t> bytes;
    if (!fs->readFile(fs->user, path, &bytes) || bytes.empty()) {
        return -1;
    }
    return mdk_render_create_compressed_texture(renderer, w, h, bytes.data(),
                                                (int32_t)bytes.size());
}

// THE SKY, as geometry (render/sky_dome.h): the level's panorama as a sphere
// around the camera in slices, its two caps flat colours. Made at level load
// (and again when the HD toggle changes) into `skyMesh` and `ranges` (at most
// 8); returns how many ranges. With `hd` a slice is its ETC1 copy and its
// indices are never expanded; without, the original 16-bit expansion as before.
int32_t buildSky3ds(MdkRenderer renderer, const formats::Dti& dti, int32_t level, bool hd,
                    MdkMeshId* skyMesh, SkyRange* ranges) {
    int32_t count = 0;
    int32_t etc1 = 0;
    const uint8_t* pal = dti.palette().rgba();
    mdk::SkyDome dome;
    if (mdk::buildSkyDome(dti.sky(), pal, dti.skyWrapWidth(), dti.skyHorizonRow(),
                          dti.skyOffset(), dti.skyTopColor(),
                          dti.skyBottomColor(), &dome)) {
        *skyMesh = mdk_render_create_mesh(renderer, dome.vertices.data(),
                                         (int32_t)dome.vertices.size());
        for (const mdk::SkySlice& slice : dome.slices) {
            if (count >= 6) break;
            SkyRange& r = ranges[count];
            r.texture = hd ? skySliceEtc1(renderer, level, count, slice.storedWidth,
                                          dome.height)
                           : -1;
            if (r.texture >= 0) {
                etc1++;
            } else {
                r.texture = mdk_render_create_index_texture(
                    renderer, slice.storedWidth, dome.height, slice.indices.data(), pal);
                // Drawn Original-shaded only (the PC's sky has no chain either).
                mdk_render_mark_unmipped(renderer, r.texture);
            }
            count++;
            r.first = slice.first;
            r.count = slice.count;
        }
        const uint8_t top[4] = {dome.topRgb[0], dome.topRgb[1], dome.topRgb[2], 255};
        const uint8_t bottom[4] = {dome.bottomRgb[0], dome.bottomRgb[1],
                                   dome.bottomRgb[2], 255};
        SkyRange& t = ranges[count++];
        t.texture = mdk_render_create_rgba_texture(renderer, 1, 1, top);
        t.first = dome.topFirst;
        t.count = dome.topCount;
        SkyRange& b = ranges[count++];
        b.texture = mdk_render_create_rgba_texture(renderer, 1, 1, bottom);
        b.first = dome.bottomFirst;
        b.count = dome.bottomCount;
        mdk::log3dsPrintf("sky: %dx%d wrap=%d horizon=%d offset=%d -> %d slices, "
                          "%d vertices, mesh=%d, index0 remapped %d texels to %d (d2=%d), "
                          "%d ETC1 slices (hd=%d)",
                          (int)dti.sky().width(), (int)dti.sky().height(),
                          (int)dti.skyWrapWidth(), (int)dti.skyHorizonRow(),
                          (int)dti.skyOffset(), (int)dome.slices.size(),
                          (int)dome.vertices.size(), (int)*skyMesh,
                          (int)dome.remappedTexels, (int)dome.remapTarget,
                          (int)dome.remapDistance, (int)etc1, (int)hd);
    } else {
        mdk::log3dsPrintf("sky: none (the level's panorama cannot be built)");
    }
    // The dome's CPU copies die here: the renderer keeps its own.
    return count;
}

int32_t buildMirrorCubes3ds(MdkRenderer renderer, const LevelData& level) {
    int32_t shifts[kMirrorRowShiftsMax];
    const int32_t count = mirrorRowShifts(level.arenas(), shifts);
    if (count == 0) {
        return 0;
    }
    const formats::Dti& dti = level.dti();
    // The panorama and the palette the oracle binds (Viewer.CreatePanorama): the
    // DTI's mirror panorama (its second one in LEVEL5/6, the sky elsewhere)
    // through the level's base palette.
    const formats::Texture& panorama = dti.mirrorSky();
    const uint8_t* pal = dti.palette().rgba();
    if (!panorama.indices() || !pal || dti.skyWrapWidth() <= 0 ||
        panorama.width() < dti.skyWrapWidth()) {
        mdk::log3dsPrintf("mirror: %d row shifts but no usable panorama -- mirrors skipped",
                          (int)count);
        return 0;
    }
    const uint64_t start = svcGetSystemTick();
    int32_t slots[kMirrorRowShiftsMax];
    int32_t made = 0;
    for (int32_t i = 0; i < count; i++) {
        slots[i] = mdk_render_create_mirror_cube(renderer, shifts[i]);
        if (slots[i] >= 0) {
            made++;
        }
    }
    if (made > 0) {
        // One face at a time: its cells (the direction's panorama column and row,
        // the expensive part) once, then each shift's indices from them.
        const int32_t texels = kMirrorCubeSize * kMirrorCubeSize;
        std::vector<int16_t> columns((size_t)texels);
        std::vector<int16_t> rows((size_t)texels);
        std::vector<uint8_t> face((size_t)texels);
        for (int32_t f = 0; f < kMirrorCubeFaces; f++) {
            mirrorCubeCells(f, kMirrorCubeSize, dti.skyWrapWidth(), dti.skyHorizonRow(),
                            dti.skyOffset(), panorama.height(), columns.data(), rows.data());
            for (int32_t i = 0; i < count; i++) {
                if (slots[i] >= 0 && mirrorCubeFace(panorama, columns.data(), rows.data(),
                                                    texels, shifts[i], face.data())) {
                    mdk_render_set_mirror_cube_face(renderer, slots[i], f, face.data(), pal);
                }
            }
        }
    }
    char list[kMirrorRowShiftsMax * 6 + 1];
    int32_t at = 0;
    list[0] = '\0';
    for (int32_t i = 0; i < count && at < (int32_t)sizeof(list) - 6; i++) {
        at += snprintf(list + at, sizeof(list) - (size_t)at, "%s%d", i ? "," : "", (int)shifts[i]);
    }
    mdk::log3dsPrintf("mirror: %d of %d cubes (row shifts %s; %s panorama), %u KiB, %.1f ms",
                      (int)made, (int)count, list, dti.hasOwnMirrorSky() ? "its own" : "the sky's",
                      (unsigned)(mdk_render_mirror_cube_bytes(renderer) / 1024u),
                      (double)(svcGetSystemTick() - start) / (double)(SYSCLOCK_ARM11 / 1000ull));
    return made;
}

void releaseSky3ds(MdkRenderer renderer, MdkMeshId* skyMesh, SkyRange* ranges, int32_t* count) {
    for (int32_t i = 0; i < *count; i++) {
        mdk_render_release_texture(renderer, ranges[i].texture);
        ranges[i] = SkyRange();
    }
    *count = 0;
    if (*skyMesh >= 0) {
        mdk_render_release_mesh(renderer, *skyMesh);
        *skyMesh = -1;
    }
}

// THE PAUSE MENU'S FONTS: MDKFONT.FTI's FONTBIG and FONTSML in its SYS_PAL
// colours, the oracle's Fonts pair (Ui.cs), uploaded into `menuBig` and
// `menuSmall`; the HUD's messages share them. False (and logged) when they do
// not load: there is then no pause menu and the game runs unchanged.
bool loadMenuFonts3ds(MdkRenderer renderer, const formats::MdkData& data, FontView* menuBig,
                      FontView* menuSmall, HudView* hud) {
    bool ready = false;
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
        if (palette >= 0 && menuBig->load(renderer, *bigFont, palette, sysPalette.rgba()) &&
            menuSmall->load(renderer, *smallFont, palette, sysPalette.rgba())) {
            ready = true;
            mdk::log3dsPrintf("menu: pause menu ready (START)");
            // The HUD's messages draw in the same pair (the C#
            // HudView's fonts are these: FONTBIG, FONTSML, SYS_PAL),
            // so they share the menu's atlases rather than upload
            // a second copy; the texts are copied out of the FTI
            // (a few KB) and the 160 KB file goes with this block.
            hud->setFonts(menuBig, menuSmall);
            hud->messages().setTexts(menuFti);
        } else {
            mdk::log3dsPrintf("menu: font atlases did not upload -- no menu");
        }
    }
    return ready;
}

// "data: NOT FOUND" has too many causes: the file system, the marker file's
// resolution, the RomFS's top entries and its manifest, said in the log.
void logDataNotFound3ds() {
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

} // namespace mdk

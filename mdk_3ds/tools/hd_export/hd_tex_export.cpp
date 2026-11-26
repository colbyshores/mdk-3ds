// ============================================================================
// hd_tex_export.cpp -- the HD texture pack's export step: every texture and
// 2D image the HD mod replaces, read out of the player's game data.
//
//   hd_tex_export <out dir> <level> [level ...]
//
// mdk_3ds/tools/build_hd_mod.py drives it and does everything after it
// (padding, the upscaler, the crop, the manifest). This file only READS THE
// GAME DATA, through mdk_core's own loaders, in the C# generator's order:
// src/Mdk.Game/HdTextures/HdGenerator.cs (Run), TextureExport.cs (Collect,
// Kurt) and src/Mdk.Game/Mods/CanvasExport.cs (Level, Menus). src/ stays the
// behaviour reference.
//
// PER LEVEL, in this order, each distinct key once (the first source wins):
//   1. TextureExport.Collect: per arena (arenas then corridors), its materials,
//      its models' materials, then the level models' (LEVELn.CMI) materials;
//      each name found in LevelData::archivesOf (the first archive that has
//      it) and seen through LevelData::paletteOf.
//   2. TextureExport.Kurt: KurtSprite.Drawn's animations, the level's own
//      first for the slides and the board (TRAVERSE.SNI, LEVELnS.SNI,
//      LEVELnO.SNI, the later archive winning), else TRAVSPRT.BNI's; each
//      frame through the DTI's palette, named <ANIM>_<frame>. Soft alpha.
//   3. CanvasExport.Level: the HUD, sniper mode's screen and the bomber's
//      sight from TRAVSPRT.BNI, through the DTI's palette. Soft alpha, images/.
// THEN THE MENUS (level 0, images/): MDKOPT, LOAD_n for levels 3-8, the
// falls' HUD through FALLP1..5. The fonts are skipped (upscaled, they blur).
//
// Output:
//   <out>/index.tsv    one line per source, in that order:
//                      level kind alpha name key width height frames
//                      (kind textures|images; alpha hard|soft; level 0 =
//                      the menus; the lines of one level have distinct keys)
//   <out>/<key>.rgba   the frames stacked downwards as RGBA8, expanded as
//                      ColourMips.Expand does: index 0 is (0,0,0,0), every
//                      other index the palette's RGB at alpha 255. Written
//                      once per key (a key names the pixels).
//
// HOST ONLY. Built with the host compiler against mdk_core's parsers; never
// part of the 3DS build.
// ============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <set>
#include <string>
#include <utility>
#include <vector>

#include "formats/arena.h"
#include "formats/bni.h"
#include "formats/cmi.h"
#include "formats/file_system.h"
#include "formats/fti.h"
#include "formats/mdk_data.h"
#include "formats/model.h"
#include "formats/palette.h"
#include "formats/sni.h"
#include "formats/sniper_screen.h"
#include "formats/sprite_animation.h"
#include "formats/texture.h"
#include "formats/texture_archive.h"
#include "game/hd_key.h"
#include "game/level_data.h"
#include "test_support.h"

namespace {

using mdk::formats::Palette;
using mdk::formats::Texture;

const int32_t kPaletteBytes = Palette::kSize * 3;

// KurtSprite.Drawn (src/Mdk.Game/Kurt/KurtSprite.cs): the states'
// animations in their table's order, then K_CHUTEC, K_MUZZF, K_BCKUP, then
// the level animations, each name once.
const char* const kKurtDrawn[] = {
    "K_STILL", "K_IDLE",  "K_RUN",    "K_SIDE",   "K_TRN45",  "K_JUMP",  "K_RJMP",
    "K_FALL",  "K_CHUTE", "K_LAND",   "K_SHOT",   "K_RUNFIR", "K_SPWEP", "K_BANG",
    "K_BFLIP", "K_SLIP",  "K_SLIDE",  "K_FSLIDE", "K_BSLIDE", "K_HANG",  "K_CRASHL",
    "K_ROLLL", "K_ROLLR", "K_HELM",   "K_CHUTEC", "K_MUZZF",  "K_BCKUP", "K_SURF",
    "K_SURFJ",
};
// KurtSprite.LevelAnimations: found in the level's archives first.
const char* const kKurtLevel[] = {"K_SURF", "K_SURFJ", "K_SLIP", "K_SLIDE", "K_FSLIDE", "K_BSLIDE"};

const int32_t kMenuLevels[] = {3, 4, 5, 6, 7, 8};  // HdOptions.AllLevels: the LOAD_n screens
const int32_t kFallCount = 5;                       // FALLP1..FALLP5
const int32_t kSniperTypes = 6;                     // SNIP_W1..6, SNIP_L1..6
const int32_t kSniperViewLeft = 20;                 // SniperOverlay.View
const int32_t kSniperViewTop = 60;

struct Source {
    int32_t level;
    const char* kind;   // "textures" or "images"
    const char* alpha;  // "hard" or "soft"
    std::string name;
    Texture texture;
    const Palette* palette;
};

// One level's (or the menus') sources, each key once, the first kept.
struct SourceList {
    std::vector<std::pair<std::string, Source>> items;
    std::set<std::string> keys;

    void add(const Source& source) {
        const std::string key = mdk::hdKeyOf(source.texture, *source.palette);
        if (keys.insert(key).second) {
            items.push_back(std::make_pair(key, source));
        }
    }
};

bool exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

// The frames as RGBA8 (ColourMips.Expand), written through a temporary file
// so an interrupted run never leaves a short one under the final name.
bool writeRgba(const std::string& path, const Texture& texture, const Palette& palette) {
    const int32_t count = texture.indexCount();
    const uint8_t* indices = texture.indices();
    const uint8_t* rgb = palette.rgba();
    std::vector<uint8_t> rgba((size_t)count * 4u, 0);
    for (int32_t i = 0; i < count; i++) {
        const uint8_t v = indices[i];
        if (v == 0) {
            continue;
        }
        rgba[(size_t)i * 4 + 0] = rgb[v * 4 + 0];
        rgba[(size_t)i * 4 + 1] = rgb[v * 4 + 1];
        rgba[(size_t)i * 4 + 2] = rgb[v * 4 + 2];
        rgba[(size_t)i * 4 + 3] = 255;
    }
    const std::string temporary = path + ".tmp";
    FILE* file = fopen(temporary.c_str(), "wb");
    if (!file) {
        return false;
    }
    const bool ok = fwrite(rgba.data(), 1, rgba.size(), file) == rgba.size();
    if (fclose(file) != 0 || !ok) {
        return false;
    }
    return rename(temporary.c_str(), path.c_str()) == 0;
}

bool readArchive(const mdk::formats::MdkData& data, const std::string& path,
                 mdk::formats::Bni* out) {
    std::vector<uint8_t> bytes;
    if (!data.read(path, &bytes) || !mdk::formats::Bni::parse(out, std::move(bytes))) {
        fprintf(stderr, "hd_tex_export: %s did not load\n", path.c_str());
        return false;
    }
    return true;
}

// A palette of 256 RGB triples at `offset` of an archive (CanvasExport.PaletteAt).
bool paletteAt(const std::vector<uint8_t>& bytes, int32_t offset, Palette* out) {
    if (offset < 0 || offset + kPaletteBytes > (int32_t)bytes.size()) {
        return false;
    }
    *out = Palette::fromRgb(bytes.data() + offset, kPaletteBytes);
    return true;
}

// An animation's frame's image, or false.
bool frameImage(const mdk::formats::Bni& bni, const std::string& name, int32_t frame,
                Texture* out) {
    mdk::formats::SpriteAnimation animation;
    mdk::formats::SpriteAnimation::Frame decoded;
    if (!bni.animation(name, &animation) || !animation.frame(frame, &decoded)) {
        return false;
    }
    *out = decoded.image;
    return true;
}

// TextureExport.Collect.
void collectSurfaces(mdk::LevelData* level, const mdk::formats::Cmi& cmi, SourceList* list) {
    std::vector<std::string> levelMaterials;
    std::set<std::string> seenModels;
    for (size_t m = 0; m < cmi.modelOffsets().size(); m++) {
        const std::string& name = cmi.modelOffsets()[m].first;
        mdk::formats::Model model;
        if (!seenModels.insert(name).second || !cmi.model(name, &model)) {
            continue;
        }
        for (size_t i = 0; i < model.materials.size(); i++) {
            levelMaterials.push_back(model.materials[i].nameString());
        }
    }
    const std::vector<const mdk::formats::Arena*>& arenas = level->arenas();
    for (size_t a = 0; a < arenas.size(); a++) {
        const mdk::formats::Arena* arena = arenas[a];
        const Palette& palette = level->paletteOf(arena);
        const std::vector<const mdk::formats::TextureArchive*> archives = level->archivesOf(arena);
        std::vector<std::string> names = arena->materials();
        std::set<std::string> arenaModels;
        for (int32_t m = 0; m < arena->modelCount(); m++) {
            const std::string& modelName = arena->modelName(m);
            const mdk::formats::Model* model = arena->model(modelName);
            if (!arenaModels.insert(modelName).second || !model) {
                continue;
            }
            for (size_t i = 0; i < model->materials.size(); i++) {
                names.push_back(model->materials[i].nameString());
            }
        }
        names.insert(names.end(), levelMaterials.begin(), levelMaterials.end());
        std::set<std::string> seen;
        for (size_t n = 0; n < names.size(); n++) {
            if (!seen.insert(names[n]).second) {
                continue;
            }
            const Texture* texture = nullptr;
            for (size_t i = 0; i < archives.size() && !texture; i++) {
                texture = archives[i]->findTexture(names[n]);
            }
            if (texture) {
                texture->indexCount();  // bring streamed pixels in before the copy
                list->add(Source{level->number(), "textures", "hard", names[n], *texture, &palette});
            }
        }
    }
}

// TextureExport.Kurt.
bool collectKurt(const mdk::formats::MdkData& data, const mdk::LevelData& level,
                 const mdk::formats::Bni& sprites, SourceList* list) {
    std::vector<mdk::formats::Sni> archives;  // SoundBank.ForLevel's order
    const char* const kArchives[] = {"TRAVERSE/TRAVERSE.SNI", "TRAVERSE/LEVEL%d/LEVEL%dS.SNI",
                                     "TRAVERSE/LEVEL%d/LEVEL%dO.SNI"};
    for (size_t a = 0; a < sizeof(kArchives) / sizeof(kArchives[0]); a++) {
        char path[96];
        snprintf(path, sizeof(path), kArchives[a], (int)level.number(), (int)level.number());
        mdk::formats::Sni sni;
        if (mdk::formats::Sni::open(&sni, data, path)) {
            archives.push_back(std::move(sni));
        }
    }
    const Palette* palette = &level.dti().palette();
    for (size_t d = 0; d < sizeof(kKurtDrawn) / sizeof(kKurtDrawn[0]); d++) {
        const std::string name = kKurtDrawn[d];
        mdk::formats::SpriteAnimation animation;
        bool found = false;
        for (size_t l = 0; l < sizeof(kKurtLevel) / sizeof(kKurtLevel[0]); l++) {
            if (name == kKurtLevel[l]) {
                for (size_t a = archives.size(); a > 0 && !found; a--) {
                    found = archives[a - 1].animation(name, &animation);
                }
            }
        }
        if (!found && sprites.has(name)) {
            found = sprites.animation(name, &animation);
        }
        if (!found) {
            continue;
        }
        for (int32_t f = 0; f < animation.frameCount(); f++) {
            mdk::formats::SpriteAnimation::Frame decoded;
            if (!animation.frame(f, &decoded)) {
                fprintf(stderr, "hd_tex_export: %s frame %d did not decode\n", name.c_str(), (int)f);
                return false;
            }
            list->add(Source{level.number(), "textures", "soft", name + "_" + std::to_string(f),
                             decoded.image, palette});
        }
    }
    return true;
}

// HudView.Images: the panel, the skull, the digits, the pickups' frames.
bool collectHud(const mdk::formats::Bni& bni, int32_t level, const Palette* palette,
                SourceList* list) {
    const char* const kImages[] = {"SC_STAT", "SKULL", "SNIP_TXT"};
    for (size_t i = 0; i < sizeof(kImages) / sizeof(kImages[0]); i++) {
        Texture image;
        if (!bni.image(kImages[i], &image)) {
            fprintf(stderr, "hd_tex_export: no image %s\n", kImages[i]);
            return false;
        }
        list->add(Source{level, "images", "soft", kImages[i], image, palette});
    }
    mdk::formats::SpriteAnimation pickups;
    if (!bni.animation("PICKUPS", &pickups)) {
        fprintf(stderr, "hd_tex_export: no animation PICKUPS\n");
        return false;
    }
    for (int32_t f = 0; f < pickups.frameCount(); f++) {
        mdk::formats::SpriteAnimation::Frame decoded;
        if (!pickups.frame(f, &decoded)) {
            return false;
        }
        list->add(Source{level, "images", "soft", "PICKUPS_" + std::to_string(f), decoded.image,
                         palette});
    }
    return true;
}

// SniperOverlay.Images and BomberOverlay.Images.
bool collectOverlays(const mdk::formats::Bni& sprites, int32_t level, const Palette* palette,
                     SourceList* list) {
    using mdk::formats::SniperScreen;
    Texture frame;
    Texture mask;
    if (!SniperScreen::frame(sprites, &frame) || !SniperScreen::mask(sprites, &mask)) {
        fprintf(stderr, "hd_tex_export: sniper mode's screen did not load\n");
        return false;
    }
    // SniperOverlay.CutView: the view's rectangle cleared, but for its edge
    // where the mask covers it.
    Texture cut = frame;
    uint8_t* cutIndices = cut.mutableIndices();
    const uint8_t* maskIndices = mask.indices();
    for (int32_t y = 0; y < SniperScreen::kViewHeight; y++) {
        for (int32_t x = 0; x < SniperScreen::kViewWidth; x++) {
            const bool edge = x == 0 || y == 0 || x == SniperScreen::kViewWidth - 1 ||
                              y == SniperScreen::kViewHeight - 1;
            if (edge && maskIndices[y * mask.width() + x] != 0) {
                continue;
            }
            cutIndices[(kSniperViewTop + y) * frame.width() + kSniperViewLeft + x] = 0;
        }
    }
    list->add(Source{level, "images", "soft", "SNIPERS1", cut, palette});
    list->add(Source{level, "images", "soft", "SNIPERS2", mask, palette});

    std::vector<std::pair<std::string, Texture>> images;
    Texture image;
    if (!frameImage(sprites, "CROSS", 0, &image)) {
        return false;
    }
    images.push_back(std::make_pair(std::string("CROSS_0"), image));
    std::vector<std::string> names = {"SNIP_RNG", "SNIP_WEP", "SNIP_TXT"};
    for (int32_t i = 1; i <= kSniperTypes; i++) {
        names.push_back("SNIP_W" + std::to_string(i));
        names.push_back("SNIP_L" + std::to_string(i));
    }
    for (size_t n = 0; n < names.size(); n++) {
        if (!sprites.image(names[n], &image)) {
            fprintf(stderr, "hd_tex_export: no image %s\n", names[n].c_str());
            return false;
        }
        images.push_back(std::make_pair(names[n], image));
    }
    mdk::formats::SpriteAnimation miss;
    if (!sprites.animation("SNIPERGA", &miss)) {
        return false;
    }
    for (int32_t f = 0; f < miss.frameCount(); f++) {
        mdk::formats::SpriteAnimation::Frame decoded;
        if (!miss.frame(f, &decoded)) {
            return false;
        }
        images.push_back(std::make_pair("SNIPERGA_" + std::to_string(f), decoded.image));
    }
    // The bomber's sight.
    const char* const kBomber[] = {"BOMBTARG", "CROSS"};
    for (size_t b = 0; b < 2; b++) {
        if (!frameImage(sprites, kBomber[b], 0, &image)) {
            return false;
        }
        images.push_back(std::make_pair(std::string(kBomber[b]) + "_0", image));
    }
    for (size_t i = 0; i < images.size(); i++) {
        list->add(Source{level, "images", "soft", images[i].first, images[i].second, palette});
    }
    return true;
}

// CanvasExport.Menus (fonts skipped). `palettes` keeps the palettes alive.
bool collectMenus(const mdk::formats::MdkData& data, std::vector<Palette>* palettes,
                  SourceList* list) {
    palettes->reserve(2 + sizeof(kMenuLevels) / sizeof(kMenuLevels[0]) + kFallCount);
    mdk::formats::Bni options;
    if (!readArchive(data, "MISC/OPTIONS.BNI", &options)) {
        return false;
    }
    const mdk::formats::ArchiveIndex::Entry* background = options.index().find("MDKOPT");
    if (background) {
        Palette palette;
        if (!paletteAt(options.bytes(), background->offset, &palette)) {
            return false;
        }
        palettes->push_back(palette);
        list->add(Source{0, "images", "soft", "MDKOPT",
                         Texture::parse("MDKOPT", options.bytes().data(), (int32_t)options.bytes().size(),
                                        background->offset + kPaletteBytes),
                         &palettes->back()});
    }
    for (size_t l = 0; l < sizeof(kMenuLevels) / sizeof(kMenuLevels[0]); l++) {
        const std::string name = "LOAD_" + std::to_string(kMenuLevels[l]);
        std::vector<uint8_t> bytes;
        if (!data.read("MISC/" + name + ".LBB", &bytes) || (int32_t)bytes.size() <= kPaletteBytes) {
            continue;
        }
        Palette palette;
        paletteAt(bytes, 0, &palette);
        palettes->push_back(palette);
        list->add(Source{0, "images", "soft", name,
                         Texture::parse("LOAD", bytes.data(), (int32_t)bytes.size(), kPaletteBytes),
                         &palettes->back()});
    }
    mdk::formats::Bni falls;
    if (!readArchive(data, "FALL3D/FALL3D.BNI", &falls)) {
        return false;
    }
    for (int32_t n = 1; n <= kFallCount; n++) {
        const std::string name = "FALLP" + std::to_string(n);
        const mdk::formats::ArchiveIndex::Entry* entry = falls.index().find(name);
        Palette palette;
        if (!entry || !paletteAt(falls.bytes(), entry->offset, &palette)) {
            fprintf(stderr, "hd_tex_export: no palette %s in FALL3D.BNI\n", name.c_str());
            return false;
        }
        palettes->push_back(palette);
        if (!collectHud(falls, 0, &palettes->back(), list)) {
            return false;
        }
    }
    return true;
}

// Appends a list to the index and writes each key's pixels once.
bool emit(const SourceList& list, const std::string& out, FILE* index,
          std::set<std::string>* written) {
    for (size_t i = 0; i < list.items.size(); i++) {
        const std::string& key = list.items[i].first;
        const Source& s = list.items[i].second;
        fprintf(index, "%d\t%s\t%s\t%s\t%s\t%d\t%d\t%d\n", (int)s.level, s.kind, s.alpha,
                s.name.c_str(), key.c_str(), (int)s.texture.width(), (int)s.texture.height(),
                (int)s.texture.frameCount());
        const std::string path = out + "/" + key + ".rgba";
        if (!written->insert(key).second || exists(path)) {
            continue;
        }
        if (!writeRgba(path, s.texture, *s.palette)) {
            fprintf(stderr, "hd_tex_export: cannot write %s\n", path.c_str());
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <out dir> <level> [level ...]\n", argv[0]);
        return 2;
    }
    const std::string out = argv[1];
    mkdir(out.c_str(), 0755);

    mdk::formats::setFileSystem(mdk::test::host::fileSystem());
    const mdk::formats::MdkData data = mdk::formats::MdkData::find(mdk::formats::fileSystem());
    if (!data.found()) {
        fprintf(stderr, "hd_tex_export: no MDK data (set MDK_DATA_DIR)\n");
        return 1;
    }
    mdk::formats::Bni sprites;
    if (!readArchive(data, "TRAVERSE/TRAVSPRT.BNI", &sprites)) {
        return 1;
    }
    const std::string indexPath = out + "/index.tsv";
    FILE* index = fopen((indexPath + ".tmp").c_str(), "w");
    if (!index) {
        fprintf(stderr, "hd_tex_export: cannot write %s\n", indexPath.c_str());
        return 1;
    }
    fprintf(index, "# level\tkind\talpha\tname\tkey\twidth\theight\tframes\n");
    std::set<std::string> written;
    for (int i = 2; i < argc; i++) {
        const int32_t number = atoi(argv[i]);
        mdk::LevelData level;
        if (!mdk::LevelData::load(&level, &data, number)) {
            fprintf(stderr, "hd_tex_export: level %d did not load\n", (int)number);
            fclose(index);
            return 1;
        }
        std::vector<uint8_t> cmiBytes;
        mdk::formats::Cmi cmi;
        char cmiPath[64];
        snprintf(cmiPath, sizeof(cmiPath), "TRAVERSE/LEVEL%d/LEVEL%d.CMI", (int)number, (int)number);
        if (!data.read(cmiPath, &cmiBytes) || !mdk::formats::Cmi::parse(&cmi, std::move(cmiBytes))) {
            fprintf(stderr, "hd_tex_export: %s did not load\n", cmiPath);
            fclose(index);
            return 1;
        }
        // Collect, then Kurt, then the canvas, the first source of a key kept
        // (HdGenerator.Run's DistinctBy).
        SourceList list;
        collectSurfaces(&level, cmi, &list);
        const Palette* palette = &level.dti().palette();
        if (!collectKurt(data, level, sprites, &list) ||
            !collectHud(sprites, number, palette, &list) ||
            !collectOverlays(sprites, number, palette, &list) ||
            !emit(list, out, index, &written)) {
            fclose(index);
            return 1;
        }
        printf("hd_tex_export: LEVEL%d: %d textures\n", (int)number, (int)list.items.size());
    }
    std::vector<Palette> menuPalettes;
    SourceList menus;
    if (!collectMenus(data, &menuPalettes, &menus) || !emit(menus, out, index, &written)) {
        fclose(index);
        return 1;
    }
    printf("hd_tex_export: menus: %d images\n", (int)menus.items.size());
    if (fclose(index) != 0 || rename((indexPath + ".tmp").c_str(), indexPath.c_str()) != 0) {
        fprintf(stderr, "hd_tex_export: cannot write %s\n", indexPath.c_str());
        return 1;
    }
    return 0;
}

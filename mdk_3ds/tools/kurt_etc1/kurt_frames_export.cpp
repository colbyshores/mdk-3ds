// ============================================================================
// kurt_frames_export.cpp -- Kurt's ORIGINAL sprite frames, exported for the
// 3DS's compressed-frame set (mdk_3ds/tools/kurt_etc1_convert.py drives it).
//
//   kurt_frames_export <out dir> [level ...]     (default levels 3..8)
//
// The data is the player's own copy, found the way the game finds it
// (MDK_DATA_DIR, ../MDK, ...: formats::MdkData::find). Nothing is committed.
//
// FOR EVERY LEVEL, every frame of every K_* animation in TRAVERSE/TRAVSPRT.BNI --
// and of KURT'S LEVEL ANIMATIONS, the ones the level's SNI archives carry (the
// slides' K_SLIP/K_SLIDE/K_FSLIDE/K_BSLIDE on level 6, the board's K_SURF/K_SURFJ
// on level 4: KurtSprite's kLevelAnimations, found as SoundBank::animation finds
// them, TRAVERSE.SNI then LEVELnS.SNI then LEVELnO.SNI, the later archive
// winning) -- is expanded through THAT LEVEL'S palette (LevelData::load: the DTI's palette
// with the system colours, exactly what the 3DS hands KurtSprite::load) by the
// 3DS's own rule (index 0 transparent, every other index the palette's RGB at
// alpha 255), and written as raw RGBA8 rows to <out>/<NAME>_<frame>@<key>.rgba,
// where <key> is game/hd_key.h's hdKeyOf -- the texture through that palette. A
// frame that looks the same in two levels has one key, so it is written once.
//
// <out>/index.tsv lists one line per (level, frame):
//   level  name  width  height  key
// The converter decides from it which file is the all-levels copy and which
// levels need their own (the HD pack's folder rule: textures/NAME, else
// textures/LEVELn/NAME).
//
// HOST ONLY. Built by the converter with the host compiler against mdk_core's
// own parsers; never part of the 3DS build.
// ============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <set>
#include <string>
#include <vector>

#include "formats/bni.h"
#include "formats/file_system.h"
#include "formats/mdk_data.h"
#include "formats/palette.h"
#include "formats/sni.h"
#include "formats/sprite_animation.h"
#include "formats/texture.h"
#include "game/hd_key.h"
#include "game/level_data.h"
#include "test_support.h"

namespace {

bool writeFile(const std::string& path, const std::vector<uint8_t>& bytes) {
    FILE* file = fopen(path.c_str(), "wb");
    if (!file) {
        return false;
    }
    const bool ok = fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    return fclose(file) == 0 && ok;
}

bool exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <out dir> [level ...]\n", argv[0]);
        return 2;
    }
    const std::string out = argv[1];
    std::vector<int32_t> levels;
    for (int i = 2; i < argc; i++) {
        levels.push_back(atoi(argv[i]));
    }
    if (levels.empty()) {
        for (int32_t level = 3; level <= 8; level++) {
            levels.push_back(level);
        }
    }
    mkdir(out.c_str(), 0755);

    mdk::formats::setFileSystem(mdk::test::host::fileSystem());
    const mdk::formats::MdkData data = mdk::formats::MdkData::find(mdk::formats::fileSystem());
    if (!data.found()) {
        fprintf(stderr, "kurt_frames_export: no MDK data (set MDK_DATA_DIR)\n");
        return 1;
    }
    std::vector<uint8_t> spriteBytes;
    mdk::formats::Bni sprites;
    if (!data.read("TRAVERSE/TRAVSPRT.BNI", &spriteBytes) ||
        !mdk::formats::Bni::parse(&sprites, std::move(spriteBytes))) {
        fprintf(stderr, "kurt_frames_export: TRAVERSE/TRAVSPRT.BNI did not load\n");
        return 1;
    }
    std::vector<std::string> names;
    for (size_t i = 0; i < sprites.entries().size(); i++) {
        const std::string& name = sprites.entries()[i].first;
        if (name.compare(0, 2, "K_") == 0) {
            names.push_back(name);
        }
    }

    FILE* index = fopen((out + "/index.tsv").c_str(), "w");
    if (!index) {
        fprintf(stderr, "kurt_frames_export: cannot write %s/index.tsv\n", out.c_str());
        return 1;
    }
    fprintf(index, "# level\tname\twidth\theight\tkey\n");
    std::set<std::string> written;
    int32_t frames = 0;
    for (size_t l = 0; l < levels.size(); l++) {
        mdk::LevelData level;
        if (!mdk::LevelData::load(&level, &data, levels[l])) {
            fprintf(stderr, "kurt_frames_export: level %d did not load\n", (int)levels[l]);
            fclose(index);
            return 1;
        }
        const mdk::formats::Palette& palette = level.dti().palette();
        const uint8_t* rgb = palette.rgba();
        // The level's archives, for its own Kurt animations (later ones win).
        std::vector<mdk::formats::Sni> archives;
        char path[96];
        const char* const kArchives[] = {"TRAVERSE/TRAVERSE.SNI", "TRAVERSE/LEVEL%d/LEVEL%dS.SNI",
                                         "TRAVERSE/LEVEL%d/LEVEL%dO.SNI"};
        for (size_t a = 0; a < sizeof(kArchives) / sizeof(kArchives[0]); a++) {
            snprintf(path, sizeof(path), kArchives[a], (int)levels[l], (int)levels[l]);
            mdk::formats::Sni sni;
            if (mdk::formats::Sni::open(&sni, data, path)) {
                archives.push_back(std::move(sni));
            }
        }
        // KurtSprite's kLevelAnimations (mdk_core/src/game/kurt_sprite.cpp).
        const char* const kLevelAnimations[] = {"K_SURF", "K_SURFJ", "K_SLIP", "K_SLIDE",
                                                "K_FSLIDE", "K_BSLIDE"};
        const size_t levelCount = sizeof(kLevelAnimations) / sizeof(kLevelAnimations[0]);
        for (size_t n = 0; n < names.size() + levelCount; n++) {
            const bool fromLevel = n >= names.size();
            const std::string name = fromLevel ? std::string(kLevelAnimations[n - names.size()])
                                               : names[n];
            mdk::formats::SpriteAnimation animation;
            bool found = false;
            if (!fromLevel) {
                found = sprites.animation(name, &animation);
            } else {
                for (size_t a = archives.size(); a > 0 && !found; a--) {
                    found = archives[a - 1].animation(name, &animation);
                }
            }
            if (!found) {
                continue;
            }
            for (int32_t f = 0; f < animation.frameCount(); f++) {
                mdk::formats::SpriteAnimation::Frame decoded;
                if (!animation.frame(f, &decoded)) {
                    break;
                }
                const mdk::formats::Texture& image = decoded.image;
                const int32_t w = image.width();
                const int32_t h = image.height();
                if (w <= 0 || h <= 0) {
                    continue;
                }
                // The key exactly as KurtSprite::ensureTexture builds it.
                mdk::formats::Texture keyed;
                keyed.setName(name);
                keyed.setWidth(w);
                keyed.setHeight(h);
                keyed.setFrameCount(1);
                keyed.setIndices(image.indices(), w * h);
                const std::string key = mdk::hdKeyOf(keyed, palette);
                const std::string stem = name + "_" + std::to_string(f);
                fprintf(index, "%d\t%s\t%d\t%d\t%s\n", (int)levels[l], stem.c_str(), (int)w,
                        (int)h, key.c_str());
                frames++;
                const std::string file = out + "/" + stem + "@" + key + ".rgba";
                if (written.count(file) || exists(file)) {
                    written.insert(file);
                    continue;
                }
                std::vector<uint8_t> rgba((size_t)w * (size_t)h * 4u, 0);
                for (int32_t i = 0; i < w * h; i++) {
                    const uint8_t v = image.indices()[i];
                    if (v == 0) {
                        continue;  // transparent: the 3DS's rule, the colour irrelevant
                    }
                    rgba[(size_t)i * 4 + 0] = rgb[v * 4 + 0];
                    rgba[(size_t)i * 4 + 1] = rgb[v * 4 + 1];
                    rgba[(size_t)i * 4 + 2] = rgb[v * 4 + 2];
                    rgba[(size_t)i * 4 + 3] = 255;
                }
                if (!writeFile(file, rgba)) {
                    fprintf(stderr, "kurt_frames_export: cannot write %s\n", file.c_str());
                    fclose(index);
                    return 1;
                }
                written.insert(file);
            }
        }
    }
    fclose(index);
    printf("kurt_frames_export: %d frames over %d levels, %d distinct images\n", (int)frames,
           (int)levels.size(), (int)written.size());
    return 0;
}

// ============================================================================
// arena_tex_export.cpp -- the arena and level textures, exported for the 3DS's
// compressed arena set (mdk_3ds/tools/arena_etc1_convert.py drives it).
//
//   arena_tex_export <out dir> [level ...]     (default levels 3..8)
//
// FOR EVERY LEVEL, every texture of every arena's OWN archive and of the
// level's archive (LevelData::archivesOf: the arena's, then the level's -- the
// first two; the rest are other arenas' archives, which those arenas export
// themselves) is expanded through THAT ARENA's palette (LevelData::paletteOf:
// the DTI's palette with the arena's own 112 colours; a corridor borrows its
// arena's by dropping its first character) by the 3DS's own rule (index 0
// transparent, every other index the palette's RGB at alpha 255), and written
// as raw RGBA8 rows to <out>/<NAME>@<key>.rgba, where <key> is game/hd_key.h's
// hdKeyOf -- the texture seen through that palette, the exact key
// ModImages::resolve computes at lookup (main_3ds.cpp's hdPackLookup). A
// texture that looks the same through two arenas' palettes has one key, so it
// is written once.
//
// THE ZERO RULE IS ALREADY APPLIED: TextureArchive::parse replaces index 0
// with the palette's black in every NON-keyed texture at parse time, so the
// indices written here are the indices the game draws with, and hdKeyOf is
// computed on those same parsed indices -- keyed textures (kind bit 0) keep
// their transparent index 0 and become ETC1A4, opaque ones become ETC1.
//
// AN ANIMATED TEXTURE is the whole vertical strip: height * frameCount rows,
// the delta form (kind 0x20000) decoded by the parser into full frames. That
// is the image ModImages' strip() asks the pack for (it divides the height
// back by the frame count), and the same strip the original uploads.
//
// <out>/index.tsv lists one line per (level, arena, texture):
//   level  arena  name  width  height  frames  key
// The converter dedupes by key (one image, one .t3x) and names the file after
// the first name a key was seen under -- the same first occurrence that wrote
// the .rgba here, so the two agree by construction.
//
// The pixels: LevelData::load drops each arena's texture pixels behind the
// arena stream, and a tool that never calls update() reads them on demand
// (game/arena_stream.h: "every read is a demand load and nothing is trimmed")
// -- the same path the console takes when a texture is drawn.
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

#include "formats/arena.h"
#include "formats/file_system.h"
#include "formats/mdk_data.h"
#include "formats/palette.h"
#include "formats/texture.h"
#include "formats/texture_archive.h"
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
        fprintf(stderr, "arena_tex_export: no MDK data (set MDK_DATA_DIR)\n");
        return 1;
    }

    FILE* index = fopen((out + "/index.tsv").c_str(), "w");
    if (!index) {
        fprintf(stderr, "arena_tex_export: cannot write %s/index.tsv\n", out.c_str());
        return 1;
    }
    fprintf(index, "# level\tarena\tname\twidth\theight\tframes\tkey\n");
    std::set<std::string> writtenKeys;
    int32_t entries = 0;
    int32_t skipped = 0;
    for (size_t l = 0; l < levels.size(); l++) {
        mdk::LevelData level;
        if (!mdk::LevelData::load(&level, &data, levels[l])) {
            fprintf(stderr, "arena_tex_export: level %d did not load\n", (int)levels[l]);
            fclose(index);
            return 1;
        }
        const std::vector<const mdk::formats::Arena*>& arenas = level.arenas();
        for (size_t a = 0; a < arenas.size(); a++) {
            const mdk::formats::Arena* arena = arenas[a];
            mdk::formats::Palette& palette = level.paletteOf(arena);
            const uint8_t* rgb = palette.rgba();
            // archivesOf builds: the arena's own, the level's, then every
            // other arena's. Only the first two belong to this arena.
            const std::vector<const mdk::formats::TextureArchive*> archives =
                level.archivesOf(arena);
            const size_t ownCount = archives.size() < 2 ? archives.size() : 2;
            for (size_t ai = 0; ai < ownCount; ai++) {
                const std::vector<std::pair<std::string, mdk::formats::Texture>>& textures =
                    archives[ai]->textures();
                for (size_t t = 0; t < textures.size(); t++) {
                    const std::string& name = textures[t].first;
                    const mdk::formats::Texture& texture = textures[t].second;
                    const int32_t w = texture.width();
                    const int32_t h = texture.height();
                    const int32_t frames = texture.frameCount() > 0 ? texture.frameCount() : 1;
                    if (w <= 0 || h <= 0) {
                        continue;
                    }
                    if (name.find('@') != std::string::npos) {
                        // The pack's key separator; a name with one could not
                        // be indexed. Never seen in retail data.
                        fprintf(stderr, "arena_tex_export: %s: name holds '@', skipped\n",
                                name.c_str());
                        skipped++;
                        continue;
                    }
                    const int32_t count = texture.indexCount();
                    if (count != frames * w * h) {
                        fprintf(stderr, "arena_tex_export: %s: %d indices is not %dx%dx%d, "
                                        "skipped\n", name.c_str(), (int)count, (int)w, (int)h,
                                (int)frames);
                        skipped++;
                        continue;
                    }
                    // The key exactly as ModImages::resolve builds it: the
                    // PARSED texture (the zero rule applied, the delta form
                    // decoded) through this arena's palette.
                    const std::string key = mdk::hdKeyOf(texture, palette);
                    fprintf(index, "%d\t%s\t%s\t%d\t%d\t%d\t%s\n", (int)levels[l],
                            arena->name().c_str(), name.c_str(), (int)w, (int)h, (int)frames,
                            key.c_str());
                    entries++;
                    if (writtenKeys.count(key)) {
                        continue;  // the same image through another palette was written
                    }
                    writtenKeys.insert(key);
                    const std::string file = out + "/" + name + "@" + key + ".rgba";
                    if (exists(file)) {
                        continue;
                    }
                    // The whole strip: frames of w x h stacked downwards, the
                    // 3DS's rule per texel (index 0 transparent -- only a
                    // keyed texture still has one after the parse).
                    std::vector<uint8_t> rgba((size_t)count * 4u, 0);
                    const uint8_t* indices = texture.indices();
                    for (int32_t i = 0; i < count; i++) {
                        const uint8_t v = indices[i];
                        if (v == 0) {
                            continue;  // transparent: the 3DS's rule, the colour irrelevant
                        }
                        rgba[(size_t)i * 4 + 0] = rgb[v * 4 + 0];
                        rgba[(size_t)i * 4 + 1] = rgb[v * 4 + 1];
                        rgba[(size_t)i * 4 + 2] = rgb[v * 4 + 2];
                        rgba[(size_t)i * 4 + 3] = 255;
                    }
                    if (!writeFile(file, rgba)) {
                        fprintf(stderr, "arena_tex_export: cannot write %s\n", file.c_str());
                        fclose(index);
                        return 1;
                    }
                }
            }
        }
    }
    fclose(index);
    printf("arena_tex_export: %d texture uses over %d levels, %d distinct images, %d skipped\n",
           (int)entries, (int)levels.size(), (int)writtenKeys.size(), (int)skipped);
    return 0;
}

#pragma once

// ============================================================================
// mod_images.h -- the enabled mods' images for one level, ported 1:1 from
// src/Mdk.Game/Mods/ModImages.cs.
//
// A texture's or a 2D image's replacement, of any size, premultiplied for
// the renderer (game/hd_key.h's HdImage). Mods are searched in order;
// within one, the exact view first (NAME@key: the texture through that
// palette, game/hd_key.h; any subfolder), then the level's folder, then
// all levels'. Names ignore case. Animated textures: one strip of the
// frames stacked downwards, or one image per frame (NAME_0, NAME_1...),
// all of one size.
//
//   mods/m/textures/WALL@9f3a....png   exact view (HD textures)
//   mods/m/textures/LEVEL3/WALL.png    LEVEL3 only
//   mods/m/textures/WALL.png           every level
//   mods/m/images/SC_STAT.png          a 2D image (HUD, menus)
//
// Files are decoded lazily on first lookup (the C# decodes the level's
// folder at opening, on every core; the pictures are the same, only the
// timing of a broken file's error line differs).
// ============================================================================

#include <stdint.h>

#include <map>
#include <string>
#include <vector>

#include "game/hd_key.h"

namespace mdk {

namespace formats {
class Palette;
class Texture;
} // namespace formats

// The enabled mods' images for one level (or the menus: no level).
class ModImages {
public:
    // What a folder of a mod holds.
    enum class Kind {
        Textures,
        Images,
    };

    ModImages() = default;

    // The images of `mods` (folders, first wins) for a level (pass
    // hasLevel=false for the menus: no level's folder).
    static ModImages open(const std::vector<std::string>& mods, bool hasLevel, int32_t level);

    static const char* folderOf(Kind kind) { return kind == Kind::Textures ? "textures" : "images"; }

    // The level's folder name (LEVEL3).
    static std::string levelFolder(int32_t level) {
        return "LEVEL" + std::to_string(level);
    }

    // The same index built from a LIST of files instead of a directory scan:
    // `files` are paths relative to `root` ("textures/LEVEL3/WALL@9f3a.t3x"),
    // one mod. The 3DS HD pack lists its files in its manifest
    // (docs/HD_TEXTURES_3DS_SPEC.md) because its SD card is not scanned; the
    // indexing rule is the scan's own.
    static ModImages fromList(const std::string& root, const std::vector<std::string>& files,
                              bool hasLevel, int32_t level);

    // The FILE a replacement would come from, by find()'s own order (exact key,
    // then the level's folder, then every level), without decoding it; whole
    // files only (no per-frame NAME_0.. sets). The 3DS imports a compressed
    // .t3x from this path instead of decoding a PNG.
    bool resolve(Kind kind, const std::string& name, const formats::Texture& texture,
                 const formats::Palette& palette, std::string* path);

    // A 3D texture's replacement (arenas, models, Kurt's frames), or false.
    bool texture(const std::string& name, const formats::Texture& texture,
                 const formats::Palette& palette, HdImage* out);

    // A 2D image's replacement (HUD, menus, fonts), or false.
    bool image(const std::string& name, const formats::Texture& texture,
               const formats::Palette& palette, HdImage* out);

    // The names replaced so far (each once), in the order found.
    const std::vector<std::string>& found() const { return found_; }

private:
    // One kind of one mod's files: by key, in the level's folder, for all
    // levels. Keys are lower-cased file stems; values are full paths.
    struct Index {
        std::map<std::string, std::string> keys;
        std::map<std::string, std::string> level;
        std::map<std::string, std::string> names;
    };

    bool find(Kind kind, const std::string& name, const formats::Texture& texture,
              const formats::Palette& palette, HdImage* out);
    // Record a replaced name, each once (the C#'s Report).
    void reportFound(const std::string& name) {
        for (size_t i = 0; i < found_.size(); i++) {
            if (found_[i] == name) {
                return;
            }
        }
        found_.push_back(name);
    }
    bool frames(const std::map<std::string, std::string>& files, const std::string& name,
                int32_t frameCount, HdImage* out);
    bool strip(const std::string& path, int32_t frameCount, HdImage* out);
    bool decoded(const std::string& path, HdImage* out);

    std::vector<std::map<Kind, Index>> mods_;
    // Decoded files (premultiplied); a false entry is a broken file.
    std::map<std::string, HdImage> decoded_;
    std::map<std::string, bool> decodedOk_;
    std::vector<std::string> found_;
};

} // namespace mdk

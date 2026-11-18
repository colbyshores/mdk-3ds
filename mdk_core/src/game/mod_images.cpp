// ============================================================================
// mod_images.cpp -- see mod_images.h.
// ============================================================================

#include "game/mod_images.h"

#include <stdio.h>

#include "formats/file_system.h"
#include "formats/palette.h"
#include "formats/png.h"
#include "formats/texture.h"
#include "game/mod_info.h"

namespace mdk {

namespace {

// The parts of a path: its directory (no trailing separator) and its stem
// (the file's own name without its last extension).
void splitPath(const std::string& path, std::string* directory, std::string* stem) {
    const size_t slash = path.find_last_of('/');
    const std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
    *directory = slash == std::string::npos ? "" : path.substr(0, slash);
    const size_t dot = file.find_last_of('.');
    *stem = dot == std::string::npos ? file : file.substr(0, dot);
}

bool hasPngExtension(const std::string& path) {
    return path.size() >= 4 && modLower(path.substr(path.size() - 4)) == ".png";
}

// Every PNG under `folder`, recursively. The seam has no stat, so an entry
// is a directory when it lists and a file when it does not.
void collectPngs(const formats::FileSystem* system, const std::string& folder,
                 std::vector<std::string>* out) {
    std::vector<std::string> entries;
    if (!system->listDirectory(system->user, folder.c_str(), &entries)) {
        return;
    }
    for (size_t i = 0; i < entries.size(); i++) {
        if (entries[i].empty() || entries[i][0] == '.') {
            continue;
        }
        const std::string path = modPathJoin(folder, entries[i]);
        std::vector<std::string> probe;
        if (system->listDirectory(system->user, path.c_str(), &probe)) {
            collectPngs(system, path, out);
        } else if (hasPngExtension(path)) {
            out->push_back(path);
        }
    }
}

// One kind of a mod's files, indexed from a list of their paths (ModImages.cs's
// Scan). The list is a directory scan on the PC (scanKind) and the HD pack's
// manifest on the 3DS (ModImages::fromList), so both index by the same rule.
void indexPaths(const std::vector<std::string>& paths, const std::string& folder, bool hasLevel,
                int32_t level, std::map<std::string, std::string>* keys,
                std::map<std::string, std::string>* levelFiles,
                std::map<std::string, std::string>* names) {
    const std::string levelPath =
        hasLevel ? modPathJoin(folder, ModImages::levelFolder(level)) : "";
    for (size_t i = 0; i < paths.size(); i++) {
        std::string directory;
        std::string stem;
        splitPath(paths[i], &directory, &stem);
        const bool inLevel = hasLevel && modLower(directory) == modLower(levelPath);
        const size_t mark = stem.find('@');
        if (mark != std::string::npos) {
            const std::string key = modLower(stem.substr(mark + 1));
            if (!keys->count(key)) {
                (*keys)[key] = paths[i];
            }
        } else if (inLevel) {
            const std::string name = modLower(stem);
            if (!levelFiles->count(name)) {
                (*levelFiles)[name] = paths[i];
            }
        } else if (modLower(directory) == modLower(folder)) {
            const std::string name = modLower(stem);
            if (!names->count(name)) {
                (*names)[name] = paths[i];
            }
        }
    }
}

void scanKind(const formats::FileSystem* system, const std::string& folder, bool hasLevel,
              int32_t level, std::map<std::string, std::string>* keys,
              std::map<std::string, std::string>* levelFiles,
              std::map<std::string, std::string>* names) {
    std::vector<std::string> paths;
    collectPngs(system, folder, &paths);
    indexPaths(paths, folder, hasLevel, level, keys, levelFiles, names);
}

// Straight alpha to premultiplied, in place (ModImages.Premultiply).
void premultiply(std::vector<uint8_t>* rgba) {
    for (size_t i = 0; i + 3 < rgba->size(); i += 4) {
        const uint32_t alpha = (*rgba)[i + 3];
        for (size_t c = 0; c < 3; c++) {
            (*rgba)[i + c] = (uint8_t)(((uint32_t)(*rgba)[i + c] * alpha + 127) / 255);
        }
    }
}

} // namespace

ModImages ModImages::open(const std::vector<std::string>& mods, bool hasLevel, int32_t level) {
    ModImages images;
    const formats::FileSystem* system = formats::fileSystem();
    if (!system || !system->listDirectory) {
        return images;
    }
    for (size_t m = 0; m < mods.size(); m++) {
        std::map<Kind, Index> kinds;
        const Kind all[] = {Kind::Textures, Kind::Images};
        for (size_t k = 0; k < 2; k++) {
            Index index;
            scanKind(system, modPathJoin(mods[m], folderOf(all[k])), hasLevel, level, &index.keys,
                     &index.level, &index.names);
            kinds[all[k]] = index;
        }
        images.mods_.push_back(kinds);
    }
    return images;
}

bool ModImages::decoded(const std::string& path, HdImage* out) {
    const std::map<std::string, bool>::const_iterator known = decodedOk_.find(path);
    if (known != decodedOk_.end()) {
        if (!known->second) {
            return false;
        }
        *out = decoded_[path];
        return true;
    }

    const formats::FileSystem* system = formats::fileSystem();
    std::vector<uint8_t> bytes;
    formats::Png::Image png;
    std::string error;
    const bool ok = system && system->readFile &&
                    system->readFile(system->user, path.c_str(), &bytes) &&
                    formats::Png::decode(bytes.data(), (int32_t)bytes.size(), &png, &error);
    if (!ok) {
        fprintf(stderr, "Mod image %s: %s\n", path.c_str(),
                error.empty() ? "cannot read the file" : error.c_str());
        decodedOk_[path] = false;
        return false;
    }
    premultiply(&png.rgba);
    HdImage image;
    image.width = png.width;
    image.height = png.height;
    image.frames = 1;
    image.rgba = png.rgba;
    decoded_[path] = image;
    decodedOk_[path] = true;
    *out = image;
    return true;
}

bool ModImages::strip(const std::string& path, int32_t frameCount, HdImage* out) {
    HdImage image;
    if (!decoded(path, &image)) {
        return false;
    }
    const int32_t frames = frameCount > 0 ? frameCount : 1;
    // Divisibility by repeated subtraction, not %: this is load-time code,
    // and a runtime divisor in a % is exactly the R11 screen's candidate
    // shape -- not worth a pin on cold code when the check is this small
    // (heights are texture-bounded, the lookup happens once).
    int32_t remainder = image.height;
    while (frames > 0 && remainder >= frames) {
        remainder -= frames;
    }
    if (frames <= 0 || remainder != 0) {
        fprintf(stderr, "Mod image %s: %d rows aren't %d frames\n", path.c_str(), (int)image.height,
                (int)frames);
        return false;
    }
    image.height /= frames;
    image.frames = frames;
    *out = image;
    return true;
}

bool ModImages::frames(const std::map<std::string, std::string>& files, const std::string& name,
                       int32_t frameCount, HdImage* out) {
    const std::map<std::string, std::string>::const_iterator whole = files.find(modLower(name));
    if (whole != files.end()) {
        return strip(whole->second, frameCount, out);
    }
    if (frameCount <= 1) {
        return false;
    }
    HdImage joined;
    for (int32_t f = 0; f < frameCount; f++) {
        const std::string frameName = modLower(name + "_" + std::to_string(f));
        const std::map<std::string, std::string>::const_iterator frame = files.find(frameName);
        if (frame == files.end()) {
            return false;
        }
        HdImage one;
        if (!decoded(frame->second, &one)) {
            return false;
        }
        if (f == 0) {
            joined.width = one.width;
            joined.height = one.height;
            joined.frames = frameCount;
        } else if (one.width != joined.width || one.height != joined.height) {
            fprintf(stderr, "Mod image %s: frames of different sizes\n", name.c_str());
            return false;
        }
        joined.rgba.insert(joined.rgba.end(), one.rgba.begin(), one.rgba.end());
    }
    *out = joined;
    return true;
}

bool ModImages::find(Kind kind, const std::string& name, const formats::Texture& texture,
                     const formats::Palette& palette, HdImage* out) {
    std::string key;
    bool keyComputed = false;
    for (size_t m = 0; m < mods_.size(); m++) {
        const Index& index = mods_[m][kind];
        if (!index.keys.empty()) {
            if (!keyComputed) {
                key = hdKeyOf(texture, palette);
                keyComputed = true;
            }
            const std::map<std::string, std::string>::const_iterator exact = index.keys.find(key);
            if (exact != index.keys.end() && strip(exact->second, texture.frameCount(), out)) {
                reportFound(name);
                return true;
            }
        }
        if (frames(index.level, name, texture.frameCount(), out) ||
            frames(index.names, name, texture.frameCount(), out)) {
            reportFound(name);
            return true;
        }
    }
    return false;
}

ModImages ModImages::fromList(const std::string& root, const std::vector<std::string>& files,
                              bool hasLevel, int32_t level) {
    ModImages images;
    std::map<Kind, Index> kinds;
    const Kind all[] = {Kind::Textures, Kind::Images};
    for (size_t k = 0; k < 2; k++) {
        const std::string prefix = std::string(folderOf(all[k])) + "/";
        std::vector<std::string> paths;
        for (size_t i = 0; i < files.size(); i++) {
            if (files[i].compare(0, prefix.size(), prefix) == 0) {
                paths.push_back(modPathJoin(root, files[i]));
            }
        }
        Index index;
        indexPaths(paths, modPathJoin(root, folderOf(all[k])), hasLevel, level, &index.keys,
                   &index.level, &index.names);
        kinds[all[k]] = index;
    }
    images.mods_.push_back(kinds);
    images.compressed_ = true;
    return images;
}

bool ModImages::resolve(Kind kind, const std::string& name, const formats::Texture& texture,
                        const formats::Palette& palette, std::string* path) {
    std::string key;
    bool keyComputed = false;
    for (size_t m = 0; m < mods_.size(); m++) {
        const Index& index = mods_[m][kind];
        if (!index.keys.empty()) {
            if (!keyComputed) {
                key = hdKeyOf(texture, palette);
                keyComputed = true;
            }
            const std::map<std::string, std::string>::const_iterator exact = index.keys.find(key);
            if (exact != index.keys.end()) {
                *path = exact->second;
                reportFound(name);
                return true;
            }
        }
        const std::string lower = modLower(name);
        std::map<std::string, std::string>::const_iterator it = index.level.find(lower);
        if (it == index.level.end()) {
            it = index.names.find(lower);
            if (it == index.names.end()) {
                continue;
            }
        }
        *path = it->second;
        reportFound(name);
        return true;
    }
    return false;
}

bool ModImages::texture(const std::string& name, const formats::Texture& texture,
                        const formats::Palette& palette, HdImage* out) {
    return find(Kind::Textures, name, texture, palette, out);
}

bool ModImages::image(const std::string& name, const formats::Texture& texture,
                      const formats::Palette& palette, HdImage* out) {
    return find(Kind::Images, name, texture, palette, out);
}

} // namespace mdk

#pragma once

// ============================================================================
// packs_3ds.h -- the compressed texture packs at level load, and the surface
// cache's lookup into them.
//
//   * the HD pack (sdmc:/3ds/mdk/hd, docs/HD_TEXTURES_3DS_SPEC.md), indexed by
//     its manifest and found BY NAME through the orig pack's byname.txt;
//   * the orig pack (romfs:/orig): the original look pre-encoded on the host;
//   * Kurt's compressed frames (romfs:/kurt/kurt.pak).
// Cold: everything here runs at level load, except the lookup, which the
// surface cache asks once per (texture, palette) pair.
// ============================================================================

#include <stdint.h>

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "formats/kurt_pack.h"
#include "game/enhanced_config.h"
#include "game/kurt_sprite.h"
#include "game/level_data.h"
#include "game/mod_images.h"
#include "hd_window_3ds.h"
#include "render/surface_cache.h"

namespace mdk {

struct LoadingScreen;

extern const char kHdPackRoot[];
extern const char kKurtPackPath[];
extern const char kOrigPackRoot[];

// The name-keyed orig index (loadOrigByName): per (palette, lower-case name),
// the orig file, its HD file and the original texture's size.
struct OrigByName {
    std::map<std::pair<const void*, std::string>, std::string> paths;
    std::map<std::pair<const void*, std::string>, std::string> hdPaths;
    std::map<std::pair<const void*, std::string>, uint32_t> dims;
    // Every (arena, HD file) row the index pairs, for the HD window's per-arena
    // lists (buildHdWindow3ds).
    std::vector<std::pair<std::string, std::string>> hdArenaRows;
};

// Kurt's pack at level load (loadKurtPack).
struct KurtPackLoad {
    mdk::formats::KurtPack pack;
    // The pack's bytes [kHeaderBytes, startEnd): the index, then the start set.
    // A staging buffer: freed as soon as the start set is imported.
    std::vector<uint8_t> front;
    int32_t opened = 0;
    int32_t bytesRead = 0;
};

// What the surface cache's lookup (hdPackLookup) reads: the packs, the name
// index, and the answer's path storage.
struct HdPackSource {
    mdk::ModImages* pack = nullptr;
    mdk::ModImages* orig = nullptr;
    OrigByName* byname = nullptr;
    std::string path;
    std::string hdPath;
    // Set by the warms around their ask (requestPackedSurface): the ask is
    // a HINT, and a hint never brings a texture's pixels back (below).
    bool hint = false;
    // The names whose lookup missed both packs this level (hdPackLookup's log).
    std::set<std::string> missLogged;
};

// The warms' ask (game/object_view.h warmObjectStep, EffectsView::warmStep): a
// hint only, and only for a surface the orig pack holds
// (SurfaceCache::requestPacked) -- any other texture is still made by its first
// draw, as before. The lookup is told it is a hint (HdPackSource::hint: no pixel
// fetch for it).
struct WarmAsk {
    mdk::SurfaceCache* surfaces = nullptr;
    HdPackSource* source = nullptr;
};

// The HD pack's manifest for `level`: the ModImages index, every texture file
// by its content key, and every file's .t3x size and format.
bool loadHdPack(int32_t level, ModImages* out, std::map<std::string, std::string>* byKey,
                std::map<std::string, uint32_t>* shapes);
// The bytes an HD file holds imported with its levels over `cap` dropped (0: none).
uint32_t hdChainBytes(uint32_t shape, int32_t cap);
// The orig pack's manifest for `level`, and each file's source size.
bool loadOrigPack(int32_t level, ModImages* out, std::map<std::string, uint32_t>* dims);
// romfs:/orig/byname.txt for `level`, joined with the HD pack by content key.
bool loadOrigByName(int32_t level, LevelData& levelData,
                    const std::map<std::string, std::string>& hdByKey,
                    const std::map<std::string, uint32_t>& origDims, OrigByName* out);
// Kurt's pack: its header, index and start set in one read, checked.
bool loadKurtPack(const char* path, KurtPackLoad* out);
// The surface cache's lookup (SurfaceCache::setColourReplacementLookup).
bool hdPackLookup(void* user, const Surface& surface, const formats::Palette* palette,
                  ColourReplacement* out);
// The warms' asks (warmObjectStep, EffectsView::warmStep); `user` is a WarmAsk.
void requestPackedSurface(void* user, const Surface& surface, const formats::Palette* palette);
void startEffectSurface(void* user, const Surface& surface, const formats::Palette* palette);
// Kurt's compressed frames for `level`: the pack's start set imported (when
// toggles.kurtCompressed and the pack reads), the rest left to stream. Logged.
void loadKurtFrames3ds(KurtSprite* kurtSprite, const EnhancedToggles& toggles, int32_t level);
// THE HD WINDOW's level-load half (hd_window_3ds.h): every arena's HD files with
// their bytes at each tier, the neighbours the level names, and each file's cap
// chosen so every arena's window fits the free linear heap less the play
// reserve. Returns the largest window's bytes (0: no HD for this level). Logged.
uint32_t buildHdWindow3ds(int32_t level, const OrigByName& origByName,
                          const std::map<std::string, uint32_t>& hdShapes,
                          const LevelData& levelData, const formats::Cmi* cmi, HdWindow* out);
// Wait (the applet loop served; the loading screen, the pause menu or the last
// frame stays up) until none of `paths` -- null: the whole window -- is still to
// come. `why` names the moment. With `loading` (null otherwise) its bar goes
// from `from` to full as the files land. Returns the milliseconds waited.
double holdHdWindow3ds(MdkRenderer renderer, const char* const* paths, int32_t count,
                       const char* why, const LoadingScreen* loading = nullptr,
                       float from = 0.0f, uint32_t frameIndex = 0);

} // namespace mdk

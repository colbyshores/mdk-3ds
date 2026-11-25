// ============================================================================
// mods_tests.cpp -- the mod system (p6 slice g): SHA-256, HdKey, ModInfo,
// ModCatalog, ModImages against fixture trees on the host file system.
//
// The hash vectors are the FIPS 180-4 examples and a HdKey value computed
// independently of this code (python3 hashlib over the HdKey.cs byte
// layout), so the tests pin the oracle's bytes, not this port's habits.
// ============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <string>
#include <vector>

#include "formats/palette.h"
#include "formats/png.h"
#include "formats/texture.h"
#include "game/hd_key.h"
#include "game/mod_catalog.h"
#include "game/mod_images.h"
#include "game/mod_info.h"
#include "formats/glb.h"
#include "formats/model.h"
#include "game/mod_models.h"
#include "game/part_fit.h"
#include "render/material_resolver.h"
#include "render/surface_cache.h"
#include "test_support.h"
#include "util/sha256.h"

// The surface cache's seam calls, recorded by render_seam_stubs.cpp.
extern int mdk_test_replaceCalls;
extern int32_t mdk_test_replaceTexture;
extern int32_t mdk_test_replacePalette;
extern int32_t mdk_test_replaceFrames;
extern int32_t mdk_test_replaceWidth;
extern int32_t mdk_test_replaceHeight;
extern int mdk_test_replaceFirstByte;
extern int mdk_test_compressedCalls;
extern int32_t mdk_test_compressedTexture;
extern std::string mdk_test_compressedPath;
// The packed original's side (render_seam_stubs.cpp).
extern MdkTextureId mdk_test_compressedFileId;
extern int mdk_test_compressedFileCalls;
extern MdkTextureId mdk_test_needsPixels;
extern std::string mdk_test_compressedFileHd;
extern int mdk_test_updateCalls;
extern MdkTextureId mdk_test_updateTexture;
extern int32_t mdk_test_updateWidth;
extern int32_t mdk_test_updateHeight;
extern int mdk_test_prepareCalls;
extern int mdk_test_indexTextures;
extern int mdk_test_texturesReleased;

namespace {

std::string g_root;

void makeDirs(const std::string& path) {
    std::string at;
    for (size_t i = 0; i < path.size(); i++) {
        at += path[i];
        if (path[i] == '/' && at.size() > 1) {
            mkdir(at.c_str(), 0755);
        }
    }
    mkdir(path.c_str(), 0755);
}

void writeBytes(const std::string& path, const std::vector<uint8_t>& bytes) {
    makeDirs(path.substr(0, path.find_last_of('/')));
    FILE* file = fopen(path.c_str(), "wb");
    if (file) {
        if (!bytes.empty()) {
            fwrite(bytes.data(), 1, bytes.size(), file);
        }
        fclose(file);
    }
}

void writeText(const std::string& path, const std::string& text) {
    writeBytes(path, std::vector<uint8_t>(text.begin(), text.end()));
}

void writePng(const std::string& path, int32_t width, int32_t height,
              const std::vector<uint8_t>& rgba) {
    std::vector<uint8_t> bytes;
    std::string error;
    if (!mdk::formats::Png::encode(width, height, rgba.data(), (int32_t)rgba.size(),
                                   mdk::formats::Png::Channels::Rgba, &bytes, &error)) {
        std::printf("  fixture PNG %s failed: %s\n", path.c_str(), error.c_str());
    }
    writeBytes(path, bytes);
}

std::vector<uint8_t> solid(int32_t width, int32_t height, uint8_t r, uint8_t g, uint8_t b,
                           uint8_t a) {
    std::vector<uint8_t> rgba((size_t)width * (size_t)height * 4);
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
        rgba[i] = r;
        rgba[i + 1] = g;
        rgba[i + 2] = b;
        rgba[i + 3] = a;
    }
    return rgba;
}

// The HdKey fixture: a 2x2 texture, indices [0,1,2,1], through a palette
// whose index 1 is (10,20,30) and index 2 is (40,50,60).
mdk::formats::Texture keyTexture() {
    mdk::formats::Texture texture;
    texture.setName("WALL");
    texture.setWidth(2);
    texture.setHeight(2);
    texture.setFrameCount(1);
    const uint8_t indices[] = {0, 1, 2, 1};
    texture.setIndices(indices, 4);
    return texture;
}

mdk::formats::Palette keyPalette() {
    mdk::formats::Palette palette;
    uint8_t* rgba = palette.mutableRgba();
    rgba[1 * 4 + 0] = 10; rgba[1 * 4 + 1] = 20; rgba[1 * 4 + 2] = 30; rgba[1 * 4 + 3] = 255;
    rgba[2 * 4 + 0] = 40; rgba[2 * 4 + 1] = 50; rgba[2 * 4 + 2] = 60; rgba[2 * 4 + 3] = 255;
    return palette;
}

std::string hexOf(const uint8_t* bytes, size_t count) {
    static const char* const kHex = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < count; i++) {
        out += kHex[bytes[i] >> 4];
        out += kHex[bytes[i] & 0xF];
    }
    return out;
}

} // namespace

MDK_TEST(sha256MatchesStandardVectors) {
    uint8_t digest[32];
    mdk::Sha256 empty;
    empty.finish(digest);
    MDK_CHECK_STR_EQ(hexOf(digest, 32),
                     "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    mdk::Sha256 abc;
    abc.update((const uint8_t*)"ab", 2);
    abc.update((const uint8_t*)"c", 1);
    abc.finish(digest);
    MDK_CHECK_STR_EQ(hexOf(digest, 32),
                     "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

MDK_TEST(hdKeyMatchesIndependentVector) {
    // Computed with python3 hashlib over the HdKey.cs layout, not by this
    // code: header(2,2,1) + indices + used-index colours.
    MDK_CHECK_STR_EQ(mdk::hdKeyOf(keyTexture(), keyPalette()), "a3d10be8e07dc5fc");

    // A palette change in a colour the texture does not use keeps the key;
    // in one it does use, the key changes.
    mdk::formats::Palette palette = keyPalette();
    palette.mutableRgba()[7 * 4 + 0] = 99;
    MDK_CHECK_STR_EQ(mdk::hdKeyOf(keyTexture(), palette), "a3d10be8e07dc5fc");
    palette.mutableRgba()[1 * 4 + 0] = 11;
    MDK_CHECK(mdk::hdKeyOf(keyTexture(), palette) != "a3d10be8e07dc5fc");
}

MDK_TEST(modInfoParsesAndFormats) {
    const mdk::ModInfo info = mdk::ModInfo::parse(
        "clean-walls", "# a comment\nname=Clean walls\nauthor = Me \npriority=10\nversion=1.0\n");
    MDK_CHECK_STR_EQ(info.folder, "clean-walls");
    MDK_CHECK_STR_EQ(info.name, "Clean walls");
    MDK_CHECK_STR_EQ(info.author, "Me");
    MDK_CHECK_EQ(info.priority, 10);

    const mdk::ModInfo bare = mdk::ModInfo::parse("bare", "priority=high\n");
    MDK_CHECK_STR_EQ(bare.name, "bare");
    MDK_CHECK_EQ(bare.priority, 0);

    const mdk::ModInfo roundTrip = mdk::ModInfo::parse("x", info.format());
    MDK_CHECK_STR_EQ(roundTrip.name, "Clean walls");
    MDK_CHECK_EQ(roundTrip.priority, 10);
}

MDK_TEST(modCatalogOrdersAndEnables) {
    const std::string root = g_root + "/catalog/mods";
    writeText(root + "/beta/mod.txt", "name=Beta\npriority=10\n");
    writeText(root + "/gamma/mod.txt", "priority=10\n");
    makeDirs(root + "/alpha");
    makeDirs(root + "/.hidden");
    writeText(root + "/notamod.txt", "priority=99\n");

    const mdk::ModCatalog catalog = mdk::ModCatalog::scan(root);
    MDK_CHECK_EQ((int)catalog.mods().size(), 3);
    MDK_CHECK_STR_EQ(catalog.mods()[0].folder, "beta");
    MDK_CHECK_STR_EQ(catalog.mods()[1].folder, "gamma");
    MDK_CHECK_STR_EQ(catalog.mods()[2].folder, "alpha");
    MDK_CHECK(catalog.has("BETA"));
    MDK_CHECK(!catalog.has("notamod.txt"));

    const std::vector<std::pair<std::string, mdk::ModState>> states = {
        {"gamma", mdk::ModState::Off}};
    const std::vector<std::string> enabled = catalog.enabledFolders(states);
    MDK_CHECK_EQ((int)enabled.size(), 2);
    MDK_CHECK_STR_EQ(enabled[0], root + "/beta");
    MDK_CHECK_STR_EQ(enabled[1], root + "/alpha");

    const std::vector<std::string> only = catalog.onlyFolders({"ALPHA"});
    MDK_CHECK_EQ((int)only.size(), 1);
    MDK_CHECK_STR_EQ(only[0], root + "/alpha");
}

MDK_TEST(modStatesParseSettingsText) {
    const std::vector<std::pair<std::string, mdk::ModState>> states =
        mdk::modStatesFromSettingsText(
            "master_volume=1\r\nmod.clean-walls=Off\nmod.hd-textures=On\n"
            "mod.bogus=Sideways\nother=Off\nmod.=Off\n");
    MDK_CHECK_EQ((int)states.size(), 2);
    MDK_CHECK_STR_EQ(states[0].first, "clean-walls");
    MDK_CHECK(states[0].second == mdk::ModState::Off);
    MDK_CHECK_STR_EQ(states[1].first, "hd-textures");
    MDK_CHECK(states[1].second == mdk::ModState::On);
}

MDK_TEST(modImagesLookupOrderAndPremultiply) {
    const std::string modA = g_root + "/images/modA";
    const std::string modB = g_root + "/images/modB";
    // A name replacement in the first mod beats everything after it; its
    // pixels come back premultiplied ((200,100,50,128) -> (100,50,25,128)).
    writePng(modA + "/textures/WALL.png", 2, 2, solid(2, 2, 200, 100, 50, 128));
    writePng(modB + "/textures/LEVEL3/WALL.png", 2, 2, solid(2, 2, 1, 2, 3, 255));
    writePng(modA + "/images/SC_STAT.png", 1, 1, solid(1, 1, 9, 8, 7, 255));

    mdk::ModImages images = mdk::ModImages::open({modA, modB}, true, 3);
    mdk::HdImage out;
    MDK_CHECK(images.texture("WALL", keyTexture(), keyPalette(), &out));
    MDK_CHECK_EQ(out.width, 2);
    MDK_CHECK_EQ((int)out.rgba[0], 100);
    MDK_CHECK_EQ((int)out.rgba[1], 50);
    MDK_CHECK_EQ((int)out.rgba[2], 25);
    MDK_CHECK_EQ((int)out.rgba[3], 128);
    MDK_CHECK(images.texture("WALL", keyTexture(), keyPalette(), &out));
    MDK_CHECK_EQ((int)images.found().size(), 1);

    // The images kind is separate from the textures kind.
    MDK_CHECK(images.image("SC_STAT", keyTexture(), keyPalette(), &out));
    MDK_CHECK(!images.texture("SC_STAT", keyTexture(), keyPalette(), &out));
}

MDK_TEST(modImagesExactKeyBeatsNameWithinOneMod) {
    const std::string mod = g_root + "/keymod";
    writePng(mod + "/textures/WALL.png", 1, 1, solid(1, 1, 1, 1, 1, 255));
    writePng(mod + "/textures/sub/WHATEVER@a3d10be8e07dc5fc.png", 1, 1,
             solid(1, 1, 42, 43, 44, 255));

    mdk::ModImages images = mdk::ModImages::open({mod}, true, 3);
    mdk::HdImage out;
    MDK_CHECK(images.texture("WALL", keyTexture(), keyPalette(), &out));
    MDK_CHECK_EQ((int)out.rgba[0], 42);
}

MDK_TEST(modImagesLevelFolderBeatsAllLevels) {
    const std::string mod = g_root + "/levelmod";
    writePng(mod + "/textures/WALL.png", 1, 1, solid(1, 1, 1, 0, 0, 255));
    writePng(mod + "/textures/LEVEL3/WALL.png", 1, 1, solid(1, 1, 2, 0, 0, 255));

    mdk::ModImages images = mdk::ModImages::open({mod}, true, 3);
    mdk::HdImage out;
    MDK_CHECK(images.texture("WALL", keyTexture(), keyPalette(), &out));
    MDK_CHECK_EQ((int)out.rgba[0], 2);

    // Another level sees only the all-levels file.
    mdk::ModImages other = mdk::ModImages::open({mod}, true, 4);
    MDK_CHECK(other.texture("WALL", keyTexture(), keyPalette(), &out));
    MDK_CHECK_EQ((int)out.rgba[0], 1);
}

MDK_TEST(modImagesFromListResolvesByTheScansOrder) {
    // THE 3DS HD PACK'S INDEX: built from its manifest's file list, not a scan,
    // and resolved to a PATH (no decode): exact key first, then the level's
    // folder, then every level -- the same order find() decodes in.
    const std::vector<std::string> files = {
        "textures/WALL.t3x",
        "textures/LEVEL3/WALL.t3x",
        "textures/LEVEL3/FLOOR.t3x",
        "textures/LEVEL3/WHATEVER@a3d10be8e07dc5fc.t3x",
        "images/SC_STAT.t3x",
    };
    mdk::ModImages pack = mdk::ModImages::fromList("sdmc:/3ds/mdk/hd", files, true, 3);
    std::string path;
    // The exact key wins over the name, wherever it sits.
    MDK_CHECK(pack.resolve(mdk::ModImages::Kind::Textures, "WALL", keyTexture(), keyPalette(),
                           &path));
    MDK_CHECK_STR_EQ(path, "sdmc:/3ds/mdk/hd/textures/LEVEL3/WHATEVER@a3d10be8e07dc5fc.t3x");
    // A texture with another key: the level's folder, then every level.
    mdk::formats::Palette other = keyPalette();
    other.mutableRgba()[1 * 4 + 0] ^= 0xFF;   // a palette the key covers -> another key
    MDK_CHECK(pack.resolve(mdk::ModImages::Kind::Textures, "WALL", keyTexture(), other, &path));
    MDK_CHECK_STR_EQ(path, "sdmc:/3ds/mdk/hd/textures/LEVEL3/WALL.t3x");
    mdk::ModImages level4 = mdk::ModImages::fromList("sdmc:/3ds/mdk/hd", files, true, 4);
    MDK_CHECK(level4.resolve(mdk::ModImages::Kind::Textures, "WALL", keyTexture(), other, &path));
    MDK_CHECK_STR_EQ(path, "sdmc:/3ds/mdk/hd/textures/WALL.t3x");
    MDK_CHECK(!level4.resolve(mdk::ModImages::Kind::Textures, "FLOOR", keyTexture(), other, &path));
    // The kinds stay apart.
    MDK_CHECK(pack.resolve(mdk::ModImages::Kind::Images, "SC_STAT", keyTexture(), other, &path));
    MDK_CHECK(!pack.resolve(mdk::ModImages::Kind::Textures, "SC_STAT", keyTexture(), other, &path));
    // A list-built index is a COMPRESSED pack: its callers (KurtSprite, HudView)
    // resolve paths for the compressed seams instead of decoding. A scanned one
    // is not.
    MDK_CHECK(pack.compressed());
    MDK_CHECK(!mdk::ModImages::open({g_root + "/keymod"}, true, 3).compressed());
    MDK_CHECK(!mdk::ModImages().compressed());
}

namespace {

const char* const kHookPath = "sdmc:/3ds/mdk/hd/textures/LEVEL3/WALL.t3x";

bool compressedLookup(void*, const mdk::Surface& surface, const mdk::formats::Palette*,
                      mdk::ColourReplacement* out) {
    if (surface.name != "WALL") {
        return false;
    }
    out->compressedPath = kHookPath;
    return true;
}

} // namespace

MDK_TEST(surfaceCacheHandsACompressedReplacementToItsSeam) {
    mdk_test_replaceCalls = 0;
    mdk_test_compressedCalls = 0;
    mdk::SurfaceCache cache((MdkRenderer)(void*)0x1);
    cache.setColourReplacementLookup(compressedLookup, nullptr);
    mdk::formats::Texture texture = keyTexture();
    mdk::formats::Palette palette = keyPalette();
    mdk::Surface lit;
    lit.texture = &texture;
    lit.name = "WALL";
    lit.shading = mdk::Shading::Lit;
    lit.frameCount = 1;
    cache.surfaceId(lit, &palette);
    MDK_CHECK_EQ(mdk_test_compressedCalls, 1);
    MDK_CHECK_EQ(mdk_test_replaceCalls, 0);   // not the decoded path
    MDK_CHECK_EQ(mdk_test_compressedTexture, 0);
    MDK_CHECK_STR_EQ(mdk_test_compressedPath, kHookPath);
    MDK_CHECK_EQ(cache.replacementFound(), 1);
    cache.surfaceId(lit, &palette);
    MDK_CHECK_EQ(mdk_test_compressedCalls, 1);   // once per pair
}

namespace {

// THE ORIG PACK'S HIT (main_3ds.cpp's lookup): the original look, asked for from
// its file instead of expanded.
const char* const kOrigPath = "romfs:/orig/textures/LEVEL3/WALL.t3x";
const MdkTextureId kPendingId = 42;

bool origLookup(void*, const mdk::Surface& surface, const mdk::formats::Palette*,
                mdk::ColourReplacement* out) {
    if (surface.name != "WALL") {
        return false;
    }
    out->compressedPath = kOrigPath;
    out->compressedOriginal = true;
    return true;
}

// WALL with two stacked frames, so the fallback's height (h x frames) is tested.
mdk::formats::Texture origTexture() {
    mdk::formats::Texture texture = keyTexture();
    texture.setFrameCount(2);
    const uint8_t indices[] = {0, 1, 2, 1, 1, 2, 0, 1};
    texture.setIndices(indices, 8);
    return texture;
}

mdk::Surface origSurface(const mdk::formats::Texture* texture) {
    mdk::Surface lit;
    lit.texture = texture;
    lit.name = "WALL";
    lit.shading = mdk::Shading::Lit;
    lit.frameCount = 2;
    return lit;
}

// The stubs' packed-original state, as a fresh test wants it: the backend hands
// out the pending id, nothing wants pixels, nothing counted yet.
void resetPackedStubs() {
    mdk_test_compressedFileId = kPendingId;
    mdk_test_compressedFileCalls = 0;
    mdk_test_needsPixels = -1;
    mdk_test_updateCalls = 0;
    mdk_test_updateTexture = -1;
    mdk_test_updateWidth = 0;
    mdk_test_updateHeight = 0;
    mdk_test_prepareCalls = 0;
    mdk_test_indexTextures = 0;
    mdk_test_compressedCalls = 0;
}

} // namespace

// The backend asks for the file and hands back a PENDING id (no I/O at the call):
// the cache keeps that id, makes no index texture, links no HD copy, and asks
// the backend once per pair.
MDK_TEST(surfaceCacheKeepsAPackedOriginalsPendingId) {
    resetPackedStubs();
    mdk::SurfaceCache cache((MdkRenderer)(void*)0x1);
    cache.setColourReplacementLookup(origLookup, nullptr);
    mdk::formats::Texture texture = origTexture();
    mdk::formats::Palette palette = keyPalette();
    const mdk::Surface lit = origSurface(&texture);
    MDK_CHECK_EQ(cache.surfaceId(lit, &palette), kPendingId);
    MDK_CHECK_EQ(mdk_test_compressedFileCalls, 1);
    MDK_CHECK_EQ(mdk_test_indexTextures, 0);    // the palette is never expanded
    MDK_CHECK_EQ(mdk_test_compressedCalls, 0);  // not an HD copy of anything
    MDK_CHECK_EQ(cache.replacementFound(), 1);
    MDK_CHECK_EQ(cache.surfaceId(lit, &palette), kPendingId);
    MDK_CHECK_EQ(cache.prepare(lit, &palette), kPendingId);
    MDK_CHECK_EQ(mdk_test_compressedFileCalls, 1);   // once per pair
    MDK_CHECK_EQ(mdk_test_indexTextures, 0);
    MDK_CHECK_EQ(mdk_test_updateCalls, 0);           // nothing wanted pixels
    mdk_test_compressedFileId = -1;
}

// A file the backend could not make (mdk_render_texture_needs_pixels): the next
// lookup hands the index pixels over once -- w x (h x frames), the size an index
// texture is made with -- under the SAME id, and asks for it to be prepared.
MDK_TEST(surfaceCacheGivesAFailedPackedOriginalItsIndexPixels) {
    resetPackedStubs();
    mdk::SurfaceCache cache((MdkRenderer)(void*)0x1);
    cache.setColourReplacementLookup(origLookup, nullptr);
    mdk::formats::Texture texture = origTexture();
    mdk::formats::Palette palette = keyPalette();
    const mdk::Surface lit = origSurface(&texture);
    MDK_CHECK_EQ(cache.surfaceId(lit, &palette), kPendingId);
    mdk_test_prepareCalls = 0;
    mdk_test_needsPixels = kPendingId;
    MDK_CHECK_EQ(cache.surfaceId(lit, &palette), kPendingId);
    MDK_CHECK_EQ(mdk_test_updateCalls, 1);
    MDK_CHECK_EQ(mdk_test_updateTexture, kPendingId);
    MDK_CHECK_EQ(mdk_test_updateWidth, 2);
    MDK_CHECK_EQ(mdk_test_updateHeight, 4);
    MDK_CHECK_EQ(mdk_test_prepareCalls, 1);
    MDK_CHECK_EQ(mdk_test_indexTextures, 0);   // the same id, no second texture
    // The backend took them (it converts the texture, needs_pixels goes false):
    // nothing more is handed over.
    mdk_test_needsPixels = -1;
    MDK_CHECK_EQ(cache.surfaceId(lit, &palette), kPendingId);
    MDK_CHECK_EQ(mdk_test_updateCalls, 1);
    MDK_CHECK_EQ(mdk_test_compressedFileCalls, 1);
    mdk_test_compressedFileId = -1;
}

// The pending id is an ordinary id to the cache's other paths: a bullet hole's
// refresh reaches it, and an arena's release gives it back (after which a draw
// asks the backend for the file again).
MDK_TEST(surfaceCacheRefreshesAndReleasesAPackedOriginal) {
    resetPackedStubs();
    mdk::SurfaceCache cache((MdkRenderer)(void*)0x1);
    cache.setColourReplacementLookup(origLookup, nullptr);
    mdk::formats::Texture texture = origTexture();
    mdk::formats::Palette palette = keyPalette();
    const mdk::Surface lit = origSurface(&texture);
    MDK_CHECK_EQ(cache.surfaceId(lit, &palette), kPendingId);
    MDK_CHECK_EQ(cache.refresh(&texture), 1);
    MDK_CHECK_EQ(mdk_test_updateTexture, kPendingId);
    MDK_CHECK_EQ(mdk_test_updateHeight, 4);
    const int releasedBefore = mdk_test_texturesReleased;
    MDK_CHECK_EQ(cache.paletteReleases(), 0);
    MDK_CHECK_EQ(cache.releasePalette(&palette), 1);
    MDK_CHECK_EQ(mdk_test_texturesReleased, releasedBefore + 1);
    MDK_CHECK_EQ(cache.paletteReleases(), 1);   // the object warm's sign to start over
    MDK_CHECK_EQ(cache.surfaceId(lit, &palette), kPendingId);
    MDK_CHECK_EQ(mdk_test_compressedFileCalls, 2);
    MDK_CHECK_EQ(mdk_test_indexTextures, 0);
    mdk_test_compressedFileId = -1;
}

// THE PUMP'S ASK FOR A HIDDEN SURFACE (SurfaceCache::requestPacked): a pack
// answer makes the pending id surfaceId would have made (and surfaceId then
// reuses it); any other answer makes nothing and leaves the pair to surfaceId.
MDK_TEST(surfaceCacheRequestPackedMakesOnlyAPackedOriginal) {
    resetPackedStubs();
    mdk::formats::Texture texture = origTexture();
    mdk::formats::Palette palette = keyPalette();
    const mdk::Surface lit = origSurface(&texture);
    {
        mdk::SurfaceCache cache((MdkRenderer)(void*)0x1);
        cache.setColourReplacementLookup(origLookup, nullptr);
        MDK_CHECK_EQ(cache.requestPacked(lit, &palette), kPendingId);
        MDK_CHECK_EQ(mdk_test_compressedFileCalls, 1);
        MDK_CHECK_EQ(mdk_test_indexTextures, 0);
        MDK_CHECK_EQ(cache.surfaceId(lit, &palette), kPendingId);   // the same id
        MDK_CHECK_EQ(cache.requestPacked(lit, &palette), kPendingId);
        MDK_CHECK_EQ(mdk_test_compressedFileCalls, 1);
        mdk_test_prepareCalls = 0;
        cache.prepareCached(lit, &palette);
        MDK_CHECK_EQ(mdk_test_prepareCalls, 1);
    }
    {
        // An HD-pack upscale (not a compressedOriginal): nothing made, nothing
        // marked -- surfaceId still makes the index texture AND attaches it.
        resetPackedStubs();
        mdk::SurfaceCache cache((MdkRenderer)(void*)0x1);
        cache.setColourReplacementLookup(compressedLookup, nullptr);
        MDK_CHECK_EQ(cache.requestPacked(lit, &palette), -1);
        MDK_CHECK_EQ(mdk_test_compressedFileCalls, 0);
        MDK_CHECK_EQ(mdk_test_indexTextures, 0);
        MDK_CHECK_EQ(mdk_test_compressedCalls, 0);
        mdk_test_prepareCalls = 0;
        cache.prepareCached(lit, &palette);
        MDK_CHECK_EQ(mdk_test_prepareCalls, 0);   // nothing cached to prepare
        cache.surfaceId(lit, &palette);
        MDK_CHECK_EQ(mdk_test_indexTextures, 1);
        MDK_CHECK_EQ(mdk_test_compressedCalls, 1);
    }
    {
        // A miss: nothing made; surfaceId makes the index texture, attaching
        // nothing.
        resetPackedStubs();
        mdk::SurfaceCache cache((MdkRenderer)(void*)0x1);
        cache.setColourReplacementLookup(origLookup, nullptr);
        mdk::formats::Texture other = origTexture();
        other.setName("OTHER");
        mdk::Surface miss = origSurface(&other);
        miss.name = "OTHER";
        MDK_CHECK_EQ(cache.requestPacked(miss, &palette), -1);
        MDK_CHECK_EQ(mdk_test_compressedFileCalls, 0);
        MDK_CHECK_EQ(mdk_test_indexTextures, 0);
        cache.surfaceId(miss, &palette);
        MDK_CHECK_EQ(mdk_test_indexTextures, 1);
        MDK_CHECK_EQ(mdk_test_compressedFileCalls, 0);
    }
    mdk_test_compressedFileId = -1;
}

MDK_TEST(modImagesFramesStripAndPerFrame) {
    const std::string mod = g_root + "/framesmod";
    writePng(mod + "/textures/ANIM.png", 1, 2, solid(1, 2, 5, 6, 7, 255));
    writePng(mod + "/textures/SPLIT_0.png", 1, 1, solid(1, 1, 10, 0, 0, 255));
    writePng(mod + "/textures/SPLIT_1.png", 1, 1, solid(1, 1, 20, 0, 0, 255));

    mdk::formats::Texture anim = keyTexture();
    anim.setName("ANIM");
    anim.setFrameCount(2);
    const uint8_t indices[] = {0, 1, 2, 1, 0, 1, 2, 1};
    anim.setIndices(indices, 8);

    mdk::ModImages images = mdk::ModImages::open({mod}, true, 3);
    mdk::HdImage out;
    MDK_CHECK(images.texture("ANIM", anim, keyPalette(), &out));
    MDK_CHECK_EQ(out.frames, 2);
    MDK_CHECK_EQ(out.height, 1);

    anim.setName("SPLIT");
    MDK_CHECK(images.texture("SPLIT", anim, keyPalette(), &out));
    MDK_CHECK_EQ(out.frames, 2);
    MDK_CHECK_EQ((int)out.rgba[0], 10);
    MDK_CHECK_EQ((int)out.rgba[4], 20);
}

namespace {

// The replacement lookup for the hook case: an image for WALL only.
const uint8_t kHookPixels[] = {77, 0, 0, 255};

bool hookLookup(void*, const mdk::Surface& surface, const mdk::formats::Palette*,
                mdk::ColourReplacement* out) {
    if (surface.name != "WALL") {
        return false;
    }
    out->width = 1;
    out->height = 1;
    out->frames = 1;
    out->rgba = kHookPixels;
    return true;
}

} // namespace

MDK_TEST(surfaceCacheReplacesLitSurfacesOncePerPair) {
    mdk_test_replaceCalls = 0;
    mdk::SurfaceCache cache((MdkRenderer)(void*)0x1);
    cache.setColourReplacementLookup(hookLookup, nullptr);

    mdk::formats::Texture texture = keyTexture();
    mdk::formats::Palette palette = keyPalette();
    mdk::Surface lit;
    lit.texture = &texture;
    lit.name = "WALL";
    lit.shading = mdk::Shading::Lit;
    lit.frameCount = 1;

    cache.surfaceId(lit, &palette);
    MDK_CHECK_EQ(mdk_test_replaceCalls, 1);
    MDK_CHECK_EQ(mdk_test_replaceTexture, 0);
    MDK_CHECK_EQ(mdk_test_replacePalette, 7);
    MDK_CHECK_EQ(mdk_test_replaceFrames, 1);
    MDK_CHECK_EQ(mdk_test_replaceFirstByte, 77);
    MDK_CHECK_EQ(cache.replacementLooked(), 1);
    MDK_CHECK_EQ(cache.replacementFound(), 1);

    // The same pair again: asked once, as the C#'s _looked set does.
    cache.surfaceId(lit, &palette);
    MDK_CHECK_EQ(mdk_test_replaceCalls, 1);

    // An Original-shaded surface is never asked about.
    mdk::Surface original = lit;
    original.shading = mdk::Shading::Original;
    mdk::formats::Texture other = keyTexture();
    other.setName("OTHER");
    original.texture = &other;
    original.name = "OTHER";
    cache.surfaceId(original, &palette);
    MDK_CHECK_EQ(mdk_test_replaceCalls, 1);
    MDK_CHECK_EQ(cache.replacementLooked(), 1);
}

namespace {

mdk::Surface spriteOf(const mdk::Surface& surface) {
    mdk::Surface sprite = surface;
    sprite.shading = mdk::Shading::Sprite;
    return sprite;
}

// An HD answer (compressedLookup's) for the name WALL only, counted: a lookup
// that answers by name, as ModImages::resolve's name fallback does.
int g_namedLookups = 0;
bool namedHdLookup(void* user, const mdk::Surface& surface, const mdk::formats::Palette* palette,
                   mdk::ColourReplacement* out) {
    g_namedLookups++;
    return compressedLookup(user, surface, palette, out);
}

} // namespace

// A SPRITE TAKES A PACKED ORIGINAL (the effects' drops, frame 650 of the level-7
// walk): the pending id, no expansion, once per pair -- from its first draw or
// from the effects warm's ask -- and the pair's later Lit draw is handed the
// same id without a second ask.
MDK_TEST(surfaceCacheGivesASpriteItsPackedOriginal) {
    mdk::formats::Texture texture = origTexture();
    mdk::formats::Palette palette = keyPalette();
    const mdk::Surface lit = origSurface(&texture);
    const mdk::Surface sprite = spriteOf(lit);
    {
        resetPackedStubs();
        mdk::SurfaceCache cache((MdkRenderer)(void*)0x1);
        cache.setColourReplacementLookup(origLookup, nullptr);
        MDK_CHECK_EQ(cache.surfaceId(sprite, &palette), kPendingId);
        MDK_CHECK_EQ(mdk_test_compressedFileCalls, 1);
        MDK_CHECK_EQ(mdk_test_indexTextures, 0);   // the palette is never expanded
        MDK_CHECK_EQ(mdk_test_compressedCalls, 0);
        MDK_CHECK_EQ(cache.replacementFound(), 1);
        MDK_CHECK_EQ(cache.surfaceId(sprite, &palette), kPendingId);
        MDK_CHECK_EQ(cache.surfaceId(lit, &palette), kPendingId);
        MDK_CHECK_EQ(mdk_test_compressedFileCalls, 1);   // once per pair
        MDK_CHECK_EQ(mdk_test_indexTextures, 0);
        // A file the backend could not make: the Sprite's draw hands its index
        // pixels over under the same id, as a Lit draw's does.
        mdk_test_needsPixels = kPendingId;
        MDK_CHECK_EQ(cache.surfaceId(sprite, &palette), kPendingId);
        MDK_CHECK_EQ(mdk_test_updateCalls, 1);
        MDK_CHECK_EQ(mdk_test_updateTexture, kPendingId);
        mdk_test_needsPixels = -1;
    }
    {
        // The effects warm's ask (requestPacked for a Sprite) makes it; the
        // draw reuses it.
        resetPackedStubs();
        mdk::SurfaceCache cache((MdkRenderer)(void*)0x1);
        cache.setColourReplacementLookup(origLookup, nullptr);
        MDK_CHECK_EQ(cache.requestPacked(sprite, &palette), kPendingId);
        MDK_CHECK_EQ(cache.surfaceId(sprite, &palette), kPendingId);
        MDK_CHECK_EQ(mdk_test_compressedFileCalls, 1);
        MDK_CHECK_EQ(mdk_test_indexTextures, 0);
    }
    {
        // An Original-shaded surface still asks nothing: the expansion.
        resetPackedStubs();
        mdk::SurfaceCache cache((MdkRenderer)(void*)0x1);
        cache.setColourReplacementLookup(origLookup, nullptr);
        mdk::Surface original = lit;
        original.shading = mdk::Shading::Original;
        MDK_CHECK_EQ(cache.requestPacked(original, &palette), -1);
        MDK_CHECK_EQ(cache.surfaceId(original, &palette), 0);
        MDK_CHECK_EQ(mdk_test_compressedFileCalls, 0);
        MDK_CHECK_EQ(mdk_test_indexTextures, 1);
    }
    mdk_test_compressedFileId = -1;
}

namespace {

// THE HD RULE'S ANSWER (main_3ds.cpp hdPackLookup's name index): the packed
// original AND the HD copy of the same pair, found by name.
const char* const kHdPath = "sdmc:/3ds/mdk/hd/textures/LEVEL3/WALL@0123456789abcdef.t3x";

bool hdOnlyLookup(void*, const mdk::Surface& surface, const mdk::formats::Palette*,
                  mdk::ColourReplacement* out) {
    if (surface.name != "WALL") {
        return false;
    }
    out->compressedPath = kOrigPath;
    out->compressedOriginal = true;
    out->hdPath = kHdPath;
    return true;
}

} // namespace

// AN HD-ONLY PAIR IS MADE FROM THE TWO FILES ALONE: one create_compressed_file
// carrying the HD path -- no index texture, no HD copy attached to an expansion,
// nothing counted as made from SD -- for a Lit draw, a Sprite draw, and a warm's
// hint alike, once per pair. The backend decides per draw which file it reads.
MDK_TEST(surfaceCacheMakesAnHdOnlyPairFromItsFilesAlone) {
    mdk::formats::Texture texture = keyTexture();
    mdk::formats::Palette palette = keyPalette();
    mdk::Surface lit;
    lit.texture = &texture;
    lit.name = "WALL";
    lit.shading = mdk::Shading::Lit;
    lit.frameCount = 1;
    const mdk::Surface sprite = spriteOf(lit);
    const mdk::Surface* firsts[3] = {&lit, &sprite, &lit};
    for (int way = 0; way < 3; way++) {
        resetPackedStubs();
        mdk_test_compressedFileHd.clear();
        mdk::SurfaceCache cache((MdkRenderer)(void*)0x1);
        cache.setColourReplacementLookup(hdOnlyLookup, nullptr);
        // way 2: the warm's hint (requestPacked) first, then the draw.
        const MdkTextureId first = way == 2 ? cache.requestPacked(lit, &palette)
                                            : cache.surfaceId(*firsts[way], &palette);
        MDK_CHECK_EQ(first, kPendingId);
        MDK_CHECK_EQ(mdk_test_compressedFileCalls, 1);
        MDK_CHECK_STR_EQ(mdk_test_compressedFileHd, kHdPath);
        MDK_CHECK_EQ(cache.surfaceId(lit, &palette), kPendingId);
        MDK_CHECK_EQ(cache.surfaceId(sprite, &palette), kPendingId);
        MDK_CHECK_EQ(mdk_test_compressedFileCalls, 1);   // once per pair
        MDK_CHECK_EQ(mdk_test_indexTextures, 0);         // never expanded
        MDK_CHECK_EQ(mdk_test_compressedCalls, 0);       // never attached to one
        MDK_CHECK_EQ(cache.sourceStats().hdAnsweredSd, 0);
    }
    mdk_test_compressedFileId = -1;
}

// AND NOTHING ELSE. An HD-pack upscale or a decoded image is not a Sprite's: the
// Sprite gets the plain expansion with nothing attached, and the pair's Lit draw
// still asks and attaches its replacement to that same texture -- as when a
// Sprite drew the pair first before Sprites asked at all. A Sprite's MISS marks
// nothing either: the Lit draw, named differently, still finds its answer.
MDK_TEST(surfaceCacheKeepsHdAndDecodedReplacementsLitOnly) {
    mdk::formats::Texture texture = keyTexture();
    mdk::formats::Palette palette = keyPalette();
    mdk::Surface lit;
    lit.texture = &texture;
    lit.name = "WALL";
    lit.shading = mdk::Shading::Lit;
    lit.frameCount = 1;
    const mdk::Surface sprite = spriteOf(lit);
    {
        resetPackedStubs();
        mdk::SurfaceCache cache((MdkRenderer)(void*)0x1);
        cache.setColourReplacementLookup(compressedLookup, nullptr);
        MDK_CHECK_EQ(cache.surfaceId(sprite, &palette), 0);
        MDK_CHECK_EQ(mdk_test_indexTextures, 1);
        MDK_CHECK_EQ(mdk_test_compressedCalls, 0);   // no HD copy on the Sprite
        MDK_CHECK_EQ(mdk_test_compressedFileCalls, 0);
        MDK_CHECK_EQ(cache.replacementFound(), 0);
        MDK_CHECK_EQ(cache.replacementLooked(), 0);  // not marked
        MDK_CHECK_EQ(cache.surfaceId(lit, &palette), 0);
        MDK_CHECK_EQ(mdk_test_indexTextures, 1);     // one texture for the pair
        MDK_CHECK_EQ(mdk_test_compressedCalls, 1);   // the Lit draw's HD copy
        MDK_CHECK_EQ(mdk_test_compressedTexture, 0);
        MDK_CHECK_EQ(cache.replacementFound(), 1);
        // Both made the pair from its standard-definition pixels though an HD
        // copy was the answer: what the 3DS's hdrule: line counts.
        MDK_CHECK_EQ(cache.sourceStats().hdAnsweredSd, 2);
    }
    {
        mdk_test_replaceCalls = 0;
        resetPackedStubs();
        mdk::SurfaceCache cache((MdkRenderer)(void*)0x1);
        cache.setColourReplacementLookup(hookLookup, nullptr);
        cache.surfaceId(sprite, &palette);
        MDK_CHECK_EQ(mdk_test_replaceCalls, 0);   // no decoded image on the Sprite
        cache.surfaceId(lit, &palette);
        MDK_CHECK_EQ(mdk_test_replaceCalls, 1);
        MDK_CHECK_EQ(mdk_test_replaceTexture, 0);
        MDK_CHECK_EQ(mdk_test_indexTextures, 1);
    }
    {
        resetPackedStubs();
        g_namedLookups = 0;
        mdk::SurfaceCache cache((MdkRenderer)(void*)0x1);
        cache.setColourReplacementLookup(namedHdLookup, nullptr);
        mdk::Surface named = sprite;
        named.name = "SL_FX";
        cache.surfaceId(named, &palette);
        MDK_CHECK_EQ(g_namedLookups, 1);
        cache.surfaceId(named, &palette);
        MDK_CHECK_EQ(g_namedLookups, 1);   // made: asked no more
        MDK_CHECK_EQ(cache.replacementLooked(), 0);
        cache.surfaceId(lit, &palette);
        MDK_CHECK_EQ(g_namedLookups, 2);
        MDK_CHECK_EQ(mdk_test_compressedCalls, 1);
        MDK_CHECK_EQ(mdk_test_indexTextures, 1);
    }
}

namespace {

// A model with one named part: a tetrahedron at the origin.
mdk::formats::Model swapModel() {
    mdk::formats::Model model;
    model.name = "TEST";
    mdk::formats::Model::Part part;
    std::snprintf(part.name, sizeof(part.name), "BODY");
    part.vertices = {{0, 0, 0}, {2, 0, 0}, {0, 2, 0}, {0, 0, 2}};
    part.triangleIndices = {0, 1, 2, 0, 1, 3, 0, 2, 3, 1, 2, 3};
    part.triangleMaterials = {0, 0, 0, 0};
    model.partList.push_back(part);
    mdk::formats::Model::Material material;
    std::snprintf(material.name, sizeof(material.name), "SKIN");
    model.materials.push_back(material);
    return model;
}

// The same tetrahedron as a glTF scene (Y up), one node named BODY.
mdk::formats::Glb::Scene swapScene() {
    mdk::formats::Glb::Scene scene;
    mdk::formats::Glb::Mesh mesh;
    mesh.name = "BODY";
    mdk::formats::Glb::Primitive primitive;
    primitive.positions = {0, 0, 0, 2, 0, 0, 0, 0, -2, 0, 2, 0};
    primitive.uvs = {0, 0, 1, 0, 0, 1, 1, 1};
    primitive.indices = {0, 1, 2, 0, 3, 1};
    primitive.material = 0;
    mesh.primitives.push_back(primitive);
    scene.meshes.push_back(mesh);
    mdk::formats::Glb::Mesh stray;
    stray.name = "NOPE";
    stray.primitives.push_back(primitive);
    scene.meshes.push_back(stray);
    mdk::formats::Glb::Material material;
    material.name = "SKIN";
    material.colour[0] = 0.25f;
    material.colour[1] = 0.5f;
    material.colour[2] = 1.0f;
    material.colour[3] = 1.0f;
    scene.materials.push_back(material);
    return scene;
}

} // namespace

MDK_TEST(modelSwapMatchesPartsByName) {
    const mdk::formats::Model model = swapModel();
    std::vector<std::string> unknown;
    mdk::ModelSwap* swap = mdk::ModelSwap::of(swapScene(), model, &unknown);
    MDK_CHECK(swap != nullptr);
    MDK_CHECK_EQ((int)unknown.size(), 1);
    MDK_CHECK_STR_EQ(unknown[0], "NOPE");
    MDK_CHECK(swap->part(0) != nullptr);
    // glTF (0,0,-2) is MDK (0, 2, 0): the third vertex of the node.
    const mdk::SwapMesh& mesh = *swap->part(0);
    MDK_CHECK_EQ((int)mesh.vertices.size(), 4);
    MDK_CHECK(mesh.vertices[2].x == 0.0f && mesh.vertices[2].y == 2.0f &&
              mesh.vertices[2].z == 0.0f);
    MDK_CHECK_EQ((int)mesh.materials.size(), 2);
    MDK_CHECK_EQ((int)swap->surfaces().size(), 1);
    MDK_CHECK_STR_EQ(swap->surfaces()[0].name, "SKIN");
    delete swap;
}

MDK_TEST(partFitFollowsTranslationAndRotation) {
    const mdk::formats::Model model = swapModel();
    const mdk::formats::Model::Part& part = model.partList[0];
    std::vector<mdk::Vec3> rest;
    for (size_t i = 0; i < part.vertices.size(); i++) {
        rest.push_back(mdk::Vec3{part.vertices[i].x, part.vertices[i].y, part.vertices[i].z});
    }
    mdk::PartFit fit(rest, part.triangleIndices);

    // Rest pose: identity.
    float m[16];
    fit.fit(rest, m);
    MDK_CHECK(m[0] == 1.0f && m[5] == 1.0f && m[12] == 0.0f);

    // Translated by (5, -3, 2): the translation row carries it.
    std::vector<mdk::Vec3> moved = rest;
    for (size_t i = 0; i < moved.size(); i++) {
        moved[i].x += 5.0f;
        moved[i].y -= 3.0f;
        moved[i].z += 2.0f;
    }
    fit.fit(moved, m);
    MDK_CHECK(m[12] > 4.99f && m[12] < 5.01f && m[13] < -2.99f && m[13] > -3.01f &&
              m[14] > 1.99f && m[14] < 2.01f);
}

MDK_TEST(modModelsLoadsLazilyFromFixture) {
    const std::string mod = g_root + "/modelmod";
    std::vector<uint8_t> bytes;
    std::string error;
    MDK_CHECK(mdk::formats::Glb::write(swapScene(), &bytes, &error));
    writeBytes(mod + "/models/TEST.glb", bytes);

    mdk::ModModels models(std::vector<std::string>{mod}, 3);
    const mdk::formats::Model model = swapModel();
    const mdk::ModelSwap* swap = models.of(model);
    MDK_CHECK(swap != nullptr);
    MDK_CHECK(swap->part(0) != nullptr);
    MDK_CHECK_EQ((int)models.found().size(), 1);
    MDK_CHECK_STR_EQ(models.found()[0], "TEST");
    // Asked again: the same swap, not a second read.
    MDK_CHECK(models.of(model) == swap);
}

// The fixture root, made once for the file-backed cases above.
struct ModsFixture {
    ModsFixture() {
        char path[] = "/tmp/mdk-mods-test-XXXXXX";
        if (mkdtemp(path)) {
            g_root = path;
        }
    }
};
static ModsFixture g_fixture;

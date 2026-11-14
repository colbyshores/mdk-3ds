// colour_tests.cpp -- the enhanced look's colour expansion
// (render/colour_mips) against the C# oracle's own bytes.
//
// Expected hashes were produced by the C# itself (ColourMips.Expand +
// FillChain over level 3's textures with the level's patched palette,
// FNV-1a 64 over the whole multi-frame chain, printed by a probe against
// the built Mdk.Engine/Mdk.Game assemblies on 2026-11-14).

#include "test_support.h"

#include "formats/case_insensitive_path.h"
#include "formats/mdk_data.h"
#include "formats/texture.h"
#include "formats/texture_archive.h"
#include "game/level_data.h"
#include "render/colour_mips.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

mdk::formats::MdkData locateData() {
    using namespace mdk::formats;
    if (const char* env = getenv("MDK_DATA_DIR")) {
        if (env[0] != '\0') {
            const FileSystem* system = fileSystem();
            const std::string marker = caseInsensitiveResolve(system, env, "TRAVERSE/TRAVSPRT.BNI");
            if (system->fileExists(system->user, marker.c_str())) {
                return MdkData(system, env);
            }
        }
    }
    const char* relatives[] = {"../../MDK", "../../../MDK", "MDK"};
    const FileSystem* system = fileSystem();
    for (size_t i = 0; i < sizeof(relatives) / sizeof(relatives[0]); i++) {
        const std::string marker =
            caseInsensitiveResolve(system, relatives[i], "TRAVERSE/TRAVSPRT.BNI");
        if (system->fileExists(system->user, marker.c_str())) {
            return MdkData(system, relatives[i]);
        }
    }
    return MdkData();
}

uint64_t fnv1a(const uint8_t* bytes, size_t count) {
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < count; i++) {
        hash = (hash ^ bytes[i]) * 1099511628211ull;
    }
    return hash;
}

void checkTexture(const mdk::LevelData& level, const char* name, int32_t frames,
                  size_t chainBytes, uint64_t expected) {
    const mdk::formats::Texture* tex = level.levelTextures().findTexture(name);
    MDK_CHECK(tex != nullptr);
    if (!tex) {
        return;
    }
    MDK_CHECK_EQ(tex->frameCount(), frames);
    const std::vector<uint8_t> chain =
        mdk::colourChain(tex->indices(), tex->width(), tex->height(), tex->frameCount(),
                         level.dti().palette().rgba(), 1024);
    MDK_CHECK_EQ(chain.size(), chainBytes);
    const uint64_t got = fnv1a(chain.data(), chain.size());
    if (got != expected) {
        std::printf("  %s chain fnv got %016llX want %016llX\n", name,
                    (unsigned long long)got, (unsigned long long)expected);
    }
    MDK_CHECK_EQ(got, expected);
}

} // namespace

MDK_TEST(ColourExpansionMatchesTheOracle) {
    const mdk::formats::MdkData game = locateData();
    if (!game.found()) {
        return;
    }
    mdk::LevelData level;
    MDK_CHECK(mdk::LevelData::load(&level, &game, 3));
    checkTexture(level, "SB_MED", 15, 81900, 0x9E3651593ED5F13Dull);
    checkTexture(level, "EXPLODE", 26, 2271880, 0x9179F875BE406221ull);
}

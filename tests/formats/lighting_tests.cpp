// lighting_tests.cpp -- the enhanced look's lighting rig (game/level_light)
// against the C# oracle's own arithmetic.
//
// The expected bits were produced by the C# itself (SkyLight.Of,
// EnhancedLook.Lighting, ArenaShape.CoverOf over level 3, printed as raw
// Single bits by a probe against the built Mdk.Game assembly on
// 2026-11-14). A float that differs in its last bit is a different rig:
// the exposure divides by a luminance, so small errors amplify.

#include "test_support.h"

#include "formats/case_insensitive_path.h"
#include "formats/dti.h"
#include "formats/fti.h"
#include "formats/mdk_data.h"
#include "formats/mto.h"
#include "game/level_data.h"
#include "game/level_light.h"
#include "render/sun_shadow.h"

#include <cstdio>
#include <stdio.h>
#include <cstdlib>
#include <cstring>

namespace {

// The data's folder, the same walk format_tests.cpp makes (kept local:
// those helpers are that file's own).
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

bool readDataFile(const std::string& relative, std::vector<uint8_t>* bytes) {
    return locateData().read(relative, bytes);
}

uint32_t bitsOf(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

void checkVec(const char* what, const mdk::Vec3& v, uint32_t x, uint32_t y, uint32_t z) {
    MDK_CHECK_EQ(bitsOf(v.x), x);
    MDK_CHECK_EQ(bitsOf(v.y), y);
    MDK_CHECK_EQ(bitsOf(v.z), z);
    if (bitsOf(v.x) != x || bitsOf(v.y) != y || bitsOf(v.z) != z) {
        std::printf("  %s: got %08X %08X %08X\n", what, bitsOf(v.x), bitsOf(v.y), bitsOf(v.z));
    }
}

struct Expected {
    const char* name;
    bool covered;
    uint32_t exposure;
    bool shadows;
};

// Level 3's arenas, from the C# probe (cover + exposure bits + shadows).
const Expected kExpected[] = {
    {"HMO_1", false, 0x3FB64CA2u, true},  {"HMO_2", true, 0x3FAC4666u, false},
    {"HMO_3", true, 0x3FAD48B4u, false},   {"HMO_4", true, 0x3FAA4E0Au, false},
    {"HMO_5", true, 0x3FACFE10u, false},   {"HMO_6", false, 0x3FBBAC99u, true},
    {"HMO_7", false, 0x3F9142AEu, true},   {"HMO_8", true, 0x3FA96E79u, false},
    {"HMO_9", false, 0x3FA972B2u, true},   {"HMO_10", true, 0x3FAD5226u, false},
    {"CHMO_1", true, 0x3FAC4989u, false},  {"CHMO_2", true, 0x3FAD1628u, false},
    {"CHMO_4", true, 0x3FAC99F7u, false},  {"CHMO_5", true, 0x3FAC4B8Eu, false},
    {"CHMO_6", true, 0x3FAC8EC2u, false},  {"CHMO_8", false, 0x3FA8D961u, true},
    {"CHMO_9", true, 0x3FACC2C9u, false},
};

} // namespace

MDK_TEST(EnhancedLightingMatchesTheOracle) {
    const mdk::formats::MdkData& game = locateData();
    if (!game.found()) {
        return;
    }
    // The game's own assembly: it applies the SYS_PAL palette patch and
    // gathers the corridors the MTO name list alone does not reach.
    mdk::LevelData level;
    MDK_CHECK(mdk::LevelData::load(&level, &game, 3));
    const mdk::formats::Dti& dti = level.dti();

    const mdk::SkyColours colours = mdk::skyLightOf(dti);
    checkVec("sky", colours.sky, 0x3EC21963u, 0x3F0A1F7Fu, 0x3F0B3C69u);
    checkVec("ground", colours.ground, 0x3F010A0Du, 0x3F0CC42Au, 0x3EB39341u);
    checkVec("sun direction", mdk::sunDirection(), 0xBEA87160u, 0x3EF08FB2u, 0xBF51B3F3u);

    int matched = 0;
    for (size_t e = 0; e < sizeof(kExpected) / sizeof(kExpected[0]); e++) {
        const mdk::formats::Arena* arena = level.arenaNamed(kExpected[e].name);
        MDK_CHECK(arena != nullptr);
        if (!arena) {
            continue;
        }
        matched++;
        const mdk::Cover cover = mdk::arenaCover(*arena);
        MDK_CHECK_EQ((int)cover, kExpected[e].covered ? (int)mdk::Cover::Covered
                                                      : (int)mdk::Cover::Open);
        const mdk::Lighting rig = mdk::enhancedLighting(dti, *arena);
        MDK_CHECK(rig.enabled);
        if (bitsOf(rig.exposure) != kExpected[e].exposure) {
            std::printf("  %s exposure got %08X want %08X\n", kExpected[e].name,
                        bitsOf(rig.exposure), kExpected[e].exposure);
        }
        MDK_CHECK_EQ(bitsOf(rig.exposure), kExpected[e].exposure);
        MDK_CHECK_EQ((int)rig.shadows, kExpected[e].shadows ? 1 : 0);
        if (std::strcmp(kExpected[e].name, "HMO_1") == 0) {
            MDK_CHECK_EQ(bitsOf(rig.sun.r), 0x3F6FF931u);
            MDK_CHECK_EQ(bitsOf(rig.sun.g), 0x3F82236Du);
            MDK_CHECK_EQ(bitsOf(rig.sun.b), 0x3F8269D8u);
            MDK_CHECK_EQ(bitsOf(rig.haze_colour.r), 0x3F35B5B6u);
            MDK_CHECK_EQ(bitsOf(rig.haze_density), 0x3951B717u);
        }
        if (std::strcmp(kExpected[e].name, "HMO_2") == 0) {
            MDK_CHECK_EQ(bitsOf(rig.sun.r), 0u);
            MDK_CHECK_EQ(bitsOf(rig.sky.r), 0x3F7E6A68u);
            MDK_CHECK_EQ(bitsOf(rig.ground.r), 0x3F321749u);
        }
    }
    MDK_CHECK_EQ(matched, (int)(sizeof(kExpected) / sizeof(kExpected[0])));
}

MDK_TEST(SunShadowMatrixMatchesTheOracle) {
    // From the C# probe (SunShadow.Matrix over the same inputs, raw bits).
    const uint32_t kM[16] = {0x3B32E910u, 0xBACD2BDCu, 0xB82CE2A1u, 0x00000000u,
                             0x3AFAB3D7u, 0x3B126AEEu, 0x3876C121u, 0x00000000u,
                             0x00000000u, 0x3AFB13B2u, 0xB8D688EFu, 0x00000000u,
                             0xBD860000u, 0xBDB20000u, 0x3F009708u, 0x3F800000u};
    float m[16];
    mdk::sunShadowMatrix(mdk::Vec3{-0.330f, 0.471f, -0.819f},
                         mdk::Vec3{10.0f, 20.0f, 30.0f}, 300.0f, 2048, m);
    for (int i = 0; i < 16; i++) {
        if (bitsOf(m[i]) != kM[i]) {
            std::printf("  M[%d] got %08X want %08X\n", i, bitsOf(m[i]), kM[i]);
        }
        MDK_CHECK_EQ(bitsOf(m[i]), kM[i]);
    }
    const uint32_t kM2[16] = {0xBC23D70Au, 0u, 0u, 0u, 0u, 0x3C23D70Au, 0u, 0u,
                              0u, 0u, 0x3903126Fu, 0u, 0u, 0u, 0x3F000000u, 0x3F800000u};
    mdk::sunShadowMatrix(mdk::Vec3{0.0f, 0.0f, 1.0f}, mdk::Vec3{0.0f, 0.0f, 0.0f},
                         100.0f, 2048, m);
    for (int i = 0; i < 16; i++) {
        MDK_CHECK_EQ(bitsOf(m[i]), kM2[i]);
    }
}

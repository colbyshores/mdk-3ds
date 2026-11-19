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
#include "formats/font.h"
#include "game/enhanced_config.h"
#include "game/enhanced_menu.h"
#include "game/level_data.h"
#include "game/level_light.h"
#include "render/post_params.h"
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

float floatOf(uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
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

MDK_TEST(PostClipMatricesMatchTheOracle) {
    // From the C# probe (2026-11-14): CreateLookAt(Zero,
    // normalize(1,2,3), up=+Z) and CreatePerspectiveFieldOfView(
    // 1.0471976 rad, 4/3, 1, 4000), printed as raw Single bits.
    const uint32_t kProj[16] = {0x3FA646E0u, 0x00000000u, 0x00000000u, 0x00000000u,
                                0x00000000u, 0x3FDDB3D6u, 0x00000000u, 0x00000000u,
                                0x00000000u, 0x00000000u, 0xBF800832u, 0xBF800000u,
                                0x00000000u, 0x00000000u, 0xBF800832u, 0x00000000u};
    // rotation * projection (FollowCamera.cs's ClipToDirection input):
    // the product is pinned bit-exact.
    const uint32_t kViewToClip[16] = {0x3F94B8F8u, 0xBF1EFDB8u, 0x3E88DF3Bu, 0x3E88D678u,
                                      0xBF14B8F8u, 0xBF9EFDB8u, 0x3F08DF3Bu, 0x3F08D678u,
                                      0x00000000u, 0x3F847E1Au, 0x3F4D4ED7u, 0x3F4D41B3u,
                                      0x00000000u, 0x00000000u, 0xBF800832u, 0x00000000u};
    // Matrix4x4.Invert of the above. Ours is Gauss-Jordan, the C#'s an
    // adjugate expansion, so the inverse is pinned to 1e-5 absolute --
    // the post pass only dots and normalizes with it.
    const uint32_t kClipToView[16] = {0x3F304389u, 0xBEB04389u, 0x312C5CC1u, 0xB0CDC032u,
                                      0xBE53FCF8u, 0xBED3FCF8u, 0x3EB0A825u, 0xB1B654FCu,
                                      0x80000000u, 0x80000000u, 0x80000000u, 0xBF7FEF9Eu,
                                      0x3E88D678u, 0x3F08D678u, 0x3F4D41B3u, 0x3F800000u};
    float proj[16], wantInv[16];
    for (int i = 0; i < 16; i++) {
        proj[i] = floatOf(kProj[i]);
        wantInv[i] = floatOf(kClipToView[i]);
    }
    // The probe's forward (Vector3.Normalize(1,2,3), raw bits) and up.
    const mdk::Vec3 forward{floatOf(0x3E88D677u), floatOf(0x3F08D677u), floatOf(0x3F4D41B2u)};
    float clipToView[16], viewToClip[16];
    MDK_CHECK(mdk::postClipMatrices(forward, mdk::Vec3{0.0f, 0.0f, 1.0f}, proj, clipToView,
                                    viewToClip));
    for (int i = 0; i < 16; i++) {
        MDK_CHECK_EQ(bitsOf(viewToClip[i]), kViewToClip[i]);
        const float diff = clipToView[i] > wantInv[i] ? clipToView[i] - wantInv[i]
                                                      : wantInv[i] - clipToView[i];
        if (diff > 1e-5f) {
            std::printf("  clipToView[%d] got %08X want %08X\n", i, bitsOf(clipToView[i]),
                        kClipToView[i]);
        }
        MDK_CHECK(diff <= 1e-5f);
    }
    // The pass constants (Renderer.Enhanced.cs).
    MDK_CHECK_EQ(bitsOf(mdk::kOcclusionRadius), 0x41400000u);   // 12
    MDK_CHECK_EQ(bitsOf(mdk::kOcclusionStrength), 0x3FC00000u); // 1.5
    MDK_CHECK_EQ(bitsOf(mdk::kGlowThreshold), 0x3F4CCCCDu);     // 0.8
    MDK_CHECK_EQ(bitsOf(mdk::kGlowIntensity), 0x3F4CCCCDu);     // 0.8
    MDK_CHECK_EQ(bitsOf(mdk::kGlowMip), 0x40400000u);           // 3
    MDK_CHECK_EQ(mdk::kSceneMips, 6u);
}

MDK_TEST(EnhancedConfigRoundTrips) {
    // Defaults are all on (the shipped enhanced look, SPEC.md 7.9).
    mdk::EnhancedToggles toggles;
    MDK_CHECK(toggles.enableSkyShadows && toggles.enableFragmentLights && toggles.enableHaze &&
              toggles.enableMirrors && toggles.enableGlowBillboards && toggles.enableFiltering &&
              toggles.enableMipmaps && toggles.enableHdTextures && toggles.enableLighting &&
              toggles.enableEdgeSmoothing);

    // The written file carries all nine keys and parses back identically.
    toggles.enableHaze = false;
    toggles.enableMirrors = false;
    toggles.enableFiltering = false;
    toggles.enableMipmaps = false;
    toggles.enableHdTextures = false;
    toggles.enableLighting = false;
    toggles.enableEdgeSmoothing = false;
    const std::string text = mdk::writeEnhancedConfig(toggles);
    mdk::EnhancedToggles back;
    MDK_CHECK(mdk::parseEnhancedConfig(text.c_str(), &back));
    MDK_CHECK(back.enableSkyShadows && back.enableFragmentLights && !back.enableHaze &&
              !back.enableMirrors && back.enableGlowBillboards && !back.enableFiltering &&
              !back.enableMipmaps && !back.enableHdTextures && !back.enableLighting &&
              !back.enableEdgeSmoothing);
    MDK_CHECK(text.find("enableEdgeSmoothing=0\n") != std::string::npos);
    // The sun shadow's diagnostic (not a look): off by default, written, and
    // read back when it is on.
    MDK_CHECK(!toggles.shadowMapFlipBytes && !back.shadowMapFlipBytes);
    MDK_CHECK(text.find("shadowMapFlipBytes=0\n") != std::string::npos);
    {
        mdk::EnhancedToggles flipped;
        flipped.shadowMapFlipBytes = true;
        mdk::EnhancedToggles flippedBack;
        MDK_CHECK(mdk::parseEnhancedConfig(mdk::writeEnhancedConfig(flipped).c_str(),
                                           &flippedBack));
        MDK_CHECK(flippedBack.shadowMapFlipBytes && flippedBack.enableSkyShadows);
    }
    // The 3DS edge smoothing's 2x2 (not a toggle of its own): off by default
    // (2x1), written, read back.
    MDK_CHECK(!toggles.edgeSmoothing2x2 && !back.edgeSmoothing2x2);
    MDK_CHECK(text.find("edgeSmoothing2x2=0\n") != std::string::npos);
    {
        mdk::EnhancedToggles fine;
        fine.edgeSmoothing2x2 = true;
        mdk::EnhancedToggles fineBack;
        MDK_CHECK(mdk::parseEnhancedConfig(mdk::writeEnhancedConfig(fine).c_str(), &fineBack));
        MDK_CHECK(fineBack.edgeSmoothing2x2 && fineBack.enableEdgeSmoothing);
    }
    // The 3DS glow's bloom (not a toggle of its own): off by default
    // (billboards), written, read back.
    MDK_CHECK(!toggles.glowBloom && !back.glowBloom);
    MDK_CHECK(text.find("glowBloom=0\n") != std::string::npos);
    {
        mdk::EnhancedToggles bloom;
        bloom.glowBloom = true;
        mdk::EnhancedToggles bloomBack;
        MDK_CHECK(mdk::parseEnhancedConfig(mdk::writeEnhancedConfig(bloom).c_str(), &bloomBack));
        MDK_CHECK(bloomBack.glowBloom && bloomBack.enableGlowBillboards);
    }
    // The 3DS haze through the fog unit (not a toggle of its own): on by
    // default, written, and the combiner haze read back when it is off.
    MDK_CHECK(toggles.hazeFog && back.hazeFog);
    MDK_CHECK(text.find("hazeFog=1\n") != std::string::npos);
    {
        mdk::EnhancedToggles combiner;
        combiner.hazeFog = false;
        mdk::EnhancedToggles combinerBack;
        MDK_CHECK(mdk::parseEnhancedConfig(mdk::writeEnhancedConfig(combiner).c_str(),
                                           &combinerBack));
        MDK_CHECK(!combinerBack.hazeFog && combinerBack.enableHaze);
        mdk::EnhancedToggles typed;
        MDK_CHECK(mdk::parseEnhancedConfig("hazeFog=0\n", &typed) && !typed.hazeFog);
    }
    // How often the 3DS redraws the shadow map (not a toggle): 0 = automatic by
    // default, written, read back; only 1-4 are taken, anything else is 0.
    MDK_CHECK(toggles.shadowMapRefresh == 0 && back.shadowMapRefresh == 0);
    MDK_CHECK(text.find("shadowMapRefresh=0\n") != std::string::npos);
    {
        mdk::EnhancedToggles every;
        every.shadowMapRefresh = 3;
        mdk::EnhancedToggles everyBack;
        MDK_CHECK(mdk::parseEnhancedConfig(mdk::writeEnhancedConfig(every).c_str(), &everyBack));
        MDK_CHECK(everyBack.shadowMapRefresh == 3);
        mdk::EnhancedToggles wide;
        MDK_CHECK(mdk::parseEnhancedConfig("shadowMapRefresh=9\n", &wide) && wide.shadowMapRefresh == 0);
    }
    // The 3DS shadow map's side (not a toggle): 0 = automatic by default,
    // written, read back; only 128/256/512 are taken, anything else is 0.
    MDK_CHECK(toggles.shadowMapSize == 0 && back.shadowMapSize == 0);
    MDK_CHECK(text.find("shadowMapSize=0\n") != std::string::npos);
    {
        mdk::EnhancedToggles sized;
        sized.shadowMapSize = 128;
        mdk::EnhancedToggles sizedBack;
        MDK_CHECK(mdk::parseEnhancedConfig(mdk::writeEnhancedConfig(sized).c_str(), &sizedBack));
        MDK_CHECK(sizedBack.shadowMapSize == 128 && sizedBack.enableSkyShadows);
        mdk::EnhancedToggles odd;
        MDK_CHECK(mdk::parseEnhancedConfig("shadowMapSize = 512\r\n", &odd) && odd.shadowMapSize == 512);
        MDK_CHECK(mdk::parseEnhancedConfig("shadowMapSize=300\n", &odd) && odd.shadowMapSize == 0);
        mdk::EnhancedToggles junk;
        junk.shadowMapSize = 256;
        MDK_CHECK(mdk::parseEnhancedConfig("shadowMapSize=big\n", &junk) && junk.shadowMapSize == 256);
        MDK_CHECK(mdk::parseEnhancedConfig("shadowMapSize=1\nenableHaze=0\n", &junk) &&
                  junk.shadowMapSize == 0 && !junk.enableHaze);
    }
    MDK_CHECK(text.find("enableLighting=0\n") != std::string::npos);
    MDK_CHECK(text.find("enableHdTextures=0\n") != std::string::npos);
    MDK_CHECK(text.find("enableMipmaps=0\n") != std::string::npos);
    MDK_CHECK(text.find("enableFiltering=0\n") != std::string::npos);

    // A partial file moves only the keys it names; comments, unknown
    // keys and malformed values leave the rest alone.
    mdk::EnhancedToggles partial;
    const char* partialText = "; a comment\n"
                              "enableSkyShadows=off\n"
                              "somethingElse=0\n"
                              "enableHaze=banana\n"
                              "enableGlowBillboards = 0\n"
                              "enableFiltering=false\n"
                              "enableMipmaps=off\n"
                              "enableHdTextures=0\n"
                              "enableLighting=no\n"
                              "enableEdgeSmoothing=0\n";
    MDK_CHECK(mdk::parseEnhancedConfig(partialText, &partial));
    MDK_CHECK(!partial.enableSkyShadows);
    MDK_CHECK(partial.enableFragmentLights);
    MDK_CHECK(partial.enableHaze);
    MDK_CHECK(partial.enableMirrors);
    MDK_CHECK(!partial.enableGlowBillboards);
    MDK_CHECK(!partial.enableFiltering);
    MDK_CHECK(!partial.enableMipmaps);
    MDK_CHECK(!partial.enableHdTextures);
    MDK_CHECK(!partial.enableLighting);
    MDK_CHECK(!partial.enableEdgeSmoothing);

    MDK_CHECK(!mdk::parseEnhancedConfig(nullptr, &partial));
}

MDK_TEST(FontParseMatchesTheOracle) {
    // Pinned from the C# Font.Parse over the real MISC/MDKFONT.FTI
    // (probe, 2026-11-14): 152 glyphs in FONTBIG, 204 in FONTSML.
    std::vector<uint8_t> ftiBytes;
    MDK_CHECK(readDataFile("MISC/MDKFONT.FTI", &ftiBytes));
    mdk::formats::Fti fti;
    MDK_CHECK(mdk::formats::Fti::parse(&fti, ftiBytes.data(), (int32_t)ftiBytes.size()));
    std::vector<uint8_t> bigBytes;
    MDK_CHECK(fti.getBytes("FONTBIG", &bigBytes));
    mdk::formats::Font big;
    MDK_CHECK(mdk::formats::Font::parse(bigBytes.data(), (int32_t)bigBytes.size(),
                                        6, &big));
    int32_t present = 0;
    for (int32_t c = 0; c < 256; c++) {
        if (big.glyph((uint8_t)c)) {
            present++;
        }
    }
    MDK_CHECK_EQ(present, 152);
    const mdk::formats::Font::Glyph* a = big.glyph('A');
    MDK_CHECK(a != nullptr);
    if (a) {
        MDK_CHECK_EQ(a->ascent, 27);
        MDK_CHECK_EQ(a->descent, 0);
        MDK_CHECK_EQ(a->width, 29);
    }
    MDK_CHECK_EQ(big.width("VIDEO GRAPHICS"), 318);
    MDK_CHECK(big.glyph(0) == nullptr);

    std::vector<uint8_t> smallBytes;
    MDK_CHECK(fti.getBytes("FONTSML", &smallBytes));
    mdk::formats::Font small;
    MDK_CHECK(mdk::formats::Font::parse(smallBytes.data(), (int32_t)smallBytes.size(),
                                        4, &small));
    present = 0;
    for (int32_t c = 0; c < 256; c++) {
        if (small.glyph((uint8_t)c)) {
            present++;
        }
    }
    MDK_CHECK_EQ(present, 204);
}

MDK_TEST(EnhancedMenuTogglesInPlace) {
    mdk::EnhancedMenu menu;
    mdk::EnhancedToggles toggles;
    mdk::EnhancedMenu::Input none;
    // Closed: edges do nothing.
    mdk::EnhancedMenu::Input down;
    down.down = true;
    MDK_CHECK(!menu.handle(down, &toggles));
    MDK_CHECK(!menu.open());
    menu.setOpen(true);
    // Navigation clamps at both ends.
    MDK_CHECK(!menu.handle(down, &toggles));
    MDK_CHECK_EQ(menu.selected(), 1);
    mdk::EnhancedMenu::Input up;
    up.up = true;
    menu.handle(up, &toggles);
    menu.handle(up, &toggles);
    MDK_CHECK_EQ(menu.selected(), 0);
    // Confirm flips the selected toggle only, and reports the change.
    mdk::EnhancedMenu::Input confirm;
    confirm.confirm = true;
    MDK_CHECK(menu.handle(confirm, &toggles));
    MDK_CHECK(!toggles.enableSkyShadows);
    MDK_CHECK(toggles.enableFragmentLights && toggles.enableHaze && toggles.enableMirrors &&
              toggles.enableGlowBillboards);
    // Left/right flip too; the last row is EDGE SMOOTHING, then LIGHTING, HD
    // TEXTURES, MIPMAPS, FILTERING, GLOW.
    for (int32_t i = 0; i < 12; i++) {
        menu.handle(down, &toggles);
    }
    MDK_CHECK_EQ(menu.selected(), 9);
    mdk::EnhancedMenu::Input left;
    left.left = true;
    MDK_CHECK(menu.handle(left, &toggles));
    MDK_CHECK(!toggles.enableEdgeSmoothing);
    MDK_CHECK(toggles.enableLighting);
    menu.handle(up, &toggles);
    MDK_CHECK_EQ(menu.selected(), 8);
    MDK_CHECK(menu.handle(left, &toggles));
    MDK_CHECK(!toggles.enableLighting);
    MDK_CHECK(toggles.enableHdTextures);
    menu.handle(up, &toggles);
    MDK_CHECK_EQ(menu.selected(), 7);
    MDK_CHECK(menu.handle(left, &toggles));
    MDK_CHECK(!toggles.enableHdTextures);
    MDK_CHECK(toggles.enableMipmaps);
    menu.handle(up, &toggles);
    MDK_CHECK_EQ(menu.selected(), 6);
    MDK_CHECK(menu.handle(left, &toggles));
    MDK_CHECK(!toggles.enableMipmaps);
    MDK_CHECK(toggles.enableFiltering);
    menu.handle(up, &toggles);
    MDK_CHECK_EQ(menu.selected(), 5);
    MDK_CHECK(menu.handle(left, &toggles));
    MDK_CHECK(!toggles.enableFiltering);
    MDK_CHECK(toggles.enableGlowBillboards);
    menu.handle(up, &toggles);
    MDK_CHECK_EQ(menu.selected(), 4);
    MDK_CHECK(menu.handle(left, &toggles));
    MDK_CHECK(!toggles.enableGlowBillboards);
    // Close leaves the toggles alone.
    mdk::EnhancedMenu::Input close;
    close.close = true;
    MDK_CHECK(!menu.handle(close, &toggles));
    MDK_CHECK(!menu.open());
    MDK_CHECK(!menu.handle(none, &toggles));
}

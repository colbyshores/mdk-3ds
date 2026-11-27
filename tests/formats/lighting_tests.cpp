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
    // The 3DS's 16-bit texture stores: ON by default, written, and read back
    // when it is off (the owner's A/B: texture16=0 is the RGBA8 store).
    MDK_CHECK(toggles.texture16 && back.texture16);
    MDK_CHECK(text.find("texture16=1\n") != std::string::npos);
    {
        mdk::EnhancedToggles wide;
        wide.texture16 = false;
        mdk::EnhancedToggles wideBack;
        MDK_CHECK(mdk::parseEnhancedConfig(mdk::writeEnhancedConfig(wide).c_str(), &wideBack));
        MDK_CHECK(!wideBack.texture16 && wideBack.enableMipmaps);
        MDK_CHECK(mdk::writeEnhancedConfig(wide).find("texture16=0\n") != std::string::npos);
        mdk::EnhancedToggles off;
        MDK_CHECK(mdk::parseEnhancedConfig("texture16 = off\n", &off) && !off.texture16);
    }
    // Kurt's compressed frames on the 3DS: ON by default, written, and read back
    // when it is off (the owner's three-way A/B: kurtCompressed=0 with texture16=1
    // is the RGBA5551 expansion, with texture16=0 the RGBA8 one).
    MDK_CHECK(toggles.kurtCompressed && back.kurtCompressed);
    MDK_CHECK(text.find("kurtCompressed=1\n") != std::string::npos);
    {
        mdk::EnhancedToggles expanded;
        expanded.kurtCompressed = false;
        mdk::EnhancedToggles expandedBack;
        MDK_CHECK(mdk::parseEnhancedConfig(mdk::writeEnhancedConfig(expanded).c_str(),
                                           &expandedBack));
        MDK_CHECK(!expandedBack.kurtCompressed && expandedBack.texture16);
        MDK_CHECK(mdk::writeEnhancedConfig(expanded).find("kurtCompressed=0\n") !=
                  std::string::npos);
        mdk::EnhancedToggles harness;
        MDK_CHECK(mdk::parseEnhancedConfig("texture16=0\nkurtCompressed=0\n", &harness) &&
                  !harness.kurtCompressed && !harness.texture16);
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
    using Action = mdk::EnhancedMenu::Action;
    mdk::EnhancedMenu menu;
    mdk::EnhancedToggles toggles;
    mdk::EnhancedMenu::Input none;
    // Closed: edges do nothing.
    mdk::EnhancedMenu::Input down;
    down.down = true;
    MDK_CHECK(menu.handle(down, &toggles) == Action::None);
    MDK_CHECK(!menu.open());
    // It opens on RESUME (row 0); the toggles are rows 1..10, QUIT the last row.
    menu.setOpen(true);
    MDK_CHECK_EQ(menu.selected(), mdk::EnhancedMenu::kResumeRow);
    MDK_CHECK_STR_EQ(mdk::EnhancedMenu::rowLabel(0), "RESUME");
    MDK_CHECK_STR_EQ(mdk::EnhancedMenu::rowLabel(1), "SKY SHADOWS");
    MDK_CHECK_STR_EQ(mdk::EnhancedMenu::rowLabel(10), "EDGE SMOOTHING");
    MDK_CHECK_STR_EQ(mdk::EnhancedMenu::rowLabel(mdk::EnhancedMenu::kQuitRow), "QUIT");
    // + OPTIONS and MAIN MENU, the C# PauseMenu's rows (package #20a).
    // 26 settings (MOVIES HALF RATE, package #20c, then TINY DEBRIS, TINY
    // ENEMIES, SCOPE IN 2D, SCOPE CLIP, ROUND CAMERAS, 3D DEPTH and 3D SCREEN
    // DISTANCE), RESUME, CHEATS (gap package #21), OPTIONS, MAIN MENU, QUIT.
    MDK_CHECK_EQ(mdk::EnhancedMenu::kRowCount, 31);
    MDK_CHECK_STR_EQ(mdk::EnhancedMenu::rowLabel(mdk::EnhancedMenu::kCheatsRow), "CHEATS");
    // Left/right on RESUME flip nothing.
    mdk::EnhancedMenu::Input left;
    left.left = true;
    MDK_CHECK(menu.handle(left, &toggles) == Action::None);
    MDK_CHECK(menu.handle(down, &toggles) == Action::None);
    MDK_CHECK_EQ(menu.selected(), 1);
    mdk::EnhancedMenu::Input up;
    up.up = true;
    menu.handle(up, &toggles);
    MDK_CHECK_EQ(menu.selected(), 0);
    // Up from RESUME wraps to QUIT, and down from QUIT back to RESUME.
    menu.handle(up, &toggles);
    MDK_CHECK_EQ(menu.selected(), mdk::EnhancedMenu::kQuitRow);
    menu.handle(down, &toggles);
    MDK_CHECK_EQ(menu.selected(), 0);
    menu.handle(down, &toggles);
    // Confirm flips the selected toggle only, and reports the change.
    mdk::EnhancedMenu::Input confirm;
    confirm.confirm = true;
    MDK_CHECK(menu.handle(confirm, &toggles) == Action::Changed);
    MDK_CHECK(!toggles.enableSkyShadows);
    MDK_CHECK(toggles.enableFragmentLights && toggles.enableHaze && toggles.enableMirrors &&
              toggles.enableGlowBillboards);
    MDK_CHECK(menu.open());
    // Left/right flip too; the last toggle is EDGE SMOOTHING (row 10), then
    // LIGHTING, HD TEXTURES, MIPMAPS, FILTERING, GLOW.
    for (int32_t i = 0; i < 9; i++) {
        menu.handle(down, &toggles);
    }
    MDK_CHECK_EQ(menu.selected(), 10);
    MDK_CHECK(menu.handle(left, &toggles) == Action::Changed);
    MDK_CHECK(!toggles.enableEdgeSmoothing);
    MDK_CHECK(toggles.enableLighting);
    menu.handle(up, &toggles);
    MDK_CHECK_EQ(menu.selected(), 9);
    MDK_CHECK(menu.handle(left, &toggles) == Action::Changed);
    MDK_CHECK(!toggles.enableLighting);
    MDK_CHECK(toggles.enableHdTextures);
    menu.handle(up, &toggles);
    MDK_CHECK(menu.handle(left, &toggles) == Action::Changed);
    MDK_CHECK(!toggles.enableHdTextures);
    MDK_CHECK(toggles.enableMipmaps);
    menu.handle(up, &toggles);
    MDK_CHECK(menu.handle(left, &toggles) == Action::Changed);
    MDK_CHECK(!toggles.enableMipmaps);
    MDK_CHECK(toggles.enableFiltering);
    menu.handle(up, &toggles);
    MDK_CHECK(menu.handle(left, &toggles) == Action::Changed);
    MDK_CHECK(!toggles.enableFiltering);
    MDK_CHECK(toggles.enableGlowBillboards);
    menu.handle(up, &toggles);
    MDK_CHECK_EQ(menu.selected(), 5);
    MDK_CHECK(menu.handle(left, &toggles) == Action::Changed);
    MDK_CHECK(!toggles.enableGlowBillboards);
    // Close (START, B or SELECT) leaves the toggles alone and resumes.
    mdk::EnhancedMenu::Input close;
    close.close = true;
    MDK_CHECK(menu.handle(close, &toggles) == Action::Closed);
    MDK_CHECK(!menu.open());
    MDK_CHECK(menu.handle(none, &toggles) == Action::None);
    // Reopened, it is back on RESUME: A there resumes, it does not quit.
    menu.setOpen(true);
    MDK_CHECK_EQ(menu.selected(), 0);
    MDK_CHECK(menu.handle(confirm, &toggles) == Action::Closed);
    MDK_CHECK(!menu.open());
}

// QUIT is a row confirmed with A, never a single press: opening the menu and
// pressing A resumes; only up (wrapping to QUIT) and then A quits.
MDK_TEST(PauseMenuQuitNeedsTheRowAndA) {
    using Action = mdk::EnhancedMenu::Action;
    mdk::EnhancedMenu menu;
    mdk::EnhancedToggles toggles;
    const mdk::EnhancedToggles before = toggles;
    mdk::EnhancedMenu::Input confirm;
    confirm.confirm = true;
    mdk::EnhancedMenu::Input up;
    up.up = true;
    // A closed menu never quits.
    MDK_CHECK(menu.handle(confirm, &toggles) == Action::None);
    menu.setOpen(true);
    MDK_CHECK(menu.handle(up, &toggles) == Action::None);
    MDK_CHECK_EQ(menu.selected(), mdk::EnhancedMenu::kQuitRow);
    // Left/right on QUIT do nothing.
    mdk::EnhancedMenu::Input right;
    right.right = true;
    MDK_CHECK(menu.handle(right, &toggles) == Action::None);
    MDK_CHECK(menu.handle(confirm, &toggles) == Action::Quit);
    // The caller is leaving: the menu stays drawn, and no toggle moved.
    MDK_CHECK(menu.open());
    MDK_CHECK(toggles.enableSkyShadows == before.enableSkyShadows &&
              toggles.enableEdgeSmoothing == before.enableEdgeSmoothing &&
              toggles.enableHaze == before.enableHaze);
    // A close edge in the same frame as A wins: it resumes.
    mdk::EnhancedMenu::Input both;
    both.confirm = true;
    both.close = true;
    MDK_CHECK(menu.handle(both, &toggles) == Action::Closed);
    MDK_CHECK(!menu.open());
}

namespace {

// The row a label sits on, or -1.
int32_t menuRowOf(const char* label) {
    for (int32_t row = 0; row < mdk::EnhancedMenu::kRowCount; row++) {
        if (strcmp(mdk::EnhancedMenu::rowLabel(row), label) == 0) {
            return row;
        }
    }
    return -1;
}

// Open the menu and walk down to `row` (each step through handle(), so the
// window scrolls as it would on the console).
void menuSelect(mdk::EnhancedMenu* menu, mdk::EnhancedToggles* toggles, int32_t row) {
    menu->setOpen(true);
    mdk::EnhancedMenu::Input down;
    down.down = true;
    for (int32_t i = 0; i < row; i++) {
        menu->handle(down, toggles);
    }
}

std::string menuValue(int32_t row, const mdk::EnhancedToggles& toggles) {
    char value[16];
    mdk::EnhancedMenu::rowValue(row, toggles, value, (int32_t)sizeof(value));
    return value;
}

} // namespace

// t2k's SLD / CHO items (t2k_core/src/ui/menu.cpp) in the pause menu: a slider
// steps by its step and stops at its ends (and says nothing changed there, so
// nothing is written); a choice wraps both ways; A does nothing on a slider and
// steps a choice forward; the 30 fps cap is a plain toggle.
MDK_TEST(PauseMenuSlidersStepAndClampChoicesWrap) {
    using Action = mdk::EnhancedMenu::Action;
    using Menu = mdk::EnhancedMenu;
    MDK_CHECK_EQ(Menu::kItemCount, 26);   // + MOVIES HALF RATE (package #20c), row 19
                                          // + TINY DEBRIS, row 20; TINY ENEMIES, row 21
                                          // + SCOPE IN 2D, row 22; SCOPE CLIP, row 23
                                          // + ROUND CAMERAS, row 24
                                          // + 3D DEPTH, row 25; 3D SCREEN DISTANCE, 26
    MDK_CHECK_EQ(Menu::kQuitRow, Menu::kRowCount - 1);
    // The rows after the ten toggles, in order.
    MDK_CHECK_EQ(menuRowOf("30 FPS CAP"), 11);
    MDK_CHECK_EQ(menuRowOf("HAZE DENSITY"), 12);
    MDK_CHECK_EQ(menuRowOf("GLOW STRENGTH"), 13);
    MDK_CHECK_EQ(menuRowOf("SHADOW DARKNESS"), 14);
    MDK_CHECK_EQ(menuRowOf("EXPOSURE"), 15);
    MDK_CHECK_EQ(menuRowOf("TINT SATURATION"), 16);
    MDK_CHECK_EQ(menuRowOf("SHADOW MAP"), 17);
    MDK_CHECK_EQ(menuRowOf("SHADOW REFRESH"), 18);
    // No AA-mode row: edge smoothing stays on/off (owner, 2026-11-19).
    MDK_CHECK(Menu::rowItem(menuRowOf("EDGE SMOOTHING"))->kind == Menu::Kind::Toggle);
    for (int32_t row = 0; row < Menu::kRowCount; row++) {
        MDK_CHECK(strstr(Menu::rowLabel(row), "2X2") == nullptr);
    }
    MDK_CHECK(Menu::rowItem(Menu::kResumeRow) == nullptr);
    MDK_CHECK(Menu::rowItem(Menu::kQuitRow) == nullptr);

    mdk::EnhancedMenu::Input left;
    left.left = true;
    mdk::EnhancedMenu::Input right;
    right.right = true;
    mdk::EnhancedMenu::Input confirm;
    confirm.confirm = true;
    mdk::EnhancedMenu::Input both;
    both.left = true;
    both.right = true;

    // THE SLIDER: haze density, 0..400 by 25, from 100.
    {
        mdk::EnhancedMenu menu;
        mdk::EnhancedToggles toggles;
        const mdk::EnhancedToggles shipped;
        menuSelect(&menu, &toggles, 12);
        MDK_CHECK_EQ(menu.selected(), 12);
        MDK_CHECK(menuValue(12, toggles) == "100%");
        MDK_CHECK(menu.handle(right, &toggles) == Action::Changed);
        MDK_CHECK_EQ(toggles.hazeDensity, 125);
        MDK_CHECK(menuValue(12, toggles) == "125%");
        MDK_CHECK(menu.handle(left, &toggles) == Action::Changed);
        MDK_CHECK(menu.handle(left, &toggles) == Action::Changed);
        MDK_CHECK_EQ(toggles.hazeDensity, 75);
        // A and left+right do nothing to a slider.
        MDK_CHECK(menu.handle(confirm, &toggles) == Action::None);
        MDK_CHECK(menu.handle(both, &toggles) == Action::None);
        MDK_CHECK_EQ(toggles.hazeDensity, 75);
        // To the bottom: 3 more steps reach 0, then it stays and says None.
        for (int32_t i = 0; i < 3; i++) {
            MDK_CHECK(menu.handle(left, &toggles) == Action::Changed);
        }
        MDK_CHECK_EQ(toggles.hazeDensity, 0);
        MDK_CHECK(menu.handle(left, &toggles) == Action::None);
        MDK_CHECK_EQ(toggles.hazeDensity, 0);
        // To the top: 16 steps reach 400, the 17th is refused.
        for (int32_t i = 0; i < 16; i++) {
            MDK_CHECK(menu.handle(right, &toggles) == Action::Changed);
        }
        MDK_CHECK_EQ(toggles.hazeDensity, 400);
        MDK_CHECK(menu.handle(right, &toggles) == Action::None);
        MDK_CHECK_EQ(toggles.hazeDensity, 400);
        // Only its own field moved.
        MDK_CHECK_EQ(toggles.glowStrength, shipped.glowStrength);
        MDK_CHECK_EQ(toggles.exposure, shipped.exposure);
        MDK_CHECK(toggles.enableHaze == shipped.enableHaze);
    }
    // A slider whose shipped value is its top: the darkness stops at 100.
    {
        mdk::EnhancedMenu menu;
        mdk::EnhancedToggles toggles;
        menuSelect(&menu, &toggles, 14);
        MDK_CHECK(menu.handle(right, &toggles) == Action::None);
        MDK_CHECK_EQ(toggles.shadowDarkness, 100);
        MDK_CHECK(menu.handle(left, &toggles) == Action::Changed);
        MDK_CHECK_EQ(toggles.shadowDarkness, 90);
        // A hand-set value out of range clamps on the next step.
        toggles.shadowDarkness = 140;
        MDK_CHECK(menu.handle(right, &toggles) == Action::Changed);
        MDK_CHECK_EQ(toggles.shadowDarkness, 100);
    }
    // Exposure's floor is 50.
    {
        mdk::EnhancedMenu menu;
        mdk::EnhancedToggles toggles;
        menuSelect(&menu, &toggles, 15);
        for (int32_t i = 0; i < 5; i++) {
            MDK_CHECK(menu.handle(left, &toggles) == Action::Changed);
        }
        MDK_CHECK_EQ(toggles.exposure, 50);
        MDK_CHECK(menu.handle(left, &toggles) == Action::None);
    }
    // THE CHOICE: the shadow map's side, AUTO -> 128 -> 256 -> 512 -> AUTO, and
    // left from AUTO wraps to 512. A steps it forward.
    {
        mdk::EnhancedMenu menu;
        mdk::EnhancedToggles toggles;
        menuSelect(&menu, &toggles, 17);
        MDK_CHECK(menuValue(17, toggles) == "AUTO");
        MDK_CHECK(menu.handle(right, &toggles) == Action::Changed);
        MDK_CHECK_EQ(toggles.shadowMapSize, 128);
        MDK_CHECK(menu.handle(confirm, &toggles) == Action::Changed);
        MDK_CHECK_EQ(toggles.shadowMapSize, 256);
        MDK_CHECK(menu.handle(right, &toggles) == Action::Changed);
        MDK_CHECK_EQ(toggles.shadowMapSize, 512);
        MDK_CHECK(menuValue(17, toggles) == "512");
        MDK_CHECK(menu.handle(right, &toggles) == Action::Changed);
        MDK_CHECK_EQ(toggles.shadowMapSize, 0);
        MDK_CHECK(menu.handle(left, &toggles) == Action::Changed);
        MDK_CHECK_EQ(toggles.shadowMapSize, 512);
        MDK_CHECK(menu.handle(both, &toggles) == Action::None);
        MDK_CHECK_EQ(toggles.shadowMapRefresh, 0);
    }
    // The refresh choice, from a value off its list: right goes to the first,
    // left to the last.
    {
        mdk::EnhancedMenu menu;
        mdk::EnhancedToggles toggles;
        menuSelect(&menu, &toggles, 18);
        toggles.shadowMapRefresh = 7;
        MDK_CHECK(menuValue(18, toggles) == "7");
        MDK_CHECK(menu.handle(right, &toggles) == Action::Changed);
        MDK_CHECK_EQ(toggles.shadowMapRefresh, 0);
        toggles.shadowMapRefresh = 7;
        MDK_CHECK(menu.handle(left, &toggles) == Action::Changed);
        MDK_CHECK_EQ(toggles.shadowMapRefresh, 4);
        MDK_CHECK(menuValue(18, toggles) == "4");
    }
    // THE 30 FPS CAP: off by default, a toggle like the others.
    {
        mdk::EnhancedMenu menu;
        mdk::EnhancedToggles toggles;
        MDK_CHECK(!toggles.cap30Fps);
        menuSelect(&menu, &toggles, 11);
        MDK_CHECK(menuValue(11, toggles) == "OFF");
        MDK_CHECK(menu.handle(confirm, &toggles) == Action::Changed);
        MDK_CHECK(toggles.cap30Fps);
        MDK_CHECK(menuValue(11, toggles) == "ON");
        MDK_CHECK(menu.handle(right, &toggles) == Action::Changed);
        MDK_CHECK(!toggles.cap30Fps);
    }
    // THE MOVIES' HALF RATE (package #20c): off by default, the last row, and a
    // line of the config file that reads back.
    {
        mdk::EnhancedMenu menu;
        mdk::EnhancedToggles toggles;
        MDK_CHECK(!toggles.movieHalfRate);
        menuSelect(&menu, &toggles, 19);
        MDK_CHECK(menuValue(19, toggles) == "OFF");
        MDK_CHECK(menu.handle(confirm, &toggles) == Action::Changed);
        MDK_CHECK(toggles.movieHalfRate);
        const std::string text = mdk::writeEnhancedConfig(toggles);
        MDK_CHECK(text.find("movieHalfRate=1\n") != std::string::npos);
        mdk::EnhancedToggles back;
        MDK_CHECK(mdk::parseEnhancedConfig(text.c_str(), &back));
        MDK_CHECK(back.movieHalfRate);
    }
    // THE TINY-DEBRIS CULL: 2 px by default, row 20 cycles OFF/1/2/3, the config
    // line reads back, an old file without it keeps the default, and a value
    // off the list reads as 2.
    {
        mdk::EnhancedMenu menu;
        mdk::EnhancedToggles toggles;
        MDK_CHECK_EQ(toggles.tinyDebrisPixels, 2);
        menuSelect(&menu, &toggles, 20);
        MDK_CHECK(menuValue(20, toggles) == "2 PX");
        MDK_CHECK(menu.handle(right, &toggles) == Action::Changed);
        MDK_CHECK_EQ(toggles.tinyDebrisPixels, 3);
        MDK_CHECK(menu.handle(right, &toggles) == Action::Changed);
        MDK_CHECK_EQ(toggles.tinyDebrisPixels, 0);
        MDK_CHECK(menuValue(20, toggles) == "OFF");
        const std::string text = mdk::writeEnhancedConfig(toggles);
        MDK_CHECK(text.find("tinyDebrisPixels=0\n") != std::string::npos);
        mdk::EnhancedToggles back;
        MDK_CHECK(mdk::parseEnhancedConfig(text.c_str(), &back));
        MDK_CHECK_EQ(back.tinyDebrisPixels, 0);
        mdk::EnhancedToggles old;
        MDK_CHECK(mdk::parseEnhancedConfig("enableHaze=1\ncap30Fps=1\n", &old));
        MDK_CHECK_EQ(old.tinyDebrisPixels, 2);
        mdk::EnhancedToggles wide;
        MDK_CHECK(mdk::parseEnhancedConfig("tinyDebrisPixels=9\n", &wide));
        MDK_CHECK_EQ(wide.tinyDebrisPixels, 2);
    }
    // THE TINY-ENEMY CULL: the same choice on row 21, its own config line.
    {
        mdk::EnhancedMenu menu;
        mdk::EnhancedToggles toggles;
        MDK_CHECK_EQ(toggles.tinyEnemyPixels, 2);
        menuSelect(&menu, &toggles, 21);
        MDK_CHECK(menuValue(21, toggles) == "2 PX");
        MDK_CHECK(menu.handle(right, &toggles) == Action::Changed);
        MDK_CHECK_EQ(toggles.tinyEnemyPixels, 3);
        MDK_CHECK_EQ(toggles.tinyDebrisPixels, 2);
        const std::string text = mdk::writeEnhancedConfig(toggles);
        MDK_CHECK(text.find("tinyEnemyPixels=3\n") != std::string::npos);
        mdk::EnhancedToggles back;
        MDK_CHECK(mdk::parseEnhancedConfig(text.c_str(), &back));
        MDK_CHECK_EQ(back.tinyEnemyPixels, 3);
        mdk::EnhancedToggles off;
        MDK_CHECK(mdk::parseEnhancedConfig("tinyEnemyPixels=0\n", &off));
        MDK_CHECK_EQ(off.tinyEnemyPixels, 0);
    }
    // THE SCOPE IN 2D: off by default (it changes the look), row 22, a config line.
    {
        mdk::EnhancedMenu menu;
        mdk::EnhancedToggles toggles;
        MDK_CHECK(!toggles.scopeMono);
        menuSelect(&menu, &toggles, 22);
        MDK_CHECK(menuValue(22, toggles) == "OFF");
        MDK_CHECK(menu.handle(confirm, &toggles) == Action::Changed);
        MDK_CHECK(toggles.scopeMono);
        const std::string text = mdk::writeEnhancedConfig(toggles);
        MDK_CHECK(text.find("scopeMono=1\n") != std::string::npos);
        mdk::EnhancedToggles back;
        MDK_CHECK(mdk::parseEnhancedConfig(text.c_str(), &back));
        MDK_CHECK(back.scopeMono);
    }
    // THE SCOPE CLIP: on by default (the same picture), row 23, a config line.
    {
        mdk::EnhancedMenu menu;
        mdk::EnhancedToggles toggles;
        MDK_CHECK(toggles.scopeClip);
        menuSelect(&menu, &toggles, 23);
        MDK_CHECK(menuValue(23, toggles) == "ON");
        MDK_CHECK(menu.handle(confirm, &toggles) == Action::Changed);
        MDK_CHECK(!toggles.scopeClip);
        const std::string text = mdk::writeEnhancedConfig(toggles);
        MDK_CHECK(text.find("scopeClip=0\n") != std::string::npos);
        mdk::EnhancedToggles back;
        MDK_CHECK(mdk::parseEnhancedConfig(text.c_str(), &back));
        MDK_CHECK(!back.scopeClip);
    }
    // THE ROUND CAMERAS: on by default (the C#'s), row 24, a config line.
    {
        mdk::EnhancedMenu menu;
        mdk::EnhancedToggles toggles;
        MDK_CHECK(toggles.sniperRoundCams);
        menuSelect(&menu, &toggles, 24);
        MDK_CHECK(menuValue(24, toggles) == "ON");
        MDK_CHECK(menu.handle(confirm, &toggles) == Action::Changed);
        MDK_CHECK(!toggles.sniperRoundCams);
        const std::string text = mdk::writeEnhancedConfig(toggles);
        MDK_CHECK(text.find("sniperRoundCams=0\n") != std::string::npos);
        mdk::EnhancedToggles back;
        MDK_CHECK(mdk::parseEnhancedConfig(text.c_str(), &back));
        MDK_CHECK(!back.sniperRoundCams);
    }
}

// Twenty rows do not fit the top screen's twelve: the window follows the
// selection, both ways and across the wrap.
MDK_TEST(PauseMenuScrollsToKeepTheSelectionInView) {
    using Menu = mdk::EnhancedMenu;
    MDK_CHECK(Menu::kRowCount > Menu::kVisibleRows);
    Menu menu;
    mdk::EnhancedToggles toggles;
    mdk::EnhancedMenu::Input down;
    down.down = true;
    mdk::EnhancedMenu::Input up;
    up.up = true;
    menu.setOpen(true);
    MDK_CHECK_EQ(menu.top(), 0);
    for (int32_t i = 0; i < Menu::kVisibleRows - 1; i++) {
        menu.handle(down, &toggles);
    }
    MDK_CHECK_EQ(menu.selected(), Menu::kVisibleRows - 1);
    MDK_CHECK_EQ(menu.top(), 0);   // the last visible row, no scroll yet
    menu.handle(down, &toggles);
    MDK_CHECK_EQ(menu.selected(), Menu::kVisibleRows);
    MDK_CHECK_EQ(menu.top(), 1);
    // Up inside the window does not scroll; up past its top does.
    menu.handle(up, &toggles);
    MDK_CHECK_EQ(menu.top(), 1);
    for (int32_t i = 0; i < Menu::kVisibleRows - 1; i++) {
        menu.handle(up, &toggles);
    }
    MDK_CHECK_EQ(menu.selected(), 0);
    MDK_CHECK_EQ(menu.top(), 0);
    // Up from RESUME wraps to QUIT, which is shown as the window's last row.
    menu.handle(up, &toggles);
    MDK_CHECK_EQ(menu.selected(), Menu::kQuitRow);
    MDK_CHECK_EQ(menu.top(), Menu::kRowCount - Menu::kVisibleRows);
    // ... and down from QUIT back to RESUME, at the top.
    menu.handle(down, &toggles);
    MDK_CHECK_EQ(menu.selected(), 0);
    MDK_CHECK_EQ(menu.top(), 0);
    // Reopening starts at the top whatever the window was.
    menu.handle(up, &toggles);
    menu.setOpen(false);
    menu.setOpen(true);
    MDK_CHECK_EQ(menu.top(), 0);
    MDK_CHECK_EQ(menu.selected(), 0);
}

// The look dials and the cap in config.ini: today's values by default (the
// shipped look), written, read back, and a hand-edited value clamped into the
// slider's range so the file and the menu never disagree.
MDK_TEST(EnhancedConfigLookDialsRoundTrip) {
    const mdk::EnhancedToggles shipped;
    for (int32_t i = 0; i < mdk::kLookDialCount; i++) {
        const mdk::LookDial& dial = mdk::kLookDials[i];
        // 100 is the default, inside the range and on the step grid, so the
        // slider can always come back to the shipped look exactly.
        MDK_CHECK_EQ(shipped.*(dial.field), 100);
        MDK_CHECK(dial.lo <= 100 && dial.hi >= 100 && dial.step > 0);
        MDK_CHECK_EQ((100 - dial.lo) % dial.step, 0);
        MDK_CHECK_EQ((dial.hi - dial.lo) % dial.step, 0);
    }
    const std::string text = mdk::writeEnhancedConfig(shipped);
    MDK_CHECK(text.find("cap30Fps=0\n") != std::string::npos);
    MDK_CHECK(text.find("hazeDensity=100\n") != std::string::npos);
    MDK_CHECK(text.find("glowStrength=100\n") != std::string::npos);
    MDK_CHECK(text.find("shadowDarkness=100\n") != std::string::npos);
    MDK_CHECK(text.find("exposure=100\n") != std::string::npos);
    MDK_CHECK(text.find("tintSaturation=100\n") != std::string::npos);
    // The 3DS reads config.ini into a 1024-byte buffer (main_3ds.cpp): a file
    // that outgrew it would lose its last keys -- these -- to the truncation.
    MDK_CHECK(text.size() < 1023u);

    mdk::EnhancedToggles moved;
    moved.cap30Fps = true;
    moved.hazeDensity = 250;
    moved.glowStrength = 0;
    moved.shadowDarkness = 60;
    moved.exposure = 140;
    moved.tintSaturation = 175;
    moved.shadowMapSize = 256;
    moved.shadowMapRefresh = 3;
    const std::string movedText = mdk::writeEnhancedConfig(moved);
    MDK_CHECK(movedText.size() < 1023u);
    mdk::EnhancedToggles back;
    MDK_CHECK(mdk::parseEnhancedConfig(movedText.c_str(), &back));
    MDK_CHECK(back.cap30Fps);
    MDK_CHECK_EQ(back.hazeDensity, 250);
    MDK_CHECK_EQ(back.glowStrength, 0);
    MDK_CHECK_EQ(back.shadowDarkness, 60);
    MDK_CHECK_EQ(back.exposure, 140);
    MDK_CHECK_EQ(back.tintSaturation, 175);
    MDK_CHECK_EQ(back.shadowMapSize, 256);
    MDK_CHECK_EQ(back.shadowMapRefresh, 3);
    MDK_CHECK(mdk::writeEnhancedConfig(back) == movedText);

    // Hand edits: clamped into range; junk leaves the value alone.
    mdk::EnhancedToggles edited;
    MDK_CHECK(mdk::parseEnhancedConfig("hazeDensity=9999\nexposure = 10\r\n"
                                       "shadowDarkness=250\nglowStrength=abc\n"
                                       "tintSaturation=-5\ncap30Fps=on\n",
                                       &edited));
    MDK_CHECK_EQ(edited.hazeDensity, 400);
    MDK_CHECK_EQ(edited.exposure, 50);
    MDK_CHECK_EQ(edited.shadowDarkness, 100);
    MDK_CHECK_EQ(edited.glowStrength, 100);
    MDK_CHECK_EQ(edited.tintSaturation, 100);   // not a whole number: ignored
    MDK_CHECK(edited.cap30Fps);
    // An old file without the new keys keeps the shipped look.
    mdk::EnhancedToggles old;
    MDK_CHECK(mdk::parseEnhancedConfig("enableHaze=1\nshadowMapSize=0\n", &old));
    MDK_CHECK(!old.cap30Fps && old.hazeDensity == 100 && old.exposure == 100);
}

// Every character the pause menu draws has a glyph in FONTSML: a missing one is
// drawn as a space, which would turn "125%" into "125 " with nothing failing.
MDK_TEST(PauseMenuTextHasGlyphs) {
    std::vector<uint8_t> ftiBytes;
    MDK_CHECK(readDataFile("MISC/MDKFONT.FTI", &ftiBytes));
    mdk::formats::Fti fti;
    MDK_CHECK(mdk::formats::Fti::parse(&fti, ftiBytes.data(), (int32_t)ftiBytes.size()));
    std::vector<uint8_t> smallBytes;
    MDK_CHECK(fti.getBytes("FONTSML", &smallBytes));
    mdk::formats::Font small;
    MDK_CHECK(mdk::formats::Font::parse(smallBytes.data(), (int32_t)smallBytes.size(), 4,
                                        &small));
    std::vector<std::string> texts;
    texts.push_back(mdk::EnhancedMenu::kHint);
    texts.push_back(">");
    mdk::EnhancedToggles states[3];
    states[1].cap30Fps = true;
    states[1].hazeDensity = 0;
    states[1].shadowMapSize = 128;
    states[1].shadowMapRefresh = 1;
    states[2].shadowMapSize = 512;
    states[2].shadowMapRefresh = 4;
    states[2].exposure = 50;
    for (int32_t row = 0; row < mdk::EnhancedMenu::kRowCount; row++) {
        texts.push_back(mdk::EnhancedMenu::rowLabel(row));
        for (int32_t s = 0; s < 3; s++) {
            texts.push_back(menuValue(row, states[s]));
        }
    }
    for (size_t t = 0; t < texts.size(); t++) {
        for (size_t c = 0; c < texts[t].size(); c++) {
            const char ch = texts[t][c];
            if (ch != ' ' && !small.glyph((uint8_t)ch)) {
                fprintf(stderr, "  no FONTSML glyph for '%c' in \"%s\"\n", ch, texts[t].c_str());
                MDK_CHECK(false);
            }
        }
    }
}

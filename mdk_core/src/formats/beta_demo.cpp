// ============================================================================
// beta_demo.cpp — see beta_demo.h.
//
// The text parsing is the demo's own shape: a line's words, the first of
// which is a keyword, and numbers after it. The C#'s float.TryParse gives
// zero for a word that is not a number, and so does this.
// ============================================================================

#include "formats/beta_demo.h"

#include <ctype.h>
#include <string.h>

#include <charconv>
#include <utility>

#include "formats/ascii_case.h"
#include "formats/bin_reader.h"
#include "formats/case_insensitive_path.h"
#include "formats/local_paths.h"

namespace mdk {
namespace formats {

namespace {

// The folders the demo is usually unpacked to.
const char* const kFolders[2] = {"BETA96", "MDK (1996-08-06) (beta demo)"};
const char kMarker[] = "TRAVERSE/LEVEL1/LEVEL1.SET";
const char kEnvironmentVariable[] = "MDK_BETA_DIR";

const int32_t kSkyWidth = 1800;
const int32_t kSkyHeight = 360;
const int32_t kSkyWrapExtra = 4;
const int32_t kPaletteBytes = Palette::kSize * 3;

// The camera pitch the retail levels usually give their arenas (degrees).
const float kCameraPitch = 4.0f;
// LEVEL3.SET has no start angle; 90 faces +Y, into the arena.
const float kDefaultStartAngle = 90.0f;
// Black in every palette of the demo (the first colour after the 16 system
// colours).
const uint8_t kOpaqueBlack = 16;
// The textures of the level archives whose index 0 is see-through
// (projectiles).
const char* const kSeeThrough[2] = {"BULLET", "BOLT"};

// The effect textures: loose animated textures (u32 frames, u16 width, u16
// height).
const char* const kEffectTextures[6] = {
    "TRAVERSE/LEVEL1/TEXTURES/TRAIL.LBA", "TRAVERSE/SPRITES/SLIME/SL_BIG.LBA",
    "TRAVERSE/SPRITES/SLIME/SL_MED.LBA", "TRAVERSE/SPRITES/SLIME/SL_SMA.LBA",
    "TRAVERSE/SPRITES/SLIME/SB_MED.LBA", "TRAVERSE/SPRITES/SLIME/SB_SMA.LBA",
};

// Kurt's sprites and the HUD's images the demo has its own versions of:
// file to TRAVSPRT.BNI entry (*.ABB RLE animations, *.LBB plain images).
const char* const kSpriteFiles[20] = {
    "SPRITES/K_STILL.ABB", "SPRITES/K_IDLE.ABB",   "SPRITES/K_RUN.ABB",
    "SPRITES/K_SIDE.ABB",  "SPRITES/K_JUMP.ABB",   "SPRITES/K_RJMP.ABB",
    "SPRITES/K_CHUTE.ABB", "SPRITES/K_SHOT.ABB",   "SPRITES/K_RUNFIR.ABB",
    "SPRITES/K_HANG.ABB",  "SPRITES/K_LOOKU.ABB",  "SPRITES/CROSS.ABB",
    "SPRITES/K_ROLLL.ABB", "SPRITES/K_ROLLR.ABB",  "SPRITES/K_HELM.ABB",
    "SPRITES/K_BCKUP.ABB", "SPRITES/K_LOOKD.ABB",  "SPRITES/K_FIRE_M.ABB",
    "SPRITES/HUD/SC_STAT.LBB", "SPRITES/HUD/SC_BSTAT.LBB",
};

// The record types of the .HOT files, the retail DTI numbers.
const uint32_t kRecordShow = 1;
const uint32_t kRecordAlien = 2;
const uint32_t kRecordPickup = 4;
const uint32_t kRecordHidePoint = 5;
const uint32_t kRecordConnection = 6;
// MSWAP (3 in the demo) is not a retail type 3 record.
const uint32_t kRecordMaterialSwap = 103;

const int32_t kStartWords = 5;
const int32_t kFirstGlassLine = 2;
const int32_t kTeleportWords = 4;
// The tolerance of a triangle on its node's plane.
const float kNodeTolerance = 0.1f;
const int32_t kEmptyLeaf = -1;

// The words of a text file's lines, without comments (';') and empty lines.
//
// The C# splits the line at the first ';' and then on spaces, tabs and
// carriage returns, and drops the empty pieces.
std::vector<std::vector<std::string>> lines(const std::string& text) {
    std::vector<std::vector<std::string>> result;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }

        std::string line = text.substr(start, end - start);
        start = end + 1;

        const size_t comment = line.find(';');
        if (comment != std::string::npos) {
            line = line.substr(0, comment);
        }

        std::vector<std::string> words;
        std::string word;
        for (size_t i = 0; i <= line.size(); i++) {
            const char c = i < line.size() ? line[i] : ' ';
            if (c == ' ' || c == '\t' || c == '\r') {
                if (!word.empty()) {
                    words.push_back(word);
                    word.clear();
                }

                continue;
            }

            word.push_back(c);
        }

        if (!words.empty()) {
            result.push_back(words);
        }
    }

    return result;
}

// A word as a number, or zero when it is not one.
//
// The C#'s float.TryParse with NumberStyles.Float: white space around it, an
// optional sign, a decimal point and an exponent. The parse is done with
// from_chars, which is locale-independent -- the game never changes the
// locale, and the demo's files are written with a full stop.
float number(const std::string& word) {
    size_t start = 0;
    while (start < word.size() && isspace((unsigned char)word[start])) {
        start++;
    }

    size_t end = word.size();
    while (end > start && isspace((unsigned char)word[end - 1])) {
        end--;
    }

    std::string text = word.substr(start, end - start);
    // from_chars takes no leading plus, and the C#'s parse does.
    if (!text.empty() && text[0] == '+') {
        text = text.substr(1);
    }

    float value = 0.0f;
    const char* first = text.c_str();
    const char* last = first + text.size();
    const std::from_chars_result result = std::from_chars(first, last, value);
    if (result.ec != std::errc() || result.ptr != last) {
        return 0.0f;
    }

    return value;
}

// The index of a name, or -1.
int32_t indexOf(const std::vector<std::string>& names, const std::string& name) {
    for (size_t i = 0; i < names.size(); i++) {
        if (names[i] == name) {
            return (int32_t)i;
        }
    }

    return -1;
}

// A record's name, which is twelve bytes and NUL-padded in the retail DTI.
void setRecordName(Dti::Record* record, const std::string& name) {
    memset(record->name, 0, sizeof(record->name));
    const size_t count = name.size() < sizeof(record->name) - 1 ? name.size() : sizeof(record->name) - 1;
    memcpy(record->name, name.data(), count);
}

// A corridor's name starts with a C.
bool isCorridor(const std::string& name) {
    return !name.empty() && name[0] == 'C';
}

// The folder of a demo level.
std::string levelDir(int32_t level) {
    return "TRAVERSE/LEVEL" + std::to_string(level) + "/";
}

// The name of a file without its folder and without its extension.
std::string fileStem(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

// A path with its "." and ".." parts resolved, as the C#'s Path.GetFullPath
// does.
std::string fullPath(const std::string& path) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string::npos) {
            end = path.size();
        }

        const std::string part = path.substr(start, end - start);
        start = end + 1;
        if (part.empty() || part == ".") {
            continue;
        }

        if (part == "..") {
            if (!parts.empty()) {
                parts.pop_back();
            }

            continue;
        }

        parts.push_back(part);
    }

    std::string result;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i > 0) {
            result += "/";
        }

        result += parts[i];
    }

    return result;
}

// A folder joined with a name.
std::string combine(const std::string& dir, const std::string& name) {
    if (dir.empty()) {
        return name;
    }

    if (dir[dir.size() - 1] == '/' || dir[dir.size() - 1] == '\\') {
        return dir + name;
    }

    return dir + "/" + name;
}

} // namespace

BetaDemo BetaDemo::find(const FileSystem* system, const MdkData* data) {
    BetaDemo demo;
    if (!system) {
        return demo;
    }

    demo.system_ = system;

    std::vector<std::string> candidates;
    if (system->getEnvironment) {
        const char* env = system->getEnvironment(system->user, kEnvironmentVariable);
        if (env && env[0] != '\0') {
            candidates.push_back(env);
        }
    }

    const std::string local = localPathsGet(system, kLocalPathsBeta);
    if (!local.empty()) {
        candidates.push_back(local);
    }

    // The demo is looked for in and next to the game's folder, and in and
    // next to the program's.
    std::vector<std::string> roots;
    if (data) {
        roots.push_back(data->dir());
    }

    if (system->programDirectory) {
        roots.push_back(system->programDirectory(system->user));
    }

    for (size_t i = 0; i < roots.size(); i++) {
        if (roots[i].empty()) {
            continue;
        }

        for (size_t k = 0; k < 2; k++) {
            candidates.push_back(combine(roots[i], kFolders[k]));
            candidates.push_back(fullPath(combine(combine(roots[i], ".."), kFolders[k])));
        }
    }

    for (size_t i = 0; i < candidates.size(); i++) {
        const std::string marker = caseInsensitiveResolve(system, candidates[i], kMarker);
        if (system->fileExists && system->fileExists(system->user, marker.c_str())) {
            demo.dir_ = candidates[i];
            demo.found_ = true;
            return demo;
        }
    }

    return demo;
}

std::string BetaDemo::pathOf(const std::string& relative) const {
    if (!system_) {
        return relative;
    }

    return caseInsensitiveResolve(system_, dir_, relative);
}

bool BetaDemo::read(const std::string& relative, std::vector<uint8_t>* outBytes) const {
    if (!system_ || !system_->readFile) {
        return false;
    }

    const std::string path = pathOf(relative);
    if (!system_->fileExists || !system_->fileExists(system_->user, path.c_str())) {
        return false;
    }

    return system_->readFile(system_->user, path.c_str(), outBytes);
}

std::string BetaDemo::text(const std::string& relative) const {
    std::vector<uint8_t> bytes;
    if (!read(relative, &bytes)) {
        return std::string();
    }

    return std::string((const char*)bytes.data(), bytes.size());
}

bool BetaDemo::loadingScreen(int32_t level, std::vector<uint8_t>* outBytes) const {
    if (!outBytes) {
        return false;
    }

    for (size_t i = 0; i < 3; i++) {
        if (kLevels[i] == level) {
            return read("DEMO/SCREEN" + std::to_string(i + 1) + ".LBB", outBytes);
        }
    }

    return false;
}

bool BetaDemo::parseSettings(const std::string& text, Dti* outDti) {
    if (!outDti) {
        return false;
    }

    const std::vector<std::vector<std::string>> allLines = lines(text);
    // The start, the sky's colours and the four glass lines.
    if (allLines.size() < (size_t)(kFirstGlassLine + Dti::kGlassCount)) {
        return false;
    }

    const std::vector<std::string>& start = allLines[0];
    if (start.size() < 4) {
        return false;
    }

    const std::vector<std::string>& sky = allLines[1];
    if (sky.size() < 3) {
        return false;
    }

    Dti dti;
    dti.setStartArena((int32_t)number(start[0]));
    const float position[3] = {number(start[1]), number(start[2]), number(start[3])};
    dti.setStartPosition(position);
    dti.setStartAngle(start.size() >= (size_t)kStartWords ? number(start[4]) : kDefaultStartAngle);
    dti.setSkyTopColor((int32_t)number(sky[0]));
    dti.setSkyBottomColor((int32_t)number(sky[1]));
    dti.setSkyHorizonRow((int32_t)number(sky[2]));

    std::vector<std::vector<uint32_t>> glass;
    for (int32_t i = 0; i < Dti::kGlassCount; i++) {
        const std::vector<std::string>& line = allLines[(size_t)(kFirstGlassLine + i)];
        std::vector<uint32_t> colour;
        for (size_t w = 0; w < line.size() && w < 4; w++) {
            // The C#'s (uint) of a float: the value truncated, and the low
            // 32 bits of it kept.
            const float value = number(line[w]);
            const int32_t whole = (int32_t)value;
            colour.push_back((uint32_t)whole);
        }

        glass.push_back(colour);
    }

    dti.setGlass(glass);
    *outDti = dti;
    return true;
}

bool BetaDemo::parseArenaNames(const std::string& text, std::vector<std::string>* outNames) {
    if (!outNames) {
        return false;
    }

    const std::vector<std::vector<std::string>> allLines = lines(text);
    if (allLines.empty() || allLines[0].empty()) {
        return false;
    }

    std::vector<std::string> names;
    int32_t line = 1;
    const int32_t count = (int32_t)number(allLines[0][0]);
    for (int32_t i = 0; i < count && line < (int32_t)allLines.size(); i++) {
        const std::vector<std::string>& words = allLines[(size_t)line];
        if (words.size() < 2) {
            return false;
        }

        names.push_back(toUpperAscii(words[0]));
        // The portals follow the name, and are not used.
        line += 1 + (int32_t)number(words[1]);
    }

    *outNames = names;
    return true;
}

bool BetaDemo::parseRecords(const std::string& text, const std::vector<std::string>& arenaNames,
                            std::vector<Dti::Record>* outRecords) {
    if (!outRecords) {
        return false;
    }

    std::vector<Dti::Record> records;
    const std::vector<std::vector<std::string>> allLines = lines(text);
    for (size_t i = 0; i < allLines.size(); i++) {
        const std::vector<std::string>& words = allLines[i];
        const std::string keyword = toUpperAscii(words[0]);
        // HIDEPT, CONNECT and MSWAP have no name.
        const bool named = keyword != "HIDEPT" && keyword != "CONNECT" && keyword != "MSWAP";
        const std::string name = (named && words.size() > 1) ? toUpperAscii(words[1]) : std::string();

        std::vector<float> n;
        for (size_t w = named ? 2 : 1; w < words.size(); w++) {
            n.push_back(number(words[w]));
        }

        Dti::Record record;
        bool keep = true;
        if (keyword == "ASHOW") {
            if (n.size() < 4) {
                return false;
            }

            record.type = kRecordShow;
            record.id = indexOf(arenaNames, name);
            record.position[0] = n[0];
            record.position[1] = n[1];
            record.position[2] = 0.0f;
            record.boxEnd[0] = n[2];
            record.boxEnd[1] = n[3];
            record.boxEnd[2] = 0.0f;
            setRecordName(&record, name);
        } else if (keyword == "ALIEN") {
            if (n.size() < 5) {
                return false;
            }

            record.type = kRecordAlien;
            record.id = (int32_t)n[0];
            record.position[0] = n[2];
            record.position[1] = n[3];
            record.position[2] = n[4];
            setRecordName(&record, name);
        } else if (keyword == "PICKUP") {
            if (n.size() < 3) {
                return false;
            }

            record.type = kRecordPickup;
            record.id = -1;
            record.position[0] = n[0];
            record.position[1] = n[1];
            record.position[2] = n[2];
            setRecordName(&record, name);
        } else if (keyword == "HIDEPT") {
            if (n.size() < 4) {
                return false;
            }

            record.type = kRecordHidePoint;
            record.id = (int32_t)n[0];
            record.position[0] = n[1];
            record.position[1] = n[2];
            record.position[2] = n[3];
        } else if (keyword == "CONNECT") {
            if (n.size() < 7) {
                return false;
            }

            record.type = kRecordConnection;
            record.id = (int32_t)n[0];
            record.position[0] = n[1];
            record.position[1] = n[3];
            record.position[2] = n[5];
            record.boxEnd[0] = n[2];
            record.boxEnd[1] = n[4];
            record.boxEnd[2] = n[6];
        } else if (keyword == "MSWAP") {
            record.type = kRecordMaterialSwap;
            record.id = -1;
        } else {
            keep = false;
        }

        if (keep) {
            records.push_back(record);
        }
    }

    *outRecords = records;
    return true;
}

std::vector<std::pair<std::string, Vec3f>> BetaDemo::parseTeleports(const std::string& text) {
    std::vector<std::pair<std::string, Vec3f>> teleports;
    const std::vector<std::vector<std::string>> allLines = lines(text);
    for (size_t i = 0; i < allLines.size(); i++) {
        const std::vector<std::string>& words = allLines[i];
        if ((int32_t)words.size() < kTeleportWords) {
            continue;
        }

        Vec3f position;
        position.x = number(words[1]);
        position.y = number(words[2]);
        position.z = number(words[3]);
        teleports.push_back(std::make_pair(toUpperAscii(words[0]), position));
    }

    return teleports;
}

std::vector<std::string> BetaDemo::arenaNames(int32_t level) const {
    std::vector<std::string> names;
    const std::string dir = levelDir(level);
    parseArenaNames(text(dir + "LEVEL" + std::to_string(level) + ".CON"), &names);
    return names;
}

bool BetaDemo::loadDti(int32_t level, Dti* outDti) const {
    if (!outDti) {
        return false;
    }

    const std::string dir = levelDir(level);
    Dti dti;
    if (!parseSettings(text(dir + "LEVEL" + std::to_string(level) + ".SET"), &dti)) {
        return false;
    }

    std::vector<uint8_t> paletteBytes;
    read(dir + "L" + std::to_string(level) + "_PAL.LBP", &paletteBytes);
    dti.mutablePalette() = Palette::fromRgb(paletteBytes.data(), (int32_t)paletteBytes.size());

    // The sky is a panorama: the first SkyWidth columns cover 360 degrees,
    // and there are four extra pixels for the wrap.
    const int32_t skySize = (kSkyWidth + kSkyWrapExtra) * kSkyHeight;
    std::vector<uint8_t> sky((size_t)skySize, 0);
    std::vector<uint8_t> file;
    read("TRAVERSE/SCREENS/BACK_" + std::to_string(level) + ".LBB", &file);
    const int32_t copied = (int32_t)file.size() < skySize ? (int32_t)file.size() : skySize;
    if (copied > 0) {
        memcpy(sky.data(), file.data(), (size_t)copied);
    }

    // BACK_3.LBB is a copy of the city's sky, in level 1's colours: it is
    // drawn in the nearest colours of the level's own palette.
    if (level == 3) {
        std::vector<uint8_t> cityPalette;
        read(levelDir(1) + "L1_PAL.LBP", &cityPalette);
        sky = remap(sky, Palette::fromRgb(cityPalette.data(), (int32_t)cityPalette.size()),
                    dti.palette());
    }

    dti.setSkyWrapWidth(kSkyWidth);
    Texture panorama;
    panorama.setName("SKY");
    panorama.setWidth(kSkyWidth + kSkyWrapExtra);
    panorama.setHeight(kSkyHeight);
    panorama.setIndices(sky.data(), (int32_t)sky.size());
    dti.setSky(panorama);
    dti.setMirrorSky(panorama);

    const std::vector<std::string> names = arenaNames(level);
    std::vector<Dti::ArenaEntry> arenas;
    for (size_t i = 0; i < names.size(); i++) {
        Dti::ArenaEntry entry;
        entry.name = names[i];
        entry.pitch = kCameraPitch;
        parseRecords(text(dir + "ARENAS/" + names[i] + ".HOT"), names, &entry.records);
        arenas.push_back(entry);
    }

    dti.setArenas(arenas);
    *outDti = dti;
    return true;
}

Arena* BetaDemo::loadWorld(int32_t level, const std::string& name) const {
    std::vector<uint8_t> bytes;
    if (!read(levelDir(level) + "ARENAS/" + name + ".BSP", &bytes) || bytes.empty()) {
        return nullptr;
    }

    Arena* arena = Arena::parseBetaWorld(name, bytes.data(), (int32_t)bytes.size());
    if (!arena) {
        return nullptr;
    }

    // The demo's NotDrawn triangles only stop Kurt.
    arena->setClipFlag(kNotDrawn);
    return arena;
}

bool BetaDemo::loadMto(int32_t level, Mto* outMto, std::vector<Arena*>* outArenas) const {
    if (!outMto || !outArenas) {
        return false;
    }

    const std::string dir = levelDir(level);
    std::vector<uint8_t> bytes;
    read(dir + "LEVEL" + std::to_string(level) + "O.MTO", &bytes);

    // The demo's MTO only has the arenas' textures: u32 count, then per
    // arena char[8] name and a u32 offset. The count is at the START, not
    // at the retail MTO's 0x14.
    std::vector<std::pair<std::string, int32_t>> textures;
    if (!bytes.empty()) {
        BinReader reader(bytes.data(), (int32_t)bytes.size(), 0);
        const uint32_t count = reader.u32();
        for (uint32_t i = 0; i < count; i++) {
            char name[9] = {0};
            reader.name(name, 8);
            textures.push_back(std::make_pair(toUpperAscii(name), (int32_t)reader.u32() + 4));
        }
    }

    Mto mto;
    std::vector<Arena*> arenas;
    const std::vector<std::string> names = arenaNames(level);
    for (size_t i = 0; i < names.size(); i++) {
        if (isCorridor(names[i])) {
            continue;
        }

        Arena* arena = loadWorld(level, names[i]);
        if (!arena) {
            continue;
        }

        for (size_t t = 0; t < textures.size(); t++) {
            if (textures[t].first != names[i]) {
                continue;
            }

            // At the offset: u32 size, then a texture archive without the
            // retail header (the offsets are from its count).
            TextureArchive archive = TextureArchive::parse(bytes.data(), (int32_t)bytes.size(),
                                                           textures[t].second, 0,
                                                           TextureArchive::Names::Upper);
            makeOpaque(&archive);
            arena->setTextures(archive);
            break;
        }

        std::vector<uint8_t> palette;
        read(dir + "ARENAS/" + names[i] + ".LBP", &palette);
        if ((int32_t)palette.size() >= kPaletteBytes) {
            arena->setPaletteRgb(palette.data() + Palette::kArenaFirstIndex * 3,
                                 Arena::kPaletteColors * 3);
        }

        mto.add(arena);
        arenas.push_back(arena);
    }

    // Moved, not copied: an Mto owns the arenas it parsed and may not be
    // copied (see mto.h). These came from add(), so the move transfers the
    // BORROWED pointers only, and the arenas themselves stay the caller's in
    // *outArenas.
    *outMto = std::move(mto);
    // The out-vector OWNS what it holds — the caller frees them, that is what
    // "the arenas belong to the caller" means — so replacing it releases what a
    // previous call left there. Assigning over it leaked a caller's whole first
    // level. Same rule LevelData::load follows for a reused out-param.
    for (size_t i = 0; i < outArenas->size(); i++) {
        delete (*outArenas)[i];
    }
    *outArenas = arenas;
    return true;
}

std::vector<Arena*> BetaDemo::loadCorridors(int32_t level) const {
    std::vector<Arena*> corridors;
    const std::vector<std::string> names = arenaNames(level);
    for (size_t i = 0; i < names.size(); i++) {
        if (!isCorridor(names[i])) {
            continue;
        }

        Arena* arena = loadWorld(level, names[i]);
        if (arena) {
            corridors.push_back(arena);
        }
    }

    return corridors;
}

bool BetaDemo::loadTextures(int32_t level, TextureArchive* outArchive) const {
    if (!outArchive) {
        return false;
    }

    const std::string dir = levelDir(level);
    std::vector<uint8_t> bytes;
    read(dir + "LEVEL" + std::to_string(level) + "S.MTI", &bytes);

    // The demo's archive has no retail header: the count is at 4 and the
    // offsets are from it.
    TextureArchive archive = TextureArchive::parse(bytes.data(), (int32_t)bytes.size(), 4, 0,
                                                   TextureArchive::Names::Upper);
    makeOpaque(&archive);

    for (size_t i = 0; i < 6; i++) {
        std::vector<uint8_t> effect;
        if (!read(kEffectTextures[i], &effect) || effect.empty()) {
            continue;
        }

        const std::string name = fileStem(kEffectTextures[i]);
        if (archive.findTexture(name)) {
            continue;
        }

        archive.add(name, Texture::parseAnimated(name, effect.data(), (int32_t)effect.size(), 0, 0));
    }

    *outArchive = archive;
    return true;
}

std::vector<Sni> BetaDemo::loadSounds(int32_t level) const {
    std::vector<Sni> sounds;
    const std::string dir = levelDir(level);
    const char* files[3] = {"TRAVERSE/TRAVERSE.SNI", nullptr, nullptr};
    std::string levelSound = dir + "LEVEL" + std::to_string(level) + "S.SNI";
    std::string levelObjects = dir + "LEVEL" + std::to_string(level) + "O.SNI";
    files[1] = levelSound.c_str();
    files[2] = levelObjects.c_str();

    for (size_t i = 0; i < 3; i++) {
        std::vector<uint8_t> bytes;
        if (!read(files[i], &bytes) || bytes.empty()) {
            continue;
        }

        Sni archive;
        if (Sni::parseBeta(&archive, bytes.data(), (int32_t)bytes.size())) {
            sounds.push_back(std::move(archive));
        }
    }

    return sounds;
}

bool BetaDemo::loadCmi(int32_t level, Cmi* outCmi) const {
    if (!outCmi) {
        return false;
    }

    const std::string dir = levelDir(level);
    std::vector<uint8_t> bytes;
    read(dir + "LEVEL" + std::to_string(level) + ".CMI", &bytes);

    Cmi cmi;
    if (!Cmi::parseBeta(&cmi, bytes.data(), (int32_t)bytes.size())) {
        return false;
    }

    *outCmi = std::move(cmi);
    return true;
}

std::vector<std::pair<std::string, Vec3f>> BetaDemo::loadTeleports() const {
    return parseTeleports(text("TRAVERSE/TELEPORT.TXT"));
}

void BetaDemo::addSprites(Bni* sprites) const {
    if (!sprites) {
        return;
    }

    for (size_t i = 0; i < 20; i++) {
        std::vector<uint8_t> bytes;
        if (!read(std::string("TRAVERSE/") + kSpriteFiles[i], &bytes) || bytes.empty()) {
            continue;
        }

        sprites->add(fileStem(kSpriteFiles[i]), bytes.data(), (int32_t)bytes.size());
    }
}

void BetaDemo::makeOpaque(TextureArchive* archive) {
    if (!archive) {
        return;
    }

    std::vector<std::pair<std::string, Texture>>& textures = archive->mutableTextures();
    for (size_t i = 0; i < textures.size(); i++) {
        Texture& texture = textures[i].second;
        if (texture.frameCount() != 1) {
            continue;
        }

        bool seeThrough = false;
        for (size_t k = 0; k < 2; k++) {
            if (texture.name() == kSeeThrough[k]) {
                seeThrough = true;
                break;
            }
        }

        if (seeThrough) {
            continue;
        }

        texture.replaceIndex(0, kOpaqueBlack);
    }
}

std::vector<uint8_t> BetaDemo::remap(const std::vector<uint8_t>& indices, const Palette& from,
                                     const Palette& to) {
    // Every colour of `from` is drawn as the nearest colour of `to`.
    std::vector<uint8_t> table((size_t)Palette::kSize, 0);
    for (int32_t i = 0; i < Palette::kSize; i++) {
        int32_t best = 0;
        int32_t bestDistance = 2147483647;
        for (int32_t k = 0; k < Palette::kSize; k++) {
            int32_t distance = 0;
            for (int32_t c = 0; c < 3; c++) {
                const int32_t d = (int32_t)from.rgba()[i * 4 + c] - (int32_t)to.rgba()[k * 4 + c];
                distance += d * d;
            }

            if (distance < bestDistance) {
                best = k;
                bestDistance = distance;
            }
        }

        table[(size_t)i] = (uint8_t)best;
    }

    std::vector<uint8_t> out(indices.size(), 0);
    for (size_t i = 0; i < indices.size(); i++) {
        out[i] = table[indices[i]];
    }

    return out;
}

int32_t BetaDemo::checkNodes(const Arena& arena) {
    const std::vector<BspNode>& nodes = arena.nodes();
    const std::vector<Vec3f>& vertices = arena.vertices();
    const std::vector<int32_t>& indices = arena.triangleIndices();
    const int32_t triangleCount = arena.triangleCount();

    int32_t bad = 0;
    for (size_t i = 0; i < nodes.size(); i++) {
        const BspNode& node = nodes[i];
        const int32_t firsts[2] = {node.frontFirst, node.backFirst};
        const int32_t counts[2] = {node.frontCount, node.backCount};

        bool off = false;
        for (int32_t list = 0; list < 2 && !off; list++) {
            if (counts[list] <= 0) {
                continue;
            }

            if (firsts[list] < 0 || firsts[list] + counts[list] > triangleCount) {
                off = true;
                break;
            }

            for (int32_t t = firsts[list]; t < firsts[list] + counts[list] && !off; t++) {
                for (int32_t k = 0; k < 3; k++) {
                    const int32_t at = t * 3 + k;
                    if (at >= (int32_t)indices.size()) {
                        off = true;
                        break;
                    }

                    const int32_t vertex = indices[(size_t)at];
                    if (vertex < 0 || vertex >= (int32_t)vertices.size()) {
                        off = true;
                        break;
                    }

                    const float distance = node.distance(vertices[(size_t)vertex]);
                    if (distance < 0.0f ? -distance > kNodeTolerance : distance > kNodeTolerance) {
                        off = true;
                        break;
                    }
                }
            }
        }

        const int32_t children[2] = {node.negative, node.positive};
        bool badChildren = false;
        for (int32_t c = 0; c < 2; c++) {
            if (children[c] < kEmptyLeaf || children[c] >= (int32_t)nodes.size()) {
                badChildren = true;
            }
        }

        if (off || badChildren) {
            bad++;
        }
    }

    return bad;
}

} // namespace formats
} // namespace mdk

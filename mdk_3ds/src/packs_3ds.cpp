// ============================================================================
// packs_3ds.cpp -- see packs_3ds.h.
// ============================================================================

#include "packs_3ds.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <set>

#include "formats/kurt_pack.h"
#include "menu_screens_3ds.h"
#include "platform/clock_3ds.h"
#include "platform/log_3ds.h"
#include "render/render_c3d_hd_set.h"

namespace mdk {

// THE HD PACK (docs/HD_TEXTURES_3DS_SPEC.md): built on the host by
// mdk_3ds/tools/build_hd_pack.sh and copied to the SD card. Its manifest lists
// every .t3x by key, so the pack is indexed by ModImages' own rule
// (ModImages::fromList) without scanning the card. In play a surface's HD file is
// found BY NAME through the orig pack's name index (loadOrigByName, spec 3.1:
// no content hash on the console), and the pair is made from its two files
// (mdk_render_create_compressed_file): the backend streams the HD copy under the
// HD budget while the toggle is on and never reads the original for it.
const char kHdPackRoot[] = "sdmc:/3ds/mdk/hd";

// KURT'S COMPRESSED FRAMES (EnhancedToggles::kurtCompressed): his ORIGINAL
// TRAVSPRT.BNI frames as ETC1A4, made on the host by
// mdk_3ds/tools/kurt_etc1_convert.py, packed into ONE file by kurt_pack.py
// (formats/kurt_pack.h) and staged into the RomFS by stage_romfs.sh (the set is
// derived from the game data the RomFS already carries, so it ships in every
// .3dsx and .cia and the hardware loop needs no separate SD copy).
const char kKurtPackPath[] = "romfs:/kurt/kurt.pak";

// THE ORIG PACK: the ORIGINAL-LOOK arena textures pre-encoded on the host --
// the same manifest, the same hdKeyOf keys, the same .t3x import as the HD
// pack -- staged into the RomFS by stage_romfs.sh, so it ships in every .3dsx
// and .cia and the hardware loop needs no separate SD copy. Its point is the
// level-load / arena-stream-in hitches: an arena index texture expanded on the
// ARM11 (palette -> RGBA8 -> 16-bit pack -> mip chain) costs 600-1600 ms per
// arena. A surface the pack holds is ASKED FOR from its .t3x at first use
// (surface_cache.h's pre-creation consult -> mdk_render_create_compressed_file):
// the renderer's reader thread reads the file off the frame, a frame boundary
// imports it (a copy: the containers are uncompressed), and only a surface in
// view that is still unread is read on the main thread. The expansion never
// runs.
// With no pack staged (the tool did not run) the manifest open fails silently
// and the expansion is today's, exactly.
const char kOrigPackRoot[] = "romfs:/orig";

// A compressed pack's manifest, indexed for `level` by ModImages' own rule.
// False when there is none. `tag` names the pack in its log lines ("hd",
// "orig"); `maxCap` is the largest build cap the backend budgets for it (see
// the column-8 gate below).
//
// `dims` (may be null) gets each file's SOURCE size, columns 3 and 4, keyed by
// the file's key (column 1, no .t3x): for the orig pack that is the original
// texture's width and stacked height (arena_etc1_convert.py), which the name
// index checks a texture against (loadOrigByName). `files` (may be null) gets
// the manifest's file list. `stamp` (may be null) gets the file's size and
// FNV-1a 64 hash: the orig manifest's is what romfs:/orig/byname.bin must have
// been built from (game/orig_byname.h).
//
// ONE READ (AGENTS.md P5): the file is read whole and checked whole, then cut
// into lines in memory -- a line-by-line fgets of a RomFS or SD file is a file
// system request per stdio buffer. The lines are the ones fgets(512) gave
// (BynameIndex::nextLine): one of more than 510 characters, its newline not
// counted, is skipped (no pack line is that long; review F10).
static bool loadCompressedManifest(const char* root, int32_t level, mdk::ModImages* out,
                            int32_t* count, const char* tag, int32_t maxCap,
                            std::map<std::string, uint32_t>* dims = nullptr,
                            std::vector<std::string>* filesOut = nullptr,
                            std::map<std::string, uint32_t>* shapes = nullptr,
                            OrigManifestStamp* stamp = nullptr) {
    const std::string manifest = std::string(root) + "/manifest.txt";
    FILE* file = fopen(manifest.c_str(), "rb");
    if (!file) {
        return false;
    }
    long size = -1;
    if (fseek(file, 0, SEEK_END) == 0) {
        size = ftell(file);
    }
    if (size < 0 || size > (long)(16u << 20) || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        mdk::log3dsPrintf("%s: manifest %s: size unreadable (%ld); not used", tag,
                          manifest.c_str(), size);
        return false;
    }
    std::vector<char> text((size_t)size);
    const size_t got = size > 0 ? fread(text.data(), 1, text.size(), file) : 0;
    fclose(file);
    if (got != text.size()) {
        mdk::log3dsPrintf("%s: manifest %s: short read (%u of %ld bytes); not used", tag,
                          manifest.c_str(), (unsigned)got, size);
        return false;
    }
    if (stamp) {
        stamp->valid = true;
        stamp->bytes = (uint32_t)text.size();
        stamp->hash = mdk::formats::BynameIndex::hash((const uint8_t*)text.data(), text.size());
    }
    std::vector<std::string> files;
    char line[512];
    bool tooBig = false;
    size_t pos = 0;
    const char* start = nullptr;
    size_t length = 0;
    bool newline = false;
    while (mdk::formats::BynameIndex::nextLine(text.data(), text.size(), &pos, &start, &length,
                                               &newline)) {
        // As fgets gave it: the newline kept, NUL-terminated.
        memcpy(line, start, length);
        if (newline) {
            line[length] = '\n';
        }
        line[length + (newline ? 1 : 0)] = '\0';
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }
        // Column 8 is the size cap the pack was built with
        // (hd_pack_convert.py): the backend budgets the largest pack texture
        // (kHdLargestBytes, sized for the worst file of the largest cap it
        // accepts), so a pack built above `maxCap` is refused, not drawn over
        // budget. The HD pack is capped at 512; the orig pack's arena textures
        // pad up to 1024x512 and is capped at 1024 -- kHdLargestBytes covers
        // that worst case (01_types.inc).
        {
            int column = 0;
            const char* field = line;
            for (const char* p = line; *p && column < 8; p++) {
                if (*p == '\t') {
                    column++;
                    field = p + 1;
                }
            }
            if (column == 8 && atoi(field) > maxCap) {
                tooBig = true;
            }
        }
        char* tab = strchr(line, '\t');
        if (!tab) {
            continue;
        }
        *tab = '\0';
        if (shapes) {
            // key \t format \t src_w \t src_h \t w \t h ...: the .t3x's own size and
            // format (an HD file's exact bytes, hdChainBytes), keyed by its path.
            const char* format = tab + 1;
            const char* c2 = strchr(format, '\t');
            const char* c3 = c2 ? strchr(c2 + 1, '\t') : nullptr;
            const char* c4 = c3 ? strchr(c3 + 1, '\t') : nullptr;
            const char* c5 = c4 ? strchr(c4 + 1, '\t') : nullptr;
            if (c4 && c5) {
                const uint32_t w = (uint32_t)atoi(c4 + 1);
                const uint32_t h = (uint32_t)atoi(c5 + 1);
                const bool a4 = strncmp(format, "etc1a4", 6) == 0;
                (*shapes)[std::string(root) + "/" + std::string(line) + ".t3x"] =
                    (w << 16) | (h & 0xFFFFu) | (a4 ? 0x80000000u : 0u);
            }
        }
        if (dims) {
            // key \t format \t src_w \t src_h ...
            const char* format = tab + 1;
            const char* srcW = strchr(format, '\t');
            const char* srcH = srcW ? strchr(srcW + 1, '\t') : nullptr;
            if (srcW && srcH) {
                const uint32_t w = (uint32_t)atoi(srcW + 1);
                const uint32_t h = (uint32_t)atoi(srcH + 1);
                (*dims)[std::string(line)] = (w << 16) | (h & 0xFFFFu);
            }
        }
        files.push_back(std::string(line) + ".t3x");
    }
    if (tooBig) {
        mdk::log3dsPrintf("%s: pack built above the %d cap the 3DS budgets for; not used", tag,
                          (int)maxCap);
        return false;
    }
    *out = mdk::ModImages::fromList(root, files, true, level);
    *count = (int32_t)files.size();
    if (filesOut) {
        filesOut->swap(files);
        return !filesOut->empty();
    }
    return !files.empty();
}

static std::string lowerName3ds(const std::string& s);

// THE HD PACK BY CONTENT KEY, for the name index (loadOrigByName): every
// texture file of the pack, keyed by the hdKeyOf in its name
// (textures/LEVELn/NAME@<key>.t3x -- hd_pack_convert.py keeps the PC mod's
// names). The key is the texture seen through a palette (game/hd_key.h), the
// same key the orig pack's file names carry, so an orig file and an HD file with
// one key are the same texture through the same palette -- matched here by
// STRING, with no hash computed on the console. Only exact-view files (`@`):
// the pack has no other kind (hd_pack_convert.py writes the PC mod's tree, and
// the C# port's --upscale-textures names every texture by its view).
bool loadHdPack(int32_t level, mdk::ModImages* out,
                std::map<std::string, std::string>* byKey,
                std::map<std::string, uint32_t>* shapes) {
    int32_t count = 0;
    std::vector<std::string> files;
    if (!loadCompressedManifest(kHdPackRoot, level, out, &count, "hd", 512, nullptr, &files,
                                shapes)) {
        return false;
    }
    for (size_t i = 0; i < files.size(); i++) {
        const std::string& file = files[i];
        if (file.compare(0, 9, "textures/") != 0) {
            continue;
        }
        const size_t slash = file.rfind('/');
        const size_t mark = file.find('@', slash == std::string::npos ? 0 : slash);
        const size_t dot = file.rfind('.');
        if (mark == std::string::npos || dot == std::string::npos || dot <= mark + 1) {
            continue;
        }
        (*byKey)[lowerName3ds(file.substr(mark + 1, dot - mark - 1))] =
            std::string(kHdPackRoot) + "/" + file;
    }
    mdk::log3dsPrintf("hd: pack manifest %d textures, %d keyed (%s)", (int)count,
                      (int)byKey->size(), kHdPackRoot);
    return true;
}

// THE BYTES AN HD FILE HOLDS IMPORTED (hdShapes' w, h, ETC1A4 bit) with its
// levels wider or taller than `cap` dropped (0: none) -- the backend's own drop
// rule (04_hd_stream.inc hdUnpack) and tex3ds' chain, levels down to 8 texels on
// the shorter side: ETC1 4 bits a texel, ETC1A4 8.
uint32_t hdChainBytes(uint32_t shape, int32_t cap) {
    uint32_t w = (shape >> 16) & 0x7FFFu;
    uint32_t h = shape & 0xFFFFu;
    const uint32_t bits = (shape & 0x80000000u) ? 8u : 4u;
    while (cap > 0 && (w > (uint32_t)cap || h > (uint32_t)cap) && w >= 16 && h >= 16) {
        w >>= 1;
        h >>= 1;
    }
    uint32_t bytes = 0;
    while (w >= 8 && h >= 8) {
        bytes += w * h * bits / 8u;
        if (w == 8 || h == 8) {
            break;
        }
        w >>= 1;
        h >>= 1;
    }
    return bytes;
}

// The orig pack, at the same level-load moment and through the same machinery
// as the HD pack (loadCompressedManifest). Its textures are the ORIGINAL look
// pre-encoded -- original resolution, a full gaussian mip chain the sampler
// only descends under the Enhanced Mode filtering+mipmaps pair (06_draw.inc)
// -- so with the pack staged the ARM11 expands nothing for the arenas that
// have a key, and the original look is the ETC1 copy sampled NEAREST at level 0
// rather than the palette expansion.
bool loadOrigPack(int32_t level, mdk::ModImages* out, std::map<std::string, uint32_t>* dims,
                  OrigManifestStamp* stamp) {
    int32_t count = 0;
    if (!loadCompressedManifest(kOrigPackRoot, level, out, &count, "orig", 1024, dims, nullptr,
                                nullptr, stamp)) {
        return false;
    }
    mdk::log3dsPrintf("orig: pack manifest %d textures (%s)", (int)count, kOrigPackRoot);
    return true;
}

static std::string lowerName3ds(const std::string& s) {
    std::string out = s;
    for (size_t i = 0; i < out.size(); i++) {
        if (out[i] >= 'A' && out[i] <= 'Z') {
            out[i] = (char)(out[i] - 'A' + 'a');
        }
    }
    return out;
}

// THE NAME-KEYED ORIG INDEX (game/orig_byname.h: romfs:/orig/byname.bin, the
// build-time table, or byname.txt when the table is refused; logged either way,
// with the time it took). manifest.txt is keyed by the CONTENT hash, so a
// ModImages lookup re-hashes the texture's whole index array -- 60-150 ms per
// texture on the ARM11, measured, which is the entire cost of the texture pumps.
// The build also knows every texture by name (arena_etc1_convert.py writes
// byname.txt from the exporter's index.tsv), so the pump and the draw can find
// the file by (arena, name) for free. A miss
// falls through to the hash path: a stale or absent name index costs time,
// never correctness. Keyed by the arena's PALETTE because that is what the
// lookup is handed -- and the palette is what the .t3x's pixels were encoded
// through. A corridor shares its arena's palette OBJECT (LevelData::paletteOf),
// so the two collide here on purpose: their rows name the same file (checked
// for all 3156 corridor rows of the shipped pack, 2026-11-23), and a row that
// ever disagrees is logged rather than silently kept.
//
// AND THE HD PACK BY NAME, from the same rows (docs/HD_TEXTURES_3DS_SPEC.md
// 3.1): a row's file is named by its content key (textures/name@<key>), and the
// HD pack's file of that key (loadHdPack's byKey) is the upscale of exactly that
// texture through exactly that palette -- so the HD file is found by (palette,
// name) too, and play never hashes a texture to find it. No pack has to be
// rebuilt: the owner's SD pack is keyed this way already. `dims` is the original
// texture's width and stacked height from the orig manifest: a texture asked for
// under a row's name but of another size is not that row's (no file is answered,
// and the texture keeps the path a miss takes).
bool loadOrigByName3ds(int32_t level, mdk::LevelData& levelData,
                       const OrigManifestStamp& manifest,
                       std::map<std::string, std::string>* hdByKey,
                       const std::map<std::string, uint32_t>& origDims, OrigByName* out) {
    const double started = mdk::nowSeconds();
    BynameLoadReport report;
    const bool have = loadOrigByName(kOrigPackRoot, level, levelData, manifest, hdByKey, origDims,
                                     out, &report);
    const double ms = (mdk::nowSeconds() - started) * 1000.0;
    if (report.source == BynameSource::Text || report.source == BynameSource::None) {
        mdk::log3dsPrintf("orig: byname.bin REFUSED (%s); the index is %s", report.refused.c_str(),
                          report.source == BynameSource::Text ? "built from byname.txt"
                                                              : "absent (no byname.txt either)");
    }
    for (size_t i = 0; i < report.collisionLines.size(); i++) {
        mdk::log3dsPrintf("orig: byname collision %s", report.collisionLines[i].c_str());
    }
    if (report.collisions > (int32_t)report.collisionLines.size()) {
        mdk::log3dsPrintf("orig: byname collisions: %d in all", (int)report.collisions);
    }
    if (have) {
        mdk::log3dsPrintf("orig: byname index %d textures, %d with an HD copy (%s: %d rows, %d "
                          "arenas, %u bytes in %d reads, %.1f ms)",
                          (int)out->answers, (int)out->hdAnswers,
                          report.source == BynameSource::Binary ? "byname.bin" : "byname.txt",
                          (int)out->index.rowCount(), (int)out->index.arenaCount(),
                          (unsigned)report.bytesRead, (int)report.reads, ms);
    }
    return have;
}

// KURT'S PACK AT LEVEL LOAD (cold; AGENTS.md P5). ONE open of romfs:/kurt/kurt.pak,
// the 32-byte header, then the index and the start set in ONE contiguous read
// (kurt_pack.py lays the start set out right after the index), and the file is
// closed: the other animations are read later, a range at a time, by the
// backend's reader thread (KurtSprite's stream), never inside a frame. Every
// read is checked: no file, a short read, a bad magic/version/layout or a file
// shorter than its header says is logged ONCE here and Kurt's frames take the
// 16-bit path for the level -- nothing is retried. A bad index ENTRY costs only
// its own frame (KurtSprite::useCompressedPack) and is named here.
bool loadKurtPack(const char* path, KurtPackLoad* out) {
    FILE* file = fopen(path, "rb");
    if (!file) {
        mdk::log3dsPrintf("sprites: no pack at %s; Kurt's frames are 16-bit", path);
        return false;
    }
    out->opened = 1;
    long length = -1;
    if (fseek(file, 0, SEEK_END) == 0) {
        length = ftell(file);
    }
    uint8_t header[mdk::formats::KurtPack::kHeaderBytes];
    bool ok = length >= 0 && fseek(file, 0, SEEK_SET) == 0 &&
              fread(header, 1, sizeof(header), file) == sizeof(header);
    if (ok) {
        out->bytesRead += (int32_t)sizeof(header);
    }
    mdk::formats::KurtPack::Header parsed;
    const mdk::formats::KurtPack::Status status =
        ok ? mdk::formats::KurtPack::parseHeader(header, sizeof(header),
                                                 length > 0x7FFFFFFFL ? 0x7FFFFFFFu
                                                                      : (uint32_t)length,
                                                 &parsed)
           : mdk::formats::KurtPack::Status::TooSmall;
    if (status != mdk::formats::KurtPack::Status::Ok) {
        fclose(file);
        mdk::log3dsPrintf("sprites: pack %s refused (%s, %ld bytes); Kurt's frames are 16-bit",
                          path, mdk::formats::KurtPack::statusName(status), length);
        return false;
    }
    // The header's bounds (parseHeader) keep this under the file's size.
    const uint32_t frontBytes = parsed.startEnd - mdk::formats::KurtPack::kHeaderBytes;
    out->front.resize(frontBytes);
    const size_t got = frontBytes > 0 ? fread(out->front.data(), 1, frontBytes, file) : 0;
    fclose(file);
    if (got != frontBytes) {
        mdk::log3dsPrintf("sprites: pack %s: short read of the index and start set (%u of %u "
                          "bytes); Kurt's frames are 16-bit",
                          path, (unsigned)got, (unsigned)frontBytes);
        std::vector<uint8_t>().swap(out->front);
        return false;
    }
    out->bytesRead += (int32_t)frontBytes;
    const uint32_t indexBytes = parsed.indexEnd() - mdk::formats::KurtPack::kHeaderBytes;
    if (mdk::formats::KurtPack::parseIndex(parsed, out->front.data(), indexBytes, &out->pack) !=
        mdk::formats::KurtPack::Status::Ok) {
        mdk::log3dsPrintf("sprites: pack %s: index refused; Kurt's frames are 16-bit", path);
        std::vector<uint8_t>().swap(out->front);
        return false;
    }
    if (out->pack.badEntries() > 0) {
        const int32_t first = out->pack.firstBadEntry();
        mdk::log3dsPrintf("sprites: pack %s: %d bad index entries (first #%d %s: %s); those "
                          "frames are 16-bit",
                          path, (int)out->pack.badEntries(), (int)first,
                          out->pack.entries()[(size_t)first].name.c_str(),
                          out->pack.firstBadReason());
    }
    return true;
}

// The surface cache's lookup into the compressed packs: a Lit or Sprite
// surface's .t3x path (a Sprite takes only an orig-pack answer: SurfaceCache
// surfaceId), answered by the pair and its names, never by the shading. `pack`
// is the HD pack (the SD card's upscale), `orig` the orig pack (the RomFS's
// original-resolution pre-encode); either may be absent.
// THE LOOKUP'S MISSES, the in-level textures the console will expand from their
// indices (docs/3DS_ARCHITECTURE.md 5): each name once a level (the source is the
// level's), with its size and why. Cold: a set lookup, a line the first time.
static void logLookupMiss(HdPackSource* source, const std::string& name,
                          const mdk::formats::Texture& texture, const char* why) {
    if (!source->missLogged.insert(name).second) {
        return;
    }
    mdk::log3dsPrintf("pack: miss %s %dx%d x%d frames: %s (expanded on this core)",
                      name.c_str(), (int)texture.width(), (int)texture.height(),
                      (int)texture.frameCount(), why);
}

bool hdPackLookup(void* user, const mdk::Surface& surface, const mdk::formats::Palette* palette,
                  mdk::ColourReplacement* out) {
    HdPackSource* source = static_cast<HdPackSource*>(user);
    if (!source || (!source->pack && !source->orig) || !surface.texture || !palette) {
        return false;
    }
    const std::string& name = surface.name.empty() ? surface.texture->name() : surface.name;
    // THE NAME INDEX FIRST, with or without an HD pack: the build knows this
    // texture by name, so the content hash is the fallback, not the rule. Both
    // names it could be keyed by are tried -- the material's and the texture's
    // own -- because an archive can hold a texture under a different name than
    // its own. A row whose original size is not this texture's is not this
    // texture's file: it is skipped (counted), never answered.
    //
    // THE HD RULE (docs/HD_TEXTURES_3DS_SPEC.md 3.1): a row with an HD file
    // answers BOTH files -- the packed original (compressedPath) and the HD copy
    // (hdPath) -- and the backend makes the texture from them alone
    // (mdk_render_create_compressed_file): with enableHdTextures on only the HD
    // file is read, with it off only the original, and the toggle stays live per
    // draw. No standard-definition pixel is read, expanded or hashed for it. An
    // ANIMATED texture (stacked frames) takes no HD copy: the pack's strip is
    // resampled to a power of two (hd_pack_convert.py), which squeezes its
    // frames -- EXPLODE's 26 frames of 128 rows become 19.7 rows each -- and the
    // backend never draws an HD copy for a stacked texture (06_draw.inc).
    bool sizeMiss = false;
    if (source->byname && source->byname->answers > 0) {
        std::string keys[2] = {lowerName3ds(name), lowerName3ds(surface.texture->name())};
        const uint32_t size =
            ((uint32_t)surface.texture->width() << 16) |
            ((uint32_t)(surface.texture->height() * surface.texture->frameCount()) & 0xFFFFu);
        for (int k = 0; k < 2; k++) {
            const int32_t hit = source->byname->find((const void*)palette, keys[k].c_str());
            if (hit < 0) {
                continue;
            }
            const mdk::formats::BynameIndex::Row row = source->byname->index.row((uint32_t)hit);
            if (row.hasDims() && row.dims != size) {
                sizeMiss = true;
                continue;
            }
            source->byname->origPath(kOrigPackRoot, (uint32_t)hit, &source->path);
            out->compressedPath = source->path.c_str();
            const std::string* hd = source->byname->hd[(uint32_t)hit];
            if (hd && surface.texture->frameCount() <= 1) {
                source->hdPath = *hd;
                out->hdPath = source->hdPath.c_str();
            }
            return true;
        }
    }
    // A HINT ANSWERS FROM THE NAME INDEX ONLY. The content hash below reads
    // and hashes the texture's whole index array (and, for an arena texture
    // whose pixels are away, first brings back its arena's whole texture block,
    // LevelData::loadPixelsFromMto). Measured on the Old 3DS: the object warm's
    // step for EXPLODE -- before the pack held its 128x3328 strip, when the name
    // index missed -- spent 104.0 ms of frame 1 in that hash. A hint exists to take work OFF the frame, so it never pays
    // for a hash: `true` with no path is SurfaceCache::requestPacked's "not a
    // pack answer" -- nothing made, nothing marked -- and the texture's first
    // draw asks the full lookup, exactly as without the warm.
    if (source->hint) {
        return true;
    }
    // WITH AN HD PACK, A NAME MISS IS NOT HASHED. The hash is what finds a
    // texture in either pack by content, and it is the cost the HD rule forbids
    // in play: 60-150 ms per texture on the Old 3DS, plus the MTO read-back of
    // its pixels. A texture the name index does not know -- one over the PICA's
    // 1024 (refused from both packs by the builds), a model's texture outside
    // the arenas' archives -- is drawn the way it would be with no pack at all:
    // expanded from its indices. (An HD pack is only kept for a level with a
    // name index: main refuses it without one.)
    if (source->pack) {
        logLookupMiss(source, name, *surface.texture, sizeMiss ? "size differs from the index"
                                                               : "not in the name index");
        return false;
    }
    // NOTHING OVER THE PICA'S 1024 IS HASHED. The orig pack holds the three
    // animated strips over it (sl_big 64x1920, fire 64x2628, explode 128x3328)
    // as pages, found by the name index above; the content hash cannot find
    // pages, and hashing EXPLODE's 128x3328 strip costs ~100 ms on the Old 3DS
    // (frame 650 of the L7 walk: obj 105.5 ms once Sprites asked the pack).
    if (surface.texture->width() > 1024 ||
        surface.texture->height() * surface.texture->frameCount() > 1024) {
        logLookupMiss(source, name, *surface.texture, "over 1024, not in the name index");
        return false;
    }
    // THE ORIG PACK BY CONTENT, with no HD pack and a name the index does not
    // know: the hdKeyOf key, so a stamped texture (05_resources.inc rekeys it)
    // matches nothing and takes the index path.
    if (source->orig && source->orig->resolve(mdk::ModImages::Kind::Textures, name,
                                              *surface.texture, *palette, &source->path)) {
        out->compressedPath = source->path.c_str();
        // THE ORIG PACK'S HIT IS ASKED FOR UP FRONT, not attached after an
        // expansion: surfaceId's pre-creation consult (surface_cache.h) turns
        // this into mdk_render_create_compressed_file, so the ARM11 expands
        // nothing for this texture -- the arena's 600-1600 ms of palette
        // expansion, filter and mip chain becomes a RomFS read on the reader
        // thread and an import at a frame boundary.
        return true;
    }
    logLookupMiss(source, name, *surface.texture, "in neither pack");
    return false;
}

void requestPackedSurface(void* user, const mdk::Surface& surface,
                          const mdk::formats::Palette* palette) {
    WarmAsk* ask = static_cast<WarmAsk*>(user);
    ask->source->hint = true;
    ask->surfaces->requestPacked(surface, palette);
    ask->source->hint = false;
}

// The effects warm's start for a large sprite (EffectsView::warmStep): ask the
// shared cache for its texture, starting its make -- the pending id when the
// pack holds it, else a sliced expansion.
void startEffectSurface(void* user, const mdk::Surface& surface,
                        const mdk::formats::Palette* palette) {
    static_cast<WarmAsk*>(user)->surfaces->prepare(surface, palette);
}

// KURT'S COMPRESSED FRAMES and the start set, at level load (cold; AGENTS.md P5:
// no SD read inside a frame): one open of the pack, its index and the start set
// in one read, the start set imported, the read's buffer freed; the other
// animations stream later (KurtSprite::pumpStream). One log line. With
// kurtCompressed off every frame is the expansion, in the texture16 store.
void loadKurtFrames3ds(KurtSprite* kurtSprite, const EnhancedToggles& toggles, int32_t level) {
    const double started = mdk::nowSeconds();
    mdk::KurtSprite::CompressedReport report;
    mdk::KurtPackLoad packLoad;
    const bool have = toggles.kurtCompressed &&
                      mdk::loadKurtPack(mdk::kKurtPackPath, &packLoad);
    kurtSprite->useCompressedPack(have ? &packLoad.pack : nullptr,
                                 level, mdk::kKurtPackPath, &report);
    const double read = mdk::nowSeconds();
    if (have) {
        const mdk::formats::KurtPack::Header& header =
            packLoad.pack.header();
        const uint32_t skip =
            header.dataOffset - mdk::formats::KurtPack::kHeaderBytes;
        kurtSprite->preloadStartSet(packLoad.front.data() + skip,
                                   header.dataOffset,
                                   header.startEnd - header.dataOffset,
                                   &report);
    }
    // The staging buffer goes now: the start set's GPU copies
    // are made (and the backend keeps each one's .t3x as its
    // re-make source, startFileBytes).
    std::vector<uint8_t>().swap(packLoad.front);
    const double done = mdk::nowSeconds();
    mdk::log3dsPrintf(
        "sprites: kurtCompressed=%d texture16=%d pack=%s: %d file opened, "
        "%d bytes read in %d ms; %d/%d frames ETC1A4 (%d bad), %d "
        "animations to stream; start set %d/%d frames imported, %d GPU "
        "bytes of %d, from %d bytes of .t3x, in %d ms; %u bytes of .t3x "
        "kept on the heap (stream bound %u)",
        (int)toggles.kurtCompressed, (int)toggles.texture16,
        have ? mdk::kKurtPackPath : "none", (int)packLoad.opened,
        (int)packLoad.bytesRead, (int)((read - started) * 1000.0),
        (int)report.compressed, (int)report.frames, (int)report.badFrames,
        (int)report.streamable, (int)report.preloaded,
        (int)report.startFrames, (int)report.preloadBytes,
        (int)report.preloadBudget, (int)report.startFileBytes,
        (int)((done - read) * 1000.0),
        (unsigned)kurtSprite->streamStats().staged,
        (unsigned)mdk::KurtSprite::kStagedCacheBytes);
}

// THE HD WINDOW AT LEVEL LOAD (hd_window_3ds.h, docs/3DS_ARCHITECTURE.md 3-4).
// Every arena's HD files -- the name index's rows for that arena, so everything
// drawn through the arena's palette -- with their bytes at 512, 256 and 128, the
// neighbours the level names, and the fit: every arena's window (it, its
// neighbours, the arenas its triggers and scripts name) against the free linear
// heap less kHdPlayReserve. Measured for the shipped levels: the largest window
// is 8.8 MiB (level 8 at CGUNT_9), every level fits at 512 in the 30 MiB heap.
//
// kHdPlayReserve is what play still takes from the linear heap after this point
// (Kurt's animation stream, the uncovered textures, effects, meshes): measured
// on the Old 3DS level-7 walk, 6.27 MB between this point and the walk's lowest
// free, so 7 MiB.
uint32_t buildHdWindow3ds(int32_t level, const OrigByName& origByName,
                          const std::map<std::string, uint32_t>& hdShapes,
                          const LevelData& levelData, const formats::Cmi* cmi, HdWindow* out) {
    static const uint32_t kHdPlayReserve = 7u * 1024u * 1024u;
    out->clear();
    int32_t unsized = 0;
    std::string arena;
    for (size_t r = 0; r < origByName.hdArenaRows.size(); r++) {
        const std::string& path = *origByName.hdArenaRows[r].path;
        const std::map<std::string, uint32_t>::const_iterator shape = hdShapes.find(path);
        if (shape == hdShapes.end()) {
            unsized++;
            continue;
        }
        const uint32_t bytes[HdWindow::kTiers] = {hdChainBytes(shape->second, 0),
                                                  hdChainBytes(shape->second, 256),
                                                  hdChainBytes(shape->second, 128)};
        arena.assign(origByName.arenaOf(origByName.hdArenaRows[r].row));
        out->addFile(arena, path, bytes);
    }
    if (out->fileCount() == 0) {
        mdk::log3dsPrintf("hd: level %d has no HD files (%d unsized)", (int)level, (int)unsized);
        return 0;
    }
    const int32_t named = addLevelNeighbours(levelData.dti(), cmi, out);
    const uint32_t freeNow = (uint32_t)linearSpaceFree();
    const uint32_t room = freeNow > kHdPlayReserve ? freeNow - kHdPlayReserve : 0u;
    const uint32_t worst = out->fit(room);
    mdk::log3dsPrintf("hd: level %d window: %d arenas, %d files (%d unsized), %d script/trigger "
                      "neighbours; largest window %u bytes, room %u (linear free %u less the %u "
                      "reserve)%s",
                      (int)level, (int)out->arenaCount(), (int)out->fileCount(), (int)unsized,
                      (int)named, (unsigned)worst, (unsigned)room, (unsigned)freeNow,
                      (unsigned)kHdPlayReserve, worst > room ? ": DOES NOT FIT at 128" : "");
    for (size_t i = 0; i < out->steppedDown().size(); i++) {
        mdk::log3dsPrintf("hd: %s steps down to %d to fit", out->steppedDown()[i].first.c_str(),
                          (int)out->steppedDown()[i].second);
    }
    return worst;
}

// THE WAIT for HD files (docs/3DS_ARCHITECTURE.md 4): the backend reads and
// imports while this thread waits, the applet loop served throughout (HOME,
// sleep, power), as the original's BSPShow loaded an arena before showing it.
// With `loading` (the level start's), its bar moves from `from` to full as the
// files land: `asked`, the files still to come when the wait starts, is the
// level-start window's size less what is already resident (normally nothing:
// the last level's copies went at its teardown frames), and the bar is the
// share of it that has landed. A frame is drawn when the count has moved, at most every quarter
// second: a frame is time the streams' pumps do not run (MdkFrameParams::loading).
double holdHdWindow3ds(MdkRenderer renderer, const char* const* paths, int32_t count,
                       const char* why, const LoadingScreen* loading, float from,
                       uint32_t frameIndex) {
    const double hold0 = mdk::nowSeconds();
    int32_t made = 0;
    int32_t pending = mdk_render_hd_pending(renderer, paths, count);
    const int32_t asked = pending;
    int32_t drawnPending = pending;
    double drawnAt = hold0;
    float drawnBar = from;
    // A wait that makes nothing for a second (no file read or imported, the
    // count unchanged: what is left cannot be read) ends it, as does a minute in
    // all, or the applet loop saying stop.
    int32_t idle = 0;
    while (pending > 0 && idle < 20 && mdk::nowSeconds() - hold0 < 60.0) {
        if (!aptMainLoop()) {
            break;
        }
        const int32_t slice = mdk_render_settle_streams(renderer, 50);
        const int32_t before = pending;
        made += slice;
        pending = mdk_render_hd_pending(renderer, paths, count);
        if (loading && pending != drawnPending && mdk::nowSeconds() - drawnAt >= 0.25) {
            drawnPending = pending;
            drawnAt = mdk::nowSeconds();
            // asked > 0 here. The whole-window count also holds linked copies the
            // pumps may queue during the wait, so it can rise past `asked`: the
            // share is clamped and the bar never goes back.
            float landed = (float)(asked - pending) / (float)asked;
            landed = landed < 0.0f ? 0.0f : (landed > 1.0f ? 1.0f : landed);
            const float bar = from + (1.0f - from) * landed;
            drawnBar = bar > drawnBar ? bar : drawnBar;
            drawLoadingScreen3ds(renderer, *loading, drawnBar, frameIndex);
        }
        if (slice == 0 && pending == before) {
            idle++;
            svcSleepThread(50000000LL);
        } else {
            idle = 0;
        }
    }
    if (!paths) {
        made += mdk_render_settle_streams(renderer, 2000);   // the packed originals asked
    }
    const double ms = (mdk::nowSeconds() - hold0) * 1000.0;
    MdkHdSetState state;
    mdk_render_hd_set_state(renderer, &state);
    mdk::log3dsPrintf("hd: wait %s: %d files asked, %d left, %d made in %.0f ms; window %d of %d "
                      "resident (%u bytes, cap %d), %d given up; linear free %u",
                      why, (int)asked, (int)pending, (int)made, ms, (int)state.resident,
                      (int)state.pinned, (unsigned)state.residentBytes, (int)state.topCap,
                      (int)state.givenUp, (unsigned)linearSpaceFree());
    return ms;
}

} // namespace mdk

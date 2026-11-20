// ============================================================================
// kurt_stream_tests.cpp -- see kurt_stream_tests.h.
//
// THE FAKE BACKEND. The render seam KurtSprite calls, defined here: textures are
// numbered and remembered (what they were made from), the range reader is a
// queue the test completes by hand -- so "in flight", "landed", "failed" and
// "short" are each a state the test puts the stream in, not a timing.
// ============================================================================

#include "kurt_stream_tests.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "formats/kurt_pack.h"
#include "game/kurt_sprite.h"
#include "render/render.h"

namespace {

int failures = 0;

#define STREAM_CHECK(cond)                                                       \
    do {                                                                          \
        if (!(cond)) {                                                            \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
            failures++;                                                           \
        }                                                                         \
    } while (0)

struct FakeTexture {
    bool compressed = false;
    uint8_t firstByte = 0;   // a compressed texture's first .t3x byte
    int32_t size = 0;
    bool released = false;
};

struct FakeRead {
    int32_t tag = -1;
    uint32_t offset = 0;
    uint32_t size = 0;
};

struct FakeBackend {
    std::vector<FakeTexture> textures;
    std::vector<FakeRead> requests;     // every request accepted, in order
    bool inFlight = false;              // one read at a time, as the 3DS's
    bool refuseCompressed = false;
    // A finished read waiting to be polled.
    bool landed = false;
    int32_t landedTag = -1;
    uint8_t* landedBytes = nullptr;
    uint32_t landedSize = 0;
    bool landedOk = false;
};

FakeBackend g_backend;
MdkRenderer const kRenderer = (MdkRenderer)&g_backend;

int32_t addTexture(const FakeTexture& texture) {
    g_backend.textures.push_back(texture);
    return (int32_t)g_backend.textures.size() - 1;
}

} // namespace

// ---------------------------------------------------------------------------
// The render seam, faked.
// ---------------------------------------------------------------------------

mdk::MdkCapabilities mdk_render_capabilities(MdkRenderer) {
    mdk::MdkCapabilities caps;
    caps.sprite_residency_bytes = 1024 * 1024;
    caps.sprite_preload_bytes = 3 * 1024 * 1024;
    return caps;
}

MdkPaletteId mdk_render_create_palette(MdkRenderer, const uint8_t*) {
    return 1;
}

MdkMeshId mdk_render_create_dynamic_mesh(MdkRenderer, int32_t) {
    return 1;
}

void mdk_render_update_mesh(MdkRenderer, MdkMeshId, const mdk::Vertex*, int32_t) {}

void mdk_render_draw_mesh(MdkRenderer, const mdk::MeshDraw*) {}

MdkTextureId mdk_render_create_index_texture(MdkRenderer, int32_t w, int32_t h, const uint8_t*,
                                             const uint8_t*) {
    FakeTexture texture;
    texture.size = w * h * 2;
    return addTexture(texture);
}

MdkTextureId mdk_render_create_compressed_texture(MdkRenderer, int32_t, int32_t,
                                                  const uint8_t* t3x, int32_t size) {
    if (g_backend.refuseCompressed || !t3x || size <= 0) {
        return -1;
    }
    FakeTexture texture;
    texture.compressed = true;
    texture.firstByte = t3x[0];
    texture.size = size;
    return addTexture(texture);
}

int32_t mdk_render_texture_bytes(MdkRenderer, MdkTextureId texture) {
    if (texture < 0 || (size_t)texture >= g_backend.textures.size()) {
        return 0;
    }
    return g_backend.textures[(size_t)texture].compressed ? 16384 : 32768;
}

int32_t mdk_render_index_texture_bytes(MdkRenderer, int32_t w, int32_t h) {
    return w * h * 2;
}

void mdk_render_release_texture(MdkRenderer, MdkTextureId texture) {
    if (texture >= 0 && (size_t)texture < g_backend.textures.size()) {
        g_backend.textures[(size_t)texture].released = true;
    }
}

void mdk_render_replace_compressed(MdkRenderer, MdkTextureId, const char*) {}

void mdk_render_replace_colour(MdkRenderer, MdkTextureId, MdkPaletteId, int32_t, int32_t, int32_t,
                               const uint8_t*) {}

bool mdk_render_read_range_async(MdkRenderer, int32_t tag, const char* path, uint32_t offset,
                                 uint32_t size) {
    if (g_backend.inFlight || !path || size == 0) {
        return false;
    }
    g_backend.inFlight = true;
    FakeRead read;
    read.tag = tag;
    read.offset = offset;
    read.size = size;
    g_backend.requests.push_back(read);
    return true;
}

bool mdk_render_poll_range_read(MdkRenderer, int32_t* tag, uint8_t** bytes, uint32_t* size,
                                bool* ok) {
    if (!g_backend.landed) {
        return false;
    }
    g_backend.landed = false;
    *tag = g_backend.landedTag;
    *bytes = g_backend.landedBytes;
    *size = g_backend.landedSize;
    *ok = g_backend.landedOk;
    g_backend.landedBytes = nullptr;
    return true;
}

namespace mdk {

// The friend KurtSprite names: its private steps, for this test only.
struct KurtSpriteTestAccess {
    static void setUp(KurtSprite* sprite) {
        sprite->renderer_ = kRenderer;
        sprite->paletteRgba_.assign(256 * 4, 0);
        sprite->palette_ = 1;
        sprite->mesh_ = 1;
        sprite->residency_.setBudget(1024 * 1024);
    }
    static void addAnimation(KurtSprite* sprite, const char* name, int32_t frames, int32_t w,
                             int32_t h) {
        KurtSprite::Animation animation;
        animation.name = name;
        for (int32_t i = 0; i < frames; i++) {
            KurtSprite::Frame frame;
            frame.width = w;
            frame.height = h;
            frame.indices.assign((size_t)w * (size_t)h, 1);
            animation.frames.push_back(std::move(frame));
        }
        sprite->animations_.push_back(std::move(animation));
    }
    static bool ensure(KurtSprite* sprite, int32_t animation, int32_t frame) {
        return sprite->ensureTexture(animation, frame);
    }
    static const KurtSprite::Frame& frame(const KurtSprite& sprite, int32_t a, int32_t f) {
        return sprite.animations_[(size_t)a].frames[(size_t)f];
    }
    static KurtSprite::StreamState state(const KurtSprite& sprite, int32_t a) {
        return sprite.animations_[(size_t)a].stream;
    }
    static bool pinned(const KurtSprite& sprite) {
        return sprite.residency_.pinnedBytes() > 0;
    }
    // The state machine's animations named, as load() leaves them.
    static void resolve(KurtSprite* sprite) {
        sprite->resolveAnimations();
    }
};

} // namespace mdk

namespace {

using mdk::KurtSprite;
using mdk::KurtSpriteTestAccess;
using mdk::formats::KurtPack;

struct PackFrame {
    const char* name;
    uint8_t level;
    uint32_t size;
    uint16_t width;
    uint16_t height;
};

void putU16(std::vector<uint8_t>* out, size_t at, uint16_t v) {
    (*out)[at] = (uint8_t)v;
    (*out)[at + 1] = (uint8_t)(v >> 8);
}

void putU32(std::vector<uint8_t>* out, size_t at, uint32_t v) {
    for (int i = 0; i < 4; i++) {
        (*out)[at + (size_t)i] = (uint8_t)(v >> (8 * i));
    }
}

// A pack in kurt_pack.py's layout (the start set first, an animation's frames
// together). Frame i's bytes are all (i + 1), so a texture's first byte says
// which frame it was made from.
std::vector<uint8_t> buildPack(const PackFrame* frames, uint32_t count, uint32_t startFrames) {
    const uint32_t dataOffset = (KurtPack::kHeaderBytes + count * KurtPack::kEntryBytes + 15) / 16 * 16;
    std::vector<uint32_t> offsets(count);
    uint32_t at = dataOffset;
    uint32_t startEnd = dataOffset;
    for (uint32_t i = 0; i < count; i++) {
        offsets[i] = at;
        if (i < startFrames) {
            startEnd = at + frames[i].size;
        }
        at = (at + frames[i].size + 15) / 16 * 16;
    }
    const uint32_t fileBytes = offsets[count - 1] + frames[count - 1].size;
    std::vector<uint8_t> out(fileBytes, 0);
    memcpy(out.data(), "KPAK", 4);
    putU16(&out, 4, KurtPack::kVersion);
    putU16(&out, 6, (uint16_t)KurtPack::kHeaderBytes);
    putU32(&out, 8, count);
    putU32(&out, 12, KurtPack::kEntryBytes);
    putU32(&out, 16, dataOffset);
    putU32(&out, 20, startEnd);
    putU32(&out, 24, fileBytes);
    putU32(&out, 28, KurtPack::kAlign);
    for (uint32_t i = 0; i < count; i++) {
        const size_t e = KurtPack::kHeaderBytes + (size_t)i * KurtPack::kEntryBytes;
        memcpy(out.data() + e, frames[i].name, strlen(frames[i].name));
        out[e + 24] = frames[i].level;
        out[e + 25] = i < startFrames ? KurtPack::kFlagStartSet : 0;
        putU16(&out, e + 28, frames[i].width);
        putU16(&out, e + 30, frames[i].height);
        putU32(&out, e + 32, offsets[i]);
        putU32(&out, e + 36, frames[i].size);
        memset(out.data() + offsets[i], (int)(i + 1), frames[i].size);
    }
    return out;
}

// Lands the read in flight with the pack's bytes for it (or a failure / a
// short read).
enum class Landing { Whole, Failed, Short };
void land(const std::vector<uint8_t>& pack, Landing how) {
    STREAM_CHECK(g_backend.inFlight && !g_backend.requests.empty());
    if (!g_backend.inFlight || g_backend.requests.empty()) {
        return;
    }
    const FakeRead& read = g_backend.requests.back();
    g_backend.inFlight = false;
    g_backend.landed = true;
    g_backend.landedTag = read.tag;
    g_backend.landedOk = how != Landing::Failed;
    g_backend.landedSize = how == Landing::Short ? read.size - 1 : read.size;
    g_backend.landedBytes = nullptr;
    if (how != Landing::Failed) {
        g_backend.landedBytes = static_cast<uint8_t*>(malloc(g_backend.landedSize));
        memcpy(g_backend.landedBytes, pack.data() + read.offset, g_backend.landedSize);
    }
}

const FakeTexture& textureOf(const KurtSprite& sprite, int32_t a, int32_t f) {
    static FakeTexture none;
    const int32_t id = KurtSpriteTestAccess::frame(sprite, a, f).texture;
    return id >= 0 && (size_t)id < g_backend.textures.size() ? g_backend.textures[(size_t)id]
                                                             : none;
}

void resetBackend() {
    free(g_backend.landedBytes);
    g_backend = FakeBackend();
}

// Animations, in the order the test adds them.
enum { kStill = 0, kRun = 1, kBang = 2, kLook = 3, kFall = 4 };

const PackFrame kFrames[] = {
    // The start set (K_STILL, then K_FALL), at the front.
    {"K_STILL_0", 0, 40, 50, 140},
    {"K_STILL_1", 0, 41, 50, 140},
    {"K_FALL_0", 0, 42, 60, 120},
    // The rest, an animation's frames together.
    {"K_BANG_0", 0, 30, 58, 144},
    {"K_BANG_1", 0, 31, 58, 144},
    {"K_LOOKD_0", 0, 50, 287, 343},
    {"K_LOOKD_0", 7, 51, 287, 343},   // level 7's own copy
    {"K_RUN_0", 0, 20, 61, 151},
    {"K_RUN_1", 0, 21, 61, 151},
    {"K_RUN_2", 0, 22, 99, 151},      // made from other data: the size disagrees
};
const uint32_t kFrameCount = sizeof(kFrames) / sizeof(kFrames[0]);

void setUpSprite(KurtSprite* sprite) {
    KurtSpriteTestAccess::setUp(sprite);
    KurtSpriteTestAccess::addAnimation(sprite, "K_STILL", 2, 50, 140);
    KurtSpriteTestAccess::addAnimation(sprite, "K_RUN", 3, 61, 151);
    KurtSpriteTestAccess::addAnimation(sprite, "K_BANG", 2, 58, 144);
    KurtSpriteTestAccess::addAnimation(sprite, "K_LOOKD", 1, 287, 343);
    KurtSpriteTestAccess::addAnimation(sprite, "K_FALL", 1, 60, 120);
}

// THE LEVEL LOAD: the start set imported from the caller's one buffer, no
// request made, nothing kept; a bad entry costs its own frame only.
void testTheLevelLoad() {
    resetBackend();
    const std::vector<uint8_t> bytes = buildPack(kFrames, kFrameCount, 3);
    KurtPack pack;
    STREAM_CHECK(KurtPack::parse(bytes.data(), bytes.size(), &pack) == KurtPack::Status::Ok);
    KurtSprite sprite;
    setUpSprite(&sprite);
    KurtSprite::CompressedReport report;
    sprite.useCompressedPack(&pack, 7, "romfs:/kurt/kurt.pak", &report);
    STREAM_CHECK(report.frames == 9);
    STREAM_CHECK(report.compressed == 8);   // K_RUN_2's size disagrees
    STREAM_CHECK(report.badFrames == 1);
    STREAM_CHECK(report.streamable == 5);
    STREAM_CHECK(KurtSpriteTestAccess::frame(sprite, kRun, 2).packSize == 0);
    // Level 7 takes its own K_LOOKD_0 (the 7th entry, bytes of 7).
    STREAM_CHECK(KurtSpriteTestAccess::frame(sprite, kLook, 0).packSize == 51);

    const KurtPack::Header& header = pack.header();
    sprite.preloadStartSet(bytes.data() + header.dataOffset, header.dataOffset,
                           header.startEnd - header.dataOffset, &report);
    STREAM_CHECK(report.startFrames == 3);
    STREAM_CHECK(report.preloaded == 3);
    STREAM_CHECK(report.startFileBytes == 40 + 41 + 42);
    STREAM_CHECK(KurtSpriteTestAccess::pinned(sprite));
    STREAM_CHECK(textureOf(sprite, kStill, 0).compressed && textureOf(sprite, kStill, 0).firstByte == 1);
    STREAM_CHECK(textureOf(sprite, kStill, 1).compressed && textureOf(sprite, kStill, 1).firstByte == 2);
    STREAM_CHECK(textureOf(sprite, kFall, 0).compressed && textureOf(sprite, kFall, 0).firstByte == 3);
    // No read was asked for, and no .t3x byte is kept.
    STREAM_CHECK(g_backend.requests.empty());
    STREAM_CHECK(sprite.streamStats().staged == 0);
    // A start-set frame drawn makes no request either.
    STREAM_CHECK(KurtSpriteTestAccess::ensure(&sprite, kStill, 1));
    STREAM_CHECK(g_backend.requests.empty());
}

// THE STREAM: stand-in, one range read for the animation, one in flight, the
// upgrade when it lands, the frame made from memory after that.
void testTheStream() {
    resetBackend();
    const std::vector<uint8_t> bytes = buildPack(kFrames, kFrameCount, 3);
    KurtPack pack;
    KurtPack::parse(bytes.data(), bytes.size(), &pack);
    KurtSprite sprite;
    setUpSprite(&sprite);
    KurtSprite::CompressedReport report;
    sprite.useCompressedPack(&pack, 3, "romfs:/kurt/kurt.pak", &report);

    // K_RUN_0 drawn: its animation's range is asked for, the index texture
    // stands in.
    STREAM_CHECK(KurtSpriteTestAccess::ensure(&sprite, kRun, 0));
    STREAM_CHECK(g_backend.requests.size() == 1);
    const KurtPack::Entry& run0 = pack.entries()[7];
    const KurtPack::Entry& run1 = pack.entries()[8];
    STREAM_CHECK(g_backend.requests[0].offset == run0.offset);
    STREAM_CHECK(g_backend.requests[0].size == run1.offset + run1.size - run0.offset);
    STREAM_CHECK(KurtSpriteTestAccess::frame(sprite, kRun, 0).standIn);
    STREAM_CHECK(!textureOf(sprite, kRun, 0).compressed);
    STREAM_CHECK(KurtSpriteTestAccess::state(sprite, kRun) == KurtSprite::StreamState::Pending);
    // K_BANG drawn while K_RUN's read is in flight: no second read; it stands in.
    STREAM_CHECK(KurtSpriteTestAccess::ensure(&sprite, kBang, 0));
    STREAM_CHECK(g_backend.requests.size() == 1);
    STREAM_CHECK(KurtSpriteTestAccess::state(sprite, kBang) == KurtSprite::StreamState::Idle);
    // Drawn again before it lands: still one read.
    STREAM_CHECK(KurtSpriteTestAccess::ensure(&sprite, kRun, 0));
    STREAM_CHECK(g_backend.requests.size() == 1);

    // It lands; the pump stages it.
    land(bytes, Landing::Whole);
    sprite.pumpStream();
    STREAM_CHECK(KurtSpriteTestAccess::state(sprite, kRun) == KurtSprite::StreamState::Ready);
    STREAM_CHECK(sprite.streamStats().reads == 1);
    STREAM_CHECK(sprite.streamStats().staged == g_backend.requests[0].size);
    STREAM_CHECK(sprite.takeStreamFailure() == nullptr);

    // The next draw replaces the stand-in with the compressed copy (frame 8's
    // bytes), and releases the stand-in.
    const int32_t standIn = KurtSpriteTestAccess::frame(sprite, kRun, 0).texture;
    STREAM_CHECK(KurtSpriteTestAccess::ensure(&sprite, kRun, 0));
    STREAM_CHECK(!KurtSpriteTestAccess::frame(sprite, kRun, 0).standIn);
    STREAM_CHECK(textureOf(sprite, kRun, 0).compressed && textureOf(sprite, kRun, 0).firstByte == 8);
    STREAM_CHECK(standIn >= 0 && g_backend.textures[(size_t)standIn].released);
    STREAM_CHECK(sprite.streamStats().upgrades == 1);
    // K_RUN_1 is made from memory at once, no read.
    STREAM_CHECK(KurtSpriteTestAccess::ensure(&sprite, kRun, 1));
    STREAM_CHECK(textureOf(sprite, kRun, 1).compressed && textureOf(sprite, kRun, 1).firstByte == 9);
    STREAM_CHECK(g_backend.requests.size() == 1);
    // K_RUN_2 (the bad entry) is the index texture, and not a stand-in.
    STREAM_CHECK(KurtSpriteTestAccess::ensure(&sprite, kRun, 2));
    STREAM_CHECK(!textureOf(sprite, kRun, 2).compressed);
    STREAM_CHECK(!KurtSpriteTestAccess::frame(sprite, kRun, 2).standIn);

    // Now K_BANG's turn: the next draw asks.
    STREAM_CHECK(KurtSpriteTestAccess::ensure(&sprite, kBang, 0));
    STREAM_CHECK(g_backend.requests.size() == 2);
    STREAM_CHECK(g_backend.requests[1].offset == pack.entries()[3].offset);

    // A RE-LOAD while it is in flight: the read is stale when it lands, dropped,
    // and the new level's stream starts Idle.
    sprite.useCompressedPack(&pack, 3, "romfs:/kurt/kurt.pak", &report);
    land(bytes, Landing::Whole);
    sprite.pumpStream();
    STREAM_CHECK(KurtSpriteTestAccess::state(sprite, kBang) == KurtSprite::StreamState::Idle);
    STREAM_CHECK(sprite.streamStats().staged == 0);
    STREAM_CHECK(sprite.streamStats().reads == 1);
    STREAM_CHECK(sprite.takeStreamFailure() == nullptr);
    // And it can be asked for again.
    STREAM_CHECK(KurtSpriteTestAccess::ensure(&sprite, kBang, 1));
    STREAM_CHECK(g_backend.requests.size() == 3);
}

// A FAILED OR SHORT READ: logged once, never asked again, the frames 16-bit.
void testTheFailures() {
    for (int short_ = 0; short_ < 2; short_++) {
        resetBackend();
        const std::vector<uint8_t> bytes = buildPack(kFrames, kFrameCount, 3);
        KurtPack pack;
        KurtPack::parse(bytes.data(), bytes.size(), &pack);
        KurtSprite sprite;
        setUpSprite(&sprite);
        KurtSprite::CompressedReport report;
        sprite.useCompressedPack(&pack, 3, "romfs:/kurt/kurt.pak", &report);

        STREAM_CHECK(KurtSpriteTestAccess::ensure(&sprite, kBang, 0));
        STREAM_CHECK(g_backend.requests.size() == 1);
        land(bytes, short_ ? Landing::Short : Landing::Failed);
        sprite.pumpStream();
        STREAM_CHECK(KurtSpriteTestAccess::state(sprite, kBang) == KurtSprite::StreamState::Failed);
        STREAM_CHECK(sprite.streamStats().failures == 1);
        STREAM_CHECK(sprite.streamStats().staged == 0);
        const char* note = sprite.takeStreamFailure();
        STREAM_CHECK(note != nullptr && strstr(note, "K_BANG") != nullptr);
        STREAM_CHECK(sprite.takeStreamFailure() == nullptr);   // once
        // Drawn again, and again: no read, the index texture, not a stand-in.
        for (int i = 0; i < 3; i++) {
            STREAM_CHECK(KurtSpriteTestAccess::ensure(&sprite, kBang, 0));
            STREAM_CHECK(KurtSpriteTestAccess::ensure(&sprite, kBang, 1));
        }
        STREAM_CHECK(g_backend.requests.size() == 1);
        STREAM_CHECK(!KurtSpriteTestAccess::frame(sprite, kBang, 0).standIn);
        STREAM_CHECK(!textureOf(sprite, kBang, 1).compressed);
        // The others still stream.
        STREAM_CHECK(KurtSpriteTestAccess::ensure(&sprite, kRun, 0));
        STREAM_CHECK(g_backend.requests.size() == 2);
    }

    // The backend refuses the bytes: the frame is 16-bit from then on, and its
    // bytes are not asked for again.
    resetBackend();
    const std::vector<uint8_t> bytes = buildPack(kFrames, kFrameCount, 3);
    KurtPack pack;
    KurtPack::parse(bytes.data(), bytes.size(), &pack);
    KurtSprite sprite;
    setUpSprite(&sprite);
    KurtSprite::CompressedReport report;
    sprite.useCompressedPack(&pack, 3, "romfs:/kurt/kurt.pak", &report);
    STREAM_CHECK(KurtSpriteTestAccess::ensure(&sprite, kRun, 0));
    land(bytes, Landing::Whole);
    sprite.pumpStream();
    g_backend.refuseCompressed = true;
    STREAM_CHECK(KurtSpriteTestAccess::ensure(&sprite, kRun, 0));
    STREAM_CHECK(!textureOf(sprite, kRun, 0).compressed);
    STREAM_CHECK(KurtSpriteTestAccess::frame(sprite, kRun, 0).packSize == 0);
    STREAM_CHECK(sprite.streamStats().importFailures == 1);
}

// THE STAGING CACHE'S BOUND: the least recently drawn animation goes first,
// and a dropped one is read again when it is wanted.
void testTheCacheBound() {
    resetBackend();
    const uint32_t big = 120 * 1024;
    const PackFrame frames[] = {
        {"K_STILL_0", 0, 16, 50, 140},
        {"K_BANG_0", 0, big, 58, 144},
        {"K_LOOKD_0", 0, big, 287, 343},
        {"K_RUN_0", 0, big, 61, 151},
    };
    const std::vector<uint8_t> bytes = buildPack(frames, 4, 1);
    KurtPack pack;
    STREAM_CHECK(KurtPack::parse(bytes.data(), bytes.size(), &pack) == KurtPack::Status::Ok);
    KurtSprite sprite;
    KurtSpriteTestAccess::setUp(&sprite);
    KurtSpriteTestAccess::addAnimation(&sprite, "K_STILL", 1, 50, 140);
    KurtSpriteTestAccess::addAnimation(&sprite, "K_RUN", 1, 61, 151);
    KurtSpriteTestAccess::addAnimation(&sprite, "K_BANG", 1, 58, 144);
    KurtSpriteTestAccess::addAnimation(&sprite, "K_LOOKD", 1, 287, 343);
    KurtSprite::CompressedReport report;
    sprite.useCompressedPack(&pack, 3, "romfs:/kurt/kurt.pak", &report);

    const int32_t order[] = {kBang, kRun, kLook};
    for (int i = 0; i < 3; i++) {
        STREAM_CHECK(KurtSpriteTestAccess::ensure(&sprite, order[i], 0));
        land(bytes, Landing::Whole);
        sprite.pumpStream();
    }
    // 3 x 120 KiB is over the 256 KiB bound: K_BANG, drawn longest ago, went.
    STREAM_CHECK(sprite.streamStats().staged == 2 * big);
    STREAM_CHECK(sprite.streamStats().stagedPeak == 3 * big);
    STREAM_CHECK(sprite.streamStats().dropped == 1);
    STREAM_CHECK(KurtSpriteTestAccess::state(sprite, kBang) == KurtSprite::StreamState::Idle);
    STREAM_CHECK(KurtSpriteTestAccess::state(sprite, kRun) == KurtSprite::StreamState::Ready);
    STREAM_CHECK(KurtSpriteTestAccess::state(sprite, kLook) == KurtSprite::StreamState::Ready);
    STREAM_CHECK(sprite.streamStats().staged <= KurtSprite::kStagedCacheBytes);
}

// THE PREFETCH: with no read in flight the pump asks for the next state
// animation not yet staged -- once, and only into room the bound has -- so the
// first draw of it is made from memory, not stood in.
void testThePrefetch() {
    resetBackend();
    const std::vector<uint8_t> bytes = buildPack(kFrames, kFrameCount, 3);
    KurtPack pack;
    KurtPack::parse(bytes.data(), bytes.size(), &pack);
    KurtSprite sprite;
    setUpSprite(&sprite);
    KurtSpriteTestAccess::resolve(&sprite);
    KurtSprite::CompressedReport report;
    sprite.useCompressedPack(&pack, 3, "romfs:/kurt/kurt.pak", &report);
    const KurtPack::Header& header = pack.header();
    sprite.preloadStartSet(bytes.data() + header.dataOffset, header.dataOffset,
                           header.startEnd - header.dataOffset, &report);
    STREAM_CHECK(g_backend.requests.empty());

    // The first pump: K_STILL and K_FALL are preloaded (nothing to read), so the
    // state order's next, K_RUN, is asked for.
    sprite.pumpStream();
    STREAM_CHECK(g_backend.requests.size() == 1);
    STREAM_CHECK(!g_backend.requests.empty() &&
                 g_backend.requests[0].offset == pack.entries()[7].offset);
    sprite.pumpStream();   // in flight: nothing more
    STREAM_CHECK(g_backend.requests.size() == 1);
    land(bytes, Landing::Whole);
    sprite.pumpStream();   // staged, and the next (K_BANG) asked for
    STREAM_CHECK(KurtSpriteTestAccess::state(sprite, kRun) == KurtSprite::StreamState::Ready);
    STREAM_CHECK(g_backend.requests.size() == 2);
    STREAM_CHECK(g_backend.requests.size() == 2 &&
                 g_backend.requests[1].offset == pack.entries()[3].offset);
    // Kurt runs: made from memory at once, no stand-in.
    STREAM_CHECK(KurtSpriteTestAccess::ensure(&sprite, kRun, 0));
    STREAM_CHECK(textureOf(sprite, kRun, 0).compressed);
    STREAM_CHECK(sprite.streamStats().standIns == 0);
    land(bytes, Landing::Whole);
    sprite.pumpStream();
    // K_LOOKD is no state of the machine: never prefetched.
    STREAM_CHECK(g_backend.requests.size() == 2);
    sprite.pumpStream();
    STREAM_CHECK(g_backend.requests.size() == 2);

    // Only into room: two 120 KiB animations staged, the third does not fit the
    // 256 KiB bound and is not asked for (nothing is dropped for a prefetch).
    resetBackend();
    const uint32_t big = 120 * 1024;
    const PackFrame frames[] = {
        {"K_STILL_0", 0, 16, 50, 140},
        {"K_BANG_0", 0, big, 58, 144},
        {"K_RUN_0", 0, big, 61, 151},
        {"K_SIDE_0", 0, big, 62, 152},
    };
    const std::vector<uint8_t> large = buildPack(frames, 4, 1);
    KurtPack largePack;
    STREAM_CHECK(KurtPack::parse(large.data(), large.size(), &largePack) == KurtPack::Status::Ok);
    KurtSprite bounded;
    KurtSpriteTestAccess::setUp(&bounded);
    KurtSpriteTestAccess::addAnimation(&bounded, "K_STILL", 1, 50, 140);
    KurtSpriteTestAccess::addAnimation(&bounded, "K_RUN", 1, 61, 151);
    KurtSpriteTestAccess::addAnimation(&bounded, "K_SIDE", 1, 62, 152);
    KurtSpriteTestAccess::addAnimation(&bounded, "K_BANG", 1, 58, 144);
    KurtSpriteTestAccess::resolve(&bounded);
    bounded.useCompressedPack(&largePack, 3, "romfs:/kurt/kurt.pak", &report);
    bounded.preloadStartSet(large.data() + largePack.header().dataOffset,
                            largePack.header().dataOffset,
                            largePack.header().startEnd - largePack.header().dataOffset, &report);
    for (int i = 0; i < 4; i++) {
        bounded.pumpStream();
        if (g_backend.inFlight) {
            land(large, Landing::Whole);
        }
    }
    bounded.pumpStream();
    STREAM_CHECK(g_backend.requests.size() == 2);   // K_RUN, K_SIDE; not K_BANG
    STREAM_CHECK(bounded.streamStats().staged == 2 * big);
    STREAM_CHECK(bounded.streamStats().dropped == 0);
    STREAM_CHECK(KurtSpriteTestAccess::state(bounded, 3) == KurtSprite::StreamState::Idle);
}

} // namespace

namespace mdk {
namespace test {

int runKurtStreamTests() {
    failures = 0;
    testTheLevelLoad();
    testTheStream();
    testTheFailures();
    testTheCacheBound();
    testThePrefetch();
    resetBackend();
    return failures;
}

} // namespace test
} // namespace mdk

// ============================================================================
// arena.cpp — see arena.h.
// ============================================================================

#include "formats/arena.h"

#include <string.h>

#include "formats/bin_reader.h"

namespace mdk {
namespace formats {

namespace {

// Names of 10 bytes: an odd count pads to 4.
const int32_t kAlignment = 4;

// A triangle of the world section: three s16 indices, an s16 material, three
// (f32, f32) texture coordinates and a u32 of flags. A vertex: three f32.
const int32_t kTriangleBytes = 36;
const int32_t kVertexBytes = 12;

// Whether `count` records of `each` bytes are still ahead of the reader.
bool fitsIn(const BinReader& reader, int32_t byteCount, uint32_t count, int32_t each) {
    const int64_t left = (int64_t)byteCount - (int64_t)reader.pos();
    return left >= 0 && (int64_t)count * (int64_t)each <= left;
}

// More embedded sounds than an arena has (the original refuses more than 16):
// a count past this is not walked.
const uint32_t kMaxArenaSounds = 1024;

// The Arena objects alive right now, behind Arena::liveCount(). A plain
// counter, not an atomic: arenas are constructed and destroyed in level load
// and teardown, which is main-thread work (the tree creates no worker thread
// today), and the only reader is a test. If a worker ever parses an arena,
// this becomes an atomic and AGENTS.md P4's handoff applies to it.
int64_t g_liveArenas = 0;

} // namespace

Arena::Arena() {
    ++g_liveArenas;
}

Arena::~Arena() {
    --g_liveArenas;
    // Nothing else to release: every buffer is a member. The destructor exists
    // so the class can be held by pointer and freed with delete, which is
    // how the arenas are handed out.
}

int64_t Arena::liveCount() {
    return g_liveArenas;
}

Arena* Arena::parse(const std::string& name, const uint8_t* bytes, int32_t byteCount, int32_t offset) {
    if (!bytes || offset < 0 || offset + 4 > byteCount) {
        return nullptr;
    }
    Arena* arena = new Arena();
    arena->name_ = name;

    // The game loads the bytes AFTER the size field, and the offsets are
    // relative to that buffer. So every offset below is `offset + 4` plus
    // the value read at it.
    const int32_t buf = offset + 4;
    if (buf + 12 > byteCount) {
        delete arena;
        return nullptr;
    }
    const int32_t models = buf + (int32_t)binU32(bytes, buf);
    const int32_t palette = buf + (int32_t)binU32(bytes, buf + 4);
    const int32_t world = buf + (int32_t)binU32(bytes, buf + 8);

    if (palette >= 0 && palette + kPaletteColors * 3 <= byteCount) {
        arena->paletteRgb_.assign(bytes + palette, bytes + palette + kPaletteColors * 3);
    }
    arena->textures_ = TextureArchive::parse(bytes, byteCount, buf + 0x10);
    // What a re-read of the textures needs: the block from its start up to the
    // models section (the section order is textures, models, palette, world in
    // every retail arena). The stream test proves a parse of just this prefix
    // gives every texture's pixels back.
    if (models > offset && models <= byteCount) {
        arena->texturePrefixBytes_ = models - offset;
    }
    arena->parseWorldSection(bytes, byteCount, world, kMaterialNameLength, kBspNodeSize);

    // The models, the animations and the sounds. The scripts' FindModel
    // needs the arena's own models (e.g. HMO_1's XH1_DOOR), so they are
    // parsed here; the sounds are read past, as the audio path keeps them
    // elsewhere.
    const int32_t firstSound = arena->parseModels(bytes, byteCount, models + 4);

    // THE ANIMATIONS' OWN BYTES (docs/ARENA_STREAMING_SPEC.md Phase 1). The
    // models section runs from `models` to the next section's start (palette
    // and world follow it in every retail arena: measured on LEVEL3O.MTO and
    // LEVEL8O.MTO, 2026-11-18) or the block's end. That span -- 2.4 MB of
    // level 8's 10.3 MB -- is all an animation reads when it bakes, so it is
    // the only part of the block the arena keeps.
    // The size field counts itself: the block is [offset, offset + size).
    const int32_t blockEnd = offset + (int32_t)binU32(bytes, offset);
    int32_t end = blockEnd < byteCount ? blockEnd : byteCount;
    if (palette > models && palette < end) {
        end = palette;
    }
    if (world > models && world < end) {
        end = world;
    }
    // ... and it STOPS AT THE ARENA'S EMBEDDED SOUNDS (Phase 5). In every retail
    // arena that has any, they are the contiguous TAIL of the models section
    // (37 arenas, 2.73 MB of the six levels' 11.37 MB of models sections,
    // measured 2026-11-18), nothing plays them (see parseModels), and no
    // animation track starts past the first of them. If one ever did, the
    // rebase below would refuse it and droppedAnimations() -- which the format
    // tests require to be zero -- would say so.
    if (firstSound > models && firstSound < end) {
        arena->embeddedSoundBytes_ = end - firstSound;
        end = firstSound;
    }
    if (models >= buf && models < end) {
        arena->keepAnimationBytes(bytes, models, end, buf, blockEnd < byteCount ? blockEnd : byteCount);
    } else {
        // No span to keep (corrupt offsets): no animation may survive the
        // parse buffer, which the streamed form frees on return.
        arena->droppedAnimations_ += (int32_t)arena->animations_.size();
        arena->animations_.clear();
    }

    return arena;
}

int64_t Arena::texturePixelBytes() const {
    int64_t bytes = 0;
    const std::vector<std::pair<std::string, Texture>>& textures = textures_.textures();
    for (size_t i = 0; i < textures.size(); i++) {
        bytes += textures[i].second.residentBytes();
    }
    return bytes;
}

int64_t Arena::dropTexturePixels() {
    int64_t freed = 0;
    std::vector<std::pair<std::string, Texture>>& textures = textures_.mutableTextures();
    for (size_t i = 0; i < textures.size(); i++) {
        freed += textures[i].second.residentBytes();
        textures[i].second.dropPixels();
    }
    return freed;
}

bool Arena::adoptTexturePixels(TextureArchive* fresh) {
    if (!fresh) {
        return false;
    }
    std::vector<std::pair<std::string, Texture>>& mine = textures_.mutableTextures();
    std::vector<std::pair<std::string, Texture>>& theirs = fresh->mutableTextures();
    if (mine.size() != theirs.size()) {
        return false;
    }
    // Check everything BEFORE anything moves: a half-adopted archive would be
    // some textures of this read and some away, with no way to tell which.
    for (size_t i = 0; i < mine.size(); i++) {
        const Texture& a = mine[i].second;
        const Texture& b = theirs[i].second;
        if (mine[i].first != theirs[i].first || a.width() != b.width() || a.height() != b.height() ||
            a.frameCount() != b.frameCount()) {
            return false;
        }
    }
    for (size_t i = 0; i < mine.size(); i++) {
        // A texture the fresh parse found no pixels for (cut short in a file
        // that is not the retail layout) has none to take. It must not stay
        // "away": a reader would then ask for the whole arena again on every
        // read, for ever. It becomes what the parse says it is -- empty.
        if (!mine[i].second.adoptPixels(&theirs[i].second) && mine[i].second.pixelsAway()) {
            mine[i].second.setIndices(nullptr, 0);
        }
    }
    return true;
}

void Arena::setPixelSource(void (*load)(void* user, const Texture* texture), void* user) {
    pixelSource_.load = load;
    pixelSource_.user = user;
    std::vector<std::pair<std::string, Texture>>& textures = textures_.mutableTextures();
    for (size_t i = 0; i < textures.size(); i++) {
        textures[i].second.setPixelSource(&pixelSource_);
    }
}

bool Arena::ownsTexture(const Texture* texture) const {
    const std::vector<std::pair<std::string, Texture>>& textures = textures_.textures();
    if (textures.empty() || !texture) {
        return false;
    }
    // One contiguous vector: the address range is the membership test.
    const std::pair<std::string, Texture>* first = &textures.front();
    const std::pair<std::string, Texture>* last = &textures.back();
    return (const void*)texture >= (const void*)first &&
           (const void*)texture < (const void*)(last + 1);
}

void Arena::keepAnimationBytes(const uint8_t* bytes, int32_t from, int32_t to, int32_t blockStart,
                               int32_t blockEnd) {
    if (animations_.empty()) {
        return;
    }
    // Decide the range FIRST, for every animation, so the rebase below cannot
    // fail halfway and leave some animations on the copy and some on the parse
    // buffer that is about to go away.
    bool inModels = true;
    for (size_t i = 0; i < animations_.size() && inModels; i++) {
        inModels = animations_[i].second.tracksWithin(from, to);
    }
    if (!inModels) {
        from = blockStart;   // never measured on retail data: the whole block
        to = blockEnd;
    }
    if (from < 0 || to <= from) {
        droppedAnimations_ += (int32_t)animations_.size();
        animations_.clear();
        return;
    }
    animationBytes_.assign(bytes + from, bytes + to);
    // An animation with a track outside even the block cannot be kept: once
    // the parse buffer is freed it would read freed memory. It is dropped,
    // and droppedAnimations() says so (the format tests require zero).
    std::vector<std::pair<std::string, ModelAnimation>> kept;
    for (size_t i = 0; i < animations_.size(); i++) {
        if (animations_[i].second.rebase(animationBytes_.data(), (int32_t)animationBytes_.size(), from)) {
            kept.push_back(animations_[i]);
        } else {
            droppedAnimations_++;
        }
    }
    animations_.swap(kept);
}

Arena* Arena::parseWorld(const std::string& name, const uint8_t* bytes, int32_t byteCount,
                         int32_t offset) {
    if (!bytes) {
        return nullptr;
    }
    Arena* arena = new Arena();
    arena->name_ = name;
    arena->parseWorldSection(bytes, byteCount, offset, kMaterialNameLength, kBspNodeSize);
    return arena;
}

Arena* Arena::parseBetaWorld(const std::string& name, const uint8_t* bytes, int32_t byteCount) {
    if (!bytes) {
        return nullptr;
    }
    Arena* arena = new Arena();
    arena->name_ = name;
    arena->parseWorldSection(bytes, byteCount, 0, kBetaMaterialNameLength, kBetaBspNodeSize);

    // The 1996 demo's material names are in lower case, and the game looks
    // them up in upper case. The conversion is done HERE, at parse time,
    // because the C# does the same and doing it later would change what the
    // game reads.
    for (size_t i = 0; i < arena->materials_.size(); i++) {
        std::string& material = arena->materials_[i];
        for (size_t c = 0; c < material.size(); c++) {
            if (material[c] >= 'a' && material[c] <= 'z') {
                material[c] = (char)(material[c] - 'a' + 'A');
            }
        }
    }
    return arena;
}

void Arena::parseWorldSection(const uint8_t* bytes, int32_t byteCount, int32_t baseOffset,
                               int32_t nameLength, int32_t nodeSize) {
    if (!bytes || baseOffset < 0 || baseOffset + 4 > byteCount) {
        return;
    }
    BinReader reader(bytes, byteCount, baseOffset);

    // EVERY COUNT IS CHECKED AGAINST THE BYTES THAT ARE LEFT before anything is
    // sized by it. A world section is what a corridor's SNI entry is, and an
    // entry that is not a world section (a WAV: 'RIFF' reads as 1.18e9
    // materials) used to run this loop until the allocator gave up -- an abort
    // on the console (review, 2026-11-18). A section whose counts do not fit is
    // no geometry at all, not half of one.
    const uint32_t materialCount = reader.u32();
    if (!fitsIn(reader, byteCount, materialCount, nameLength)) {
        return;
    }
    for (uint32_t i = 0; i < materialCount; i++) {
        char name[64];
        reader.name(name, nameLength);
        materials_.push_back(name);
    }

    // Names of 10 bytes: an odd count pads to 4.
    if ((materialCount * (uint32_t)nameLength) % (uint32_t)kAlignment != 0) {
        reader.skip(2);
    }

    const uint32_t nodeCount = reader.u32();
    if (!fitsIn(reader, byteCount, nodeCount, nodeSize)) {
        materials_.clear();
        return;
    }
    nodes_.resize(nodeCount);
    for (uint32_t i = 0; i < nodeCount; i++) {
        // The reader is moved to the next node rather than read through:
        // the fields after the eighth are not the same in the two layouts,
        // and the node's SIZE is what separates them.
        const int32_t next = reader.pos() + nodeSize;
        BspNode& node = nodes_[i];
        reader.vec3(node.normal);
        node.d = reader.f32();
        node.negative = reader.s16();
        node.positive = reader.s16();
        node.frontCount = (int32_t)reader.u16();
        node.frontFirst = reader.s16();
        node.backCount = (int32_t)reader.u16();
        node.backFirst = reader.s16();
        reader.setPos(next);
    }

    const uint32_t triangleCount = reader.u32();
    if (!fitsIn(reader, byteCount, triangleCount, kTriangleBytes)) {
        materials_.clear();
        nodes_.clear();
        return;
    }
    triangleIndices_.resize((size_t)triangleCount * 3);
    triangleMaterials_.resize(triangleCount);
    triangleUvs_.resize((size_t)triangleCount * 3);
    triangleFlags_.resize(triangleCount);
    for (uint32_t i = 0; i < triangleCount; i++) {
        triangleIndices_[i * 3 + 0] = reader.s16();
        triangleIndices_[i * 3 + 1] = reader.s16();
        triangleIndices_[i * 3 + 2] = reader.s16();
        triangleMaterials_[i] = reader.s16();
        for (int32_t k = 0; k < 3; k++) {
            triangleUvs_[i * 3 + k].x = reader.f32();
            triangleUvs_[i * 3 + k].y = reader.f32();
        }
        triangleFlags_[i] = reader.u32();
    }

    const uint32_t vertexCount = reader.u32();
    if (!fitsIn(reader, byteCount, vertexCount, kVertexBytes)) {
        materials_.clear();
        nodes_.clear();
        triangleIndices_.clear();
        triangleMaterials_.clear();
        triangleUvs_.clear();
        triangleFlags_.clear();
        return;
    }
    vertices_.resize(vertexCount);
    for (uint32_t i = 0; i < vertexCount; i++) {
        reader.vec3(&vertices_[i].x);
    }
}

int32_t Arena::parseModels(const uint8_t* bytes, int32_t byteCount, int32_t baseOffset) {
    if (!bytes || baseOffset < 0 || baseOffset + 12 > byteCount) {
        return -1;
    }
    BinReader reader(bytes, byteCount, baseOffset);

    const uint32_t animationCount = reader.u32();
    const uint32_t modelCount = reader.u32();
    const uint32_t soundCount = reader.u32();

    for (uint32_t i = 0; i < animationCount; i++) {
        char name[16];
        reader.name(name, 8);
        const int32_t offset = baseOffset + (int32_t)reader.u32();
        ModelAnimation animation;
        if (ModelAnimation::parse(&animation, name, bytes, byteCount, offset)) {
            animations_.push_back(std::make_pair(std::string(name), animation));
        }
    }

    for (uint32_t i = 0; i < modelCount; i++) {
        char name[16];
        reader.name(name, 8);
        const int32_t offset = baseOffset + (int32_t)reader.u32();
        Model model;
        if (Model::parse(&model, name, bytes, byteCount, offset)) {
            models_.push_back(std::make_pair(std::string(name), model));
        }
    }

    // THE ARENA'S OWN SOUNDS are not kept. An entry is the SNI's: a 12-byte
    // name, u16 flags, u16, u32 offset (from this section) and u32 length, of a
    // WAV inside the arena's block. The original registers them when the
    // arena's stream completes (0x41983c -> 0x4310cc, at most 16) and they go
    // with the arena. The C# parses them into Arena.Sounds and nothing reads
    // that, so the oracle's bank -- and this port's -- has never had them
    // (docs/OPEN_ITEMS.md). What IS used here is where the first one starts:
    // the arena keeps its models section only up to there.
    int32_t firstSound = -1;
    for (uint32_t i = 0; i < soundCount && i < kMaxArenaSounds; i++) {
        if (reader.pos() + 24 > byteCount) {
            break;
        }
        reader.skip(16);   // the name, the flags and the volume
        const int32_t offset = baseOffset + (int32_t)reader.u32();
        reader.skip(4);    // the length
        if (offset > baseOffset && (firstSound < 0 || offset < firstSound)) {
            firstSound = offset;
        }
    }
    return firstSound;
}

const Model* Arena::model(const std::string& name) const {
    for (size_t i = 0; i < models_.size(); i++) {
        if (models_[i].first == name) {
            return &models_[i].second;
        }
    }
    return nullptr;
}

const ModelAnimation* Arena::animation(const std::string& name) const {
    for (size_t i = 0; i < animations_.size(); i++) {
        if (animations_[i].first == name) {
            return &animations_[i].second;
        }
    }
    return nullptr;
}

bool Arena::hasGroup(int32_t group) const {
    for (size_t i = 0; i < triangleFlags_.size(); i++) {
        if ((int32_t)(triangleFlags_[i] >> 24) == group) {
            return true;
        }
    }
    return false;
}

uint32_t Arena::groupState(int32_t group) const {
    std::map<int32_t, uint32_t>::const_iterator it = groupStates_.find(group);
    return it == groupStates_.end() ? 0u : it->second;
}

void Arena::setGroupState(int32_t group, uint32_t state) {
    groupStates_[group] = state;
}

void Arena::setGroupMaterial(int32_t group, int32_t material) {
    // C# TriangleGroups.SetMaterial: the group must exist and not be group 0.
    if (group == 0 || !hasGroup(group)) {
        return;
    }
    groupMaterials_[group] = material;
}

void Arena::clearGroupMaterial(int32_t group) {
    // A full save's group with no material (TriangleGroups.Restore, package #19).
    groupMaterials_.erase(group);
}

bool Arena::groupMaterial(int32_t group, int32_t* out) const {
    std::map<int32_t, int32_t>::const_iterator it = groupMaterials_.find(group);
    if (it == groupMaterials_.end()) {
        return false;
    }
    *out = it->second;
    return true;
}

void Arena::applyDisabledTriangles() {
    if (clipFlag_ != 0) {
        return;
    }
    for (size_t i = 0; i < triangleFlags_.size(); i++) {
        if ((triangleFlags_[i] & kGroupDisabled) != 0) {
            triangleFlags_[i] |= kGroupHidden | kGroupNotSolid;
        }
    }
}

} // namespace formats
} // namespace mdk

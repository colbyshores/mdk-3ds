// ============================================================================
// arena_stream.cpp -- see arena_stream.h.
// ============================================================================

#include "game/arena_stream.h"

#include <string.h>

#include "formats/arena.h"
#include "formats/texture_archive.h"
#include "game/level_data.h"

namespace mdk {

namespace {

// An arena block's buffer starts after its u32 size, and its texture archive
// is 0x10 into that buffer (Arena::parse): TextureArchive::parse's base offset
// for a block read from its own start.
const int32_t kBlockTextureBase = 4 + 0x10;

} // namespace

void ArenaStream::attach(LevelData* level) {
    reset();
    level_ = level;
    if (!level_) {
        return;
    }
    const std::vector<const formats::Arena*>& arenas = level_->arenas();
    for (size_t i = 0; i < arenas.size(); i++) {
        formats::Arena* arena = streamed(arenas[i]->name());
        if (!arena) {
            continue;
        }
        arena->setPixelSource(&ArenaStream::loadThunk, this);
        arena->dropTexturePixels();
    }
}

void ArenaStream::reset() {
    level_ = nullptr;
    resident_.clear();
    kept_.clear();
    abandon();
    std::vector<uint8_t>().swap(chunk_);
    trimPending_ = false;
    failures_.clear();
    stats_ = Stats();
}

formats::Arena* ArenaStream::streamed(const std::string& name) const {
    if (!level_ || name.empty() || !level_->mto().has(name)) {
        return nullptr;
    }
    formats::Arena* arena = level_->arenaNamedMutable(name);
    return (arena && arena->texturePrefixBytes() > 0) ? arena : nullptr;
}

bool ArenaStream::pixelsIn(const formats::Arena* arena) {
    const std::vector<std::pair<std::string, formats::Texture>>& textures =
        arena->textures().textures();
    for (size_t i = 0; i < textures.size(); i++) {
        if (textures[i].second.pixelsAway()) {
            return false;
        }
    }
    return true;
}

void ArenaStream::update(const std::string& current, const std::string& second,
                         bool secondActive) {
    std::vector<std::string> drawn;
    drawn.push_back(current);
    if (secondActive && !second.empty()) {
        drawn.push_back(second);
    }
    update(current, second, secondActive, drawn);
}

namespace {

bool contains(const std::vector<std::string>& names, const std::string& name) {
    for (size_t i = 0; i < names.size(); i++) {
        if (names[i] == name) {
            return true;
        }
    }
    return false;
}

} // namespace

bool ArenaStream::isKept(const std::string& name) const {
    return contains(kept_, name);
}

void ArenaStream::update(const std::string& current, const std::string& second,
                         bool secondActive, const std::vector<std::string>& drawn) {
    if (!level_) {
        return;
    }
    // WHICH ARENA HAS THE STREAM: Kurt's if it is an MTO arena, else the second
    // if that is one (arena_switch 0x41970c takes them in that order). With
    // neither -- Kurt in a corridor, nothing behind a door -- the resident one
    // STAYS, as the original's buffer does until another arena needs it: turning
    // back into the arena just left costs nothing.
    const formats::Arena* cur = streamed(current);
    const formats::Arena* sec = (second != current) ? streamed(second) : nullptr;
    const std::string want = cur ? current : (sec ? second : std::string());

    // WHAT ELSE KEEPS ITS PIXELS THIS FRAME: the second of a teleport's MTO pair
    // (counted: the original would thrash its one buffer), and any MTO arena the
    // frame draws that is not the wanted one (see the header: the drawn list is
    // the tick's middle, the pair its end).
    std::vector<std::string> kept;
    if (cur && sec) {
        kept.push_back(second);
        stats_.pairFrames++;
    }
    for (size_t i = 0; i < drawn.size(); i++) {
        if (drawn[i] != want && !contains(kept, drawn[i]) && streamed(drawn[i])) {
            kept.push_back(drawn[i]);
        }
    }
    if (kept != kept_) {
        // Something stopped (or started) being kept: the trim's business.
        kept_.swap(kept);
        trimPending_ = true;
    }

    if (!want.empty() && want != resident_) {
        switchTo(want);
    } else if (trimPending_) {
        // Something stopped being kept (the frame after a teleport, the end of
        // a pair), or a reader brought an arena in during the last frame's
        // draws: it goes NOW, before this update reads a byte, for the reason
        // switchTo trims first -- the peak stays one arena's.
        trim();
        trimPending_ = false;
    }

    if (!streaming_.empty()) {
        // SHOWN MEANS WHOLE (BSPShow 0x41a11c loops the step until the stream
        // is done): an arena that is drawn this frame is finished now, and the
        // bytes that took are a stall the frontend logs. A preloading second
        // arena (opcode 223, a type-3 trigger) takes its one step.
        if (contains(drawn, streaming_)) {
            const int64_t before = stats_.bytesRead;
            while (!streaming_.empty() && step()) {
            }
            stats_.stalls++;
            stats_.stallBytes += stats_.bytesRead - before;
        } else {
            step();
        }
    }
    // A kept arena that is drawn must have its pixels too (it normally still
    // does: it was the resident one a frame ago).
    for (size_t i = 0; i < kept_.size(); i++) {
        if (contains(drawn, kept_[i])) {
            loadNow(kept_[i]);
        }
    }

    if (trimPending_) {
        trim();
        trimPending_ = false;
        const int32_t in = residentArenas();
        if (in > stats_.maxResidentArenas) {
            stats_.maxResidentArenas = in;
        }
    }
}

void ArenaStream::switchTo(const std::string& name) {
    abandon();
    resident_ = name;
    stats_.switches++;
    // THE OLD ARENA GOES FIRST, before a byte of the new one is asked for: the
    // original has one buffer, so it cannot do otherwise, and here it is what
    // keeps the peak at one arena's pixels instead of the old arena's plus the
    // new one's staging plus the new one's pixels (review, 2026-11-18). What is
    // still drawn this frame is in kept_ and stays.
    trim();
    trimPending_ = false;
    const formats::Arena* arena = streamed(name);
    if (!arena || pixelsIn(arena) || failedTooOften(name)) {
        // Already whole (a demand load, or the kept half of a pair), or not
        // readable: nothing to stream.
        return;
    }
    streaming_ = name;
    total_ = arena->texturePrefixBytes();
    cursor_ = 0;
    staging_.resize((size_t)total_);
}

bool ArenaStream::step() {
    if (streaming_.empty() || !level_) {
        return false;
    }
    int32_t count = total_ - cursor_;
    if (count > kChunkBytes) {
        count = kChunkBytes;
    }
    const std::string name = streaming_;
    if (count > 0) {
        if (!level_->mto().readBlock(name, cursor_, count, &chunk_) ||
            (int32_t)chunk_.size() != count) {
            stats_.readFailures++;
            noteFailure(name);
            abandon();
            return false;
        }
        memcpy(staging_.data() + cursor_, chunk_.data(), (size_t)count);
        cursor_ += count;
        stats_.steps++;
        stats_.bytesRead += count;
    }
    if (cursor_ < total_) {
        return true;
    }
    // THE LAST CHUNK IS IN: only now is anything parsed (arena_load_step
    // 0x41983c sets the arena up after its final read, never per chunk).
    formats::Arena* arena = streamed(name);
    const bool ok = arena && adopt(arena, staging_.data(), total_);
    abandon();
    if (!ok) {
        stats_.readFailures++;
        noteFailure(name);
        return false;
    }
    return true;
}

bool ArenaStream::adopt(formats::Arena* arena, const uint8_t* bytes, int32_t byteCount) {
    // The same parse the level's load ran over the same bytes, so the pixels
    // are the ones that were dropped (the format tests compare them).
    formats::TextureArchive fresh =
        formats::TextureArchive::parse(bytes, byteCount, kBlockTextureBase);
    if (!arena->adoptTexturePixels(&fresh)) {
        return false;
    }
    trimPending_ = true;
    // The high-water mark, taken at the one moment it can rise.
    const int64_t in = residentBytes();
    if (in > stats_.peakPixelBytes) {
        stats_.peakPixelBytes = in;
    }
    return true;
}

void ArenaStream::abandon() {
    streaming_.clear();
    // The storage goes too: between streams the stream holds nothing.
    std::vector<uint8_t>().swap(staging_);
    cursor_ = 0;
    total_ = 0;
}

bool ArenaStream::loadNow(const std::string& name) {
    formats::Arena* arena = streamed(name);
    if (!arena) {
        return false;
    }
    if (pixelsIn(arena)) {
        return true;
    }
    if (failedTooOften(name)) {
        return false;
    }
    if (streaming_ == name) {
        // Its own stream is part-way: finish that, keeping what was read.
        while (!streaming_.empty() && step()) {
        }
        return pixelsIn(arena);
    }
    // Another arena has the stream: this one is read on the side, whole.
    const int32_t count = arena->texturePrefixBytes();
    std::vector<uint8_t> bytes;
    if (!level_->mto().readBlock(name, 0, count, &bytes) || (int32_t)bytes.size() != count) {
        stats_.readFailures++;
        noteFailure(name);
        return false;
    }
    stats_.bytesRead += count;
    if (!adopt(arena, bytes.data(), count)) {
        stats_.readFailures++;
        noteFailure(name);
        return false;
    }
    return true;
}

void ArenaStream::loadThunk(void* user, const formats::Texture* texture) {
    ArenaStream* self = static_cast<ArenaStream*>(user);
    if (!self || !self->level_ || !texture) {
        return;
    }
    const std::vector<const formats::Arena*>& arenas = self->level_->arenas();
    for (size_t i = 0; i < arenas.size(); i++) {
        if (!arenas[i]->ownsTexture(texture)) {
            continue;
        }
        const std::string& name = arenas[i]->name();
        if (self->failedTooOften(name)) {
            return;   // a counted failure already; not a read per reader
        }
        const int64_t before = self->stats_.bytesRead;
        self->stats_.demandLoads++;
        self->loadNow(name);
        self->stats_.demandBytes += self->stats_.bytesRead - before;
        return;
    }
}

void ArenaStream::trim() {
    const std::vector<const formats::Arena*>& arenas = level_->arenas();
    for (size_t i = 0; i < arenas.size(); i++) {
        const std::string& name = arenas[i]->name();
        if (name == resident_ || name == streaming_ || isKept(name)) {
            continue;
        }
        formats::Arena* arena = streamed(name);
        if (arena && arena->texturePixelBytes() > 0) {
            arena->dropTexturePixels();
            stats_.trims++;
        }
    }
}

int32_t ArenaStream::residentArenas() const {
    if (!level_) {
        return 0;
    }
    int32_t count = 0;
    const std::vector<const formats::Arena*>& arenas = level_->arenas();
    for (size_t i = 0; i < arenas.size(); i++) {
        const formats::Arena* arena = streamed(arenas[i]->name());
        if (arena && arena->texturePixelBytes() > 0) {
            count++;
        }
    }
    return count;
}

int64_t ArenaStream::residentBytes() const {
    if (!level_) {
        return 0;
    }
    int64_t bytes = 0;
    const std::vector<const formats::Arena*>& arenas = level_->arenas();
    for (size_t i = 0; i < arenas.size(); i++) {
        const formats::Arena* arena = streamed(arenas[i]->name());
        if (arena) {
            bytes += arena->texturePixelBytes();
        }
    }
    return bytes;
}

bool ArenaStream::failedTooOften(const std::string& name) const {
    for (size_t i = 0; i < failures_.size(); i++) {
        if (failures_[i].first == name) {
            return failures_[i].second >= kMaxReadFailures;
        }
    }
    return false;
}

void ArenaStream::noteFailure(const std::string& name) {
    for (size_t i = 0; i < failures_.size(); i++) {
        if (failures_[i].first == name) {
            failures_[i].second++;
            return;
        }
    }
    failures_.push_back(std::make_pair(name, 1));
}

} // namespace mdk

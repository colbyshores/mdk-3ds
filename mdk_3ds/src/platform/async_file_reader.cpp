// ============================================================================
// async_file_reader.cpp -- see async_file_reader.h.
// ============================================================================

#include "platform/async_file_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/log_3ds.h"
#include "platform/stream_worker_3ds.h"

namespace mdk {

namespace {

// A channel's place in the worker's order (async_file_reader.h, WHICH READ GOES
// NEXT): lower goes first.
int32_t channelRank(int32_t channel) {
    switch (channel) {
        case AsyncFileReader::kChannelPack: return 0;
        case AsyncFileReader::kChannelFile: return 1;
        case AsyncFileReader::kChannelOrig: return 2;
        default: return 4;   // kChannelHd
    }
}

} // namespace

bool AsyncFileReader::start() {
    if (running_) {
        return true;
    }
    LightLock_Init(&lock_);
    files_ = 0;
    failed_ = 0;
    bytes_ = 0;
    busyTicks_ = 0;
    maxTicks_ = 0;
    startTick_ = svcGetSystemTick();
    // The reads are the streaming worker's (stream_worker_3ds.h): no thread of
    // the reader's own. Without the worker nothing would read: say so, refuse.
    running_ = streamWorkerRunning();
    if (running_) {
        streamWorkerAttachReader(this);
    }
    log3dsPrintf("hd: reader %s (the streaming worker's reads, %u-byte chunks)",
                 running_ ? "attached" : "FAILED: no streaming worker", (unsigned)kChunkBytes);
    return running_;
}

void AsyncFileReader::stop() {
    if (running_) {
        // Detached, the worker is not inside step() and will not enter it again:
        // the read in progress and the slots are this thread's now.
        streamWorkerAttachReader(nullptr);
        running_ = false;
        say("at stop");
    }
    closeCurrent();
    for (int32_t i = 0; i < kSlots; i++) {
        free(slots_[i].bytes);
        slots_[i] = Slot();
    }
}

bool AsyncFileReader::request(int32_t tag, const char* path) {
    return requestRange(kChannelHd, tag, path, 0, 0);
}

bool AsyncFileReader::requestRange(int32_t channel, int32_t tag, const char* path,
                                   uint32_t offset, uint32_t length) {
    return queue(channel, tag, path, offset, length, nullptr);
}

bool AsyncFileReader::requestFile(int32_t channel, int32_t tag, const char* path, Work work) {
    return queue(channel, tag, path, 0, 0, work);
}

bool AsyncFileReader::queue(int32_t channel, int32_t tag, const char* path, uint32_t offset,
                            uint32_t length, Work work) {
    if (!running_ || !path || strlen(path) >= kPathMax || length > kMaxRangeBytes) {
        return false;
    }
    bool queued = false;
    LightLock_Lock(&lock_);
    for (int32_t i = 0; i < kSlots; i++) {
        if (slots_[i].state == Free) {
            slots_[i].state = Requested;
            slots_[i].channel = channel;
            slots_[i].tag = tag;
            slots_[i].offset = offset;
            slots_[i].length = length;
            slots_[i].work = work;
            slots_[i].order = nextOrder_++;
            snprintf(slots_[i].path, kPathMax, "%s", path);
            queued = true;
            break;
        }
    }
    LightLock_Unlock(&lock_);
    if (queued) {
        streamWorkerWake();
    }
    return queued;
}

bool AsyncFileReader::poll(int32_t channel, int32_t* tag, char* path, uint8_t** bytes,
                           size_t* size, bool* ok) {
    if (!running_) {
        return false;
    }
    bool found = false;
    LightLock_Lock(&lock_);
    for (int32_t i = 0; i < kSlots; i++) {
        if (slots_[i].state == Done && slots_[i].channel == channel) {
            *tag = slots_[i].tag;
            memcpy(path, slots_[i].path, kPathMax);
            *bytes = slots_[i].bytes;
            *size = slots_[i].size;
            *ok = slots_[i].ok;
            slots_[i] = Slot();
            found = true;
            break;
        }
    }
    LightLock_Unlock(&lock_);
    return found;
}

bool AsyncFileReader::pollTag(int32_t channel, int32_t tag, uint8_t** bytes, size_t* size,
                              bool* ok) {
    if (!running_) {
        return false;
    }
    bool found = false;
    LightLock_Lock(&lock_);
    for (int32_t i = 0; i < kSlots; i++) {
        if (slots_[i].state == Done && slots_[i].channel == channel && slots_[i].tag == tag &&
            !slots_[i].discarded) {
            *bytes = slots_[i].bytes;
            *size = slots_[i].size;
            *ok = slots_[i].ok;
            slots_[i] = Slot();
            found = true;
            break;
        }
    }
    LightLock_Unlock(&lock_);
    return found;
}

void AsyncFileReader::discard(int32_t channel, int32_t tag) {
    if (!running_) {
        return;
    }
    LightLock_Lock(&lock_);
    for (int32_t i = 0; i < kSlots; i++) {
        Slot& slot = slots_[i];
        if (slot.state == Free || slot.channel != channel || slot.tag != tag) {
            continue;
        }
        if (slot.state == Reading) {
            slot.discarded = true;   // the worker frees it when the read ends
        } else {
            free(slot.bytes);        // Requested (no bytes yet) or Done
            slot = Slot();
        }
    }
    LightLock_Unlock(&lock_);
}

int32_t AsyncFileReader::busy(int32_t channel) {
    int32_t count = 0;
    LightLock_Lock(&lock_);
    for (int32_t i = 0; i < kSlots; i++) {
        if (slots_[i].state != Free && slots_[i].channel == channel) {
            count++;
        }
    }
    LightLock_Unlock(&lock_);
    return count;
}

int32_t AsyncFileReader::queuedReads() {
    int32_t count = 0;
    LightLock_Lock(&lock_);
    for (int32_t i = 0; i < kSlots; i++) {
        if (slots_[i].state == Requested || slots_[i].state == Reading) {
            count++;
        }
    }
    LightLock_Unlock(&lock_);
    return count;
}

int32_t AsyncFileReader::busyAll() {
    int32_t count = 0;
    LightLock_Lock(&lock_);
    for (int32_t i = 0; i < kSlots; i++) {
        if (slots_[i].state != Free) {
            count++;
        }
    }
    LightLock_Unlock(&lock_);
    return count;
}

void AsyncFileReader::say(const char* when) {
    const double msPerTick = 1.0 / (double)(SYSCLOCK_ARM11 / 1000ull);
    const double wallMs = (double)(svcGetSystemTick() - startTick_) * msPerTick;
    const double busyMs = (double)busyTicks_ * msPerTick;
    log3dsPrintf("reader: %s: %u files (%u failed), %llu bytes; %.0f ms of the worker's time "
                 "(%.1f files/s, %.0f KB/s of it); longest open-to-finish %.1f ms; %.1f s attached",
                 when, (unsigned)files_, (unsigned)failed_, (unsigned long long)bytes_, busyMs,
                 busyMs > 0.0 ? (double)files_ * 1000.0 / busyMs : 0.0,
                 busyMs > 0.0 ? (double)bytes_ / busyMs : 0.0,
                 (double)maxTicks_ * msPerTick, wallMs / 1000.0);
}

void AsyncFileReader::closeCurrent() {
    if (curFile_ != nullptr) {
        fclose(curFile_);
        curFile_ = nullptr;
    }
    free(curBytes_);
    curBytes_ = nullptr;
    cur_ = -1;
}

void AsyncFileReader::finishCurrent(bool ok) {
    if (curFile_ != nullptr) {
        fclose(curFile_);
        curFile_ = nullptr;
    }
    uint8_t* bytes = ok ? curBytes_ : nullptr;
    if (!ok) {
        free(curBytes_);
    }
    curBytes_ = nullptr;
    size_t size = bytes ? curSize_ : 0;
    const uint64_t readBytes = size;
    if (bytes && curWork_) {
        // The request's work, here on the worker, in one piece (the HD copies'
        // LZ11 unpack, 1-94 ms; a menu GIF's decode, ~70-120 ms): the music's
        // eight seconds of queued buffers outlast it many times over.
        uint32_t worked = 0;
        uint8_t* result = curWork_(bytes, (uint32_t)size, &worked);
        free(bytes);
        bytes = result;
        size = result ? worked : 0;
    }
    const uint64_t latency = svcGetSystemTick() - curStartTick_;
    LightLock_Lock(&lock_);
    if (slots_[cur_].discarded) {
        // discard() while it was read: nobody wants it.
        free(bytes);
        slots_[cur_] = Slot();
    } else {
        slots_[cur_].bytes = bytes;
        slots_[cur_].size = bytes ? size : 0;
        slots_[cur_].ok = bytes != nullptr;
        slots_[cur_].state = Done;
    }
    LightLock_Unlock(&lock_);
    cur_ = -1;
    if (readBytes > 0) {
        files_++;
    } else {
        failed_++;
    }
    bytes_ += readBytes;
    maxTicks_ = latency > maxTicks_ ? latency : maxTicks_;
    if (readBytes > 0 && files_ % kSayEvery == 0) {
        say("running");
    }
}

bool AsyncFileReader::step() {
    const uint64_t t0 = svcGetSystemTick();
    const bool worked = stepOnce();
    if (worked) {
        busyTicks_ += svcGetSystemTick() - t0;
    }
    return worked;
}

bool AsyncFileReader::stepOnce() {
    if (cur_ < 0) {
        // The best-ranked channel's oldest request (WHICH READ GOES NEXT). The
        // order is compared by difference, so a wrapped counter still orders.
        int32_t slot = -1;
        uint32_t offset = 0;
        uint32_t length = 0;
        LightLock_Lock(&lock_);
        for (int32_t i = 0; i < kSlots; i++) {
            if (slots_[i].state != Requested) {
                continue;
            }
            if (slot < 0) {
                slot = i;
                continue;
            }
            const int32_t rank = channelRank(slots_[i].channel);
            const int32_t bestRank = channelRank(slots_[slot].channel);
            if (rank < bestRank ||
                (rank == bestRank && (int32_t)(slots_[i].order - slots_[slot].order) < 0)) {
                slot = i;
            }
        }
        if (slot >= 0) {
            slots_[slot].state = Reading;
            memcpy(curPath_, slots_[slot].path, kPathMax);
            offset = slots_[slot].offset;
            length = slots_[slot].length;
            curWork_ = slots_[slot].work;
        }
        LightLock_Unlock(&lock_);
        if (slot < 0) {
            return false;
        }
        // Opened and sized: the whole file (length 0) or `length` bytes at
        // `offset` -- a short file is a failed read, never a partial buffer.
        cur_ = slot;
        curStartTick_ = svcGetSystemTick();
        curDone_ = 0;
        curSize_ = 0;
        curFile_ = fopen(curPath_, "rb");
        bool ok = curFile_ != nullptr;
        if (ok && length == 0) {
            long end = -1;
            if (fseek(curFile_, 0, SEEK_END) == 0) {
                end = ftell(curFile_);
            }
            ok = end > 0 && fseek(curFile_, 0, SEEK_SET) == 0;
            curSize_ = ok ? (uint32_t)end : 0;
        } else if (ok) {
            ok = length <= kMaxRangeBytes && fseek(curFile_, (long)offset, SEEK_SET) == 0;
            curSize_ = length;
        }
        if (ok) {
            curBytes_ = static_cast<uint8_t*>(malloc(curSize_));
            ok = curBytes_ != nullptr;
        }
        if (!ok) {
            finishCurrent(false);
        }
        return true;
    }
    // A read nobody wants any more ends here, not after its last chunk.
    LightLock_Lock(&lock_);
    const bool dropped = slots_[cur_].discarded;
    if (dropped) {
        slots_[cur_] = Slot();
    }
    LightLock_Unlock(&lock_);
    if (dropped) {
        closeCurrent();
        return true;
    }
    // One chunk.
    const uint32_t want = curSize_ - curDone_ < kChunkBytes ? curSize_ - curDone_ : kChunkBytes;
    const size_t got = fread(curBytes_ + curDone_, 1, want, curFile_);
    curDone_ += (uint32_t)got;
    if (got != want) {
        finishCurrent(false);
    } else if (curDone_ == curSize_) {
        finishCurrent(true);
    }
    return true;
}

} // namespace mdk

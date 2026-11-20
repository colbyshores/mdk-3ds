// ============================================================================
// async_file_reader.cpp -- see async_file_reader.h.
// ============================================================================

#include "platform/async_file_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/log_3ds.h"
#include "platform/vfp_fpscr.h"
#include "platform/worker_core.h"

namespace mdk {

namespace {

// The whole file into a malloc'd buffer, or null. Only the worker calls it.
uint8_t* readWhole(const char* path, size_t* size) {
    FILE* file = fopen(path, "rb");
    if (!file) {
        return nullptr;
    }
    uint8_t* bytes = nullptr;
    long length = -1;
    if (fseek(file, 0, SEEK_END) == 0) {
        length = ftell(file);
    }
    if (length > 0 && fseek(file, 0, SEEK_SET) == 0) {
        bytes = static_cast<uint8_t*>(malloc((size_t)length));
        if (bytes && fread(bytes, 1, (size_t)length, file) != (size_t)length) {
            free(bytes);
            bytes = nullptr;
        }
    }
    fclose(file);
    if (bytes) {
        *size = (size_t)length;
    }
    return bytes;
}

// `length` bytes at `offset` into a malloc'd buffer, or null -- a short read
// (the file ends early) is null, never a partial buffer. Only the worker calls it.
uint8_t* readRange(const char* path, uint32_t offset, uint32_t length) {
    if (length == 0 || length > AsyncFileReader::kMaxRangeBytes) {
        return nullptr;
    }
    FILE* file = fopen(path, "rb");
    if (!file) {
        return nullptr;
    }
    uint8_t* bytes = nullptr;
    if (fseek(file, (long)offset, SEEK_SET) == 0) {
        bytes = static_cast<uint8_t*>(malloc(length));
        if (bytes && fread(bytes, 1, length, file) != length) {
            free(bytes);
            bytes = nullptr;
        }
    }
    fclose(file);
    return bytes;
}

// THE THREAD'S ENTRY, a plain function so the R1 gate (tools/runner/gates_vfp.py)
// can resolve the routine threadCreate names and read its body.
void asyncFileReaderThread(void* self) {
    // R1, AT THE WORKER'S ENTRY. Flush-to-zero and default-NaN are MDK's chosen
    // numerical mode, and FPSCR is per thread: the main thread's state is not
    // evidence of this one's. This thread does no float work today (it reads
    // files), but decaying values can reach subnormals in any float code a later
    // change adds here, and modern libctru already initialises the mode on its
    // supported thread entry -- so this is the defensive, read-back form the
    // contract asks every float-capable thread to carry.
    const bool fzdn = vfpThreadFzDn();
    if (!fzdn) {
        log3dsPrintf("hd: reader thread FZ/DN read-back FAILED");
    }
    static_cast<AsyncFileReader*>(self)->run();
}

} // namespace

bool AsyncFileReader::start() {
    if (running_) {
        return true;
    }
    LightLock_Init(&lock_);
    LightEvent_Init(&wake_, RESET_ONESHOT);
    exit_ = false;
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    // One below the main thread (a larger number is a lower priority) -- and so
    // also below the DSP audio worker (main - 1), which shares core 1 with it and
    // must preempt an SD read for its 25 ms music refill. On kWorkerCore (core 1,
    // the owner decision in worker_core.h), off the main thread's core; if
    // threadCreate refuses it, on the process's default core as before.
    const s32 workerPriority = priority < 0x3F ? priority + 1 : priority;
    int core = kWorkerCore;
    thread_ = threadCreate(asyncFileReaderThread, this, kStackBytes, workerPriority, core, false);
    if (thread_ == nullptr && core != kWorkerCoreFallback) {
        core = kWorkerCoreFallback;
        thread_ = threadCreate(asyncFileReaderThread, this, kStackBytes, workerPriority, core,
                               false);
    }
    running_ = thread_ != nullptr;
    log3dsPrintf("hd: reader thread %s (priority 0x%X, stack %u, core %d%s)",
                 running_ ? "started" : "FAILED", (unsigned)workerPriority, (unsigned)kStackBytes,
                 core, core != kWorkerCore ? " (kWorkerCore refused)" : "");
    return running_;
}

void AsyncFileReader::stop() {
    if (running_) {
        LightLock_Lock(&lock_);
        exit_ = true;
        LightLock_Unlock(&lock_);
        LightEvent_Signal(&wake_);
        threadJoin(thread_, U64_MAX);
        threadFree(thread_);
        thread_ = nullptr;
        running_ = false;
    }
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
            snprintf(slots_[i].path, kPathMax, "%s", path);
            queued = true;
            break;
        }
    }
    LightLock_Unlock(&lock_);
    if (queued) {
        LightEvent_Signal(&wake_);
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

void AsyncFileReader::run() {
    char path[kPathMax];
    for (;;) {
        int32_t slot = -1;
        uint32_t offset = 0;
        uint32_t length = 0;
        Work work = nullptr;
        LightLock_Lock(&lock_);
        if (exit_) {
            LightLock_Unlock(&lock_);
            return;
        }
        for (int32_t i = 0; i < kSlots; i++) {
            if (slots_[i].state == Requested) {
                slots_[i].state = Reading;
                memcpy(path, slots_[i].path, kPathMax);
                offset = slots_[i].offset;
                length = slots_[i].length;
                work = slots_[i].work;
                slot = i;
                break;
            }
        }
        LightLock_Unlock(&lock_);
        if (slot < 0) {
            LightEvent_Wait(&wake_);
            continue;
        }
        size_t size = length;
        uint8_t* bytes = length > 0 ? readRange(path, offset, length) : readWhole(path, &size);
        if (bytes && work) {
            // The request's work, here on the worker (the slideshow's GIF decode).
            uint32_t worked = 0;
            uint8_t* result = work(bytes, (uint32_t)size, &worked);
            free(bytes);
            bytes = result;
            size = result ? worked : 0;
        }
        LightLock_Lock(&lock_);
        if (slots_[slot].discarded) {
            // discard() while it was read: nobody wants it.
            free(bytes);
            slots_[slot] = Slot();
        } else {
            slots_[slot].bytes = bytes;
            slots_[slot].size = bytes ? size : 0;
            slots_[slot].ok = bytes != nullptr;
            slots_[slot].state = Done;
        }
        LightLock_Unlock(&lock_);
    }
}

} // namespace mdk

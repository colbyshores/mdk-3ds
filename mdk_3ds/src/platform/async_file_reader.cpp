// ============================================================================
// async_file_reader.cpp -- see async_file_reader.h.
// ============================================================================

#include "platform/async_file_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/log_3ds.h"
#include "platform/vfp_fpscr.h"

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
    // One below the main thread (a larger number is a lower priority), and on the
    // application core (-2: the default core of the process).
    const s32 workerPriority = priority < 0x3F ? priority + 1 : priority;
    thread_ = threadCreate(asyncFileReaderThread, this, kStackBytes, workerPriority, -2, false);
    running_ = thread_ != nullptr;
    log3dsPrintf("hd: reader thread %s (priority 0x%X, stack %u)", running_ ? "started" : "FAILED",
                 (unsigned)workerPriority, (unsigned)kStackBytes);
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
    if (!running_ || !path || strlen(path) >= kPathMax) {
        return false;
    }
    bool queued = false;
    LightLock_Lock(&lock_);
    for (int32_t i = 0; i < kSlots; i++) {
        if (slots_[i].state == Free) {
            slots_[i].state = Requested;
            slots_[i].tag = tag;
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

bool AsyncFileReader::poll(int32_t* tag, char* path, uint8_t** bytes, size_t* size, bool* ok) {
    if (!running_) {
        return false;
    }
    bool found = false;
    LightLock_Lock(&lock_);
    for (int32_t i = 0; i < kSlots; i++) {
        if (slots_[i].state == Done) {
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

int32_t AsyncFileReader::busy() {
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
        LightLock_Lock(&lock_);
        if (exit_) {
            LightLock_Unlock(&lock_);
            return;
        }
        for (int32_t i = 0; i < kSlots; i++) {
            if (slots_[i].state == Requested) {
                slots_[i].state = Reading;
                memcpy(path, slots_[i].path, kPathMax);
                slot = i;
                break;
            }
        }
        LightLock_Unlock(&lock_);
        if (slot < 0) {
            LightEvent_Wait(&wake_);
            continue;
        }
        size_t size = 0;
        uint8_t* bytes = readWhole(path, &size);
        LightLock_Lock(&lock_);
        slots_[slot].bytes = bytes;
        slots_[slot].size = bytes ? size : 0;
        slots_[slot].ok = bytes != nullptr;
        slots_[slot].state = Done;
        LightLock_Unlock(&lock_);
    }
}

} // namespace mdk

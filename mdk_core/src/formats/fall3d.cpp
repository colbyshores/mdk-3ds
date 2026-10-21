// ============================================================================
// fall3d.cpp — see fall3d.h for the format.
// ============================================================================

#include "formats/fall3d.h"

#include <string.h>

#include "formats/bin_reader.h"

namespace mdk {
namespace formats {

namespace {

// The pixels per group of the records.
const int32_t kHazeGroup = 4;
const int32_t kPickupNameLength = 12;
// The size field in front of a haze entry.
const int32_t kSizeField = 4;

} // namespace

bool Fall3d::haze(const Bni& bni, const std::string& name, std::vector<uint8_t>* outPixels) {
    if (!outPixels) {
        return false;
    }

    const ArchiveIndex::Entry* entry = bni.index().find(name);
    if (!entry) {
        return false;
    }

    const std::vector<uint8_t>& bytes = bni.bytes();
    const int32_t length = (int32_t)bytes.size();
    const int32_t total = kHazeWidth * kHazeRows;
    std::vector<uint8_t> pixels((size_t)total, 0);

    int32_t at = entry->offset + kSizeField;
    for (int32_t row = 0; row < kHazeRows; row++) {
        if (at + 4 > length) {
            return false;
        }

        uint32_t leftCount = 0;
        memcpy(&leftCount, bytes.data() + at, 4);
        at += 4;
        const int32_t left = (int32_t)leftCount * kHazeGroup;
        if (left < 0 || at + left > length) {
            return false;
        }

        memcpy(pixels.data() + row * kHazeWidth, bytes.data() + at, (size_t)left);
        at += left;

        if (at + 8 > length) {
            return false;
        }

        uint32_t middleCount = 0;
        uint32_t rightCount = 0;
        memcpy(&middleCount, bytes.data() + at, 4);
        memcpy(&rightCount, bytes.data() + at + 4, 4);
        at += 8;
        const int32_t middle = (int32_t)middleCount * kHazeGroup;
        const int32_t right = (int32_t)rightCount * kHazeGroup;
        if (right < 0 || at + right > length) {
            return false;
        }

        // The three runs together are one row: the middle's width says how
        // much of the row blends, and the middle itself is not stored.
        const int32_t target = row * kHazeWidth + left + middle;
        if (target < 0 || right > total - target) {
            return false;
        }

        memcpy(pixels.data() + target, bytes.data() + at, (size_t)right);
        at += right;
    }

    *outPixels = pixels;
    return true;
}

std::vector<std::string> Fall3d::pickups(const Bni& bni, const std::string& name) {
    std::vector<std::string> names;
    if (!bni.has(name)) {
        return names;
    }

    const ArchiveIndex::Entry* entry = bni.index().find(name);
    if (!entry) {
        return names;
    }

    const std::vector<uint8_t>& bytes = bni.bytes();
    const int32_t end = entry->offset + entry->size;
    for (int32_t at = entry->offset; at + kPickupNameLength <= end && at < (int32_t)bytes.size();
         at += kPickupNameLength) {
        if (bytes[(size_t)at] == 0) {
            break;
        }

        // The name is NUL-padded; the C#'s Bin.Ascii stops at the first NUL.
        std::string pickup;
        for (int32_t i = 0; i < kPickupNameLength && at + i < (int32_t)bytes.size(); i++) {
            const uint8_t c = bytes[(size_t)(at + i)];
            if (c == 0) {
                break;
            }

            pickup.push_back((char)c);
        }

        names.push_back(pickup);
    }

    return names;
}

} // namespace formats
} // namespace mdk

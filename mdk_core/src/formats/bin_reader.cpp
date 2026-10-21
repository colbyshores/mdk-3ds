// ============================================================================
// bin_reader.cpp — see bin_reader.h for why the reads go through memcpy.
// ============================================================================

#include "formats/bin_reader.h"

#include <string.h>

namespace mdk {
namespace formats {

uint16_t binU16(const uint8_t* bytes, int32_t at) {
    uint16_t value = 0;
    memcpy(&value, bytes + at, 2);
    return value;
}

int16_t binS16(const uint8_t* bytes, int32_t at) {
    int16_t value = 0;
    memcpy(&value, bytes + at, 2);
    return value;
}

uint32_t binU32(const uint8_t* bytes, int32_t at) {
    uint32_t value = 0;
    memcpy(&value, bytes + at, 4);
    return value;
}

int32_t binS32(const uint8_t* bytes, int32_t at) {
    int32_t value = 0;
    memcpy(&value, bytes + at, 4);
    return value;
}

float binF32(const uint8_t* bytes, int32_t at) {
    float value = 0.0f;
    memcpy(&value, bytes + at, 4);
    return value;
}

void binAscii(const uint8_t* bytes, int32_t at, int32_t length, char* out) {
    int32_t stop = length;
    for (int32_t i = 0; i < length; i++) {
        if (bytes[at + i] == 0) {
            stop = i;
            break;
        }
    }
    memcpy(out, bytes + at, (size_t)stop);
    out[stop] = '\0';
}

} // namespace formats
} // namespace mdk

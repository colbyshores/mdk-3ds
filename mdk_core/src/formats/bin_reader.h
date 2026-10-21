#pragma once

// ============================================================================
// bin_reader.h — the sequential little-endian reader the formats share.
//
// THE C# ORIGINAL is src/Mdk.Formats/BinReader.cs. This is a 1:1 port.
//
// WHY THE READS GO THROUGH MEMCPY, AND WHY THAT IS NOT OPTIMISM:
//
// The C#'s BinaryPrimitives.ReadSingleLittleEndian is alignment-agnostic: it
// reads a span and assembles the value byte by byte. A C++ `*(float*)ptr`
// is not. The formats read binary data that is NOT guaranteed to be
// aligned: MDK's files interleave 16-bit and 32-bit fields, so a float
// field can sit at a 2-byte-aligned address.
//
// The 3DS's ARM11 (ARMv6K) tolerates an unaligned LDR/STR but NOT an
// unaligned LDRD/STRD, and the compiler is free to use the latter -- or to
// fold the load into one. So a `*(float*)` into a misaligned buffer is not
// "slow", it is "undefined, and the optimiser may make it worse".
//
// ../forsaken/3DS_PORT_NOTES.md lists the ARM unaligned-access trap among
// the porting hazards; this is that hazard, and the fix is memcpy. AGENTS.md
// R10 requires -fno-strict-aliasing in BOTH builds for the same reason.
//
// memcpy of a fixed 2 or 4 bytes compiles to a single load on the target,
// so this costs nothing. What it buys is that the code is CORRECT rather
// than correct-if-lucky.
// ============================================================================

#include <stdint.h>
#include <string.h>

#include <vector>

namespace mdk {
namespace formats {

// A sequential little-endian reader over a byte buffer.
//
// The buffer is BORROWED, never owned: it must outlive the reader. The
// reader is a plain class with no virtual functions, per AGENTS.md §10.
class BinReader {
public:
    BinReader(const uint8_t* bytes, int32_t length, int32_t pos = 0)
        : bytes_(bytes), length_(length), pos_(pos) {}

    // A reader over a vector. The vector is BORROWED and must outlive the
    // reader. The length is given separately because the callers that own
    // a vector usually know the count they want to read.
    BinReader(const std::vector<uint8_t>& bytes, int32_t length, int32_t pos = 0)
        : bytes_(bytes.data()), length_(length), pos_(pos) {}

    bool eof() const { return pos_ >= length_; }
    int32_t pos() const { return pos_; }
    void setPos(int32_t value) { pos_ = value; }
    int32_t length() const { return length_; }
    void skip(int32_t count) { pos_ += count; }

    // EVERY READ IS BOUNDS CHECKED.
    //
    // The C# throws when a file is truncated, and the C++ has no
    // exceptions. So a read past the end returns zero and the position
    // still advances: the parse carries on and the caller ends up with a
    // visibly wrong value rather than a crash.
    //
    // THIS MATTERS MOST ON THE 3DS, where a data file can be missing or
    // short and a crash there is a crash in the game, not an exception in a
    // test. The formats are a COLD path (AGENTS.md §2), so the check costs
    // nothing that matters.
    uint8_t u8() {
        const uint8_t value = (pos_ >= 0 && pos_ < length_) ? bytes_[pos_] : 0;
        pos_++;
        return value;
    }

    uint16_t u16() {
        uint16_t value = 0;
        if (pos_ >= 0 && pos_ + 2 <= length_) {
            memcpy(&value, bytes_ + pos_, 2);
        }
        pos_ += 2;
        return value;
    }

    int16_t s16() {
        int16_t value = 0;
        if (pos_ >= 0 && pos_ + 2 <= length_) {
            memcpy(&value, bytes_ + pos_, 2);
        }
        pos_ += 2;
        return value;
    }

    uint32_t u32() {
        uint32_t value = 0;
        if (pos_ >= 0 && pos_ + 4 <= length_) {
            memcpy(&value, bytes_ + pos_, 4);
        }
        pos_ += 4;
        return value;
    }

    int32_t s32() {
        int32_t value = 0;
        if (pos_ >= 0 && pos_ + 4 <= length_) {
            memcpy(&value, bytes_ + pos_, 4);
        }
        pos_ += 4;
        return value;
    }

    float f32() {
        float value = 0.0f;
        if (pos_ >= 0 && pos_ + 4 <= length_) {
            memcpy(&value, bytes_ + pos_, 4);
        }
        pos_ += 4;
        return value;
    }

    // Three floats, in order. The out parameter is a plain array so that no
    // project-defined type is needed for a vector.
    void vec3(float out[3]) {
        out[0] = f32();
        out[1] = f32();
        out[2] = f32();
    }

    // A fixed-length, NUL-padded ASCII string. The result is written into
    // `out`, which must have room for `length` bytes PLUS a terminator, and
    // is always NUL-terminated. The C#'s Bin.Ascii stops at the first NUL
    // within the field; this does the same.
    void name(char* out, int32_t length) {
        int32_t stop = length;
        for (int32_t i = 0; i < length; i++) {
            if (pos_ + i < 0 || pos_ + i >= length_) {
                break;
            }
            if (bytes_[pos_ + i] == 0) {
                stop = i;
                break;
            }
        }
        if (stop > 0 && pos_ >= 0 && pos_ + stop <= length_) {
            memcpy(out, bytes_ + pos_, (size_t)stop);
        }
        out[stop] = '\0';
        pos_ += length;
    }

    // A run of bytes. The result POINTS INTO THE BUFFER; it is not a copy.
    // The length is returned through `outLength` (which may be null).
    const uint8_t* buffer(int32_t length, int32_t* outLength = nullptr) {
        if (outLength) {
            *outLength = length;
        }
        const uint8_t* result = (bytes_ && pos_ >= 0 && pos_ <= length_) ? (bytes_ + pos_) : nullptr;
        pos_ += length;
        return result;
    }

private:
    const uint8_t* bytes_ = nullptr;
    int32_t length_ = 0;
    int32_t pos_ = 0;
};

// The random-access reads, at an offset.
//
// These are the C#'s `Bin` static class. They are free functions rather
// than methods because they do not advance a position.
uint16_t binU16(const uint8_t* bytes, int32_t at);
int16_t binS16(const uint8_t* bytes, int32_t at);
uint32_t binU32(const uint8_t* bytes, int32_t at);
int32_t binS32(const uint8_t* bytes, int32_t at);
float binF32(const uint8_t* bytes, int32_t at);

// ASCII up to the first NUL within `length` bytes, written into `out` (which
// must have room for `length` + 1). The C#'s Bin.Ascii.
void binAscii(const uint8_t* bytes, int32_t at, int32_t length, char* out);

} // namespace formats
} // namespace mdk

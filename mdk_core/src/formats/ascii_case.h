#pragma once

// ============================================================================
// ascii_case.h — ASCII names in lower case and in upper case.
//
// The C# uses string.ToLowerInvariant and string.ToUpperInvariant on the
// names these formats hold. Those names are ASCII (MDK's object, part and
// arena names are), so the invariant culture and the ASCII rules agree, and
// a byte-wise conversion gives the same answer without dragging the locale
// machinery into a game that must run on a 3DS.
//
// HEADER ONLY, on purpose: it is two functions, and a header needs no
// object file, so it cannot collide with another file's object in the
// 3DS build (which keys objects by `notdir`).
// ============================================================================

#include <stddef.h>

#include <string>

namespace mdk {
namespace formats {

// The name in lower case.
inline std::string toLowerAscii(const std::string& text) {
    std::string out = text;
    for (size_t i = 0; i < out.size(); i++) {
        if (out[i] >= 'A' && out[i] <= 'Z') {
            out[i] = (char)(out[i] - 'A' + 'a');
        }
    }
    return out;
}

// The name in upper case.
inline std::string toUpperAscii(const std::string& text) {
    std::string out = text;
    for (size_t i = 0; i < out.size(); i++) {
        if (out[i] >= 'a' && out[i] <= 'z') {
            out[i] = (char)(out[i] - 'a' + 'A');
        }
    }
    return out;
}

} // namespace formats
} // namespace mdk

// cheats.cpp -- see cheats.h. Ports src/Mdk.Game/Flow/Cheats.cs and the filter
// of Viewer.TypeCheats (Viewer.cs:862-882).

#include "game/cheats.h"

#include "game/game_settings.h"
#include "game/game_state.h"

namespace mdk {
namespace flow {
namespace {

// C# Cheats.GoreCheat and DebugCheat.
const char* const kGoreCheat = "TOOSCARYFORME";
const char* const kDebugCheat = "SEETHEWHOLEGAME";
// DebugCheat.Length: the window is as long as the longest cheat.
constexpr size_t kWindow = 15;

// C# string.EndsWith(value, StringComparison.Ordinal).
bool endsWith(const std::string& text, const char* value) {
    const std::string tail(value);
    return text.size() >= tail.size() &&
           text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
}

} // namespace

// C# Cheats.Type (Cheats.cs:17-35).
Cheats::Cheat Cheats::type(char letter) {
    // `_typed = (_typed + letter)[Math.Max(0, _typed.Length + 1 - DebugCheat.Length)..]`
    typed_ += letter;
    if (typed_.size() > kWindow) {
        typed_.erase(0, typed_.size() - kWindow);
    }
    if (endsWith(typed_, kGoreCheat)) {
        typed_.clear();
        settings_->gore = !settings_->gore;
        return Gore;
    }
    if (endsWith(typed_, kDebugCheat)) {
        typed_.clear();
        state_->debugKeys = !state_->debugKeys;
        return DebugKeys;
    }
    return None;
}

const char* Cheats::name(Cheat cheat) {
    switch (cheat) {
        case Gore: return "Gore";
        case DebugKeys: return "DebugKeys";
        default: return "None";
    }
}

// Viewer.TypeCheats: `typed.ToUpperInvariant().Where(char.IsAsciiLetterUpper)`.
std::string cheatLetters(const std::string& typed) {
    std::string letters;
    for (size_t i = 0; i < typed.size(); i++) {
        char c = typed[i];
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        if (c >= 'A' && c <= 'Z') {
            letters += c;
        }
    }
    return letters;
}

} // namespace flow
} // namespace mdk

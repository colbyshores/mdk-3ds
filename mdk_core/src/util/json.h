#pragma once

// ============================================================================
// json.h -- the JSON the save files are made of, written byte for byte as
// .NET's System.Text.Json writes it, and read back as its JsonNode reads it
// (package #19, save/load).
//
// WHY A WRITER OF ITS OWN AND NOT A LIBRARY. The save file format is the C#'s
// (Flow/SaveGames.cs, Scripts/Snapshot.cs): a save written by the C# loads here
// and one written here loads in the C#, and the goldens compare the two
// programs' saves BYTE FOR BYTE. That pins three things a general JSON library
// does its own way:
//
//   * FLOATS. `JsonValue.Create(float)` is written as the SHORTEST decimal that
//     reads back as the same float, .NET's "R" shape: fixed notation while the
//     decimal exponent is -5 < e < 9, else `d.dddE+XX` (sign always, at least
//     two exponent digits): 0.1, 100, 123456790, 1E+09, 1E-05, 1.1754944E-38,
//     -0. formatFloat below; tests/formats pins it against 200 000 floats the
//     C# formatted (golden/json_floats.txt).
//   * STRINGS. JavaScriptEncoder.Default escapes the HTML-sensitive characters
//     too: < > & ' " + ` become < > & ' " + `
//     (upper-case hex), the controls \b \t \n \f \r stay short, the other
//     controls and every non-ASCII code point become \uXXXX (a UTF-16 pair above
//     U+FFFF), and / is NOT escaped.
//   * LAYOUT. Compact: no spaces, no newline, members in insertion order.
//
// The reader is a small DOM over a source text: nodes in one vector, objects'
// members in source order (a JsonObject keeps its order), and each value's
// exact source span (SaveGame.Parse keeps the snapshot as GetRawText, the
// original bytes). It fails on malformed text rather than guess: no comments,
// no trailing commas, nesting at most 64 deep (JsonDocument's default).
//
// House style (AGENTS.md §10): plain classes, no templates, no exceptions --
// every reader returns false or a default on a missing or mistyped value.
// ============================================================================

#include <stdint.h>
#include <stddef.h>

#include <string>
#include <vector>

namespace mdk {
namespace json {

// The shortest decimal that reads back as `value`, as .NET writes a float
// (`float.ToString("R", InvariantCulture)`, which is what Utf8JsonWriter's
// WriteNumberValue(float) writes). `value` must be finite; NaN and the
// infinities are not JSON numbers (the C#'s Snapshot writes them as strings,
// see floatText). Returns the length written into `out` (at most 32 bytes,
// NUL-terminated).
int formatFloat(float value, char out[32]);

// The text .NET's InvariantCulture gives any float, finite or not: formatFloat's
// for a finite one, "NaN", "Infinity" or "-Infinity" otherwise (Snapshot.Number).
std::string floatText(float value);

// Appends `text` as a JSON string literal with JavaScriptEncoder.Default's
// escaping. `text` is UTF-8; a byte that does not start a valid UTF-8 sequence
// is taken as the Latin-1 character of that value.
void appendString(std::string* out, const std::string& text);

// A compact JSON writer: commas and colons are its business, the caller says
// what comes next. Mismatched begin/end calls are the caller's bug; the writer
// tracks at most kMaxDepth levels and stops separating deeper than that.
class Writer {
public:
    static const int kMaxDepth = 64;

    explicit Writer(std::string* out) : out_(out) {}

    void beginObject();
    void endObject();
    void beginArray();
    void endArray();
    // An object member's name; its value is the next call.
    void key(const char* name);
    void key(const std::string& name);

    void valueInt(int32_t value);
    // A float number when finite; its text as a string otherwise
    // (Snapshot.Number). Kurt's own fields write a bare float, which the C#
    // would throw on for a non-finite value -- they are always finite.
    void valueFloat(float value);
    void valueBool(bool value);
    void valueNull();
    void valueString(const std::string& value);
    // A value written as is (Utf8JsonWriter.WriteRawValue): valid JSON text.
    void valueRaw(const std::string& text);

private:
    void separate();

    std::string* out_;
    int depth_ = 0;
    // Per open level: whether it has a member yet, and whether a key was just
    // written (the value that follows takes no comma).
    bool hasMember_[kMaxDepth + 1] = {};
    bool afterKey_ = false;
};

// A parsed JSON text. Node 0 is the root after a successful parse.
class Document {
public:
    enum Type { Null, False, True, Number, String, Array, Object };
    static const int kNone = -1;
    static const int kMaxDepth = 64;

    // Parses `text`; false (and an empty document) on malformed JSON or
    // trailing non-space text.
    bool parse(const std::string& text);

    int root() const { return nodes_.empty() ? kNone : 0; }
    Type type(int node) const;
    bool isNull(int node) const { return node == kNone || type(node) == Null; }

    // An object's member by name, kNone when absent or `node` is not an object.
    int member(int node, const char* name) const;
    // Members and elements in source order: first, then next until kNone.
    int first(int node) const;
    int next(int node) const;
    // A member's name (the node is a member VALUE, as first/next return).
    const std::string& name(int node) const;
    // An array's length / element.
    int count(int node) const;
    int at(int node, int index) const;

    // Values. asInt is JsonNode.GetValue<int>: a number token that is an
    // integer in range; false otherwise. asFloat reads a number token (the
    // nearest float, as float.Parse); asDouble the nearest double.
    bool asInt(int node, int32_t* out) const;
    bool asFloat(int node, float* out) const;
    bool asDouble(int node, double* out) const;
    bool asBool(int node, bool* out) const;
    bool asString(int node, std::string* out) const;
    // The node's exact text in the source (JsonElement.GetRawText).
    std::string raw(int node) const;

    // Convenience readers with the C#'s `?? fallback` shape.
    int32_t intOr(int node, int32_t fallback) const;
    float floatOr(int node, float fallback) const;
    std::string stringOr(int node, const std::string& fallback) const;

private:
    struct Node {
        Type type = Null;
        int firstChild = kNone;
        int lastChild = kNone;
        int nextSibling = kNone;
        int children = 0;
        int nameIndex = -1;  // into names_, for an object's member
        size_t begin = 0;    // the source span
        size_t end = 0;
        std::string text;    // a string's value, or a number's token
    };

    int parseValue(int depth);
    bool parseString(std::string* out);
    void skipSpace();
    int add(Type type);

    std::string text_;
    size_t at_ = 0;
    std::vector<Node> nodes_;
    std::vector<std::string> names_;
};

} // namespace json
} // namespace mdk

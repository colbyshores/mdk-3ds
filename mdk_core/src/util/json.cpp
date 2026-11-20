// json.cpp -- see json.h: .NET's System.Text.Json output, byte for byte, and a
// small reader for it (package #19, save/load).

#include "util/json.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace mdk {
namespace json {

namespace {

// The `prec` significant digits of `a` (> 0, finite) correctly rounded, and the
// decimal exponent of the first: a ~= 0.d1d2... * 10^(exp + 1). The double holds
// the float exactly, so %e rounds the float's exact value.
void roundedDigits(float a, int prec, char digits[16], int* exp) {
    char buf[48];
    snprintf(buf, sizeof(buf), "%.*e", prec - 1, (double)a);
    int n = 0;
    const char* p = buf;
    for (; *p != 'e'; p++) {
        if (*p >= '0' && *p <= '9') {
            digits[n++] = *p;
        }
    }
    digits[n] = '\0';
    *exp = atoi(p + 1);
}

// Whether the decimal digits (as d.ddd x 10^exp) read back as `a`.
bool readsBack(const char* digits, int exp, float a) {
    char buf[48];
    snprintf(buf, sizeof(buf), "%c.%se%d", digits[0], digits + 1, exp);
    return strtof(buf, nullptr) == a;
}

// digits +/- 1 in the last place, carrying; false when it leaves the length
// (all nines up, or a leading zero down).
bool stepDigits(const char* digits, int delta, char out[16], int* exp) {
    const int n = (int)strlen(digits);
    memcpy(out, digits, (size_t)n + 1);
    int i = n - 1;
    if (delta > 0) {
        while (i >= 0 && out[i] == '9') {
            out[i--] = '0';
        }
        if (i < 0) {
            // 99..9 + 1 = 100..0: one more digit; renormalise to the same length.
            out[0] = '1';
            for (int k = 1; k < n; k++) {
                out[k] = '0';
            }
            *exp += 1;
            return true;
        }
        out[i]++;
        return true;
    }
    while (i >= 0 && out[i] == '0') {
        out[i--] = '9';
    }
    if (i < 0) {
        return false;
    }
    out[i]--;
    if (out[0] == '0') {
        if (n == 1) {
            return false;
        }
        // 10..0 - 1 = 9..9 one place lower.
        for (int k = 0; k < n; k++) {
            out[k] = '9';
        }
        *exp -= 1;
    }
    return true;
}

// |candidate - a| in doubles, for the tie between two neighbours that both read back.
double distanceTo(const char* digits, int exp, float a) {
    char buf[48];
    snprintf(buf, sizeof(buf), "%c.%se%d", digits[0], digits + 1, exp);
    return fabs(strtod(buf, nullptr) - (double)a);
}

void appendHex4(std::string* out, uint32_t unit) {
    static const char kHex[] = "0123456789ABCDEF";
    char buf[6] = {'\\', 'u', kHex[(unit >> 12) & 0xF], kHex[(unit >> 8) & 0xF],
                   kHex[(unit >> 4) & 0xF], kHex[unit & 0xF]};
    out->append(buf, 6);
}

// The code point at `text[*i]` (advancing *i), or the byte as Latin-1 when it does
// not start a valid UTF-8 sequence.
uint32_t nextCodePoint(const std::string& text, size_t* i) {
    const unsigned char c = (unsigned char)text[*i];
    int extra = 0;
    uint32_t cp = c;
    uint32_t minimum = 0;
    if (c >= 0xC0 && c < 0xE0) {
        extra = 1;
        cp = c & 0x1F;
        minimum = 0x80;
    } else if (c >= 0xE0 && c < 0xF0) {
        extra = 2;
        cp = c & 0x0F;
        minimum = 0x800;
    } else if (c >= 0xF0 && c < 0xF5) {
        extra = 3;
        cp = c & 0x07;
        minimum = 0x10000;
    }
    if (extra == 0 || *i + (size_t)extra >= text.size()) {
        // ASCII, a byte that starts nothing, or a sequence cut off by the end.
        (*i)++;
        return c;
    }
    uint32_t value = cp;
    for (int k = 1; k <= extra; k++) {
        const unsigned char b = (unsigned char)text[*i + (size_t)k];
        if ((b & 0xC0) != 0x80) {
            (*i)++;
            return c;
        }
        value = (value << 6) | (b & 0x3F);
    }
    if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value < 0xE000)) {
        (*i)++;
        return c;
    }
    *i += (size_t)extra + 1;
    return value;
}

void appendUtf8(std::string* out, uint32_t cp) {
    if (cp < 0x80) {
        out->push_back((char)cp);
    } else if (cp < 0x800) {
        out->push_back((char)(0xC0 | (cp >> 6)));
        out->push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out->push_back((char)(0xE0 | (cp >> 12)));
        out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out->push_back((char)(0xF0 | (cp >> 18)));
        out->push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back((char)(0x80 | (cp & 0x3F)));
    }
}

} // namespace

int formatFloat(float value, char out[32]) {
    int len = 0;
    if (signbit(value)) {
        out[len++] = '-';
    }
    const float a = fabsf(value);
    if (a == 0.0f) {
        out[len++] = '0';
        out[len] = '\0';
        return len;
    }

    // The shortest digits that read back (Ryu's answer: the fewest digits, and of
    // those the nearest). The correctly rounded n digits are the nearest n-digit
    // decimal; at a power of two the interval is lopsided, so when they miss, a
    // neighbour one unit away can still be inside it.
    char digits[16] = {0};
    int exp = 0;
    for (int prec = 1; prec <= 9; prec++) {
        char rounded[16];
        int e = 0;
        roundedDigits(a, prec, rounded, &e);
        if (readsBack(rounded, e, a)) {
            memcpy(digits, rounded, sizeof(digits));
            exp = e;
            break;
        }
        char up[16];
        char down[16];
        int eUp = e;
        int eDown = e;
        const bool hasUp = stepDigits(rounded, 1, up, &eUp) && readsBack(up, eUp, a);
        const bool hasDown = stepDigits(rounded, -1, down, &eDown) && readsBack(down, eDown, a);
        if (hasUp && (!hasDown || distanceTo(up, eUp, a) <= distanceTo(down, eDown, a))) {
            memcpy(digits, up, sizeof(digits));
            exp = eUp;
            break;
        }
        if (hasDown) {
            memcpy(digits, down, sizeof(digits));
            exp = eDown;
            break;
        }
    }
    int n = (int)strlen(digits);
    while (n > 1 && digits[n - 1] == '0') {
        digits[--n] = '\0';
    }

    if (exp >= 9 || exp < -4) {
        // d[.ddd]E+XX
        out[len++] = digits[0];
        if (n > 1) {
            out[len++] = '.';
            memcpy(out + len, digits + 1, (size_t)n - 1);
            len += n - 1;
        }
        len += snprintf(out + len, (size_t)(32 - len), "E%c%02d", exp < 0 ? '-' : '+',
                        exp < 0 ? -exp : exp);
        return len;
    }
    if (exp >= 0) {
        const int whole = exp + 1;
        for (int k = 0; k < whole; k++) {
            out[len++] = k < n ? digits[k] : '0';
        }
        if (n > whole) {
            out[len++] = '.';
            memcpy(out + len, digits + whole, (size_t)(n - whole));
            len += n - whole;
        }
    } else {
        out[len++] = '0';
        out[len++] = '.';
        for (int k = 0; k < -exp - 1; k++) {
            out[len++] = '0';
        }
        memcpy(out + len, digits, (size_t)n);
        len += n;
    }
    out[len] = '\0';
    return len;
}

std::string floatText(float value) {
    if (isnan(value)) {
        return "NaN";
    }
    if (isinf(value)) {
        return value < 0.0f ? "-Infinity" : "Infinity";
    }
    char buf[32];
    formatFloat(value, buf);
    return std::string(buf);
}

void appendString(std::string* out, const std::string& text) {
    out->push_back('"');
    size_t i = 0;
    while (i < text.size()) {
        const uint32_t cp = nextCodePoint(text, &i);
        switch (cp) {
            case '\b': out->append("\\b"); continue;
            case '\t': out->append("\\t"); continue;
            case '\n': out->append("\\n"); continue;
            case '\f': out->append("\\f"); continue;
            case '\r': out->append("\\r"); continue;
            case '\\': out->append("\\\\"); continue;
            case '"': case '&': case '\'': case '+': case '<': case '>': case '`':
                appendHex4(out, cp);
                continue;
            default:
                break;
        }
        if (cp >= 0x20 && cp < 0x7F) {
            out->push_back((char)cp);
        } else if (cp < 0x10000) {
            appendHex4(out, cp);
        } else {
            const uint32_t v = cp - 0x10000;
            appendHex4(out, 0xD800 + (v >> 10));
            appendHex4(out, 0xDC00 + (v & 0x3FF));
        }
    }
    out->push_back('"');
}

// ---------------------------------------------------------------------------
// Writer

void Writer::separate() {
    if (afterKey_) {
        afterKey_ = false;
        return;
    }
    if (depth_ > 0 && depth_ <= kMaxDepth) {
        if (hasMember_[depth_]) {
            out_->push_back(',');
        }
        hasMember_[depth_] = true;
    }
}

void Writer::beginObject() {
    separate();
    out_->push_back('{');
    depth_++;
    if (depth_ <= kMaxDepth) {
        hasMember_[depth_] = false;
    }
}

void Writer::endObject() {
    out_->push_back('}');
    depth_--;
}

void Writer::beginArray() {
    separate();
    out_->push_back('[');
    depth_++;
    if (depth_ <= kMaxDepth) {
        hasMember_[depth_] = false;
    }
}

void Writer::endArray() {
    out_->push_back(']');
    depth_--;
}

void Writer::key(const char* name) {
    key(std::string(name));
}

void Writer::key(const std::string& name) {
    separate();
    appendString(out_, name);
    out_->push_back(':');
    afterKey_ = true;
}

void Writer::valueInt(int32_t value) {
    separate();
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", (int)value);
    out_->append(buf);
}

void Writer::valueFloat(float value) {
    separate();
    if (!isfinite(value)) {
        appendString(out_, floatText(value));
        return;
    }
    char buf[32];
    formatFloat(value, buf);
    out_->append(buf);
}

void Writer::valueBool(bool value) {
    separate();
    out_->append(value ? "true" : "false");
}

void Writer::valueNull() {
    separate();
    out_->append("null");
}

void Writer::valueString(const std::string& value) {
    separate();
    appendString(out_, value);
}

void Writer::valueRaw(const std::string& text) {
    separate();
    out_->append(text);
}

// ---------------------------------------------------------------------------
// Document

int Document::add(Type type) {
    Node node;
    node.type = type;
    nodes_.push_back(node);
    return (int)nodes_.size() - 1;
}

void Document::skipSpace() {
    while (at_ < text_.size()) {
        const char c = text_[at_];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            return;
        }
        at_++;
    }
}

static int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool Document::parseString(std::string* out) {
    // at_ is on the opening quote.
    at_++;
    out->clear();
    while (at_ < text_.size()) {
        const unsigned char c = (unsigned char)text_[at_++];
        if (c == '"') {
            return true;
        }
        if (c < 0x20) {
            return false;
        }
        if (c != '\\') {
            out->push_back((char)c);
            continue;
        }
        if (at_ >= text_.size()) {
            return false;
        }
        const char e = text_[at_++];
        switch (e) {
            case '"': out->push_back('"'); break;
            case '\\': out->push_back('\\'); break;
            case '/': out->push_back('/'); break;
            case 'b': out->push_back('\b'); break;
            case 'f': out->push_back('\f'); break;
            case 'n': out->push_back('\n'); break;
            case 'r': out->push_back('\r'); break;
            case 't': out->push_back('\t'); break;
            case 'u': {
                uint32_t units[2] = {0, 0};
                int got = 0;
                for (int u = 0; u < 2; u++) {
                    if (u == 1) {
                        if (!(units[0] >= 0xD800 && units[0] < 0xDC00) || at_ + 6 > text_.size() ||
                            text_[at_] != '\\' || text_[at_ + 1] != 'u') {
                            break;
                        }
                        at_ += 2;
                    }
                    if (at_ + 4 > text_.size()) {
                        return false;
                    }
                    uint32_t unit = 0;
                    for (int k = 0; k < 4; k++) {
                        const int h = hexValue(text_[at_ + (size_t)k]);
                        if (h < 0) {
                            return false;
                        }
                        unit = (unit << 4) | (uint32_t)h;
                    }
                    at_ += 4;
                    units[got++] = unit;
                }
                if (got == 2 && units[1] >= 0xDC00 && units[1] < 0xE000) {
                    appendUtf8(out, 0x10000 + ((units[0] - 0xD800) << 10) + (units[1] - 0xDC00));
                } else {
                    // A lone surrogate becomes U+FFFD, as .NET's decoder makes it.
                    for (int k = 0; k < got; k++) {
                        const bool lone = units[k] >= 0xD800 && units[k] < 0xE000;
                        appendUtf8(out, lone ? 0xFFFD : units[k]);
                    }
                }
                break;
            }
            default:
                return false;
        }
    }
    return false;
}

int Document::parseValue(int depth) {
    if (depth > kMaxDepth) {
        return kNone;
    }
    skipSpace();
    if (at_ >= text_.size()) {
        return kNone;
    }
    const size_t begin = at_;
    const char c = text_[at_];
    int node = kNone;
    if ((c == '{' || c == '[') && depth >= kMaxDepth) {
        // `depth` counts the containers around this value: 64 nested are the most
        // JsonDocument reads (JsonReaderOptions.MaxDepth), the 65th refuses.
        return kNone;
    }
    if (c == '{' || c == '[') {
        const bool object = c == '{';
        node = add(object ? Object : Array);
        at_++;
        skipSpace();
        const char close = object ? '}' : ']';
        if (at_ < text_.size() && text_[at_] == close) {
            at_++;
        } else {
            for (;;) {
                int nameIndex = -1;
                if (object) {
                    skipSpace();
                    if (at_ >= text_.size() || text_[at_] != '"') {
                        return kNone;
                    }
                    std::string name;
                    if (!parseString(&name)) {
                        return kNone;
                    }
                    skipSpace();
                    if (at_ >= text_.size() || text_[at_] != ':') {
                        return kNone;
                    }
                    at_++;
                    names_.push_back(name);
                    nameIndex = (int)names_.size() - 1;
                }
                const int child = parseValue(depth + 1);
                if (child == kNone) {
                    return kNone;
                }
                nodes_[(size_t)child].nameIndex = nameIndex;
                Node& parent = nodes_[(size_t)node];
                if (parent.lastChild == kNone) {
                    parent.firstChild = child;
                } else {
                    nodes_[(size_t)parent.lastChild].nextSibling = child;
                }
                nodes_[(size_t)node].lastChild = child;
                nodes_[(size_t)node].children++;
                skipSpace();
                if (at_ >= text_.size()) {
                    return kNone;
                }
                if (text_[at_] == ',') {
                    at_++;
                    continue;
                }
                if (text_[at_] == close) {
                    at_++;
                    break;
                }
                return kNone;
            }
        }
    } else if (c == '"') {
        std::string value;
        if (!parseString(&value)) {
            return kNone;
        }
        node = add(String);
        nodes_[(size_t)node].text = value;
    } else if (text_.compare(at_, 4, "true") == 0) {
        node = add(True);
        at_ += 4;
    } else if (text_.compare(at_, 5, "false") == 0) {
        node = add(False);
        at_ += 5;
    } else if (text_.compare(at_, 4, "null") == 0) {
        node = add(Null);
        at_ += 4;
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        // -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
        size_t p = at_;
        if (text_[p] == '-') {
            p++;
        }
        if (p >= text_.size() || !(text_[p] >= '0' && text_[p] <= '9')) {
            return kNone;
        }
        if (text_[p] == '0') {
            p++;
        } else {
            while (p < text_.size() && text_[p] >= '0' && text_[p] <= '9') {
                p++;
            }
        }
        if (p < text_.size() && text_[p] == '.') {
            p++;
            const size_t digits = p;
            while (p < text_.size() && text_[p] >= '0' && text_[p] <= '9') {
                p++;
            }
            if (p == digits) {
                return kNone;
            }
        }
        if (p < text_.size() && (text_[p] == 'e' || text_[p] == 'E')) {
            p++;
            if (p < text_.size() && (text_[p] == '+' || text_[p] == '-')) {
                p++;
            }
            const size_t digits = p;
            while (p < text_.size() && text_[p] >= '0' && text_[p] <= '9') {
                p++;
            }
            if (p == digits) {
                return kNone;
            }
        }
        node = add(Number);
        nodes_[(size_t)node].text = text_.substr(at_, p - at_);
        at_ = p;
    } else {
        return kNone;
    }
    nodes_[(size_t)node].begin = begin;
    nodes_[(size_t)node].end = at_;
    return node;
}

bool Document::parse(const std::string& text) {
    text_ = text;
    at_ = 0;
    nodes_.clear();
    names_.clear();
    // The root is node 0: parseValue adds it first.
    const int root = parseValue(0);
    skipSpace();
    if (root != 0 || at_ != text_.size()) {
        nodes_.clear();
        names_.clear();
        return false;
    }
    return true;
}

Document::Type Document::type(int node) const {
    if (node < 0 || node >= (int)nodes_.size()) {
        return Null;
    }
    return nodes_[(size_t)node].type;
}

int Document::member(int node, const char* name) const {
    if (type(node) != Object) {
        return kNone;
    }
    // The LAST member of that name wins, as JsonNode.Parse keeps it (a JsonObject
    // throws on a duplicate; JsonDocument's TryGetProperty finds the last).
    int found = kNone;
    for (int child = nodes_[(size_t)node].firstChild; child != kNone;
         child = nodes_[(size_t)child].nextSibling) {
        if (names_[(size_t)nodes_[(size_t)child].nameIndex] == name) {
            found = child;
        }
    }
    return found;
}

int Document::first(int node) const {
    const Type t = type(node);
    return t == Object || t == Array ? nodes_[(size_t)node].firstChild : kNone;
}

int Document::next(int node) const {
    if (node < 0 || node >= (int)nodes_.size()) {
        return kNone;
    }
    return nodes_[(size_t)node].nextSibling;
}

const std::string& Document::name(int node) const {
    static const std::string kEmpty;
    if (node < 0 || node >= (int)nodes_.size() || nodes_[(size_t)node].nameIndex < 0) {
        return kEmpty;
    }
    return names_[(size_t)nodes_[(size_t)node].nameIndex];
}

int Document::count(int node) const {
    const Type t = type(node);
    return t == Object || t == Array ? nodes_[(size_t)node].children : 0;
}

int Document::at(int node, int index) const {
    if (type(node) != Array || index < 0) {
        return kNone;
    }
    int child = nodes_[(size_t)node].firstChild;
    for (int k = 0; k < index && child != kNone; k++) {
        child = nodes_[(size_t)child].nextSibling;
    }
    return child;
}

bool Document::asInt(int node, int32_t* out) const {
    if (type(node) != Number) {
        return false;
    }
    const std::string& token = nodes_[(size_t)node].text;
    if (token.find_first_of(".eE") != std::string::npos) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const long long value = strtoll(token.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0' || value < INT32_MIN || value > INT32_MAX) {
        return false;
    }
    *out = (int32_t)value;
    return true;
}

bool Document::asFloat(int node, float* out) const {
    if (type(node) != Number) {
        return false;
    }
    *out = strtof(nodes_[(size_t)node].text.c_str(), nullptr);
    return true;
}

bool Document::asDouble(int node, double* out) const {
    if (type(node) != Number) {
        return false;
    }
    *out = strtod(nodes_[(size_t)node].text.c_str(), nullptr);
    return true;
}

bool Document::asBool(int node, bool* out) const {
    const Type t = type(node);
    if (t != True && t != False) {
        return false;
    }
    *out = t == True;
    return true;
}

bool Document::asString(int node, std::string* out) const {
    if (type(node) != String) {
        return false;
    }
    *out = nodes_[(size_t)node].text;
    return true;
}

std::string Document::raw(int node) const {
    if (node < 0 || node >= (int)nodes_.size()) {
        return std::string();
    }
    const Node& n = nodes_[(size_t)node];
    return text_.substr(n.begin, n.end - n.begin);
}

int32_t Document::intOr(int node, int32_t fallback) const {
    int32_t value = 0;
    return asInt(node, &value) ? value : fallback;
}

float Document::floatOr(int node, float fallback) const {
    float value = 0.0f;
    return asFloat(node, &value) ? value : fallback;
}

std::string Document::stringOr(int node, const std::string& fallback) const {
    std::string value;
    return asString(node, &value) ? value : fallback;
}

} // namespace json
} // namespace mdk

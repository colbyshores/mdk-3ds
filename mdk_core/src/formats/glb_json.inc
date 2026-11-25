// glb_json.inc -- the JSON chunk's value, parser and writer.
// Part of glb.cpp's single translation unit (see its header): pasted inside
// its anonymous namespace after the file-local constants, which it uses.
// No include guards, no includes of its own; the order is load-bearing.

// === JSON =========================================================================

// A JSON value.
class JsonValue {
public:
    enum class Kind {
        Undefined,
        Null,
        Boolean,
        Number,
        String,
        Array,
        Object,
    };

    Kind kind = Kind::Undefined;
    bool boolean = false;
    // A string's value, or a number's source text.
    std::string text;
    // An array's elements.
    std::vector<JsonValue> items;
    // An object's members, in the order they were written.
    std::vector<std::pair<std::string, JsonValue>> members;

    // The member of an object, or null.
    const JsonValue* member(const char* name) const {
        if (kind != Kind::Object) {
            return nullptr;
        }

        for (size_t i = 0; i < members.size(); i++) {
            if (members[i].first == name) {
                return &members[i].second;
            }
        }

        return nullptr;
    }

    // The element of an array, or null.
    const JsonValue* element(int32_t index) const {
        if (kind != Kind::Array || index < 0 || index >= (int32_t)items.size()) {
            return nullptr;
        }

        return &items[(size_t)index];
    }

    int32_t count() const { return kind == Kind::Array ? (int32_t)items.size() : 0; }

    bool isNumber() const { return kind == Kind::Number; }
    bool isString() const { return kind == Kind::String; }
    bool isTrue() const { return kind == Kind::Boolean && boolean; }

    // The number as an int, the way the C#'s GetInt32 reads it: the source
    // text must be a plain integer that fits.
    bool asInt(int32_t* out) const {
        if (kind != Kind::Number) {
            return false;
        }

        size_t i = 0;
        if (i < text.size() && (text[i] == '-' || text[i] == '+')) {
            i++;
        }

        if (i >= text.size()) {
            return false;
        }

        for (size_t k = i; k < text.size(); k++) {
            if (text[k] < '0' || text[k] > '9') {
                return false;
            }
        }

        long long value = 0;
        for (size_t k = 0; k < text.size(); k++) {
            if (value > 2147483647LL) {
                return false;
            }

            value = value * 10 + (text[k] - '0');
        }

        if (text[0] == '-') {
            value = -value;
        }

        if (value < -2147483648LL || value > 2147483647LL) {
            return false;
        }

        *out = (int32_t)value;
        return true;
    }

    // The number as a float, the way the C#'s GetSingle reads it.
    bool asFloat(float* out) const {
        if (kind != Kind::Number) {
            return false;
        }

        const char* start = text.c_str();
        char* end = nullptr;
        const float value = strtof(start, &end);
        if (end != start + text.size()) {
            return false;
        }

        *out = value;
        return true;
    }
};

// The JSON parser.
class JsonParser {
public:
    JsonParser(const char* text, int32_t length) : text_(text), length_(length) {}

    bool parse(JsonValue* out) {
        skipWhitespace();
        if (!parseValue(out)) {
            return false;
        }

        skipWhitespace();
        return pos_ == length_;
    }

private:
    void skipWhitespace() {
        while (pos_ < length_) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                pos_++;
            } else {
                break;
            }
        }
    }

    bool literal(const char* word) {
        const int32_t n = (int32_t)strlen(word);
        if (pos_ + n > length_ || memcmp(text_ + pos_, word, (size_t)n) != 0) {
            return false;
        }

        pos_ += n;
        return true;
    }

    bool parseValue(JsonValue* out) {
        if (pos_ >= length_) {
            return false;
        }

        switch (text_[pos_]) {
        case '{':
            return parseObject(out);
        case '[':
            return parseArray(out);
        case '"': {
            out->kind = JsonValue::Kind::String;
            return parseString(&out->text);
        }
        case 't':
            if (!literal("true")) {
                return false;
            }

            out->kind = JsonValue::Kind::Boolean;
            out->boolean = true;
            return true;
        case 'f':
            if (!literal("false")) {
                return false;
            }

            out->kind = JsonValue::Kind::Boolean;
            out->boolean = false;
            return true;
        case 'n':
            if (!literal("null")) {
                return false;
            }

            out->kind = JsonValue::Kind::Null;
            return true;
        default:
            return parseNumber(out);
        }
    }

    bool parseObject(JsonValue* out) {
        out->kind = JsonValue::Kind::Object;
        pos_++; // '{'
        skipWhitespace();
        if (pos_ < length_ && text_[pos_] == '}') {
            pos_++;
            return true;
        }

        while (true) {
            skipWhitespace();
            if (pos_ >= length_ || text_[pos_] != '"') {
                return false;
            }

            std::string name;
            if (!parseString(&name)) {
                return false;
            }

            skipWhitespace();
            if (pos_ >= length_ || text_[pos_] != ':') {
                return false;
            }

            pos_++;
            skipWhitespace();
            JsonValue value;
            if (!parseValue(&value)) {
                return false;
            }

            out->members.push_back(std::make_pair(name, value));
            skipWhitespace();
            if (pos_ >= length_) {
                return false;
            }

            if (text_[pos_] == ',') {
                pos_++;
                continue;
            }

            if (text_[pos_] == '}') {
                pos_++;
                return true;
            }

            return false;
        }
    }

    bool parseArray(JsonValue* out) {
        out->kind = JsonValue::Kind::Array;
        pos_++; // '['
        skipWhitespace();
        if (pos_ < length_ && text_[pos_] == ']') {
            pos_++;
            return true;
        }

        while (true) {
            skipWhitespace();
            JsonValue value;
            if (!parseValue(&value)) {
                return false;
            }

            out->items.push_back(value);
            skipWhitespace();
            if (pos_ >= length_) {
                return false;
            }

            if (text_[pos_] == ',') {
                pos_++;
                continue;
            }

            if (text_[pos_] == ']') {
                pos_++;
                return true;
            }

            return false;
        }
    }

    // A string, with the escapes decoded into UTF-8.
    bool parseString(std::string* out) {
        pos_++; // '"'
        out->clear();
        while (pos_ < length_) {
            const unsigned char c = (unsigned char)text_[pos_];
            if (c == '"') {
                pos_++;
                return true;
            }

            if (c == '\\') {
                pos_++;
                if (pos_ >= length_) {
                    return false;
                }

                const char escape = text_[pos_++];
                switch (escape) {
                case '"':
                    out->push_back('"');
                    break;
                case '\\':
                    out->push_back('\\');
                    break;
                case '/':
                    out->push_back('/');
                    break;
                case 'b':
                    out->push_back('\b');
                    break;
                case 'f':
                    out->push_back('\f');
                    break;
                case 'n':
                    out->push_back('\n');
                    break;
                case 'r':
                    out->push_back('\r');
                    break;
                case 't':
                    out->push_back('\t');
                    break;
                case 'u': {
                    uint32_t code = 0;
                    if (!parseHex4(&code)) {
                        return false;
                    }

                    // A high surrogate is followed by a low one.
                    if (code >= 0xD800 && code <= 0xDBFF && pos_ + 1 < length_ && text_[pos_] == '\\' &&
                        text_[pos_ + 1] == 'u') {
                        pos_ += 2;
                        uint32_t low = 0;
                        if (!parseHex4(&low)) {
                            return false;
                        }

                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    }

                    appendUtf8(code, out);
                    break;
                }
                default:
                    return false;
                }

                continue;
            }

            if (c < 0x20) {
                return false;
            }

            out->push_back((char)c);
            pos_++;
        }

        return false;
    }

    bool parseHex4(uint32_t* out) {
        if (pos_ + 4 > length_) {
            return false;
        }

        uint32_t value = 0;
        for (int32_t i = 0; i < 4; i++) {
            const char c = text_[pos_ + i];
            value <<= 4;
            if (c >= '0' && c <= '9') {
                value |= (uint32_t)(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= (uint32_t)(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= (uint32_t)(c - 'A' + 10);
            } else {
                return false;
            }
        }

        pos_ += 4;
        *out = value;
        return true;
    }

    static void appendUtf8(uint32_t code, std::string* out) {
        if (code < 0x80) {
            out->push_back((char)code);
        } else if (code < 0x800) {
            out->push_back((char)(0xC0 | (code >> 6)));
            out->push_back((char)(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out->push_back((char)(0xE0 | (code >> 12)));
            out->push_back((char)(0x80 | ((code >> 6) & 0x3F)));
            out->push_back((char)(0x80 | (code & 0x3F)));
        } else {
            out->push_back((char)(0xF0 | (code >> 18)));
            out->push_back((char)(0x80 | ((code >> 12) & 0x3F)));
            out->push_back((char)(0x80 | ((code >> 6) & 0x3F)));
            out->push_back((char)(0x80 | (code & 0x3F)));
        }
    }

    // A number. The source text is kept, because the C# reads a number from
    // its text and not from a parsed double.
    bool parseNumber(JsonValue* out) {
        const int32_t start = pos_;
        if (pos_ < length_ && text_[pos_] == '-') {
            pos_++;
        }

        if (pos_ >= length_ || text_[pos_] < '0' || text_[pos_] > '9') {
            return false;
        }

        while (pos_ < length_ && text_[pos_] >= '0' && text_[pos_] <= '9') {
            pos_++;
        }

        if (pos_ < length_ && text_[pos_] == '.') {
            pos_++;
            if (pos_ >= length_ || text_[pos_] < '0' || text_[pos_] > '9') {
                return false;
            }

            while (pos_ < length_ && text_[pos_] >= '0' && text_[pos_] <= '9') {
                pos_++;
            }
        }

        if (pos_ < length_ && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            pos_++;
            if (pos_ < length_ && (text_[pos_] == '+' || text_[pos_] == '-')) {
                pos_++;
            }

            if (pos_ >= length_ || text_[pos_] < '0' || text_[pos_] > '9') {
                return false;
            }

            while (pos_ < length_ && text_[pos_] >= '0' && text_[pos_] <= '9') {
                pos_++;
            }
        }

        out->kind = JsonValue::Kind::Number;
        out->text.assign(text_ + start, (size_t)(pos_ - start));
        return true;
    }

    const char* text_;
    int32_t length_;
    int32_t pos_ = 0;
};

// The JSON writer: compact, in the order the C#'s Utf8JsonWriter writes.
class JsonWriter {
public:
    void startObject() { comma(); out_ += '{'; first_ = true; }
    void endObject() { out_ += '}'; first_ = false; }
    void startArray() { comma(); out_ += '['; first_ = true; }
    void endArray() { out_ += ']'; first_ = false; }
    void startObject(const char* key) { comma(); writeName(key); out_ += '{'; first_ = true; }
    void startArray(const char* key) { comma(); writeName(key); out_ += '['; first_ = true; }

    void number(const char* key, int32_t value) {
        comma();
        writeName(key);
        out_ += std::to_string(value);
        first_ = false;
    }

    void numberValue(int32_t value) {
        comma();
        out_ += std::to_string(value);
        first_ = false;
    }

    void string(const char* key, const std::string& value) {
        comma();
        writeName(key);
        quote(value);
        first_ = false;
    }

    void boolean(const char* key, bool value) {
        comma();
        writeName(key);
        out_ += value ? "true" : "false";
        first_ = false;
    }

    void numberValue(float value) {
        comma();
        char buffer[32];
        const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), value);
        out_.append(buffer, (size_t)(result.ptr - buffer));
        first_ = false;
    }

    const std::string& text() const { return out_; }

    // A piece of JSON written by hand, put into an array.
    void appendRaw(const std::string& value) {
        out_ += value;
        first_ = false;
    }

private:
    void comma() {
        if (!first_) {
            out_ += ',';
        }
    }

    void writeName(const char* key) {
        quote(key);
        out_ += ':';
    }

    void quote(const std::string& value) {
        out_ += '"';
        for (size_t i = 0; i < value.size(); i++) {
            const char c = value[i];
            switch (c) {
            case '"':
                out_ += "\\\"";
                break;
            case '\\':
                out_ += "\\\\";
                break;
            case '\b':
                out_ += "\\b";
                break;
            case '\f':
                out_ += "\\f";
                break;
            case '\n':
                out_ += "\\n";
                break;
            case '\r':
                out_ += "\\r";
                break;
            case '\t':
                out_ += "\\t";
                break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buffer[8];
                    snprintf(buffer, sizeof(buffer), "\\u%04x", (unsigned)(unsigned char)c);
                    out_ += buffer;
                } else {
                    out_ += c;
                }

                break;
            }
        }

        out_ += '"';
    }

    std::string out_;
    bool first_ = true;
};

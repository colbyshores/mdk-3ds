// ============================================================================
// script_decoder.cpp — see script_decoder.h.
// ============================================================================

#include "formats/script_decoder.h"

#include "formats/bin_reader.h"
#include "formats/script_opcodes.h"

namespace mdk {
namespace formats {

namespace {

// The action byte of a gosub to one of two targets.
const uint8_t kActionGosubElse = 0xFE;

// The action byte of a gosub.
const uint8_t kActionGosub = 0xFC;

// The action byte of a goto.
const uint8_t kActionGoto = 0x0C;

// The action byte of a return.
const uint8_t kActionReturn = 0xFD;

// The kind of a value operand that is a float literal rather than a
// variable.
const int32_t kLiteralKind = 3;

// The command that makes the operands an action.
const int32_t kCommandGoto = 7;

// The command that makes the operands two floats.
const int32_t kCommandMove = 43;

// The comparisons that take two bounds.
const int32_t kCondBetween = 7;
const int32_t kCondOutside = 8;

// The mode counts of the layouts that ask for a mode byte and then floats.
int32_t modeCount159(int32_t mode) { return (mode == 1 || mode == 2) ? 3 : 0; }
int32_t modeCount189(int32_t mode) { return mode == 0 ? 2 : (mode == 1 ? 1 : 0); }
int32_t modeCount180(int32_t mode) { return mode != 0 ? 3 : 0; }
int32_t modeCount203(int32_t mode) { return mode == 1 ? 1 : 0; }
int32_t modeCount224(int32_t mode) { return mode != 0 ? 2 : 0; }
int32_t modeCount242(int32_t mode) { return mode != 0 ? 12 : 0; }

// An earlier operand as an int, the way the C#'s `(int)prev[n]!` casts it.
bool prevInt(const std::vector<ScriptValue>& prev, size_t index, int32_t* out) {
    if (index >= prev.size() || prev[index].kind != ScriptValueKind::Int) {
        return false;
    }
    *out = prev[index].intValue;
    return true;
}

} // namespace

ScriptDecoder::ScriptDecoder(const uint8_t* bytes, int32_t byteCount)
    : bytes_(bytes), byteCount_(byteCount) {}

ScriptDecoder::ScriptDecoder(const uint8_t* bytes, int32_t byteCount, ScriptDialect dialect,
                             const std::vector<std::pair<int32_t, int32_t>>* paths)
    : bytes_(bytes), byteCount_(byteCount), dialect_(dialect) {
    if (paths) {
        for (size_t i = 0; i < paths->size(); i++) {
            paths_[(*paths)[i].first] = (*paths)[i].second;
        }
    }
}

// --- the primitive reads ---------------------------------------------------

bool ScriptDecoder::readU8(uint8_t* out) {
    if (!canRead(1)) {
        return false;
    }
    *out = bytes_[p_++];
    return true;
}

bool ScriptDecoder::readF32(float* out) {
    if (!canRead(4)) {
        return false;
    }
    *out = binF32(bytes_, p_);
    p_ += 4;
    return true;
}

bool ScriptDecoder::readU32(uint32_t* out) {
    if (!canRead(4)) {
        return false;
    }
    *out = binU32(bytes_, p_);
    p_ += 4;
    return true;
}

bool ScriptDecoder::readS32(int32_t* out) {
    if (!canRead(4)) {
        return false;
    }
    *out = binS32(bytes_, p_);
    p_ += 4;
    return true;
}

bool ScriptDecoder::readOffset(int32_t* out) {
    uint32_t value = 0;
    if (!readU32(&value)) {
        return false;
    }
    // The offsets are from 4, and 0 stays 0: it means "none".
    *out = value == 0 ? 0 : (int32_t)value + kFileBase;
    return true;
}

bool ScriptDecoder::readPascalString(std::string* out) {
    uint8_t length = 0;
    if (!readU8(&length)) {
        return false;
    }
    if (!canRead((int32_t)length)) {
        return false;
    }
    // The C#'s Bin.Ascii stops at the first NUL.
    std::string text;
    for (int32_t i = 0; i < (int32_t)length; i++) {
        if (bytes_[p_ + i] == 0) {
            break;
        }
        text.push_back((char)bytes_[p_ + i]);
    }
    p_ += length;
    *out = text;
    return true;
}

bool ScriptDecoder::readFloats(int32_t count, std::vector<ScriptValue>* out) {
    out->clear();
    if (count < 0) {
        return false;
    }
    for (int32_t i = 0; i < count; i++) {
        float value = 0.0f;
        if (!readF32(&value)) {
            return false;
        }
        out->push_back(scriptFloat(value));
    }
    return true;
}

bool ScriptDecoder::readAction(BranchAction* out) {
    uint8_t action = 0;
    if (!readU8(&action)) {
        return false;
    }
    BranchAction result;
    switch (action) {
        case kActionGosubElse: {
            int32_t target = 0;
            int32_t elseTarget = 0;
            if (!readOffset(&target)) {
                return false;
            }
            if (!readOffset(&elseTarget)) {
                return false;
            }
            result.type = BranchAction::Kind::GosubElse;
            result.target = target;
            result.elseTarget = elseTarget;
            break;
        }
        case kActionGosub: {
            int32_t target = 0;
            if (!readOffset(&target)) {
                return false;
            }
            result.type = BranchAction::Kind::Gosub;
            result.target = target;
            break;
        }
        case kActionGoto: {
            int32_t target = 0;
            if (!readOffset(&target)) {
                return false;
            }
            result.type = BranchAction::Kind::Goto;
            result.target = target;
            break;
        }
        case kActionReturn:
            result.type = BranchAction::Kind::Return;
            break;
        default:
            // An action the decoder does not know. It is still an action,
            // and the C# keeps its byte.
            result.type = BranchAction::Kind::None;
            result.raw = (int32_t)action;
            break;
    }
    action_ = result;
    hasAction_ = true;
    *out = result;
    return true;
}

bool ScriptDecoder::readValue(ScriptValue* out) {
    uint8_t kind = 0;
    if (!readU8(&kind)) {
        return false;
    }
    if (kind == kLiteralKind) {
        float value = 0.0f;
        if (!readF32(&value)) {
            return false;
        }
        *out = scriptFloat(value);
        return true;
    }
    uint8_t index = 0;
    if (!readU8(&index)) {
        return false;
    }
    *out = scriptVariable((int32_t)kind, (int32_t)index);
    return true;
}

// --- the layouts that depend on the earlier operands ------------------------

bool ScriptDecoder::readCommandArgs(const std::vector<ScriptValue>& prev, ScriptValue* out) {
    int32_t command = 0;
    if (!prevInt(prev, 0, &command)) {
        return false;
    }
    if (command == kCommandGoto) {
        BranchAction action;
        if (!readAction(&action)) {
            return false;
        }
        *out = scriptAction(action);
        return true;
    }
    if (command == kCommandMove) {
        std::vector<ScriptValue> floats;
        if (!readFloats(2, &floats)) {
            return false;
        }
        *out = scriptList(floats);
        return true;
    }
    *out = scriptNull();
    return true;
}

bool ScriptDecoder::readSelectorArgs(int32_t selector, ScriptValue* out) {
    std::vector<ScriptValue> args;
    if (selector == 6 || selector == 10) {
        float value = 0.0f;
        if (!readF32(&value)) {
            return false;
        }
        args.push_back(scriptFloat(value));
    }
    if (selector == 2 || selector == 4 || selector == 5 || selector == 6 || selector == 7 || selector == 10) {
        std::string name;
        if (!readPascalString(&name)) {
            return false;
        }
        args.push_back(scriptString(name));
    }
    if (selector == 5) {
        uint32_t value = 0;
        if (!readU32(&value)) {
            return false;
        }
        args.push_back(scriptInt((int32_t)value));
    }
    *out = scriptList(args);
    return true;
}

bool ScriptDecoder::readOrigin(const std::vector<ScriptValue>& prev, ScriptValue* out) {
    int32_t value = 0;
    if (!prevInt(prev, 4, &value)) {
        return false;
    }
    if (value != 0) {
        *out = scriptNull();
        return true;
    }
    std::vector<ScriptValue> floats;
    if (!readFloats(3, &floats)) {
        return false;
    }
    *out = scriptList(floats);
    return true;
}

bool ScriptDecoder::readParams(const std::vector<ScriptValue>& prev, ScriptValue* out) {
    int32_t value = 0;
    if (!prevInt(prev, 0, &value)) {
        return false;
    }
    if (value == 0) {
        *out = scriptNull();
        return true;
    }
    std::vector<ScriptValue> floats;
    if (!readFloats(4, &floats)) {
        return false;
    }
    *out = scriptList(floats);
    return true;
}

bool ScriptDecoder::readPartName(ScriptValue* out) {
    std::string part;
    if (!readPascalString(&part)) {
        return false;
    }
    if (!part.empty()) {
        *out = scriptString(part);
        return true;
    }
    // The name is empty, so the real name follows.
    std::string name;
    if (!readPascalString(&name)) {
        return false;
    }
    std::vector<ScriptValue> items;
    items.push_back(scriptString(std::string()));
    items.push_back(scriptString(name));
    *out = scriptList(items);
    return true;
}

bool ScriptDecoder::readSound(const std::vector<ScriptValue>& prev, ScriptValue* out) {
    int32_t value = 0;
    if (!prevInt(prev, 0, &value)) {
        return false;
    }
    if (value != 1) {
        *out = scriptNull();
        return true;
    }
    std::string name;
    if (!readPascalString(&name)) {
        return false;
    }
    *out = scriptString(name);
    return true;
}

bool ScriptDecoder::readTypeName(const std::vector<ScriptValue>& prev, ScriptValue* out) {
    int32_t value = 0;
    if (!prevInt(prev, 0, &value)) {
        return false;
    }
    if (value != 0xFF) {
        *out = scriptNull();
        return true;
    }
    std::string name;
    if (!readPascalString(&name)) {
        return false;
    }
    *out = scriptString(name);
    return true;
}

bool ScriptDecoder::readBox(const std::vector<ScriptValue>& prev, ScriptValue* out) {
    int32_t value = 0;
    if (!prevInt(prev, 2, &value)) {
        return false;
    }
    std::vector<ScriptValue> floats;
    if (!readFloats(value == 2 ? 4 : 6, &floats)) {
        return false;
    }
    *out = scriptList(floats);
    return true;
}

bool ScriptDecoder::readParts(const std::vector<ScriptValue>& prev, ScriptValue* out) {
    int32_t value = 0;
    if (!prevInt(prev, 0, &value)) {
        return false;
    }
    if (value != 0 && value != 1 && value != 2) {
        *out = scriptNull();
        return true;
    }
    // The count is only read when the mode asks for the parts.
    uint8_t count = 0;
    if (!readU8(&count)) {
        return false;
    }
    std::vector<ScriptValue> items;
    for (int32_t i = 0; i < (int32_t)count; i++) {
        std::string name;
        if (!readPascalString(&name)) {
            return false;
        }
        items.push_back(scriptString(name));
    }
    *out = scriptList(items);
    return true;
}

bool ScriptDecoder::readPosition(const std::vector<ScriptValue>& prev, ScriptValue* out) {
    int32_t value = 0;
    if (!prevInt(prev, 1, &value)) {
        return false;
    }
    if (value != 0xFF) {
        *out = scriptNull();
        return true;
    }
    std::vector<ScriptValue> floats;
    if (!readFloats(3, &floats)) {
        return false;
    }
    *out = scriptList(floats);
    return true;
}

bool ScriptDecoder::readHitTarget(const std::vector<ScriptValue>& prev, ScriptValue* out) {
    int32_t value = 0;
    if (!prevInt(prev, 2, &value)) {
        return false;
    }
    if ((value & 2) == 0) {
        *out = scriptNull();
        return true;
    }
    int32_t target = 0;
    if (!readOffset(&target)) {
        return false;
    }
    *out = scriptInt(target);
    return true;
}

bool ScriptDecoder::readOp89Position(int32_t flags, ScriptValue* out) {
    const int32_t kPoint = 0x10;
    const int32_t kIndex = 0x20;
    const int32_t kOffset = 0x40;
    if ((flags & kPoint) != 0) {
        std::vector<ScriptValue> floats;
        if (!readFloats(3, &floats)) {
            return false;
        }
        *out = scriptList(floats);
        return true;
    }
    if ((flags & kIndex) != 0) {
        uint8_t index = 0;
        if (!readU8(&index)) {
            return false;
        }
        *out = scriptInt((int32_t)index);
        return true;
    }
    if ((flags & kOffset) != 0) {
        std::vector<ScriptValue> floats;
        if (!readFloats(3, &floats)) {
            return false;
        }
        *out = scriptList(floats);
        return true;
    }
    *out = scriptNull();
    return true;
}

bool ScriptDecoder::readOp61Origin(ScriptValue* out) {
    uint8_t mode = 0;
    if (!readU8(&mode)) {
        return false;
    }
    std::vector<ScriptValue> items;
    items.push_back(scriptInt((int32_t)mode));
    if (mode == 0) {
        uint8_t value = 0;
        if (!readU8(&value)) {
            return false;
        }
        items.push_back(scriptInt((int32_t)value));
    } else {
        std::string name;
        if (!readPascalString(&name)) {
            return false;
        }
        items.push_back(scriptString(name));
    }
    *out = scriptList(items);
    return true;
}

bool ScriptDecoder::readOp83Args(ScriptValue* out) {
    if (!canRead(1)) {
        return false;
    }
    if (bytes_[p_] != 0xFF) {
        return readValue(out);
    }
    p_++;
    std::vector<ScriptValue> floats;
    if (!readFloats(2, &floats)) {
        return false;
    }
    *out = scriptList(floats);
    return true;
}

bool ScriptDecoder::modeThenFloats(int32_t (*count)(int32_t), ScriptValue* out) {
    uint8_t mode = 0;
    if (!readU8(&mode)) {
        return false;
    }
    std::vector<ScriptValue> items;
    items.push_back(scriptInt((int32_t)mode));
    std::vector<ScriptValue> floats;
    if (!readFloats(count((int32_t)mode), &floats)) {
        return false;
    }
    items.insert(items.end(), floats.begin(), floats.end());
    *out = scriptList(items);
    return true;
}

bool ScriptDecoder::readOp193Mode(ScriptValue* out) {
    uint8_t mode = 0;
    if (!readU8(&mode)) {
        return false;
    }
    std::vector<ScriptValue> items;
    items.push_back(scriptInt((int32_t)mode));
    if (mode == 3) {
        std::string name;
        if (!readPascalString(&name)) {
            return false;
        }
        items.push_back(scriptString(name));
    }
    *out = scriptList(items);
    return true;
}

bool ScriptDecoder::readOp245Args(ScriptValue* out) {
    if (!canRead(1)) {
        return false;
    }
    if (bytes_[p_] != 0) {
        std::string name;
        if (!readPascalString(&name)) {
            return false;
        }
        std::vector<ScriptValue> items;
        items.push_back(scriptString(name));
        *out = scriptList(items);
        return true;
    }
    p_++;
    uint8_t value = 0;
    if (!readU8(&value)) {
        return false;
    }
    std::string name;
    if (!readPascalString(&name)) {
        return false;
    }
    std::vector<ScriptValue> items;
    items.push_back(scriptInt((int32_t)value));
    items.push_back(scriptString(name));
    *out = scriptList(items);
    return true;
}

bool ScriptDecoder::readOp248Args(ScriptValue* out) {
    uint8_t mode = 0;
    if (!readU8(&mode)) {
        return false;
    }
    float x = 0.0f;
    if (!readF32(&x)) {
        return false;
    }
    std::vector<ScriptValue> items;
    items.push_back(scriptInt((int32_t)mode));
    items.push_back(scriptFloat(x));
    std::vector<ScriptValue> floats;
    if (!readFloats(mode == 0 ? 2 : 1, &floats)) {
        return false;
    }
    items.insert(items.end(), floats.begin(), floats.end());
    *out = scriptList(items);
    return true;
}

bool ScriptDecoder::readOp172Position(ScriptValue* out) {
    uint8_t mode = 0;
    if (!readU8(&mode)) {
        return false;
    }
    std::vector<ScriptValue> items;
    items.push_back(scriptInt((int32_t)mode));
    if (mode == 3) {
        uint8_t value = 0;
        if (!readU8(&value)) {
            return false;
        }
        items.push_back(scriptInt((int32_t)value));
        *out = scriptList(items);
        return true;
    }
    std::vector<ScriptValue> floats;
    if (!readFloats(3, &floats)) {
        return false;
    }
    items.insert(items.end(), floats.begin(), floats.end());
    *out = scriptList(items);
    return true;
}

bool ScriptDecoder::readOp173Target(ScriptValue* out) {
    std::string name;
    if (!readPascalString(&name)) {
        return false;
    }
    std::vector<ScriptValue> items;
    if (!name.empty()) {
        items.push_back(scriptString(name));
        *out = scriptList(items);
        return true;
    }
    items.push_back(scriptString(std::string()));
    std::string other;
    if (!readPascalString(&other)) {
        return false;
    }
    items.push_back(scriptString(other));
    std::vector<ScriptValue> floats;
    if (!readFloats(4, &floats)) {
        return false;
    }
    items.insert(items.end(), floats.begin(), floats.end());
    *out = scriptList(items);
    return true;
}

bool ScriptDecoder::readOp181Source(ScriptValue* out) {
    uint8_t mode = 0;
    if (!readU8(&mode)) {
        return false;
    }
    std::vector<ScriptValue> items;
    items.push_back(scriptInt((int32_t)mode));
    if (mode == 0) {
        uint8_t value = 0;
        if (!readU8(&value)) {
            return false;
        }
        items.push_back(scriptInt((int32_t)value));
        uint32_t id = 0;
        if (!readU32(&id)) {
            return false;
        }
        items.push_back(scriptInt((int32_t)id));
        *out = scriptList(items);
        return true;
    }
    if (mode == 1) {
        uint8_t value = 0;
        if (!readU8(&value)) {
            return false;
        }
        items.push_back(scriptInt((int32_t)value));
        std::vector<ScriptValue> floats;
        if (!readFloats(2, &floats)) {
            return false;
        }
        items.insert(items.end(), floats.begin(), floats.end());
        uint32_t id = 0;
        if (!readU32(&id)) {
            return false;
        }
        items.push_back(scriptInt((int32_t)id));
        *out = scriptList(items);
        return true;
    }
    *out = scriptList(items);
    return true;
}

bool ScriptDecoder::readOp228TypeName(ScriptValue* out) {
    if (!canRead(1)) {
        return false;
    }
    if (bytes_[p_] != 0) {
        std::string name;
        if (!readPascalString(&name)) {
            return false;
        }
        *out = scriptString(name);
        return true;
    }
    p_++;
    uint8_t value = 0;
    if (!readU8(&value)) {
        return false;
    }
    std::vector<ScriptValue> items;
    items.push_back(scriptString(std::string()));
    items.push_back(scriptInt((int32_t)value));
    *out = scriptList(items);
    return true;
}

// --- the repeats -----------------------------------------------------------

bool ScriptDecoder::readRepeatCode32(const std::vector<ScriptValue>& prev, ScriptValue* out) {
    uint8_t count = 0;
    if (!readU8(&count)) {
        return false;
    }
    std::vector<ScriptValue> items;
    for (int32_t i = 0; i < (int32_t)count; i++) {
        ScriptValue value;
        if (!read(OperandCode::Code32, prev, &value)) {
            return false;
        }
        std::vector<ScriptValue> item;
        item.push_back(value);
        items.push_back(scriptList(item));
    }
    *out = scriptList(items);
    return true;
}

bool ScriptDecoder::readRepeatPstr(const std::vector<ScriptValue>& prev, ScriptValue* out) {
    uint8_t count = 0;
    if (!readU8(&count)) {
        return false;
    }
    std::vector<ScriptValue> items;
    for (int32_t i = 0; i < (int32_t)count; i++) {
        ScriptValue value;
        if (!read(OperandCode::Pstr, prev, &value)) {
            return false;
        }
        std::vector<ScriptValue> item;
        item.push_back(value);
        items.push_back(scriptList(item));
    }
    *out = scriptList(items);
    return true;
}

bool ScriptDecoder::readRepeatU8Code32(const std::vector<ScriptValue>& prev, ScriptValue* out) {
    uint8_t count = 0;
    if (!readU8(&count)) {
        return false;
    }
    std::vector<ScriptValue> items;
    for (int32_t i = 0; i < (int32_t)count; i++) {
        ScriptValue first;
        ScriptValue second;
        if (!read(OperandCode::U8, prev, &first)) {
            return false;
        }
        if (!read(OperandCode::Code32, prev, &second)) {
            return false;
        }
        std::vector<ScriptValue> item;
        item.push_back(first);
        item.push_back(second);
        items.push_back(scriptList(item));
    }
    *out = scriptList(items);
    return true;
}

// --- the operand dispatch --------------------------------------------------

bool ScriptDecoder::read(OperandCode code, const std::vector<ScriptValue>& prev, ScriptValue* out) {
    switch (code) {
        case OperandCode::U8: {
            uint8_t value = 0;
            if (!readU8(&value)) {
                return false;
            }
            *out = scriptInt((int32_t)value);
            return true;
        }
        case OperandCode::S8: {
            uint8_t value = 0;
            if (!readU8(&value)) {
                return false;
            }
            // The C#'s (sbyte) cast: 128..255 become negative.
            *out = scriptInt(value < 128 ? (int32_t)value : (int32_t)value - 256);
            return true;
        }
        case OperandCode::U16: {
            if (!canRead(2)) {
                return false;
            }
            *out = scriptInt((int32_t)binU16(bytes_, p_));
            p_ += 2;
            return true;
        }
        case OperandCode::S16: {
            if (!canRead(2)) {
                return false;
            }
            *out = scriptInt((int32_t)binS16(bytes_, p_));
            p_ += 2;
            return true;
        }
        case OperandCode::U32: {
            uint32_t value = 0;
            if (!readU32(&value)) {
                return false;
            }
            *out = scriptInt((int32_t)value);
            return true;
        }
        case OperandCode::S32: {
            int32_t value = 0;
            if (!readS32(&value)) {
                return false;
            }
            *out = scriptInt(value);
            return true;
        }
        case OperandCode::F32: {
            float value = 0.0f;
            if (!readF32(&value)) {
                return false;
            }
            *out = scriptFloat(value);
            return true;
        }
        case OperandCode::Pstr: {
            std::string value;
            if (!readPascalString(&value)) {
                return false;
            }
            *out = scriptString(value);
            return true;
        }
        case OperandCode::Code32:
        case OperandCode::Data32: {
            int32_t value = 0;
            if (!readOffset(&value)) {
                return false;
            }
            *out = scriptInt(value);
            return true;
        }
        case OperandCode::Action: {
            BranchAction value;
            if (!readAction(&value)) {
                return false;
            }
            *out = scriptAction(value);
            return true;
        }
        case OperandCode::Value: {
            ScriptValue value;
            if (!readValue(&value)) {
                return false;
            }
            *out = value;
            return true;
        }
        case OperandCode::Cond: {
            uint8_t op = 0;
            if (!readU8(&op)) {
                return false;
            }
            std::vector<ScriptValue> items;
            items.push_back(scriptInt((int32_t)op));
            float value = 0.0f;
            if (!readF32(&value)) {
                return false;
            }
            items.push_back(scriptFloat(value));
            // "Between" and "outside" take two bounds.
            if (op == kCondBetween || op == kCondOutside) {
                if (!readF32(&value)) {
                    return false;
                }
                items.push_back(scriptFloat(value));
            }
            *out = scriptList(items);
            return true;
        }
        default:
            break;
    }

    // The layouts that depend on the earlier operands.
    switch (code) {
        case OperandCode::Op4CommandArgs:
            return readCommandArgs(prev, out);
        case OperandCode::Op4SelectorArgs: {
            int32_t selector = 0;
            if (!prevInt(prev, 2, &selector)) {
                return false;
            }
            return readSelectorArgs(selector, out);
        }
        case OperandCode::Op2Origin:
            return readOrigin(prev, out);
        case OperandCode::Op164Params:
            return readParams(prev, out);
        case OperandCode::Op42Part:
            return readPartName(out);
        case OperandCode::Op249Sound:
            return readSound(prev, out);
        case OperandCode::Op250TypeName:
            return readTypeName(prev, out);
        case OperandCode::Op250Box:
            return readBox(prev, out);
        case OperandCode::Op174B:
        case OperandCode::Op175B: {
            int32_t value = 0;
            if (!prevInt(prev, 1, &value)) {
                return false;
            }
            if (value != 7 && value != 8) {
                *out = scriptNull();
                return true;
            }
            float bound = 0.0f;
            if (!readF32(&bound)) {
                return false;
            }
            *out = scriptFloat(bound);
            return true;
        }
        case OperandCode::Op129Parts:
            return readParts(prev, out);
        case OperandCode::Op132Position:
            return readPosition(prev, out);
        case OperandCode::Op89Position: {
            int32_t flags = 0;
            if (!prevInt(prev, 0, &flags)) {
                return false;
            }
            return readOp89Position(flags, out);
        }
        case OperandCode::Op61Origin:
            return readOp61Origin(out);
        case OperandCode::Op83Args:
            return readOp83Args(out);
        case OperandCode::Op159Position:
            return modeThenFloats(modeCount159, out);
        case OperandCode::Op189Args:
            return modeThenFloats(modeCount189, out);
        case OperandCode::Op193Mode:
            return readOp193Mode(out);
        case OperandCode::Op245Args:
            return readOp245Args(out);
        case OperandCode::Op248Args:
            return readOp248Args(out);
        case OperandCode::Op172Position:
        case OperandCode::Op178Position:
            return readOp172Position(out);
        case OperandCode::Op173Target:
            return readOp173Target(out);
        case OperandCode::Op180Offset:
            return modeThenFloats(modeCount180, out);
        case OperandCode::Op181Source:
            return readOp181Source(out);
        case OperandCode::Op203Height:
            return modeThenFloats(modeCount203, out);
        case OperandCode::Op224Params:
            return modeThenFloats(modeCount224, out);
        case OperandCode::Op228TypeName:
            return readOp228TypeName(out);
        case OperandCode::Op242Params:
            return modeThenFloats(modeCount242, out);
        case OperandCode::Op158HitTarget:
            return readHitTarget(prev, out);
        case OperandCode::RepeatCode32:
            return readRepeatCode32(prev, out);
        case OperandCode::RepeatPstr:
            return readRepeatPstr(prev, out);
        case OperandCode::RepeatU8Code32:
            return readRepeatU8Code32(prev, out);
        default:
            break;
    }

    // The C# throws InvalidDataException for an operand code it does not
    // know. There is none in the table, so this is only reachable if the
    // table grows a code the decoder does not handle.
    return false;
}

// --- the decoder -----------------------------------------------------------

bool ScriptDecoder::decode(int32_t pc, Instruction* out) {
    if (!out) {
        return false;
    }
    const std::map<int32_t, Instruction>::const_iterator cached = cache_.find(pc);
    if (cached != cache_.end()) {
        *out = cached->second;
        return true;
    }
    if (invalid_.find(pc) != invalid_.end()) {
        return false;
    }

    if (dialect_ == ScriptDialect::Beta1996) {
        return decodeBeta(pc, out);
    }

    if (pc < 0 || pc >= byteCount_) {
        return false;
    }
    const int32_t opcode = bytes_[pc];
    p_ = pc + 1;
    hasAction_ = false;
    action_ = BranchAction();

    std::vector<ScriptValue> operands;
    if (opcode != kEnd) {
        const ScriptOpcodeInfo* info = scriptOpcodeInfo(opcode);
        if (!info) {
            // An opcode that is not in the table: the C# returns null and
            // remembers it.
            invalid_.insert(pc);
            return false;
        }
        for (int32_t i = 0; i < info->operandCount; i++) {
            ScriptValue value;
            if (!read(info->operands[i], operands, &value)) {
                invalid_.insert(pc);
                return false;
            }
            operands.push_back(value);
        }
    }

    Instruction instruction;
    instruction.pc = pc;
    instruction.opcode = opcode;
    instruction.operands = operands;
    instruction.next = p_;
    instruction.hasAction = hasAction_;
    instruction.action = action_;
    cache_[pc] = instruction;
    *out = instruction;
    return true;
}

const Instruction* ScriptDecoder::find(int32_t pc) {
    RecentSlot& slot = recent_.slots[(size_t)((uint32_t)pc & (uint32_t)(kRecentSlots - 1))];
    if (slot.instruction != nullptr && slot.pc == pc) {
        return slot.instruction;
    }
    std::map<int32_t, Instruction>::const_iterator cached = cache_.find(pc);
    if (cached == cache_.end()) {
        // Not decoded yet (or invalid): decode() caches it, or says it is invalid.
        Instruction decoded;
        if (!decode(pc, &decoded)) {
            return nullptr;
        }
        cached = cache_.find(pc);
        if (cached == cache_.end()) {
            return nullptr;
        }
    }
    slot.pc = pc;
    slot.instruction = &cached->second;
    return slot.instruction;
}

int32_t ScriptDecoder::preload(const std::vector<int32_t>& starts) {
    if (dialect_ != ScriptDialect::Retail) {
        return 0;
    }

    std::set<int32_t> seen;
    // The C#'s stack: the last start is the first one popped.
    std::vector<int32_t> work(starts.begin(), starts.end());
    while (!work.empty()) {
        int32_t p = work.back();
        work.pop_back();
        while (seen.insert(p).second) {
            Instruction instruction;
            if (!decode(p, &instruction)) {
                break;
            }
            if (instruction.opcode == kEnd) {
                break;
            }
            std::vector<int32_t> list;
            targets(instruction, &list);
            for (size_t i = 0; i < list.size(); i++) {
                work.push_back(list[i]);
            }
            const ScriptOpcodeInfo* info = scriptOpcodeInfo(instruction.opcode);
            if (info && info->noFallthrough) {
                break;
            }
            p = instruction.next;
        }
    }
    return (int32_t)seen.size();
}

void ScriptDecoder::targets(const Instruction& instruction, std::vector<int32_t>* out) {
    if (!out) {
        return;
    }
    const ScriptOpcodeInfo* info = scriptOpcodeInfo(instruction.opcode);
    if (info) {
        for (int32_t i = 0; i < info->operandCount; i++) {
            if (i >= (int32_t)instruction.operands.size()) {
                break;
            }
            const ScriptValue& value = instruction.operands[(size_t)i];
            switch (info->operands[i]) {
                case OperandCode::Code32:
                case OperandCode::Op158HitTarget:
                    if (value.kind == ScriptValueKind::Int && value.intValue != 0) {
                        out->push_back(value.intValue);
                    }
                    break;
                case OperandCode::RepeatCode32:
                    if (value.kind == ScriptValueKind::List) {
                        for (size_t k = 0; k < value.items.size(); k++) {
                            const ScriptValue& item = value.items[k];
                            if (item.kind == ScriptValueKind::List && !item.items.empty()) {
                                const ScriptValue& target = item.items[0];
                                if (target.kind == ScriptValueKind::Int && target.intValue != 0) {
                                    out->push_back(target.intValue);
                                }
                            }
                        }
                    }
                    break;
                case OperandCode::RepeatU8Code32:
                    if (value.kind == ScriptValueKind::List) {
                        for (size_t k = 0; k < value.items.size(); k++) {
                            const ScriptValue& item = value.items[k];
                            if (item.kind == ScriptValueKind::List && item.items.size() > 1) {
                                const ScriptValue& target = item.items[1];
                                if (target.kind == ScriptValueKind::Int && target.intValue != 0) {
                                    out->push_back(target.intValue);
                                }
                            }
                        }
                    }
                    break;
                default:
                    break;
            }
        }
    }

    if (instruction.hasAction && instruction.action.type != BranchAction::Kind::None) {
        if (instruction.action.target != 0) {
            out->push_back(instruction.action.target);
        }
        if (instruction.action.elseTarget != 0) {
            out->push_back(instruction.action.elseTarget);
        }
    }
}

} // namespace formats
} // namespace mdk

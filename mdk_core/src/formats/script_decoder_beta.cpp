// ============================================================================
// script_decoder_beta.cpp — see script_decoder_beta.h.
// ============================================================================

#include "formats/script_decoder_beta.h"

#include <string.h>

#include "formats/ascii_case.h"
#include "formats/bin_reader.h"
#include "formats/cmi.h"
#include "formats/script_decoder.h"
#include "formats/script_opcodes.h"

namespace mdk {
namespace formats {

namespace {

// The demo's branch action bytes.
const uint8_t kBetaGoto = 0x0C;
const uint8_t kBetaGosub = 0xF0;
const uint8_t kBetaReturn = 0xF1;

// The retail opcodes the demo's gosub and return become.
const int32_t kRetailGosub = 252;
const int32_t kRetailReturn = 253;

// The selector that names an object by its id.
const int32_t kSelectorById = 5;

// The commands of the demo's command_objects.
const int32_t kCommandGoto = 7;
const int32_t kCommandMove = 43;

// The demo's opcodes that are read by their own layouts.
const int32_t kBetaFollowPath = 2;
const int32_t kBetaCommandObjects = 4;
const int32_t kBetaFire = 61;

} // namespace

// --- the demo's decoder ----------------------------------------------------

bool ScriptDecoder::decodeBeta(int32_t pc, Instruction* out) {
    if (!out || pc < 0 || pc >= byteCount_) {
        return false;
    }
    const int32_t opcode = bytes_[pc];
    p_ = pc + 1;
    hasAction_ = false;
    action_ = BranchAction();

    if (opcode == kEnd) {
        Instruction instruction;
        instruction.pc = pc;
        instruction.opcode = opcode;
        instruction.next = p_;
        cache_[pc] = instruction;
        *out = instruction;
        return true;
    }

    std::vector<ScriptValue> operands;
    int32_t special = 0;
    if (decodeSpecial(opcode, &operands, &special)) {
        Instruction instruction;
        instruction.pc = pc;
        instruction.opcode = special;
        instruction.operands = operands;
        instruction.next = p_;
        instruction.hasAction = hasAction_;
        instruction.action = action_;
        cache_[pc] = instruction;
        *out = instruction;
        return true;
    }

    const BetaOpcodeInfo* layout = scriptBetaOpcodeInfo(opcode);
    if (!layout) {
        // An opcode the demo does not have: the C# returns null.
        invalid_.insert(pc);
        return false;
    }

    for (int32_t i = 0; i < layout->operandCount; i++) {
        ScriptValue value;
        if (!readBeta(layout->operands[i], operands, &value)) {
            invalid_.insert(pc);
            return false;
        }
        operands.push_back(value);
    }

    std::vector<ScriptValue> retail;
    retailOperands(opcode, operands, &retail);

    Instruction instruction;
    instruction.pc = pc;
    instruction.opcode = layout->opcode;
    instruction.operands = retail;
    instruction.next = p_;
    instruction.hasAction = hasAction_;
    instruction.action = action_;
    cache_[pc] = instruction;
    *out = instruction;
    return true;
}

// The demo's opcodes 2, 4 and 61, whose operands depend on earlier ones.
// Returns the opcode to run, or false for the others.
bool ScriptDecoder::decodeSpecial(int32_t opcode, std::vector<ScriptValue>* operands, int32_t* outOpcode) {
    switch (opcode) {
        case kBetaFollowPath: {
            int32_t path = 0;
            if (!readOffset(&path)) {
                return false;
            }
            // The CONVERTED path's offset, which is 0 when the path has
            // not been converted.
            const std::map<int32_t, int32_t>::const_iterator found = paths_.find(path);
            operands->push_back(scriptInt(found == paths_.end() ? 0 : found->second));
            *outOpcode = (int32_t)BetaOpcode::FollowPath;
            return true;
        }
        case kBetaCommandObjects: {
            // The selector's id is a byte.
            uint8_t command = 0;
            if (!readU8(&command)) {
                return false;
            }
            ScriptValue arguments = scriptNull();
            if (command == kCommandGoto) {
                BranchAction action;
                if (!readBetaAction(&action)) {
                    return false;
                }
                arguments = scriptAction(action);
            } else if (command == kCommandMove) {
                std::vector<ScriptValue> floats;
                if (!readFloats(2, &floats)) {
                    return false;
                }
                arguments = scriptList(floats);
            }

            uint8_t selector = 0;
            if (!readU8(&selector)) {
                return false;
            }
            std::vector<ScriptValue> selected;
            if (selector == 2 || selector == 4 || selector == kSelectorById) {
                std::string name;
                if (!readPascalString(&name)) {
                    return false;
                }
                selected.push_back(scriptString(toUpperAscii(name)));
            }
            if (selector == kSelectorById) {
                uint8_t id = 0;
                if (!readU8(&id)) {
                    return false;
                }
                selected.push_back(scriptInt((int32_t)id));
            }

            operands->push_back(scriptInt((int32_t)command));
            operands->push_back(arguments);
            operands->push_back(scriptInt((int32_t)selector));
            operands->push_back(scriptList(selected));
            *outOpcode = kBetaCommandObjects;
            return true;
        }
        case kBetaFire: {
            // Where from (a reference point or a part), whether it is
            // aimed, then three floats.
            uint8_t mode = 0;
            if (!readU8(&mode)) {
                return false;
            }
            std::vector<ScriptValue> origin;
            origin.push_back(scriptInt((int32_t)mode));
            if (mode == 0) {
                uint8_t point = 0;
                if (!readU8(&point)) {
                    return false;
                }
                origin.push_back(scriptInt((int32_t)point));
            } else {
                std::string name;
                if (!readPascalString(&name)) {
                    return false;
                }
                origin.push_back(scriptString(toUpperAscii(name)));
            }
            operands->push_back(scriptList(origin));

            uint8_t aimed = 0;
            if (!readU8(&aimed)) {
                return false;
            }
            operands->push_back(scriptInt((int32_t)aimed));
            for (int32_t i = 0; i < 3; i++) {
                float value = 0.0f;
                if (!readF32(&value)) {
                    return false;
                }
                operands->push_back(scriptFloat(value));
            }
            *outOpcode = (int32_t)BetaOpcode::Fire;
            return true;
        }
        default:
            break;
    }
    return false;
}

// The operands the retail handlers take, where the demo's differ.
void ScriptDecoder::retailOperands(int32_t opcode, const std::vector<ScriptValue>& operands,
                                    std::vector<ScriptValue>* out) const {
    const int32_t kStopHere = -2;
    switch (opcode) {
        case 21:
            // Stops the path where it is.
            out->clear();
            out->push_back(scriptInt(kStopHere));
            return;
        case 23:
            out->clear();
            out->push_back(scriptInt(1));
            return;
        case 77:
            out->clear();
            out->push_back(scriptInt(1));
            if (!operands.empty()) {
                out->push_back(operands[0]);
            }
            return;
        case 81:
            // Not multiplied by the frame time.
            out->clear();
            out->push_back(scriptInt(0));
            if (!operands.empty()) {
                out->push_back(operands[0]);
            }
            return;
        case 89:
            // Played where the object is.
            out->clear();
            out->push_back(scriptInt(0));
            out->push_back(scriptNull());
            if (!operands.empty()) {
                out->push_back(operands[0]);
            }
            return;
        case 92: {
            // The frame itself, not the frame + 1.
            out->clear();
            int32_t frame = 0;
            if (!operands.empty() && operands[0].kind == ScriptValueKind::Int) {
                frame = operands[0].intValue;
            }
            out->push_back(scriptInt(frame + 1));
            if (operands.size() > 1) {
                out->push_back(operands[1]);
            }
            return;
        }
        case 129: {
            // No mode.
            out->clear();
            out->push_back(scriptInt(0));
            std::vector<ScriptValue> names;
            if (!operands.empty() && operands[0].kind == ScriptValueKind::List) {
                for (size_t i = 0; i < operands[0].items.size(); i++) {
                    const ScriptValue& entry = operands[0].items[i];
                    if (entry.kind == ScriptValueKind::List && !entry.items.empty()) {
                        names.push_back(entry.items[0]);
                    }
                }
            }
            out->push_back(scriptList(names));
            return;
        }
        default:
            break;
    }
    *out = operands;
}

bool ScriptDecoder::readBeta(OperandCode code, const std::vector<ScriptValue>& prev, ScriptValue* out) {
    if (code == OperandCode::Action) {
        BranchAction action;
        if (!readBetaAction(&action)) {
            return false;
        }
        *out = scriptAction(action);
        return true;
    }
    if (code == OperandCode::Pstr) {
        // The demo's names are in lower case; the port's are in upper case.
        std::string name;
        if (!readPascalString(&name)) {
            return false;
        }
        *out = scriptString(toUpperAscii(name));
        return true;
    }
    if (code != OperandCode::RepeatCode32 && code != OperandCode::RepeatPstr) {
        return read(code, prev, out);
    }

    // The demo's repeats hold one item each, and the item is wrapped in an
    // array of one.
    uint8_t count = 0;
    if (!readU8(&count)) {
        return false;
    }
    std::vector<ScriptValue> items;
    for (int32_t i = 0; i < (int32_t)count; i++) {
        ScriptValue value;
        if (!readBeta(code == OperandCode::RepeatCode32 ? OperandCode::Code32 : OperandCode::Pstr, prev,
                     &value)) {
            return false;
        }
        std::vector<ScriptValue> item;
        item.push_back(value);
        items.push_back(scriptList(item));
    }
    *out = scriptList(items);
    return true;
}

bool ScriptDecoder::readBetaAction(BranchAction* out) {
    uint8_t action = 0;
    if (!readU8(&action)) {
        return false;
    }
    BranchAction result;
    switch (action) {
        case kBetaGosub: {
            int32_t target = 0;
            if (!readOffset(&target)) {
                return false;
            }
            result.type = BranchAction::Kind::Gosub;
            result.target = target;
            break;
        }
        case kBetaGoto: {
            int32_t target = 0;
            if (!readOffset(&target)) {
                return false;
            }
            result.type = BranchAction::Kind::Goto;
            result.target = target;
            break;
        }
        case kBetaReturn:
            result.type = BranchAction::Kind::Return;
            break;
        default:
            result.type = BranchAction::Kind::None;
            result.raw = (int32_t)action;
            break;
    }
    action_ = result;
    hasAction_ = true;
    *out = result;
    return true;
}

// --- the demo's entry points ------------------------------------------------

ScriptDecoder scriptDecoderFor(const Cmi& cmi) {
    if (cmi.dialect() == ScriptDialect::Retail) {
        return ScriptDecoder(cmi.bytes().data(), (int32_t)cmi.bytes().size());
    }
    return ScriptDecoder(cmi.bytes().data(), (int32_t)cmi.bytes().size(), ScriptDialect::Beta1996,
                          &cmi.betaPaths());
}

ScriptDecoder scriptDecoderBeta(const uint8_t* bytes, int32_t byteCount) {
    return ScriptDecoder(bytes, byteCount, ScriptDialect::Beta1996, nullptr);
}

std::vector<int32_t> scriptBetaPaths(const Cmi& cmi) {
    const int32_t kFollowPath = 2;
    const std::vector<uint8_t>& bytes = cmi.bytes();
    const int32_t byteCount = (int32_t)bytes.size();

    ScriptDecoder decoder = scriptDecoderBeta(bytes.data(), byteCount);
    std::vector<int32_t> paths;
    std::set<int32_t> seen;
    // The C#'s stack: the last entry point is the first one popped.
    std::vector<int32_t> todo(cmi.entryPoints().begin(), cmi.entryPoints().end());
    while (!todo.empty()) {
        int32_t pc = todo.back();
        todo.pop_back();
        while (pc > 0 && pc < byteCount && seen.insert(pc).second) {
            const int32_t opcode = bytes[(size_t)pc];

            // The path is at pc + 1, UNALIGNED: the opcode is a byte and
            // the offset is a u32.
            if (opcode == kFollowPath && pc + 5 <= byteCount) {
                const uint32_t raw = binU32(bytes.data(), pc + 1);
                if (raw != 0) {
                    const int32_t path = (int32_t)raw + ScriptDecoder::kFileBase;
                    bool known = false;
                    for (size_t i = 0; i < paths.size(); i++) {
                        if (paths[i] == path) {
                            known = true;
                            break;
                        }
                    }
                    if (!known) {
                        paths.push_back(path);
                    }
                }
            }

            Instruction instruction;
            if (!decoder.decode(pc, &instruction)) {
                break;
            }
            std::vector<int32_t> list;
            ScriptDecoder::targets(instruction, &list);
            for (size_t i = 0; i < list.size(); i++) {
                todo.push_back(list[i]);
            }

            // The opcodes after which a script does not go on: end, stop,
            // goto and return.
            if (opcode == ScriptDecoder::kEnd || opcode == 9 || opcode == 12 || opcode == kBetaReturn) {
                break;
            }
            pc = instruction.next;
        }
    }
    return paths;
}

} // namespace formats
} // namespace mdk

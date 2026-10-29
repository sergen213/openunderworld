#include "script/Vm.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>

namespace ou::script::vm {
namespace {
float asFloat(uint32_t v) { return std::bit_cast<float>(v); }
uint32_t fromFloat(float v) { return std::bit_cast<uint32_t>(v); }
Value local(size_t frame, uint32_t offset) {
    Value v; v.kind = Value::Kind::Local; v.frame = uint32_t(frame); v.offset = offset; return v;
}
Value member(int32_t uid, uint32_t offset) {
    Value v; v.kind = Value::Kind::Member; v.uid = uid; v.offset = offset; return v;
}
uint32_t nativeMoveSize(const ScriptType* type) {
    if (!type) return 0;
    const std::string& n = type->nativeName;
    if (n == "cameraspline" || n == "cameraanchor") return 8;
    if (n == "messagechannel") return 20;
    if (n == "sound" || n == "instanceref" || n == "playersensor" || n == "clock" || n == "inputaction") return 4;
    return 0;
}
} // namespace

Program::Program(std::vector<ScriptType> types) : types_(std::move(types)) {
    for (size_t i = 0; i < types_.size(); ++i) {
        byId_[types_[i].id] = i;
        for (size_t j = 0; j < types_[i].functions.size(); ++j) {
            const auto& f = types_[i].functions[j];
            methods_[{f.self.section, f.self.offset}] = {types_[i].id, j};
            methods_[{f.proto.section, f.proto.offset}] = {types_[i].id, j};
        }
    }
}
const ScriptType* Program::type(uint32_t id) const {
    const auto it = byId_.find(id);
    return it == byId_.end() ? nullptr : &types_[it->second];
}
const char* Program::nativeName(uint32_t id) const {
    const auto* t = type(id);
    return t ? t->nativeName.c_str() : "";
}
const Member* Program::member(uint32_t typeId, uint32_t offset) const {
    for (const ScriptType* t = type(typeId); t; t = type(t->superId))
        for (const auto& m : t->members) if (m.offset == offset) return &m;
    return nullptr;
}
bool Program::isA(uint32_t derived, uint32_t base) const {
    size_t depth = 0;
    for (const ScriptType* t = type(derived); t && depth++ < 64; t = type(t->superId))
        if (t->id == base) return true;
    return false;
}
const Function* Program::method(uint32_t typeId, int16_t vt) const {
    for (const ScriptType* t = type(typeId); t; t = type(t->superId))
        for (const auto& f : t->functions) if (f.vtIndex == vt) return &f;
    return nullptr;
}
const Function* Program::state(uint32_t typeId, uint32_t n) const {
    for (const ScriptType* t = type(typeId); t; t = type(t->superId))
        if (!t->states.empty()) return n < t->states.size() ? method(t->states[n]) : nullptr;
    return nullptr;
}
// StartObject: Run enters state 0 of the type's state table (0x57B560); types without a table keep the older rule.
const Function* Program::startup(uint32_t typeId) const {
    if (const Function* s = state(typeId, 0)) return s;
    for (const ScriptType* t = type(typeId); t; t = type(t->superId))
        if (!t->functions.empty())
            return t->functions.front().vtIndex == -1 || t->functions.front().vtIndex == 20 ? &t->functions.front() : nullptr;
    return nullptr;
}
const Function* Program::mainFunction(uint32_t typeId) const {
    for (const ScriptType* t = type(typeId); t; t = type(t->superId))
        if (!t->functions.empty()) {
            if (t->functions.size() <= 2) return nullptr;
            const Function& f = t->functions[2];
            bool waits = false, physics = false;
            for (const auto& in : f.code) {
                waits |= in.op == 0x80;
                for (const auto& p : in.ptrs)
                    if (const Function* callee = method(p); callee && callee->native && owner(callee) == 3566) physics = true;
            }
            return waits && physics ? &f : nullptr;
        }
    return nullptr;
}
const Function* Program::method(cdc::Pointer p) const {
    const auto it = methods_.find({p.section, p.offset});
    if (it == methods_.end()) return nullptr;
    const auto* t = type(it->second.first);
    return t ? &t->functions[it->second.second] : nullptr;
}
std::shared_ptr<Instance> Program::instantiate(int32_t uid, uint32_t typeId) {
    auto out = std::make_shared<Instance>();
    out->uid = uid; out->typeId = typeId; out->program = shared_from_this();
    std::vector<const ScriptType*> chain;
    for (const ScriptType* t = type(typeId); t; t = type(t->superId)) chain.push_back(t);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        for (const auto& m : (*it)->members) {
            const uint8_t base = m.type & 0x0F;
            const bool channel = !std::strcmp(nativeName(m.typeId), "messagechannel");
            if (m.init && (base <= 4 || m.typeId == kInstanceRefType || channel || !std::strcmp(nativeName(m.typeId), "terraingroup")))
                out->fields[m.offset] = channel && !m.initText.empty() ? channelId(m.initText) : *m.init;
            // uberobjectcommand / uberobjectevent: {uber object DTP id, command / event index}
            if (m.init && m.init2 && (!std::strcmp(nativeName(m.typeId), "uberobjectcommand") ||
                                      !std::strcmp(nativeName(m.typeId), "uberobjectevent")))
                out->fields[m.offset] = *m.init, out->fields[m.offset + 4u] = *m.init2;
        }
    out->initial = out->fields;
    out->initialStrings = out->strings;
    out->initialProperties = out->properties;
    return out;
}
uint32_t Program::functionIndex(const Function* f) const {
    for (const auto& t : types_)
        for (size_t i = 0; i < t.functions.size(); ++i) if (&t.functions[i] == f) return uint32_t(i);
    return 0;
}
uint32_t Program::owner(const Function* f) const {
    for (const auto& t : types_)
        for (const auto& g : t.functions) if (&g == f) return t.id;
    return 0;
}

bool Machine::call(Instance& object, int16_t vt, std::span<const Value> args) {
    if (!object.program) return false;
    const Function* f = vt == -2 ? object.program->startup(object.typeId)
        : vt == -3 ? object.program->mainFunction(object.typeId) : object.program->method(object.typeId, vt);
    return f ? start(object, *f, args) : true; // no handler: inherited empty
}
bool Machine::start(Instance& object, const Function& f, std::span<const Value> args) {
    frames_.clear(); steps_ = 0; wait_ = 0; failure_.clear();
    if (!push(object, f, args)) return false;
    return run() != Step::Failed;
}

bool Machine::tick(float seconds) {
    if (frames_.empty()) return true;
    failure_.clear();
    wait_ = std::max(0.0f, wait_ - std::max(0.0f, seconds));
    if (wait_ > 0) return true;
    return run() != Step::Failed;
}

Machine::Step Machine::run() {
    while (!frames_.empty()) {
        const Step result = step();
        if (result == Step::Failed) {
            if (failure_.empty()) failure_ = "budget/stack";
            frames_.clear();
            return result;
        }
        if (result == Step::Suspended) return result;
        if (result == Step::Complete) {
            frames_.pop_back();
            if (!frames_.empty()) ++frames_.back().pc;
        }
    }
    return Step::Complete;
}

bool Machine::push(Instance& object, const Function& f, std::span<const Value> args) {
    if (frames_.size() >= 100 || !object.program) return false;
    frames_.push_back({});
    Frame& fr = frames_.back();
    fr.object = &object; fr.function = &f;
    fr.locals.resize(std::max<uint32_t>(f.localCount, 1));
    if (!f.argCount) fr.locals[0] = Value::object(object.uid); // no argument occupies offset 0 (missing ones read 0)
    // `symbol` locals start at their declared value (function local records, DXHR cdc::Function +0x0C): the property
    // names passed to instanceref Get/SetProperty, e.g. l3_main_bedroom 12059 vt 28 local +5 = 10814.
    // `music` locals hold their initial {char* track, i32 slot}: the slot as the value, the track as its text.
    for (const Member& l : f.locals) {
        const char* type = object.program->nativeName(l.typeId);
        const bool symbol = l.init && !std::strcmp(type, "symbol"), music = l.initAt && !std::strcmp(type, "music");
        // uberobjectevent / uberobjectcommand locals {object, index} (l5_valaskjalf_entryb 7187 vt 54, l7_arctic_sea
        // 9115's constructor): natives receive the local's value, so it holds the index.
        if (l.init2 && (!std::strcmp(type, "uberobjectevent") || !std::strcmp(type, "uberobjectcommand"))) {
            if (l.offset >= fr.locals.size()) fr.locals.resize(l.offset + 1);
            fr.locals[l.offset] = Value::scalar(*l.init2);
            continue;
        }
        if (!symbol && !music) continue;
        if (l.offset >= fr.locals.size()) fr.locals.resize(l.offset + 1);
        fr.locals[l.offset] = Value::scalar(symbol ? *l.init : l.init2.value_or(0));
        if (music) fr.locals[l.offset].text = &l.initText;
    }
    for (size_t i = 0; i < args.size(); ++i) { // locals are addressed by byte offset; arguments start at 0
        const size_t off = i < f.argOffsets.size() ? f.argOffsets[i] : 4 * i;
        if (off >= fr.locals.size()) fr.locals.resize(off + 1);
        fr.locals[off] = args[i];
    }
    return true;
}

std::shared_ptr<Instance> Machine::resolveObject(const Value& v) {
    if (v.kind != Value::Kind::Object && v.kind != Value::Kind::Member) return nullptr;
    return resolve_(v.uid);
}
uint32_t Machine::load(const Value& v) const {
    switch (v.kind) {
    case Value::Kind::Scalar: return v.bits;
    case Value::Kind::Object: return uint32_t(v.uid);
    case Value::Kind::Constant: return v.ptr.offset;
    case Value::Kind::Local:
        if (v.frame < frames_.size() && v.offset < frames_[v.frame].locals.size())
            return frames_[v.frame].locals[v.offset].bits;
        return 0;
    case Value::Kind::Member: {
        auto o = resolve_(v.uid);
        if (!o) return 0;
        const auto it = o->fields.find(v.offset);
        return it == o->fields.end() ? 0 : it->second;
    }
    }
    return 0;
}
bool Machine::store(const Value& v, uint32_t bits) {
    return write(v, Value::scalar(bits));
}
Value Machine::read(const Value& v) const {
    if (v.kind == Value::Kind::Local && v.frame < frames_.size() && v.offset < frames_[v.frame].locals.size())
        return frames_[v.frame].locals[v.offset];
    if (v.kind == Value::Kind::Member) {
        auto o = resolve_(v.uid);
        if (o) {
            const Member* m = o->program->member(o->typeId, v.offset);
            if (m && (m->type & 0x0F) == 4) {
                Value out; out.kind = Value::Kind::Constant; out.text = &o->strings[v.offset]; return out;
            }
        }
    }
    if (v.kind == Value::Kind::Object || v.kind == Value::Kind::Constant) return v;
    return Value::scalar(load(v));
}
bool Machine::write(const Value& destination, const Value& source) {
    const Value src = read(source);
    if (destination.kind == Value::Kind::Local && destination.frame < frames_.size()) {
        auto& l = frames_[destination.frame].locals;
        if (destination.offset >= l.size()) l.resize(destination.offset + 1);
        l[destination.offset] = src;
        return true;
    }
    if (destination.kind == Value::Kind::Member) {
        auto o = resolveObject(destination);
        if (!o) return true; // the exe skips stores through a null object (MOVE cases 0x2E/0x2F)
        if (src.kind == Value::Kind::Constant && src.text) {
            o->strings[destination.offset] = *src.text;
            o->fields[destination.offset] = !src.text->empty();
        } else {
            o->fields[destination.offset] = load(src);
        }
        return true;
    }
    return destination.kind == Value::Kind::Scalar && destination.bits == 0; // null address: skipped
}

int Machine::invoke(Instance& current, const Function& f, Value receiver, Frame& frame) {
    if (frame.stack.size() < size_t(f.argCount)) return 0;
    // A local may hold the address of a caller's local (an argument passed by address): follow it to the value.
    auto deref = [&](Value v) {
        for (int i = 0; i < 4 && v.kind == Value::Kind::Local; ++i) v = read(v);
        return v;
    };
    // A native whose receiver address is null (a member of a null object, 0x11/0x12/0x15) is skipped by the exe
    // (0x57CE50 native call: the prototype's receiver flag set and a null receiver -> next instruction).
    const bool nullNative = f.native && receiver.kind == Value::Kind::Scalar && receiver.bits == 0 && !receiver.text;
    receiver = deref(receiver); // object or native value held in a local
    std::vector<Value> args(frame.stack.end() - f.argCount, frame.stack.end());
    if (f.native) // natives get values, not addresses of the caller's locals (a cameraspline argument held in a local)
        for (Value& a : args) a = deref(a);
    // A caller that discards the result pushes no result address (l3_chapel soundvolume `globals.member12.vt19()`).
    const Value* slot = f.returnType && frame.stack.size() > f.argCount ? &*(frame.stack.end() - f.argCount - 1) : nullptr;
    const bool hasResult = slot && (slot->kind == Value::Kind::Local || slot->kind == Value::Kind::Member ||
                                     (slot->kind == Value::Kind::Scalar && slot->bits == 0));
    const Value resultAddress = hasResult ? *slot : Value{};
    Value result;
    if (nullNative) {
        frame.stack.resize(frame.stack.size() - f.argCount - hasResult);
        return hasResult && !write(resultAddress, Value::scalar(0)) ? 0 : 1;
    }
    if (f.native) {
        NativeContext nc{current.program->owner(&f), current.program->functionIndex(&f), receiver, args, {}};
        nc.name = current.program->nativeName(nc.typeId);
        if (!native_(nc)) {
            failure_ = std::string("native ") + nc.name + " fn " + std::to_string(nc.function);
            return 0;
        }
        if (nc.wait) return 3; // blocking native (e.g. WaitUntilBirthed): stack untouched, retried next tick
        result = nc.result;
    }
    frame.stack.resize(frame.stack.size() - f.argCount - hasResult);
    if (f.native) return hasResult && !write(resultAddress, result) ? 0 : 1;
    auto target = resolveObject(receiver);
    if (!target) {
        if (hasResult && !write(resultAddress, Value::scalar(0))) return 0;
        return 1; // the exe reports a null script receiver and continues the caller
    }
    if (event_ && event_(*target, f.vtIndex)) return 1;
    if (!push(*target, f, args)) return 0;
    // A function with a return value writes it through local 0, the caller's result address (l3_chapel ignitable 12855
    // vt 54: PUSHL32 0 ... MOVE8); its arguments follow it.
    if (hasResult && (!f.argCount || (!f.argOffsets.empty() && f.argOffsets[0] >= 4))) frames_.back().locals[0] = resultAddress;
    return 2;
}

Machine::Step Machine::step() {
    if (frames_.empty()) return Step::Complete;
    Frame& fr = frames_.back();
    Instance& object = *fr.object;
    const Function& f = *fr.function;
    auto fail = [&] {
        if (failure_.empty() && fr.pc < f.code.size()) {
            char buf[16];
            std::snprintf(buf, sizeof buf, "op 0x%02X", f.code[fr.pc].op);
            failure_ = buf;
        }
        return Step::Failed;
    };
    auto done = [&] { return Step::Complete; };
    auto suspend = [&] { return Step::Suspended; };
    auto pop = [&]() -> Value {
        if (fr.stack.empty()) return {};
        Value v = fr.stack.back(); fr.stack.pop_back(); return v;
    };
    auto binary = [&](auto fn) -> bool {
        if (fr.stack.size() < 2) return false;
        const uint32_t b = fr.stack.back().bits; fr.stack.pop_back();
        fr.stack.back().bits = fn(fr.stack.back().bits, b);
        return true;
    };
    size_t& pc = fr.pc;
    while (pc < f.code.size()) {
        if (++steps_ > 100000) { failure_ = "budget"; return fail(); }
        const Instr& in = f.code[pc];
        const uint32_t arg = uint32_t(in.arg);
        auto pushScalar = [&](uint32_t v) { fr.stack.push_back(Value::scalar(v)); };
        auto pushLocal = [&](uint32_t off, uint32_t mask = 0xFFFFFFFFu) { pushScalar(load(local(frames_.size() - 1, off)) & mask); };
        auto pushMember = [&](int32_t uid, uint32_t off, uint32_t mask = 0xFFFFFFFFu) { pushScalar(load(member(uid, off)) & mask); };
        auto jump = [&]() -> bool {
            const int64_t target = int64_t(in.word) + 1 + in.arg;
            if (target < 0) return false;
            pc = 0;
            while (pc < f.code.size() && f.code[pc].word < target) ++pc;
            return pc < f.code.size() && f.code[pc].word == target;
        };
        // 0 failed, 1 continue, 2 suspended: a blocking native retries this instruction next tick.
        auto callWith = [&](const Function& callee, const Value& receiver, bool popped) -> int {
            const int invoked = invoke(object, callee, receiver, fr);
            if (!invoked) return 0;
            if (invoked == 3) {
                if (popped) fr.stack.push_back(receiver);
                wait_ = 0;
                return 2;
            }
            if (invoked == 1) ++pc;
            return 1;
        };
        auto direct = [&](bool popReceiver, bool virtualCall) -> int {
            if (in.ptrs.empty()) return 0;
            Value receiver = popReceiver ? pop() : Value::object(object.uid);
            const Function* callee = object.program->method(in.ptrs.back());
            if (virtualCall && callee && callee->vtIndex >= 0) {
                // Dispatch on the receiver's own type; keep the static callee if that type does not override it.
                if (auto target = resolveObject(receiver))
                    if (const Function* own = target->program->method(target->typeId, callee->vtIndex)) callee = own;
            }
            if (!callee) return 0;
            return callWith(*callee, receiver, popReceiver);
        };
        auto called = [&](int r) { return r == 0 ? fail() : r == 2 ? suspend() : Step::Continue; };
        switch (in.op) {
        case 0x00: ++pc; break; // NOP (statement number)
        case 0x01: if (fr.stack.empty()) return fail(); fr.stack.pop_back(); ++pc; break;
        case 0x02: pushScalar(in.imm); ++pc; break;
        case 0x03: pushScalar(arg); ++pc; break;
        case 0x04: pushScalar(arg & 0xFF); ++pc; break;
        case 0x05: {
            if (in.ptrs.empty()) return fail();
            Value v; v.kind = Value::Kind::Constant; v.ptr = in.ptrs[0]; v.text = &in.text; fr.stack.push_back(v); ++pc; break;
        }
        case 0x06: pushLocal(arg, 0xFF); ++pc; break;
        case 0x07: pushLocal(arg, 0xFFFF); ++pc; break;
        case 0x08: { // PUSHL32; a string argument stays a string (unit hint handlers compare and store it: EQC / MOVEC)
            const Value v = read(local(frames_.size() - 1, arg));
            if (v.kind == Value::Kind::Constant || v.kind == Value::Kind::Local || v.kind == Value::Kind::Member)
                fr.stack.push_back(v); // a string, or an address held in the local (the result address)
            else pushLocal(arg);
            ++pc; break;
        }
        case 0x0A: {
            if (arg < fr.locals.size()) fr.stack.push_back(fr.locals[arg]);
            else pushScalar(0);
            ++pc; break;
        }
        case 0x09: fr.stack.push_back(local(frames_.size() - 1, arg)); ++pc; break;
        case 0x0B: pushMember(object.uid, arg); ++pc; break;
        case 0x0E: fr.stack.push_back(read(member(object.uid, arg))); ++pc; break;
        case 0x10: {
            const uint32_t uid = load(member(object.uid, arg));
            fr.stack.push_back(uid ? Value::object(int32_t(uid)) : Value::scalar(0));
            ++pc; break;
        }
        case 0x0C: pushMember(object.uid, arg, 0xFF); ++pc; break;
        case 0x0D: pushMember(object.uid, arg, 0xFFFF); ++pc; break;
        case 0x0F: fr.stack.push_back(member(object.uid, arg)); ++pc; break;
        case 0x11: case 0x12: case 0x15: { // member of an object on the stack; a null object yields 0 (exe 0x57D040)
            Value v = pop(); auto o = resolveObject(v); if (!o) { pushScalar(0); ++pc; break; }
            const auto it = o->fields.find(arg);
            if (it == o->fields.end()) pushScalar(0);
            else if (in.op == 0x11) pushScalar(it->second & 0xFF);
            else if (in.op == 0x12) pushScalar(it->second & 0xFFFF);
            else if (in.op == 0x15) fr.stack.push_back(Value::object(int32_t(it->second)));
            else pushScalar(it->second);
            ++pc; break;
        }
        case 0x13: {
            Value v = pop(); auto o = resolveObject(v);
            fr.stack.push_back(o ? read(member(o->uid, arg)) : Value::scalar(0)); ++pc; break;
        }
        case 0x14: { Value v = pop(); auto o = resolveObject(v); fr.stack.push_back(o ? member(o->uid, arg) : Value::scalar(0)); ++pc; break; }
        case 0x2E: { // MOVE8: source on top, destination below it
            if (fr.stack.size() < 2) return fail();
            const uint32_t src = load(pop());
            Value d = pop();
            if (!store(d, src & 0xFF)) return fail();
            ++pc; break;
        }
        case 0x2F: case 0x30: case 0x31: { // MOVE32 / MOVER / MOVEC
            if (fr.stack.size() < 2) return fail();
            Value src = pop(), d = pop();
            if (!write(d, src)) return fail();
            ++pc; break;
        }
        case 0x34: { // MOVEU: source address on top, destination below; operand is the native value type
            const uint32_t size = nativeMoveSize(object.program->type(in.ref));
            if (!size || fr.stack.size() < 2) return fail();
            Value src = pop(), d = pop();
            auto address = [](Value::Kind k) { return k == Value::Kind::Local || k == Value::Kind::Member; };
            if (!address(src.kind) || !address(d.kind)) return fail();
            for (uint32_t off = 0; off < size; off += 4) {
                Value s = src, dest = d;
                s.offset += off; dest.offset += off;
                if (!write(dest, s)) return fail();
            }
            ++pc; break;
        }
        case 0x3D: if (fr.stack.empty()) return fail(); fr.stack.back().bits = 0u - fr.stack.back().bits; ++pc; break;
        case 0x3E: if (!binary([](uint32_t a, uint32_t b) { return a + b; })) return fail(); ++pc; break;
        case 0x3F: if (!binary([](uint32_t a, uint32_t b) { return a - b; })) return fail(); ++pc; break;
        case 0x40: if (!binary([](uint32_t a, uint32_t b) { return a * b; })) return fail(); ++pc; break;
        case 0x41: {
            if (!binary([](uint32_t a, uint32_t b) {
                    if (!b) return a & 0x80000000u ? 0x80000000u : a ? 0x7FFFFFFFu : 0u;
                    if (a == 0x80000000u && b == 0xFFFFFFFFu) return a;
                    return uint32_t(int32_t(a) / int32_t(b));
                })) return fail();
            ++pc; break;
        }
        case 0x42: {
            if (!binary([](uint32_t a, uint32_t b) {
                    if (!b) return 0u;
                    if (a == 0x80000000u && b == 0xFFFFFFFFu) return 0u;
                    return uint32_t(int32_t(a) % int32_t(b));
                })) return fail();
            ++pc; break;
        }
        case 0x43: if (!binary([](uint32_t a, uint32_t b) { return uint32_t(int32_t(a) < int32_t(b)); })) return fail(); ++pc; break;
        case 0x44: if (!binary([](uint32_t a, uint32_t b) { return uint32_t(int32_t(a) <= int32_t(b)); })) return fail(); ++pc; break;
        case 0x45: if (!binary([](uint32_t a, uint32_t b) { return uint32_t(a == b); })) return fail(); ++pc; break;
        case 0x46: if (!binary([](uint32_t a, uint32_t b) { return uint32_t(uint8_t(a) == uint8_t(b)); })) return fail(); ++pc; break;
        case 0x47: if (fr.stack.empty()) return fail(); fr.stack.back().bits = fromFloat(-asFloat(fr.stack.back().bits)); ++pc; break;
        case 0x48: if (!binary([](uint32_t a, uint32_t b) { return fromFloat(asFloat(a) + asFloat(b)); })) return fail(); ++pc; break;
        case 0x49: if (!binary([](uint32_t a, uint32_t b) { return fromFloat(asFloat(a) - asFloat(b)); })) return fail(); ++pc; break;
        case 0x4A: if (!binary([](uint32_t a, uint32_t b) { return fromFloat(asFloat(a) * asFloat(b)); })) return fail(); ++pc; break;
        case 0x4B: {
            if (!binary([](uint32_t a, uint32_t b) {
                    if (asFloat(b) == 0) return asFloat(a) > 0 ? 0x7F7FFFFFu : asFloat(a) < 0 ? 0xFF7FFFFFu : 0u;
                    return fromFloat(asFloat(a) / asFloat(b));
                })) return fail();
            ++pc; break;
        }
        case 0x4C: if (!binary([](uint32_t a, uint32_t b) { return uint32_t(asFloat(a) < asFloat(b)); })) return fail(); ++pc; break;
        case 0x4D: if (!binary([](uint32_t a, uint32_t b) { return uint32_t(asFloat(a) <= asFloat(b)); })) return fail(); ++pc; break;
        case 0x4E: if (!binary([](uint32_t a, uint32_t b) { return uint32_t(asFloat(a) == asFloat(b)); })) return fail(); ++pc; break;
        case 0x4F: { // EQC
            if (fr.stack.size() < 2) return fail();
            const Value b = read(pop()), a = read(pop());
            static const std::string empty;
            const auto* as = a.kind == Value::Kind::Constant && a.text ? a.text : &empty;
            const auto* bs = b.kind == Value::Kind::Constant && b.text ? b.text : &empty;
            pushScalar(*as == *bs); ++pc; break;
        }
        case 0x50: if (fr.stack.empty()) return fail(); fr.stack.back().bits = !truth(fr.stack.back()); ++pc; break;
        case 0x51: if (!binary([](uint32_t a, uint32_t b) { return uint32_t((a & 0xFF) && (b & 0xFF)); })) return fail(); ++pc; break;
        case 0x52: if (!binary([](uint32_t a, uint32_t b) { return uint32_t((a & 0xFF) || (b & 0xFF)); })) return fail(); ++pc; break;
        case 0x53: if (!binary([](uint32_t a, uint32_t b) { return uint32_t(bool(a & 0xFF) != bool(b & 0xFF)); })) return fail(); ++pc; break;
        case 0x54: if (!binary([](uint32_t a, uint32_t b) { return a & b; })) return fail(); ++pc; break;
        case 0x55: if (!binary([](uint32_t a, uint32_t b) { return a | b; })) return fail(); ++pc; break;
        case 0x56: if (!binary([](uint32_t a, uint32_t b) { return a ^ b; })) return fail(); ++pc; break;
        case 0x57: if (!binary([](uint32_t a, uint32_t b) { return a << (b & 31); })) return fail(); ++pc; break;
        case 0x58: if (!binary([](uint32_t a, uint32_t b) { return uint32_t(int32_t(a) >> (b & 31)); })) return fail(); ++pc; break;
        case 0x59: if (fr.stack.empty()) return fail(); fr.stack.back().bits = ~fr.stack.back().bits; ++pc; break;
        case 0x5A: if (fr.stack.empty()) return fail(); {
                const float v = asFloat(fr.stack.back().bits);
                fr.stack.back().bits = !std::isfinite(v) || v > float(std::numeric_limits<int32_t>::max()) ||
                    v < float(std::numeric_limits<int32_t>::min()) ? 0x80000000u : uint32_t(int32_t(v));
            } ++pc; break;
        case 0x5B: if (fr.stack.empty()) return fail(); fr.stack.back().bits = asFloat(fr.stack.back().bits) != 0; ++pc; break;
        case 0x5C: if (fr.stack.empty()) return fail(); fr.stack.back().bits = fromFloat(float(int32_t(fr.stack.back().bits))); ++pc; break;
        case 0x5D: if (fr.stack.empty()) return fail(); fr.stack.back().bits = fr.stack.back().bits != 0; ++pc; break;
        case 0x5E: if (fr.stack.empty()) return fail(); fr.stack.back().bits = uint8_t(fr.stack.back().bits); ++pc; break;
        case 0x5F: if (fr.stack.empty()) return fail(); fr.stack.back().bits = fromFloat(float(uint8_t(fr.stack.back().bits))); ++pc; break;
        case 0x60: case 0x61: { // CTOI / CTOF
            if (fr.stack.empty()) return fail();
            const Value v = read(fr.stack.back());
            const char* s = v.kind == Value::Kind::Constant && v.text ? v.text->c_str() : "";
            fr.stack.back() = Value::scalar(in.op == 0x60 ? uint32_t(std::atoi(s)) : fromFloat(float(std::atof(s))));
            ++pc; break;
        }
        case 0x62: if (fr.stack.empty()) return fail(); fr.stack.back().bits = load(fr.stack.back()) != 0; ++pc; break; // CTOB
        case 0x63: if (!jump()) return fail(); break;
        case 0x64: {
            if (fr.stack.empty()) return fail();
            if (!truth(pop())) {
                if (!jump()) return fail();
            } else {
                ++pc;
            }
            break;
        }
        case 0x65: { // CALLU: direct Function, receiver on top after arguments
            if (in.ptrs.empty() || fr.stack.empty()) return fail();
            Value receiver = pop();
            const Function* callee = object.program->method(in.ptrs[0]);
            if (!callee) return fail();
            return called(callWith(*callee, receiver, true));
        }
        case 0x66: case 0x67: return called(direct(false, false)); // CALL1 static / CALL2 this, super (non-virtual)
        case 0x68: return called(direct(false, true)); // CALL3: this, virtual (base 3586 fn 8 -> the reveal's vt 64)
        case 0x69: return called(direct(true, false));  // receiver, direct
        case 0x6A: return called(direct(true, true));   // receiver, virtual
        case 0x6B: case 0x6C: { // SSEQU / SSEQ1: replace this handler with the child sequence
            if (in.ptrs.empty()) return fail();
            Value receiver = in.op == 0x6B ? pop() : Value::object(object.uid);
            const Function* callee = object.program->method(in.ptrs[0]);
            if (!callee) return fail();
            const int invoked = invoke(object, *callee, receiver, fr);
            if (!invoked) return fail();
            if (invoked == 3) {
                if (in.op == 0x6B) fr.stack.push_back(receiver);
                wait_ = 0;
                return suspend();
            }
            // A native sequence (0x57CE50 case 0x6A: 0x574960 starts its latent object, the thread waits on it) resumes
            // after the SSEQU once it is done: l1_cine_ship_sinking vt 11 waits until cinematic 1005 is birthed, then plays.
            if (invoked == 1) {
                ++pc;
                return Step::Continue;
            }
            frames_.erase(frames_.end() - 2); // discard the parent; its caller resumes after the sequence
            return Step::Continue;
        }
        case 0x7B: return done();
        case 0x77: { // START: replace this frame with a fresh fiber at the next statement boundary
            // With a function word (0x57CE50 switch case 0x76 = op - 1: a non-null word is called on the object via
            // 0x57AF00, then the thread goes on): plate subclasses' state 0 runs base 3938's constructor this way.
            if (!in.ptrs.empty()) return called(direct(false, false));
            const uint32_t target = in.word + 1 + uint32_t(in.arg);
            fr.stack.clear();
            fr.locals.assign(std::max<uint32_t>(f.localCount, 1), Value{});
            fr.locals[0] = Value::object(object.uid);
            pc = std::lower_bound(f.code.begin(), f.code.end(), target,
                                  [](const Instr& code, uint32_t word) { return code.word < word; }) - f.code.begin();
            return pc < f.code.size() ? Step::Continue : done();
        }
        case 0x78: case 0x79: case 0x7A:
            // TSSTART / TPSTART n start a thread at the next word and skip the parent past the block; TEND parks the
            // thread (0x57CE50 cases 0x77-0x79). Ours run the block inline in this thread [I] (one l7 type uses them).
            ++pc; break;
        case 0x1D: case 0x1E: { // DREF32 / DREFA: index on top, element 0 address below; null stays null
            if (fr.stack.size() < 2) return fail();
            const uint32_t index = pop().bits;
            Value& a = fr.stack.back();
            if (a.kind == Value::Kind::Local || a.kind == Value::Kind::Member) {
                a.offset += (in.op == 0x1D ? 4u : uint32_t(in.arg) & 0xFFFFFFu) * index;
                if (in.op == 0x1D) a = Value::scalar(load(a));
            }
            ++pc; break;
        }
        case 0x20: // DREFD: dynamic array -> {data, count}; dynamic arrays are not modelled and read as empty [I]
            if (fr.stack.empty()) return fail();
            fr.stack.back() = Value::scalar(0), pushScalar(0); ++pc; break;
        case 0x21: // BOUNDS n: an index >= n nulls the address below it
            if (fr.stack.size() < 2) return fail();
            if (fr.stack.back().bits >= (uint32_t(in.arg) & 0xFFFFFFu)) fr.stack[fr.stack.size() - 2] = Value::scalar(0);
            ++pc; break;
        case 0x22: { // DBOUNDS: index on top, count below; out of range -> null data and index
            if (fr.stack.size() < 3) return fail();
            const uint32_t index = pop().bits;
            if (index < fr.stack.back().bits) fr.stack.back() = Value::scalar(index);
            else fr.stack.back() = Value::scalar(0), fr.stack[fr.stack.size() - 2] = Value::scalar(0);
            ++pc; break;
        }
        case 0x7C: wait_ = 0; ++pc; return suspend(); // SLEEP: yield until the next tick (exe case 0x7B pops nothing)
        case 0x80: { // WAIT seconds
            if (fr.stack.empty()) return fail();
            wait_ = std::max(0.0f, asFloat(load(pop())));
            ++pc;
            return suspend();
        }
        case 0x7D: { // STATE: pop the state index; tru.exe 0x57C830 ends this object's threads and enters that state
            if (fr.stack.empty()) return fail();
            const uint32_t n = load(pop());
            Instance& self = object;
            stateChanged_ = self.uid;
            size_t k = 0;
            while (frames_[k].object != &self) ++k;
            frames_.erase(frames_.begin() + ptrdiff_t(k), frames_.end());
            // 0x57B560 runs the state's function (type +0x40 table) as the object's new thread; state 0 is only
            // entered by Run(). This machine's own object continues here; another object's state starts beside the
            // caller, which resumes at once.
            const Function* sf = n > 0 ? self.program->state(self.typeId, n) : nullptr;
            if (k == 0) {
                if (sf && !push(self, *sf)) return Step::Failed;
            } else {
                ++frames_.back().pc; // the ended call returns to its caller
                if (sf) starts_.push_back({self.uid, sf});
            }
            return Step::Continue;
        }
        case 0x7E: { // ISTYPE
            Value v = pop(); auto o = resolveObject(v);
            fr.stack.push_back(o && in.ref && o->program->isA(o->typeId, in.ref) ? v : Value::scalar(0));
            ++pc; break;
        }
        default: return fail();
        }
    }
    return done();
}

} // namespace ou::script::vm

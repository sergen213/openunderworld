#include "script/Vm.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <numbers>
#include <stdexcept>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

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
    if (n == "vector" || n == "rotation") return 16; // {x, y, z, w} floats (valueNative)
    if (n == "sound" || n == "instanceref" || n == "playersensor" || n == "clock" || n == "inputaction") return 4;
    return 0;
}
// A DYNARRAY member (the builtin container CALLU and DREFD work on); maps are not modelled.
bool dynamicArray(const Instance& o, uint32_t offset) {
    if (!o.program || offset >= 0x4000) return false;
    const Member* m = o.program->member(o.typeId, offset);
    return m && (m->type & 0x8F) == kDataDynArray;
}
// Byte range of element `index` of the array at `array` in Instance::fields / strings.
template <class Map>
void eraseElement(Map& map, uint32_t array, uint32_t index) {
    map.erase(map.lower_bound(elementOffset(array, index)), map.lower_bound(elementOffset(array, index) + 0x100));
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
    if (isElement(offset)) { // a dynamic-array element: its record from the array's initial value
        if (offset & 0xFF) return nullptr;
        const Member* array = member(typeId, offset >> 16 & 0x3FFF);
        const uint32_t index = offset >> 8 & 0xFF;
        return array && index < array->elements.size() ? &array->elements[index] : nullptr;
    }
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
            return t->functions.front().vtIndex == -1 ? &t->functions.front() : nullptr;
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
            // A dynamic array's field is its element count (the exe's array +0x18); elements: elementOffset().
            if ((m.type & 0x8F) == kDataDynArray && !m.elements.empty() && m.offset < 0x4000) {
                out->fields[m.offset] = uint32_t(m.elements.size());
                for (uint32_t i = 0; i < m.elements.size(); ++i) // scalar / instanceref elements hold their value
                    if (const Member& e = m.elements[i]; e.init && ((e.type & 0x0F) - 1u < 3u || e.typeId == kInstanceRefType))
                        out->fields[elementOffset(m.offset, i)] = *e.init;
            }
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
    const bool ok = run() != Step::Failed;
    if (wait_ <= 0) wait_ = 0;
    return ok;
}

// The thread timer (ScriptExec +8, update 0x57E970): each update takes dt off it and runs the thread once it is no longer
// positive; WAIT (0x57CE50 case 0x7F) adds its seconds to it, so the overshoot of one wait is taken off the next and a
// WAIT 2.0 loop runs every 2.0 s on average, not every whole tick after it. After the run a timer at or below 0 is 0.
bool Machine::tick(float seconds) {
    if (frames_.empty()) return true;
    failure_.clear();
    wait_ -= std::max(0.0f, seconds);
    if (wait_ > 0) return true;
    steps_ = 0; // the instruction budget bounds one resume: a thread that yields (SLEEP / WAIT) may poll for ever
    const bool ok = run() != Step::Failed;
    if (wait_ <= 0) wait_ = 0;
    return ok;
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
        if (l.init2 && (!std::strcmp(type, "uberobjectevent") || !std::strcmp(type, "uberobjectcommand") ||
                        !std::strcmp(type, "uberobjectquery"))) {
            if (l.offset >= fr.locals.size()) fr.locals.resize(l.offset + 1);
            fr.locals[l.offset] = Value::scalar(*l.init2);
            continue;
        }
        // `marker` locals name an intro (its +0x52 marker id is what watervolumebase 0 / 2 look up): l7_yggdrasil_end's
        // pool 8309 sets its level to marker intros 8680..8683 from locals of its constructor and its rise states.
        if (l.init && !std::strcmp(type, "marker")) {
            if (l.offset >= fr.locals.size()) fr.locals.resize(l.offset + 1);
            fr.locals[l.offset] = Value::object(int32_t(*l.init));
            continue;
        }
        // `damagetype` locals are constants a hit's damage class is compared with (damagetype 0): the base script of
        // l7_yggdrasil_end's Natla holds 13, Mjolnir's class, in her hit callback (vt 22).
        const bool damage = l.init && !std::strcmp(type, "damagetype");
        // BOOL / INT / FLOAT locals start at their record's value too: the frame setup 0x57A850 constructs every local
        // record (Function +0xC, 0x14 bytes) at frame + offset from its init (+0x10) through 0x579770 (a BOOL copies one
        // byte). 6515 fn 1 (the fire volumes' side test) compares against its FLOAT local +12 = pi so.
        if (const uint8_t base = l.type & 0x8F; l.init && base >= 1 && base <= 3) {
            if (l.offset >= fr.locals.size()) fr.locals.resize(l.offset + 1);
            fr.locals[l.offset] = Value::scalar(base == 1 ? *l.init & 0xFF : *l.init);
            continue;
        }
        if (!symbol && !music && !damage) continue;
        if (l.offset >= fr.locals.size()) fr.locals.resize(l.offset + 1);
        fr.locals[l.offset] = Value::scalar(symbol || damage ? *l.init : l.init2.value_or(0));
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
    const Value receiverAddress = receiver;
    receiver = deref(receiver); // object or native value held in a local
    std::vector<Value> args(frame.stack.end() - f.argCount, frame.stack.end());
    // A caller that discards the result pushes no result address (l3_chapel soundvolume `globals.member12.vt19()`).
    const Value* slot = f.returnType && frame.stack.size() > f.argCount ? &*(frame.stack.end() - f.argCount - 1) : nullptr;
    const bool hasResult = slot && (slot->kind == Value::Kind::Local || slot->kind == Value::Kind::Member ||
                                     (slot->kind == Value::Kind::Scalar && slot->bits == 0));
    const Value resultAddress = hasResult ? *slot : Value{};
    if (nullNative) {
        frame.stack.resize(frame.stack.size() - f.argCount - hasResult);
        return hasResult && !write(resultAddress, Value::scalar(0)) ? 0 : 1;
    }
    if (f.native) {
        const uint32_t type = current.program->owner(&f), index = current.program->functionIndex(&f);
        bool ok = true;
        if (valueNative(current.program->nativeName(type), index, receiverAddress, args, hasResult ? &resultAddress : nullptr, ok)) {
            if (!ok) {
                failure_ = std::string("native ") + current.program->nativeName(type) + " fn " + std::to_string(index);
                return 0;
            }
            frame.stack.resize(frame.stack.size() - f.argCount - hasResult);
            return 1;
        }
        for (Value& a : args) a = deref(a); // natives get values, not addresses of the caller's locals (a cameraspline argument held in a local)
        NativeContext nc{type, index, receiver, args, {}};
        nc.name = current.program->nativeName(nc.typeId);
        if (!native_(nc)) {
            failure_ = std::string("native ") + nc.name + " fn " + std::to_string(nc.function);
            return 0;
        }
        if (nc.wait) return 3; // blocking native (e.g. WaitUntilBirthed): stack untouched, retried next tick
        frame.stack.resize(frame.stack.size() - f.argCount - hasResult);
        if (!hasResult) return 1;
        if (!nc.wordCount) return write(resultAddress, nc.result) ? 1 : 0;
        const Value at = valueAt(resultAddress);
        for (uint32_t i = 0; i < nc.wordCount; ++i) {
            Value word = at;
            word.offset += 4 * i;
            if (!store(word, nc.words[i])) return 0;
        }
        return 1;
    }
    frame.stack.resize(frame.stack.size() - f.argCount - hasResult);
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

Value Machine::valueAt(Value a) const {
    for (int i = 0; i < 4 && a.kind == Value::Kind::Local && a.frame < frames_.size() && a.offset < frames_[a.frame].locals.size(); ++i) {
        const Value& held = frames_[a.frame].locals[a.offset];
        if (held.kind != Value::Kind::Local && held.kind != Value::Kind::Member) break;
        a = held;
    }
    return a;
}

// `vector` (type 220) and `rotation` (type 219) values are 16 bytes, {x, y, z, w} floats (a rotation: euler angles,
// z = yaw), at a local's or member's byte offset: words at +0, +4, +8, +12 (tru.exe keeps them inline in the frame /
// instance data; the interpreter passes their address as receiver and as native-typed argument, 0x57DE34). Thunks
// `handler(receiver*, argc, args*, result*)`:
// vector  0 / 3, 1 / 4, 2 / 5 get x / y / z (0x796540 / 0x7962A0 / 0x796440); 6 / 7 / 8 set x / y / z (0x796550 /
//         0x796560 / 0x796450); 9 copy from args[0] (0x796570, null: nothing); 10 Set(x, y, z), w = 0 (0x796460);
//         11 length sqrt(x²+y²+z²) (0x7ABAB0); 12 scale all four by args[0] (0x7ABAF0).
// rotation 0 between args[0] and args[1] (0x796360 -> 0x4A2F40: 0x49DA10 normalises both, h = (a + b) / 2, quaternion
//         (a × h, a · h), normalised (0x49B000); 0x49D450 -> matrix 0x49CF90 -> euler 0x49B440: x = atan2(m12, m22),
//         y = atan2(-m02, |(m00, m01)|), z = atan2(m01, m00) of the row-vector matrix); 1 / 2 / 3 rows 0 / 1 / 2 of its
//         matrix (0x4A3840 / 0x4A3870 / 0x4A38A0 over 0x49D500: identity, then z, y, x applied); 4 / 7, 5 / 8, 6 / 9
//         get x / y / z; 10 / 11 / 12 set x / y / z; 13 copy; 14 Set(x, y, z); 15 z += args[0] degrees (0x7964B0,
//         x 0.0174533). Not modelled: vector 13 / 14, rotation 16 / 17 (bool-argument engine calls 0x79D8E0 / 0x7987B0;
//         no shipped bytecode calls them).
bool Machine::valueNative(const char* type, uint32_t fn, const Value& receiver, std::span<const Value> args,
                          const Value* resultAddress, bool& ok) {
    const bool vector = !std::strcmp(type, "vector"), rotation = !std::strcmp(type, "rotation");
    if (!vector && !rotation) return false;
    const Value self = valueAt(receiver);
    auto component = [](Value at, uint32_t i) { at.offset += 4 * i; return at; };
    auto get = [&](const Value& at, uint32_t i) { return asFloat(load(component(at, i))); };
    auto set = [&](const Value& at, uint32_t i, float v) { return store(component(at, i), fromFloat(v)); };
    auto getVec = [&](const Value& at) { return glm::vec4(get(at, 0), get(at, 1), get(at, 2), get(at, 3)); };
    auto setVec = [&](const Value& at, glm::vec4 v) { return set(at, 0, v.x) && set(at, 1, v.y) && set(at, 2, v.z) && set(at, 3, v.w); };
    auto returns = [&](float v) { return !resultAddress || store(*resultAddress, fromFloat(v)); };
    auto returnsVec = [&](glm::vec4 v) { return !resultAddress || setVec(valueAt(*resultAddress), v); };
    auto isNull = [](const Value& v) { return v.kind == Value::Kind::Scalar && v.bits == 0 && !v.text; };
    const auto argf = [&](size_t i) { return i < args.size() ? asFloat(load(args[i])) : 0.0f; };
    // rotation -> matrix rows (0x49D500): column-vector R = Rx * Ry * Rz, row i of the exe's matrix = R e_i.
    auto rows = [&](const Value& at) {
        const glm::mat4 r = glm::rotate(glm::mat4(1), get(at, 0), glm::vec3(1, 0, 0)) *
                            glm::rotate(glm::mat4(1), get(at, 1), glm::vec3(0, 1, 0)) *
                            glm::rotate(glm::mat4(1), get(at, 2), glm::vec3(0, 0, 1));
        return glm::mat3(r);
    };
    static constexpr uint8_t kVectorArgs[] = {0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 3, 0, 1};
    static constexpr uint8_t kRotationArgs[] = {2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 3, 1};
    const std::span<const uint8_t> argc = vector ? std::span<const uint8_t>(kVectorArgs) : std::span<const uint8_t>(kRotationArgs);
    const bool address = self.kind == Value::Kind::Local || self.kind == Value::Kind::Member;
    if (fn >= argc.size() || args.size() != argc[fn] || !address) { ok = false; return true; }
    if (vector) {
        switch (fn) {
        case 0: case 3: ok = returns(get(self, 0)); break;
        case 1: case 4: ok = returns(get(self, 1)); break;
        case 2: case 5: ok = returns(get(self, 2)); break;
        case 6: case 7: case 8: ok = set(self, fn - 6, argf(0)); break;
        case 9: ok = isNull(args[0]) || setVec(self, getVec(valueAt(args[0]))); break;
        case 10: ok = setVec(self, {argf(0), argf(1), argf(2), 0.0f}); break;
        case 11: ok = returns(glm::length(glm::vec3(getVec(self)))); break;
        case 12: ok = setVec(self, getVec(self) * argf(0)); break;
        }
        return true;
    }
    switch (fn) {
    case 0: { // RotationBetween(a, b): both null -> nothing
        if (isNull(args[0]) && isNull(args[1])) break;
        const glm::vec3 a = glm::normalize(glm::vec3(getVec(valueAt(args[0])))), b = glm::normalize(glm::vec3(getVec(valueAt(args[1]))));
        if (!std::isfinite(a.x + b.x)) { ok = setVec(self, glm::vec4(0)); break; } // a zero vector [I: 0x49DA10 bails out]
        const glm::vec3 h = (a + b) * 0.5f, xyz = glm::cross(a, h);
        glm::quat q(glm::dot(a, h), xyz.x, xyz.y, xyz.z);
        // a = -b: no half vector; 0x49DA10 then turns pi about an axis normal to a [I: about z for a horizontal a].
        q = glm::length(q) > 1e-6f ? glm::normalize(q)
                                   : glm::angleAxis(std::numbers::pi_v<float>, std::abs(a.z) < 0.999f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0));
        const glm::mat3 r = glm::mat3_cast(q); // column-vector; the exe's row-vector m_ij = r[i][j] (glm r[column][row])
        ok = setVec(self, {std::atan2(r[1][2], r[2][2]), std::atan2(-r[0][2], std::hypot(r[0][0], r[0][1])),
                           std::atan2(r[0][1], r[0][0]), 0.0f});
        break;
    }
    case 1: case 2: case 3: ok = returnsVec(glm::vec4(rows(self)[fn - 1], 0.0f)); break;
    case 4: case 7: ok = returns(get(self, 0)); break;
    case 5: case 8: ok = returns(get(self, 1)); break;
    case 6: case 9: ok = returns(get(self, 2)); break;
    case 10: case 11: case 12: ok = set(self, fn - 10, argf(0)); break;
    case 13: ok = isNull(args[0]) || setVec(self, getVec(valueAt(args[0]))); break;
    case 14: ok = setVec(self, {argf(0), argf(1), argf(2), 0.0f}); break;
    case 15: ok = set(self, 2, get(self, 2) + argf(0) * 0.0174532924f); break;
    }
    return true;
}

// Builtin container methods (cdc::NativeBuiltInImpl 0x57BF90; records 0x8A7718 DYNARRAY, 0x8A7688 MAP), called by CALLU
// with the stack as for a native (0x57DE34): arguments, then a result address for non-void ones, then the receiver; a
// null receiver or a null result address skips the call. DYNARRAY: 0 Size -> int (0x574A30), 1 Clear (0x5795A0),
// 2 Resize(int) (0x579520), 3 Remove(int) (0x57CE30: 0 <= i < count), 4 Push(value) (0x57BE50), 5 Pop (0x5795B0).
// MAP: 0 Size -> int, 1 Clear, 2 Find(string) -> bool, 3 Erase(string); maps are not modelled (no shipped bytecode
// calls them: the 82 units' only builtin operand is the fire volumes' DYNARRAY Size), so a call on a map fails.
bool Machine::builtin(const Instr& in, Frame& fr) {
    const bool array = in.builtinType == kDataDynArray, map = in.builtinType == kDataMap;
    static constexpr uint8_t kArrayArgs[] = {0, 0, 1, 1, 1, 0}, kMapArgs[] = {0, 0, 1, 1};
    auto unsupported = [&](const char* what) {
        failure_ = std::string("builtin ") + (array ? "array" : map ? "map" : "type " + std::to_string(in.builtinType)) +
                   " fn " + std::to_string(in.builtinIndex) + what;
        return false;
    };
    if ((!array && !map) || in.builtinIndex >= (array ? std::size(kArrayArgs) : std::size(kMapArgs))) return unsupported("");
    const size_t argc = array ? kArrayArgs[in.builtinIndex] : kMapArgs[in.builtinIndex];
    const bool result = in.builtinIndex == 0 || (map && in.builtinIndex == 2);
    if (fr.stack.size() < 1 + argc + result) return unsupported(": stack");
    Value receiver = fr.stack.back();
    fr.stack.pop_back();
    for (int i = 0; i < 4 && receiver.kind == Value::Kind::Local; ++i) receiver = read(receiver); // held in a local
    std::vector<Value> args(fr.stack.end() - ptrdiff_t(argc), fr.stack.end());
    for (Value& a : args)
        for (int i = 0; i < 4 && a.kind == Value::Kind::Local; ++i) a = read(a);
    const Value address = result ? *(fr.stack.end() - ptrdiff_t(argc) - 1) : Value{};
    fr.stack.resize(fr.stack.size() - argc - result);
    if (receiver.kind == Value::Kind::Scalar && receiver.bits == 0 && !receiver.text) return true;   // null receiver
    if (result && address.kind == Value::Kind::Scalar && address.bits == 0 && !address.text) return true; // null result
    if (map) return unsupported(": maps are not modelled");
    const auto o = receiver.kind == Value::Kind::Member ? resolveObject(receiver) : nullptr;
    if (!o || !dynamicArray(*o, receiver.offset)) return unsupported(": receiver is not a dynamic array");
    const uint32_t at = receiver.offset;
    uint32_t& count = o->fields[at]; // std::map: stays valid while elements are inserted / erased
    count = std::min(count, kMaxElements);
    auto erase = [&](uint32_t i) { eraseElement(o->fields, at, i), eraseElement(o->strings, at, i); };
    auto moveDown = [&](auto& values, uint32_t j) { // element j -> j - 1
        const auto first = values.lower_bound(elementOffset(at, j)), last = values.lower_bound(elementOffset(at, j + 1));
        std::vector<std::pair<uint32_t, typename std::decay_t<decltype(values)>::mapped_type>> moved(first, last);
        values.erase(first, last);
        for (auto& [k, v] : moved) values[k - 0x100] = std::move(v);
    };
    switch (in.builtinIndex) {
    case 0: return write(address, Value::scalar(count)); // Size
    case 1: // Clear (0x579540: destroys every element)
        for (uint32_t i = 0; i < count; ++i) erase(i);
        count = 0;
        return true;
    case 2: { // Resize (0x5794A0): new elements start zeroed, dropped ones are destroyed
        const uint32_t n = args[0].bits;
        if (n > kMaxElements) return unsupported(": size out of range");
        for (uint32_t i = std::min(n, count); i < std::max(n, count); ++i) erase(i);
        count = n;
        return true;
    }
    case 3: { // Remove (0x57CE30 -> 0x57CDD0): out of range does nothing; later elements move down
        const uint32_t i = args[0].bits;
        if (i >= count) return true;
        erase(i);
        for (uint32_t j = i + 1; j < count; ++j) moveDown(o->fields, j), moveDown(o->strings, j);
        --count;
        return true;
    }
    case 4: { // Push (0x57BE50)
        if (count >= kMaxElements) return unsupported(": more elements than modelled");
        Value element = receiver;
        element.offset = elementOffset(at, count);
        erase(count++);
        return write(element, args[0]);
    }
    case 5: // Pop (0x5795B0): the last element is destroyed
        if (count) erase(--count);
        return true;
    }
    return unsupported("");
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
        case 0x10: { // PUSHSR: a script reference; a dynamic-array member is the array itself (DREFD, builtins)
            if (dynamicArray(object, arg)) { fr.stack.push_back(member(object.uid, arg)); ++pc; break; }
            const uint32_t uid = load(member(object.uid, arg));
            fr.stack.push_back(uid ? Value::object(int32_t(uid)) : Value::scalar(0));
            ++pc; break;
        }
        case 0x0C: pushMember(object.uid, arg, 0xFF); ++pc; break;
        case 0x0D: pushMember(object.uid, arg, 0xFFFF); ++pc; break;
        case 0x0F: fr.stack.push_back(member(object.uid, arg)); ++pc; break;
        case 0x11: case 0x12: case 0x15: { // member of an object on the stack; a null object yields 0 (exe 0x57D040)
            Value v = pop(); auto o = resolveObject(v); if (!o) { pushScalar(0); ++pc; break; }
            if (in.op == 0x15 && dynamicArray(*o, arg)) { fr.stack.push_back(member(o->uid, arg)); ++pc; break; }
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
            // A playersensor value is the handle of its native sensor (NtPlayerSensorInst): the copy in a local names the
            // same sensor, here its member / array element (the fire volumes' constructor attaches sensors[i] so).
            if (d.kind == Value::Kind::Local && src.kind == Value::Kind::Member && d.frame < frames_.size() &&
                object.program->type(in.ref)->nativeName == "playersensor") {
                auto& l = frames_[d.frame].locals;
                if (d.offset >= l.size()) l.resize(d.offset + 1);
                l[d.offset] = src;
                ++pc; break;
            }
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
        case 0x65: { // CALLU (0x57DE34): native method on the receiver on top, after the arguments
            if (in.builtinType) {
                if (!builtin(in, fr)) return fail();
                ++pc; break;
            }
            if (in.ptrs.empty() || fr.stack.empty()) return fail();
            Value receiver = pop();
            const Function* callee = object.program->method(in.ptrs[0]);
            if (!callee) return fail();
            if (callee->native) {
                // A null receiver drops the arguments and the result slot without a call, the result untouched
                // (0x5766F0); a non-void native whose result address is null is not called either.
                const bool nullReceiver = receiver.kind == Value::Kind::Scalar && receiver.bits == 0 && !receiver.text;
                const Value* slot = callee->returnType && fr.stack.size() > callee->argCount
                    ? &fr.stack[fr.stack.size() - callee->argCount - 1] : nullptr;
                const bool hasResult = slot && (slot->kind == Value::Kind::Local || slot->kind == Value::Kind::Member ||
                                                (slot->kind == Value::Kind::Scalar && slot->bits == 0 && !slot->text));
                const bool nullResult = hasResult && slot->kind == Value::Kind::Scalar;
                if (nullReceiver || nullResult) {
                    if (fr.stack.size() < size_t(callee->argCount) + hasResult) return fail();
                    fr.stack.resize(fr.stack.size() - callee->argCount - hasResult);
                    ++pc; break;
                }
            }
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
            if (a.kind == Value::Kind::Member && isElement(a.offset)) { // dynamic-array data (DREFD): element `index`
                if (index >= kMaxElements || (a.offset & 0xFFFF) != 0) a = Value::scalar(0);
                else a.offset += index << 8;
                if (in.op == 0x1D && a.kind == Value::Kind::Member) a = Value::scalar(load(a));
            } else if (a.kind == Value::Kind::Local || a.kind == Value::Kind::Member) {
                a.offset += (in.op == 0x1D ? 4u : uint32_t(in.arg) & 0xFFFFFFu) * index;
                if (in.op == 0x1D) a = Value::scalar(load(a));
            }
            ++pc; break;
        }
        case 0x20: { // DREFD: dynamic array -> {data, count} (the exe's array +0x20 / +0x18); not a modelled array: empty
            if (fr.stack.empty()) return fail();
            const Value a = fr.stack.back();
            const auto o = a.kind == Value::Kind::Member ? resolveObject(a) : nullptr;
            if (o && dynamicArray(*o, a.offset)) {
                fr.stack.back() = member(o->uid, elementOffset(a.offset, 0));
                pushScalar(load(a));
            } else {
                fr.stack.back() = Value::scalar(0), pushScalar(0);
            }
            ++pc; break;
        }
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
        case 0x80: { // WAIT seconds: added to the thread timer; a timer still at or below 0 goes on at once (case 0x7F)
            if (fr.stack.empty()) return fail();
            wait_ += asFloat(load(pop()));
            ++pc;
            if (wait_ > 0) return suspend();
            break;
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

nlohmann::json Machine::save() const {
    const auto value = [](const Value& v) {
        return nlohmann::json{int(v.kind), v.bits, v.uid, v.offset, v.frame, v.ptr.section, v.ptr.offset,
                              v.text ? nlohmann::json(*v.text) : nlohmann::json(nullptr)};
    };
    nlohmann::json frames = nlohmann::json::array();
    for (const Frame& fr : frames_) {
        nlohmann::json locals = nlohmann::json::array(), stack = nlohmann::json::array();
        for (const Value& v : fr.locals) locals.push_back(value(v));
        for (const Value& v : fr.stack) stack.push_back(value(v));
        const Program& p = *fr.object->program;
        frames.push_back({fr.object->uid, p.owner(fr.function), p.functionIndex(fr.function), fr.pc, std::move(locals), std::move(stack)});
    }
    return {{"frames", std::move(frames)}, {"steps", steps_}, {"wait", wait_}};
}

bool Machine::load(const nlohmann::json& j) {
    frames_.clear(), texts_.clear(), failure_.clear(), stateChanged_.reset(), starts_.clear();
    try {
        const auto value = [&](const nlohmann::json& a) {
            Value v;
            v.kind = Value::Kind(a.at(0).get<int>()), v.bits = a.at(1), v.uid = a.at(2), v.offset = a.at(3), v.frame = a.at(4);
            v.ptr = {a.at(5).get<uint32_t>(), a.at(6).get<uint32_t>()};
            if (!a.at(7).is_null()) v.text = &texts_.emplace_back(a.at(7).get<std::string>());
            if (int(v.kind) > int(Value::Kind::Constant)) throw std::runtime_error("value kind");
            return v;
        };
        for (const auto& f : j.at("frames")) {
            const auto object = resolve_(f.at(0).get<int32_t>());
            const ScriptType* type = object && object->program ? object->program->type(f.at(1).get<uint32_t>()) : nullptr;
            const size_t index = f.at(2);
            if (!type || index >= type->functions.size()) throw std::runtime_error("frame function");
            Frame& fr = frames_.emplace_back();
            fr.object = object.get(), fr.function = &type->functions[index], fr.pc = f.at(3);
            for (const auto& v : f.at(4)) fr.locals.push_back(value(v));
            for (const auto& v : f.at(5)) fr.stack.push_back(value(v));
        }
        steps_ = j.at("steps"), wait_ = j.at("wait");
        return true;
    } catch (const std::exception&) { // nlohmann errors derive from std::exception
        frames_.clear(), texts_.clear();
        return false;
    }
}

} // namespace ou::script::vm

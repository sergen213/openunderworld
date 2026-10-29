// Bounded cdc SCMD interpreter. Opcode semantics: tru.exe 0x57CE50; layouts in Script.hpp.
#pragma once

#include "script/Script.hpp"

#include <functional>
#include <deque>
#include <unordered_map>

namespace ou::script::vm {

class Program;
struct Instance {
    int32_t uid = 0;
    uint32_t typeId = 0;
    std::shared_ptr<Program> program;
    std::map<uint32_t, uint32_t> fields, initial;
    std::map<uint32_t, std::string> strings, initialStrings;
    std::map<uint64_t, uint32_t> properties, initialProperties; // (member offset << 32) | property id
};

class Program : public std::enable_shared_from_this<Program> {
public:
    explicit Program(std::vector<ScriptType> types);
    const ScriptType* type(uint32_t id) const;
    const Member* member(uint32_t typeId, uint32_t offset) const;
    const char* nativeName(uint32_t id) const;
    bool isA(uint32_t derived, uint32_t base) const;
    const Function* method(uint32_t typeId, int16_t vt) const;
    const Function* startup(uint32_t typeId) const; // first function if vt -1 (a vt 20 state callback is not run at birth: l7 puzzle_cheat_button 9279 lowered drawbridge 9116); state 0 (Run, 0x57B560) is not used [U: breaks the l2 elevator chain]
    const Function* state(uint32_t typeId, uint32_t n) const; // most-derived state table with entry n
    const Function* mainFunction(uint32_t typeId) const; // function 2: WAIT-based multibody polling script
    const Function* method(cdc::Pointer p) const; // Function record or Prototype
    std::shared_ptr<Instance> instantiate(int32_t uid, uint32_t typeId);
    uint32_t functionIndex(const Function* f) const;
    uint32_t owner(const Function* f) const;

private:
    std::vector<ScriptType> types_;
    std::unordered_map<uint32_t, size_t> byId_;
    std::map<std::pair<uint32_t, uint32_t>, std::pair<uint32_t, size_t>> methods_;
};

struct Value {
    enum class Kind : uint8_t { Scalar, Object, Local, Member, Constant } kind = Kind::Scalar;
    uint32_t bits = 0;
    int32_t uid = 0;
    uint32_t offset = 0, frame = 0;
    cdc::Pointer ptr{};
    const std::string* text = nullptr; // Constant string; owned by Instr or Instance::strings
    static Value scalar(uint32_t v) { Value x; x.bits = v; return x; }
    static Value object(int32_t uid) { Value x; x.kind = Kind::Object; x.uid = uid; return x; }
};

struct NativeContext {
    uint32_t typeId = 0;
    uint32_t function = 0;
    Value receiver;
    std::span<const Value> args;
    Value result;
    const char* name = "";  // native type name ("instanceref", ...)
    bool wait = false;      // set by a blocking native: the call is retried next tick (stack untouched)
};

class Machine {
public:
    using Resolve = std::function<std::shared_ptr<Instance>(int32_t uid)>;
    using Native = std::function<bool(NativeContext&)>; // false = unsupported binding
    using Event = std::function<bool(Instance&, int16_t vt)>; // true = virtual handler consumed inline
    Machine(Resolve resolve, Native native, Event event = {}) : resolve_(std::move(resolve)), native_(std::move(native)),
        event_(std::move(event)) {}
    bool call(Instance& object, int16_t vt, std::span<const Value> args = {}); // false: unsupported opcode/call/binding or budget exceeded
    bool start(Instance& object, const Function& f, std::span<const Value> args = {}); // a new thread running f (entered state)
    bool tick(float seconds);                // resume after SLEEP/WAIT; false on execution failure
    bool active() const { return !frames_.empty(); }
    const std::string& failure() const { return failure_; } // why the last call/tick failed ("op 0x78", "native x fn 3", ...)
    // A STATE ran on that object (uid): its other threads end (tru.exe 0x57C830).
    std::optional<int32_t> takeStateChange() { return std::exchange(stateChanged_, std::nullopt); }
    // States entered by objects called from another object's frames: the runtime starts each as its own thread.
    std::vector<std::pair<int32_t, const Function*>> takeStarts() { return std::exchange(starts_, {}); }

private:
    struct Frame {
        Instance* object = nullptr;
        const Function* function = nullptr;
        std::vector<Value> locals, stack;
        size_t pc = 0;
    };
    enum class Step { Continue, Complete, Suspended, Failed };
    Step run();
    Step step();
    bool push(Instance& object, const Function& f, std::span<const Value> args = {});
    int invoke(Instance& object, const Function& f, Value receiver, Frame& frame); // 0 failed, 1 inline, 2 child pushed
    std::shared_ptr<Instance> resolveObject(const Value& v);
    uint32_t load(const Value& v) const;
    Value read(const Value& v) const;
    bool store(const Value& v, uint32_t bits);
    bool write(const Value& destination, const Value& source);
    bool truth(const Value& v) const { return (load(v) & 0xFF) != 0; }

    Resolve resolve_;
    Native native_;
    Event event_;
    std::deque<Frame> frames_;
    size_t steps_ = 0;
    float wait_ = 0;
    std::string failure_;
    std::optional<int32_t> stateChanged_;
    std::vector<std::pair<int32_t, const Function*>> starts_;
};

} // namespace ou::script::vm

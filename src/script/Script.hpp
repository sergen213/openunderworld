// TRU scripted objects: cdc script types (DRM section type 8), intro script links, trigger volumes/planes,
// switches, doors and push blocks. Evidence, observed vs inferred: docs/formats/SCRIPTS.md.
// Frame: Z-up game units, world frame (unit render-terrain frame + the unit's streaming offset).
#pragma once

#include "anim/Anim.hpp"
#include "cdc/Cdc.hpp"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ou::collision { class CollisionMesh; }

namespace ou::script {

using glm::vec2;
using glm::vec3;

// ---- cdc script types (section type 8, ScriptTypeStreamData) -------------------------------------
// One bytecode word: opcode in the top byte (cdc SCMD_* numbering, as cdcEngineDXHR ByteCodeEnum.h),
// signed 24-bit argument; the relocated operand words that follow an opcode are attached to it.
struct Instr {
    uint8_t op = 0;
    int32_t arg = 0;
    std::vector<cdc::Pointer> ptrs; // CALLU/CALLn/PUSHC operands (function, prototype, constant)
    uint32_t ref = 0;               // ISTYPE operand: script type section id (resource reference)
    uint32_t imm = 0;               // PUSH operand word
};
inline constexpr uint8_t kOpPush = 0x02, kOpPushSA = 0x0F, kOpCallU = 0x65, kOpCall1 = 0x66, kOpCall5 = 0x6A,
                         kOpRet = 0x7B, kOpCount = 0x86;

struct Member {
    uint8_t type = 0;       // DataType byte: low nibble DTVOID..NTENUM (10 NATIVE, 11 SCRIPT), 0x80 = A<B>
    uint32_t typeId = 0;    // NATIVE/SCRIPT: script section id of the type (A for A<B>); 0 otherwise
    uint32_t argTypeId = 0; // A<B>: script section id of B
    uint16_t offset = 0;    // byte offset in the object instance
    uint16_t name = 0;      // symbol id (the PC data has no member names)
    std::optional<uint32_t> init; // first u32 of the initial value (instanceref: target intro unique id)
};

struct Function {
    int16_t vtIndex = -1; // prototype vtable slot; -1 = not virtual
    cdc::Pointer proto{}; // its prototype (CALLn operands point at prototypes)
    std::vector<Instr> code;
};

struct ScriptType {
    uint32_t id = 0, superId = 0; // section ids (0 = no super type)
    std::string nativeName;       // native-backed types only ("instance", "triggervolumebase", ...)
    uint16_t size = 0;            // instance data bytes
    std::vector<Member> members;
    std::vector<Function> functions;
};
// Parses drm.sections[index] (type 8). Throws cdc::FormatError on layout mismatch or unknown opcodes.
ScriptType parseScriptType(const cdc::Drm& drm, uint32_t index);

inline constexpr uint32_t kInstanceRefType = 51; // script id of native "instanceref" (game/script/game/game)
// Script vtable slots the runtime dispatches to. Triggervolume: [O] triggervolume.drm script 3484
// (Enter 54 / Exit 55 are called natively, they call OnFirstIn/OnIn and OnOut). Plane cross and switch
// use: [I] the slots almost every triggerplane / switch intro script overrides.
inline constexpr int16_t kVtOnIn = 70, kVtOnOut = 71, kVtOnFirstIn = 72, kVtPlaneCross = 54, kVtSwitchUsed = 55;

// Intro unique ids a function dereferences: PUSHSA of an instanceref member, in bytecode order, including
// the targets of non-virtual functions of the same type it calls (CALLn to one of their prototypes).
std::vector<int32_t> functionTargets(const ScriptType& t, const Function& f);

// ---- trigger shapes (intro data; tru.exe 0x5CD1B0 volumes, 0x5CCC20 planes) --------------------------
enum class ShapeKind : uint8_t { Box = 1, Sphere = 2, Plane = 3 };
struct Shape {
    ShapeKind kind = ShapeKind::Box;
    vec3 half{0};          // box: half extents; plane: x/z = half size of the local XZ rectangle
    float radius = 0;      // sphere
    bool oneSided = false; // plane: data+0x44 != 0
};
// Shape of a trigger volume (`plane` false) or plane from its intro data; nullopt if a volume's data
// has an unknown shape type. Throws cdc::FormatError on out-of-range reads.
std::optional<Shape> parseShape(const cdc::Drm& unit, const cdc::Pointer& data, bool plane);

// Both exe tests use the instance position + (0, 0, 200).
inline constexpr float kTestPointHeight = 200;
// Is `p` inside `s` placed at `pos` with Euler `rot` (R = Rx*Ry*Rz)? Planes: always false.
bool inside(const Shape& s, vec3 pos, vec3 rot, vec3 p);
// Does the segment a->b cross plane `s`? One-sided planes only count a->b going to local -Y [I].
bool crosses(const Shape& s, vec3 pos, vec3 rot, vec3 a, vec3 b);

// ---- runtime ------------------------------------------------------------------------------------------
enum class Kind : uint8_t { Volume, Plane, Switch, Door, Block };
enum class EventKind : uint8_t { Enter, FirstIn, In, Out, Cross, Use, Activate };
const char* eventName(EventKind k);
struct Event {
    EventKind kind;
    int32_t source = 0; // intro unique id of the object the event happened to
    int32_t target = 0; // Activate: the receiving intro (= source for the others)
    uint64_t tick = 0;
};

struct Object {
    int32_t uid = 0;
    std::string unit, name; // unit base name, objectlist.txt name
    Kind kind = Kind::Volume;
    vec3 pos{0}, rot{0};    // world frame (intro + unit offset), Euler radians
    Shape shape;            // Volume/Plane
    std::map<int16_t, std::vector<int32_t>> links; // vtIndex -> targets (intro script)
    // Animation (Switch/Door): skeleton of the object's render model + the clips its states play.
    std::shared_ptr<const scene::Skeleton> skeleton;
    std::shared_ptr<const std::vector<anim::Clip>> clips; // [0] transition (pulling/opening), [1] held idle
    vec3 half{100};         // Block: half size [I]
    // Runtime state (saved).
    int state = 0;          // Door: 0 closed, 1 opening, 2 open; Switch: 0 idle, 1 pulling, 2 used
    double t = 0;           // seconds into the current clip
    bool inside = false, firstIn = false;
    vec3 offset{0};         // Block displacement
};

struct UnitRef {
    std::string name; // base name ("l3_main_bedroom")
    std::shared_ptr<const cdc::Drm> drm;
    vec3 offset{0};   // world = unit frame + offset
};

struct TickInput {
    vec3 pos{0};   // Lara's root, world frame
    float yaw = 0; // facing: forward = (cos yaw, sin yaw, 0) (game::Player::forward)
    vec2 move{0};  // stick, y = forward
    bool interact = false;
};

class Runtime {
public:
    // objectlist.txt and object DRMs (animations). Without it units load no objects.
    void setBigfile(const cdc::BigfileSet* bigfile) { bigfile_ = bigfile; }
    // Keeps objects of units still present, adds new units, drops missing ones. Throws cdc::FormatError.
    void setUnits(const std::vector<UnitRef>& units);
    void addObject(Object o); // setUnits and tests; applies pending restored state
    void tick(const TickInput& in, const collision::CollisionMesh* col = nullptr);

    const std::vector<Object>& objects() const { return objects_; }
    const Object* find(int32_t uid) const;
    const std::vector<Event>& events() const { return events_; } // last tick, dispatch order
    bool holding() const { return holding_; }  // last tick: Lara grips a push block (interact held next to it)
    vec3 carried() const { return carried_; }  // last tick: displacement of the gripped block; move Lara by it
    uint64_t ticks() const { return tick_; }
    // Model-space bone palette of a posed object (animated Switch/Door; a displaced Block gets one matrix
    // for every vertex). nullopt = bind pose.
    std::optional<std::vector<glm::mat4>> pose(const Object& o) const;
    // Volume and plane outlines, world frame, ABGR colour (render::DebugLine).
    std::vector<std::pair<std::pair<vec3, vec3>, uint32_t>> debugLines() const;

    // State keyed by intro unique id (decimal string): loaded objects plus objects of units streamed out or
    // not loaded yet (kept and applied when their unit loads). restore drops state not in `j`.
    nlohmann::json serialize() const;
    void restore(const nlohmann::json& j);

private:
    void addUnit(const UnitRef& u);
    void fire(EventKind k, Object& o);
    Object* findMut(int32_t uid);

    const cdc::BigfileSet* bigfile_ = nullptr;
    std::map<uint16_t, std::string> objectNames_;
    struct AnimSet { std::shared_ptr<const scene::Skeleton> skeleton; std::shared_ptr<const std::vector<anim::Clip>> clips; };
    std::map<std::string, std::shared_ptr<AnimSet>> animSets_; // object name -> skeleton + clips
    std::vector<Object> objects_;
    nlohmann::json stash_ = nlohmann::json::object(); // state of objects not currently loaded
    std::vector<Event> events_;
    uint64_t tick_ = 0;
    vec3 lastPos_{0};
    bool hasLast_ = false, interactHeld_ = false, holding_ = false;
    vec3 carried_{0};
};

} // namespace ou::script

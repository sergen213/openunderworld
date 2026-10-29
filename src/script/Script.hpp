// TRU scripted objects: cdc script types (DRM section type 8), intro script links, trigger volumes/planes,
// switches, doors and push blocks. Evidence, observed vs inferred: docs/formats/SCRIPTS.md.
// Frame: Z-up game units, world frame (unit render-terrain frame + the unit's streaming offset).
#pragma once

#include "anim/Anim.hpp"
#include "cdc/Cdc.hpp"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ou::collision { class CollisionMesh; }
namespace ou::formats { struct MultiSpline; }

namespace ou::script {

namespace vm { struct Instance; struct Value; class Program; class Machine; }

using glm::vec2;
using glm::vec3;

// ---- cdc script types (section type 8, ScriptTypeStreamData) -------------------------------------
// One bytecode word: opcode in the top byte (cdc SCMD_* numbering, as cdcEngineDXHR ByteCodeEnum.h),
// signed 24-bit argument; the relocated operand words that follow an opcode are attached to it.
struct Instr {
    uint8_t op = 0;
    int32_t arg = 0;
    uint32_t word = 0;            // raw code-word offset (JMP/JMPF use these, including operands)
    std::vector<cdc::Pointer> ptrs; // CALLU/CALLn/PUSHC operands (function, prototype, constant)
    uint32_t ref = 0;               // ISTYPE operand: script type section id (resource reference)
    uint32_t imm = 0;               // PUSH operand word
    std::string text;               // PUSHC: relocated constant string
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
    bool inlineInit = false; // BOOL/INT/FLOAT default stored in the member record; not applied by instantiate [U: applying
                             // all of them changes l1 kraken-room scripts (GameReal.KrakenRoomRappel)]
    std::optional<uint32_t> init2; // terraingroup: model index (init's low u16 is the stream unit id)
    std::optional<cdc::Pointer> initAt; // NATIVE/SCRIPT: where the initial value lives (sound: inline SoundPlex)
};

struct Function {
    cdc::Pointer self{};  // Function record, used by CALLU/CALLn operands
    int16_t vtIndex = -1; // prototype vtable slot; -1 = not virtual
    cdc::Pointer proto{}; // its prototype (CALLn operands point at prototypes)
    uint16_t argCount = 0, localCount = 0;
    std::vector<Member> locals; // +0x0C: DataMember records (offset = byte offset in the frame, init = initial value)
    // Byte offset of each argument in the callee's locals (prototype +0x0C: 0x14-byte records, u16 +0x0C). Arguments
    // start at 0 in declaration order with their own sizes (natla_end_mover 19053 vt 0: native 20 bytes, int @20,
    // string @24); there is no `this` local.
    std::vector<uint16_t> argOffsets;
    uint8_t returnType = 0; // DataType low nibble; zero = void
    bool native = false;
    uint32_t wordCount = 0;
    std::vector<Instr> code;
};

struct ScriptType {
    uint32_t id = 0, superId = 0; // section ids (0 = no super type)
    std::string nativeName;       // native-backed types only ("instance", "triggervolumebase", ...)
    uint16_t size = 0;            // instance data bytes
    std::vector<Member> members;
    std::vector<Function> functions;
    // +0x40 (count: u8 +0x3E): Function record run on entering state n (tru.exe 0x57B560; Run() enters state 0)
    std::vector<cdc::Pointer> states;
    std::vector<int16_t> prototypes; // +0x34: declared prototypes' vtable slots (native types bind callbacks by index)
};
// Parses drm.sections[index] (type 8). Throws cdc::FormatError on layout mismatch or unknown opcodes.
ScriptType parseScriptType(const cdc::Drm& drm, uint32_t index);

inline constexpr uint32_t kInstanceRefType = 51; // script id of native "instanceref" (game/script/game/game)
// Script vtable slots the runtime dispatches to. Triggervolume: [O] triggervolume.drm script 3484
// (Enter 54 / Exit 55 are called natively, they call OnFirstIn/OnIn and OnOut). Plane cross and switch
// use: [I] the slots almost every triggerplane / switch intro script overrides.
inline constexpr int16_t kVtOnIn = 70, kVtOnOut = 71, kVtOnFirstIn = 72, kVtPlaneCross = 54, kVtSwitchUsed = 55;
// Pole levers (`*phole_lever*`): their script polls the lever's own multibody joint and calls vt 54 whenever it
// moved (l4_ballcourt 6502 fn 2 -> fn 3, which sets door 6505's joint limit) [O]; using one fires that slot [I].
inline constexpr int16_t kVtLeverMoved = 54;

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
enum class Kind : uint8_t { Volume, Plane, Switch, Door, Block, Scripted };
enum class EventKind : uint8_t { Enter, FirstIn, In, Out, Cross, Use, Activate, Start, Main, Checkpoint, CinematicEnd,
                                 PlayerMessage }; // PlayerMessage: target = INSTANCE_Post message id sent to Lara
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
    std::map<int16_t, std::vector<int32_t>> links; // vtIndex -> targets (legacy fallback when VM is unsupported)
    std::shared_ptr<vm::Instance> script;          // parsed intro type + mutable fields; null in synthetic tests
    std::shared_ptr<vm::Machine> machine;          // active handler; retained across SLEEP/WAIT ticks
    std::vector<std::shared_ptr<vm::Machine>> fibers; // earlier handlers still sleeping beside it (threads)
    bool mainPending = false;                      // start function 2 after the constructor finishes
    // Animation (Switch/Door): skeleton of the object's render model + the clips its states play.
    std::shared_ptr<const scene::Skeleton> skeleton;
    std::shared_ptr<const std::vector<anim::Clip>> clips; // [0] transition (pulling/opening), [1] held idle
    std::shared_ptr<const std::vector<std::string>> clipNames; // animation-list names of `clips` (clip effects)
    vec3 half{100};         // Block: half size [I]
    // Runtime state (saved).
    int state = 0;          // Door: 0 closed, 1 opening, 2 open; Switch: 0 idle, 1 pulling, 2 used
    double t = 0;           // seconds into the current clip
    bool inside = false, firstIn = false;
    bool tracksLara = false; // triggervolumebase Add(Lara): her enter/exit also call prototypes 10/11 (vt 58/59)
    vec3 offset{0};         // Block displacement
    glm::mat4 body{1};      // Door: model-space rigid transform set by src/physics (physics doors), identity = at rest
    bool hidden = false;    // instance.fn60 visibility; saved
    int32_t engagement = -1; // instance.fn59 engagement-property selector; saved
    // Spline mover (intro +0x68; tru.exe 0x5BA280 setup, 0x5CF260 per-frame update, `spline` natives 0x4AA8B0..):
    // the path moves the object (`body`), so its collision carries Lara like any moved dynamic.
    struct Spline {
        std::shared_ptr<const formats::MultiSpline> path;
        vec3 offset{0};          // unit streaming offset (world = path + MultiSpline::origin + offset)
        float t = 0;             // 30 Hz frames
        int dir = 1;             // +1 forward, -1 backward (0x5C7080)
        uint8_t mode = 0;        // 0 none, 1 once, 2 loop, 3 loop `count` times, 4 ping-pong (0x5C7100)
        bool playing = false;    // instance flag 0x2000000 (0x5C0950 / 0x5C0960)
        bool toKey = false;      // flag 1: play until `target`, then stop (0x5CC760)
        bool clip = false;       // flag 2: play range [from, to] (0x4AA930)
        int16_t target = 0, from = 0, to = 0, count = 0;
        std::vector<std::array<int32_t, 3>> ranges; // fn 19: {from, to, id}; entering/leaving one calls vt 40 / 41
    };
    std::optional<Spline> spline;
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
    void setBigfile(const cdc::BigfileSet* bigfile);
    const cdc::BigfileSet* bigfile() const { return bigfile_; }
    const vm::Instance* playerScript() const { return playerScript_.get(); }
    using MultibodyNative = std::function<bool(int32_t uid, uint32_t function, std::span<const vm::Value> args,
                                               uint32_t& result)>;
    void setMultibodyNative(MultibodyNative native) { multibody_ = std::move(native); }
    using MessageNative = std::function<bool(uint32_t channel, uint32_t id, std::string_view payload)>;
    void setMessageNative(MessageNative native) { message_ = std::move(native); }
    using UberNative = std::function<bool(int32_t uid, uint32_t function, uint32_t command, uint32_t commandIndex,
                                         std::span<const vm::Value> args, uint32_t& result)>;
    void setUberNative(UberNative native) { uber_ = std::move(native); }
    using TerrainNative = std::function<bool(int32_t uid, uint16_t streamUnitId, uint32_t modelIndex, uint32_t function,
                                             std::span<const vm::Value> args, uint32_t& result)>;
    void setTerrainNative(TerrainNative native) { terrain_ = std::move(native); }
    using LightNative = std::function<bool(int32_t uid, uint32_t lightId, uint32_t function,
                                           std::span<const vm::Value> args, uint32_t& result)>;
    void setLightNative(LightNative native) { light_ = std::move(native); }
    using HealthNative = std::function<bool(uint32_t function, std::span<const vm::Value> args, uint32_t& result)>;
    void setHealthNative(HealthNative native) { health_ = std::move(native); }
    // markup SetEnabled (0) / IsEnabled (1) on the markup with `id` of the calling object's unit (tru.exe 0x4A56B0).
    using MarkupNative = std::function<bool(const std::string& unit, uint32_t id, uint32_t function,
                                            std::span<const vm::Value> args, uint32_t& result)>;
    void setMarkupNative(MarkupNative native) { markup_ = std::move(native); }
    // "sound" natives (tru.exe thunks 0x79C570..0x79C5D0): 1/3 Play3D(anchor), 2/4 Play, 5 Stop. plex = the member's
    // initial value (an inline SoundPlex node in the owner's unit DRM); anchor = intro uid for 3D plays, 0 for 2D.
    using SoundNative = std::function<bool(int32_t owner, uint32_t member, cdc::Pointer plex, uint32_t function, int32_t anchor)>;
    void setSoundNative(SoundNative native) { sound_ = std::move(native); }
    // enemybase / generatorbase natives and health 1/2 on a non-player script, on the intro `uid` (World -> ai::Enemies).
    using EnemyNative = std::function<bool(int32_t uid, std::string_view type, uint32_t function,
                                           std::span<const vm::Value> args, uint32_t& result)>;
    void setEnemyNative(EnemyNative native) { enemy_ = std::move(native); }
    // Spline camera requests. op 0: autosplinecamera 0 (thunk 0x7B37D0 -> 0x7759F0 -> AutoSplineCamera 0x5E26F0)
    // plays camera spline `index` of `unit` (the cameraspline value: {initial data, stream unit of the owner, data
    // word 0}, 0x775760); `value` identifies it to cameraSplineDone. op 1: gamebase 52 (0x7767A0 -> CameraManager
    // +0x50 0x5EDFC0) switches to camera type `index`: 8 = the AutoSplineCamera, 1 = gameplay. op 2: gamebase 43
    // (0x776700) is camera type `index` the current one.
    using CameraNative = std::function<bool(int op, const std::string& unit, uint32_t index, uint32_t value)>;
    void setCameraNative(CameraNative native) { camera_ = std::move(native); }
    // The spline camera finished (0x5E2CA0 -> 0x77A3D0): every dispatcher (NsDispatcherBase, bind 0x7B3E60) runs
    // its prototype-7 slot with the cameraspline value.
    void cameraSplineDone(uint32_t value);
    // larabase 56 SetControlEnabledForReveal (0x777FC0: input flag 0x8AB4E6 zeroes the move axes).
    bool playerControl() const { return playerControl_; }
    // gamebase natives 4-7 = the global music variables (0x4A3E00/10/30/50 -> 0x583D40/30, array 0xCCB560).
    std::array<uint8_t, 256>& gameValues() { return gameValues_; }
    // Help-on-demand condition ("hint_NN") a unit script last stored: unit vt 28 copies its string argument into a
    // string member (l3_main_bedroom type 12059 +168) [V-data bytecode]. Empty when none.
    std::string helpCondition() const;
    // Keeps objects of units still present, adds new units, drops missing ones. Throws cdc::FormatError.
    void setUnits(const std::vector<UnitRef>& units);
    void addObject(Object o, bool start = true); // setUnits and tests; applies pending restored state
    void endCinematic(int32_t uid); // run the object's non-virtual post-stream helper
    // Lara pulled grapple target `uid` (an uber grapple target, script type 3565's subclasses). They override slots
    // 55..60 with handlers that send their uberobjectcommand members (levers 59/60, shiva arms 55/56, uber walls and
    // the naga statue 56/57, the snake head 57) [O]; which slot the exe calls on a pull is [I]: the first overridden
    // void one that does more than return runs, as its own thread. False if none ran to completion.
    bool grapplePulled(int32_t uid);
    void tick(const TickInput& in, const collision::CollisionMesh* col = nullptr);

    const std::vector<Object>& objects() const { return objects_; }
    const Object* find(int32_t uid) const;
    const std::vector<Event>& events() const { return events_; } // last tick, dispatch order
    uint64_t visibilityVersion() const { return visibilityVersion_; }
    // Lara's hidden draw groups (Instance +0x23C, tru.exe 0x5B3DD0): bit n-1 hides model primgroups of group n
    // (SubMesh::drawGroup). Changes bump visibilityVersion. Default: the Thor gear groups of lara.drm type 205
    // (+40/+44/+48 = 10, 11 gauntlets, 12 belt) hidden; instance native 61 from scripts shows/hides them.
    uint32_t playerHiddenGroups() const { return playerHiddenGroups_; }
    const std::set<std::string>& playerObjects() const { return playerObjects_; } // carried gear (g_belt, ...)
    void setPlayerObjects(std::set<std::string> objects) { playerObjects_ = std::move(objects); } // chapter starts
    uint32_t unitEffect() const { return unitEffect_; }
    // globaleffect FadeOut/FadeIn screen wipe: 0 = clear, 1 = black; moves by 1/frames each tick.
    float screenFade() const { return fade_; }
    bool holding() const { return holding_; }  // last tick: Lara grips a push block (interact held next to it)
    vec3 carried() const { return carried_; }  // last tick: displacement of the gripped block; move Lara by it
    uint64_t ticks() const { return tick_; }
    // Handler failures since construction: (intro uid, Machine::failure reason) -> count. The survey reads these.
    const std::map<std::pair<int32_t, std::string>, size_t>& failures() const { return failures_; }
    // Model-space bone palette of a posed object (animated Switch/Door; a displaced Block or a moved physics door
    // gets one matrix for every vertex). nullopt = bind pose.
    std::optional<std::vector<glm::mat4>> pose(const Object& o) const;
    void setBody(int32_t uid, const glm::mat4& model); // physics door transform (Object::body); unknown uids ignored
    // Volume and plane outlines, world frame, ABGR colour (render::DebugLine).
    std::vector<std::pair<std::pair<vec3, vec3>, uint32_t>> debugLines() const;

    // State keyed by intro unique id (decimal string): loaded objects plus objects of units streamed out or
    // not loaded yet (kept and applied when their unit loads). restore drops state not in `j`.
    nlohmann::json serialize() const;
    void restore(const nlohmann::json& j);

private:
    void addUnit(const UnitRef& u);
    void fire(EventKind k, Object& o, bool announce = true);
    Object* findMut(int32_t uid);
    void recordFailure(const Object& o);
    void recordFailure(int32_t uid, const std::string& name, const vm::Machine& m);
    void endOtherThreads(int32_t uid, const vm::Machine* keep);
    void adopt(const std::shared_ptr<vm::Machine>& m);
    std::shared_ptr<vm::Instance> instance(int32_t uid) const; // object, Lara or unit/global script by uid
    std::shared_ptr<vm::Machine> makeMachine(int32_t source);
    std::shared_ptr<vm::Instance> auxiliaryInstance(const std::shared_ptr<vm::Program>& program, uint32_t typeId);
    void tickSpline(Object& o);   // 0x5CF260: advance, clip / key / loop handling, range callbacks
    bool splineNative(Object& o, uint32_t function, std::span<const vm::Value> args, uint32_t& result);
    std::vector<std::pair<int32_t, std::shared_ptr<vm::Machine>>> threads_; // states entered from another object's call

    const cdc::BigfileSet* bigfile_ = nullptr;
    std::shared_ptr<vm::Instance> playerScript_;
    MultibodyNative multibody_;
    MessageNative message_;
    UberNative uber_;
    TerrainNative terrain_;
    LightNative light_;
    HealthNative health_;
    MarkupNative markup_;
    SoundNative sound_;
    CameraNative camera_;
    std::vector<std::string> cameraUnits_; // unit names by cameraspline value high word
    bool playerControl_ = true;
    uint32_t cameraSplineValue(const vm::Value& v); // (unit ordinal << 16) | spline index; 0xFFFFFFFF = unknown
    // Runs `uid`'s override of the prototype `prototype` of native base `base` (an engine callback) with `args`.
    void callback(int32_t uid, const char* base, size_t prototype, std::span<const vm::Value> args);
    EnemyNative enemy_;
    std::array<uint8_t, 256> gameValues_{};
    std::map<uint16_t, std::string> objectNames_;
    struct AnimSet { std::shared_ptr<const scene::Skeleton> skeleton; std::shared_ptr<const std::vector<anim::Clip>> clips;
                    std::shared_ptr<const std::vector<std::string>> names; };
    std::map<std::string, std::shared_ptr<AnimSet>> animSets_; // object name -> skeleton + clips
    std::vector<Object> objects_;
    std::map<std::pair<const vm::Program*, uint32_t>, std::shared_ptr<vm::Instance>> auxiliary_; // unit/global scripts
    int32_t nextAuxiliaryUid_ = -1;
    std::map<std::string, std::pair<const vm::Program*, uint32_t>> unitScripts_; // unit -> its NsUnit script instance key
    nlohmann::json stash_ = nlohmann::json::object(); // state of objects not currently loaded
    std::vector<Event> events_;
    uint64_t visibilityVersion_ = 0;
    uint32_t playerHiddenGroups_ = 0, playerGearGroups_ = 0; // current / default (setBigfile) hidden draw groups
    std::set<std::string> playerObjects_; // Thor gear objects Lara carries (larabase 37/38/42)
    uint32_t unitEffect_ = 0;
    uint64_t tick_ = 0;
    std::map<std::pair<int32_t, std::string>, size_t> failures_;
    int fallbackDepth_ = 0;
    struct Clock { int state = 0; float elapsed = 0, alarm = 0; bool fired = false; }; // state 0 stopped, 1 paused, 2 running
    std::map<uint64_t, Clock> clocks_; // (uid << 32) | member offset
    float fade_ = 0, fadeTarget_ = 0, fadeStep_ = 0;
    vec3 lastPos_{0};
    bool hasLast_ = false, interactHeld_ = false, holding_ = false;
    vec3 carried_{0};
};

} // namespace ou::script

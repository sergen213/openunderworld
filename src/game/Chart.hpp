// Lara's player state charts from lara.drm, run as tru.exe runs them (docs/re/PLAYER_CHART.md "Runtime").
//
// ChartData is the immutable chart as stored (states, transitions, conditions, properties, entry actions, events);
// ChartRuntime is tru.exe's chart agent (current state, state time, queued transition, the property values in force)
// with its evaluation order: 0x787E60 tick -> 0x787C50 evaluate -> 0x787570 find -> 0x785920 conditions ->
// 0x787890 change. Conditions, actions and property handlers come from a Registry keyed by the chart's ids; the
// game state they read comes through ChartContext. Families of conditions / actions live in
// src/game/chart/<Family>.cpp and are listed in src/game/chart/Families.cpp. A condition nobody ported is false
// (whatever its negation) and counted in ChartRuntime::stats; an unported action does nothing and is counted.
#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ou::cdc { class BigfileSet; }

namespace ou::game {
struct ObjData;
struct PlayerState;
struct Actions;
class Player;
class World;
} // namespace ou::game

namespace ou::game::chart {

// One condition of a transition (8 bytes): {s16 group, s16 id, s16 value, u8 flags}. `id` picks the function
// (registry 0x784C50); `value` is its parameter (a compare mode, an enum, an input action, a threshold); group 1000
// makes `value` an index into the chart variables (0x7820E0 / 0x782060); flags bit 0 negates. The group is otherwise
// only the condition's family number (100 buttons, 400 stick, 500 player, 600 markup ...).
struct Cond {
    int16_t group = 0, id = 0, value = 0;
    uint8_t flags = 0;
    bool negate() const { return flags & 1; }
    bool fromVariable() const { return group == 1000; }
};

// A state property (8 bytes): {u16 id, u16 type, u32 value}. `target`: where `value` points inside the player data
// (types 41 tuning record {u32 type, value}, 27 attack record, ...), -1 when it is not a pointer.
struct Property {
    uint16_t id = 0, type = 0;
    uint32_t raw = 0;
    int64_t target = -1;
};

// A transition (0x14 bytes): +0 target state, +2/+8 flags, +4/+0xC conditions (all must hold). Flags (0x787570 /
// 0x787890 / 0x7876E0): 1 fires only on the animation-end tick (else queued), 2 mirror on, 4 re-enter the same state,
// 5 mirror off, 6 take the queued transition instead, 7 deferred while the agent's +0x53C is set (queued) [U what sets it].
struct Transition {
    int16_t target = -1;
    std::vector<int16_t> flags;
    std::vector<Cond> conds;
    bool hasFlag(int16_t f) const;
};

// An event handler (0x10 bytes): {u32 event id, u8 result when a transition fired, u8 result when none did, u8 record,
// u16 count, -> {s16 transition, s16}[count]} (0x7876E0). Event 0x15 = the animation ended (0x768CF0).
struct Event {
    uint32_t id = 0;
    uint8_t onFire = 0, onMiss = 0, record = 0;
    std::vector<int16_t> transitions;
};

// A state (0x28 bytes): +2 anim request kind (0x444BA0: 0 / 1 = pattern, 2 = mirrored-aware), +4 request (< 0 none),
// +6 parent (its transitions, actions and properties are inherited), +0xA/+0x14 properties, +0xC/+0x18 actions,
// +0xE/+0x1C transitions, +0x10/+0x20 events.
struct State {
    int16_t requestKind = 0, request = -1, parent = -1;
    std::vector<Property> props;
    std::vector<uint16_t> actions;
    std::vector<int16_t> transitions, events;
};

// A chart variable / tuning record {u32 type, value}: 1 int, 3 float (frames), 4 float (distance), 5 bool.
struct Variable {
    uint32_t type = 0, raw = 0;
    int asInt() const;    // tru.exe 0x7820E0: floats truncated, bools 0 / 1
    float asFloat() const; // 0x782060
};

// lara.drm anim pattern = chart anim request (object DTP +0x60), with its first clip's name and length.
struct Request {
    std::string clip;
    float seconds = 0, rate = 1;
    uint8_t mode = 1, start = 0, blend = 0; // mode 1 once, 2 loop; frames at 30 Hz
};

// One chart header (0x28 bytes, player data +0x12500 / +0x12528): {s16 root, s16 agent, -> linked chart, u16 states,
// u16 transitions, -> states[], -> transitions[], -> events[], -> state pool, -> transition pool}.
struct ChartData {
    std::string name;
    int16_t root = 0, agent = 0;
    std::vector<State> states;
    std::vector<Transition> transitions;
    std::vector<Event> events;
    const State* state(int i) const { return i >= 0 && size_t(i) < states.size() ? &states[size_t(i)] : nullptr; }
};

struct Charts {
    ChartData player, upper;     // +0x12500 Lara (agent 0), +0x12528 upper body / weapons (agent 1)
    std::vector<Variable> variables; // pointer table at player data +0x18CC (400 entries)
    std::vector<Request> requests;
    // The player data block's bytes and its same-section pointers, for families that follow property pointers.
    std::vector<uint8_t> bytes;
    std::unordered_map<uint32_t, uint32_t> pointers;
    uint32_t dataOffset = 0; // the data block's start inside `bytes`

    int value(const Cond& c) const;     // the condition's parameter, a variable's when group 1000
    float valueF(const Cond& c) const;
    std::optional<uint32_t> pointer(uint32_t at) const;
    template <class T>
    T data(uint32_t offset) const { // a field of the player data block (offset from its start)
        T v{};
        if (size_t(dataOffset) + offset + sizeof(T) <= bytes.size()) std::memcpy(&v, bytes.data() + dataOffset + offset, sizeof(T));
        return v;
    }
    // SCPROPERTY_GetBool / GetInt / GetFloat (0x783770 / 0x783590 / 0x783680) of a property's value.
    bool propBool(const Property& p) const;
    int propInt(const Property& p) const;
    float propFloat(const Property& p) const;
};
std::optional<Charts> loadCharts(const ObjData& lara);
std::optional<Charts> loadCharts(const cdc::BigfileSet& bigfile);

class ChartRuntime;

// The game state the chart's conditions and actions read, over Player / World (WorldContext) or a test double.
class ChartContext {
public:
    virtual ~ChartContext() = default;
    // An input action's state (generalbank action map: 52 Jump, 50 / 51 / 101 Crouch, 42 Sprint, 99 / 32 Interact,
    // 46 Grapple, 25 Manual Aim, 102 Grenade ...). known = false: the action is not mapped (its conditions count as
    // unported). seconds: since it last changed (0 on the tick it changed).
    struct Action {
        bool known = false, down = false, edge = false;
        float seconds = 0;
    };
    virtual Action action(int id) const = 0;
    // The stick's level as tru.exe's move-speed condition reads it (player globals 0xFADFB0 -> +0xB0 -> +0x18) [I: the
    // stick's length 0..1].
    virtual float stickLevel() const = 0;
    virtual glm::vec2 stick() const { return {}; } // world-space wish direction, length <= 1
    virtual float fallDistance() const { return 0; } // player +0x6DC (z on leaving the ground) - z
    virtual const PlayerState* player() const { return nullptr; }
    virtual const World* world() const { return nullptr; }
    // The runtime evaluating (valid inside condition / action / property calls).
    ChartRuntime& chart() const { return *chart_; }

    // ---- group B ----
    // Chart-side copies of tru.exe player fields (player globals 0xFADB58 + offset: +0xEDC clearance class, +0xED4 /
    // +0xED8 attach surface class, +0x6C hurdle class, ...), written by the chart's actions and read by its
    // conditions. Shadow mode records action effects here instead of applying them to the live Player. Unset = 0.
    int& playerInt(int offset) { return playerInt_[offset]; }
    float& playerFloat(int offset) { return playerFloat_[offset]; }
    // Float fields of the chart agent (+0x374, +0x390, +0xF4, +0xF8, ...; ChartRuntime::field holds the int ones).
    float& agentFloat(int offset) { return agentFloat_[offset]; }
    // A tru.exe markup slot of the player: attached (+0xC94 markup, +0xC98 param, +0xCA0 grab point, +0xCC0 normal)
    // or jump target (+0xA64, +0xA68, +0x9F0, +0xA6C query result flags, +0xA80 target type).
    struct MarkupSlot {
        int markup = -1;          // index into World::collision().markup; -1 none (pointer 0)
        uint32_t flags = 0;       // markup +0x28 (collision::markupFlags of its type)
        float param = 0;          // segment index + fraction along it
        glm::vec3 point{0}, normal{0};
        int result = 0, type = 0; // jump target only: +0xA6C, +0xA80
        bool live = false;        // 0x5BE2D0: the markup rides a live instance (ours: moving markup)
        explicit operator bool() const { return markup >= 0; }
    };
    virtual MarkupSlot attachedMarkup() const { return {}; }
    virtual MarkupSlot jumpTarget() const { return {}; }
    // 0x5C3AA0: segment `seg` of markup `m` (start, end); nullopt when it has none.
    virtual std::optional<std::pair<glm::vec3, glm::vec3>> markupSegment(int, int) const { return {}; }
    virtual int markupSegmentCount(int) const { return 0; }
    // 0x493330 with ray flags 0x2C19E7 (the player code's line query) from a to b: the hit's distance from a, nullopt
    // without a hit. hasCollision() false: there is no world to ask (conditions using it count as unported).
    virtual std::optional<float> playerRay(glm::vec3, glm::vec3) const { return {}; }
    virtual bool hasCollision() const { return false; }
    virtual glm::vec3 position() const { return {}; }          // instance +0x20 (her root)
    virtual float heading() const { return 0; }                // instance +0x38, radians (forward = (cos, sin))
    virtual glm::vec3 rootBone() const { return position(); }  // instance bone matrices +0x90 -> +0x70
    virtual float controlYaw() const { return heading(); }     // 0x6EBA90 on 0xFADFB0 +0x50 (camera control yaw)
    virtual uint32_t gameMs() const { return 0; }              // 0xE7F154 game time, ms
    // The hang clearance class tru.exe 0x74E290 computes for the attached ledge (1, 2, 5, 8, 11, 12); 0 = unknown.
    // markupAttachInit (0x76D010) stores it in playerInt(0xEDC).
    virtual int ledgeClearanceClass() const { return 0; }
    // ---- end group B ----
    // ---- group A ----
    // Ground locomotion / landing (src/game/chart/Ground.cpp). nullopt = the game cannot say (the condition counts as
    // unported).
    // gameTracker_timeMult of the tick running (30 Hz frames; the shadow sets it before ChartRuntime::tick).
    float timeMult = 0.5f;
    // Seconds since action `id` was last pressed (the action record's +8 press time against the input clock +0x1C).
    virtual std::optional<float> sincePress(int) const { return std::nullopt; }
    // !0x7145D0: a ceiling lower than the standing height (player data +0x10 = 512.01) over her (condition 0x1F7).
    virtual std::optional<bool> lowCeiling() const { return std::nullopt; }
    // 0x6F2800: the combat stance (weapon drawn / aiming; condition 0x1FA).
    virtual std::optional<bool> combatMode() const { return std::nullopt; }
    // 0x74FD70: the sprint stamina allows a sprint (condition 0x205).
    virtual std::optional<bool> canSprint() const { return std::nullopt; }
    // 0x7594C0 == 2: a wall to plant her feet on ahead of the jump (condition 0x203).
    virtual std::optional<bool> wallJumpAttach() const { return std::nullopt; }
    // Agent +0x90 (game ms of the last message 0x1C9, too-steep ground; handler 0x76FC30) set and at most 99 ms ago
    // (condition 0x206).
    virtual std::optional<bool> wallSlideRecent() const { return std::nullopt; }
    // Player +0x68 bit 30, the fly cheat (condition 0x207); ours has no such mode.
    virtual bool cheatMode() const { return false; }
    // Effects the ground actions have on her anim layer, recorded chart-side (shadow mode does not apply them):
    // 0x438C40 playback rate of every sub-layer, the blend parameters +0x8C / +0x90 of the chart's anim section, the
    // procedural lean (player +0xF18 controller +0x40) and the root alignment blend of action 231 (0x75F550).
    struct AnimEffects {
        float rate = 1;
        float blend8C = 0, blend90 = 0;
        float lean = 0;
        struct Align { bool active = false, positionOnly = false; float duration = 0; glm::vec3 pos{0}; } align;
    } anim;
    // ---- end group A ----

private:
    friend class ChartRuntime;
    ChartRuntime* chart_ = nullptr;
    std::unordered_map<int, int> playerInt_;
    std::unordered_map<int, float> playerFloat_;
    std::unordered_map<int, float> agentFloat_;
};

using CondFn = bool (*)(ChartContext&, const Cond&); // the raw truth; the runtime applies the negation
using ActFn = void (*)(ChartContext&, uint16_t action);
using PropFn = void (*)(ChartContext&, const Property&);
// tru.exe's SC enum node functions (+0x20..+0x34): Entry, Reentry, PreAnimEntry, PreAnimReentry, Exit, Process.
struct ActionFns {
    ActFn entry = nullptr, reentry = nullptr, preEntry = nullptr, preReentry = nullptr, exit = nullptr, process = nullptr;
};

class Registry {
public:
    struct CondEntry { CondFn fn = nullptr; std::string name, family; };
    struct ActionEntry { ActionFns fns; std::string name, family; };
    struct PropEntry { PropFn fn = nullptr; std::string name, family; };
    void condition(int16_t id, CondFn fn, std::string name, std::string family);
    void action(uint16_t id, ActionFns fns, std::string name, std::string family);
    void property(uint16_t id, PropFn fn, std::string name, std::string family);
    const CondEntry* condition(int16_t id) const;
    const ActionEntry* action(uint16_t id) const;
    const PropEntry* property(uint16_t id) const;
    const std::map<int16_t, CondEntry>& conditions() const { return conds_; }
    const std::map<uint16_t, ActionEntry>& actions() const { return actions_; }
    // Every ported family (src/game/chart/Families.cpp) for Lara's chart (tru.exe tables 0x8B2CE8 / 0x8B2280 /
    // 0x8B0A50 over the base 0x8B57E0 / 0x8B6650 / 0x8B4200) and the upper-body chart (0x8B2838 / 0x8B26E0 / 0x8B1E00).
    static const Registry& player();
    static const Registry& upper();

private:
    std::map<int16_t, CondEntry> conds_;
    std::map<uint16_t, ActionEntry> actions_;
    std::map<uint16_t, PropEntry> props_;
};
void registerFamilies(Registry& player, Registry& upper); // src/game/chart/Families.cpp
// ---- group B ----
// Shared ports in src/game/chart/Ledge.cpp: 0x770BA0 (action 404's body: agent +0xF0 low 6 bits, the markup contact /
// lookahead probe) and 0x76D010 (per-markup-class attach init: player +0xEDC clearance class, agent +0x3E4), both run
// by action 420's entry.
void markupContactProbe(ChartContext& ctx);
void markupAttachInit(ChartContext& ctx);
// ---- end group B ----

// tru.exe 0x7607C0's value of an action: 1 on the press tick, frames held + 1 while down, -1 on the release tick,
// -1 - frames while up (frames = 30 Hz frames since the change, at least 1).
int actionValue(const ChartContext::Action& a);

// tru.exe's chart agent (0x7873C0) for one chart.
class ChartRuntime {
public:
    ChartRuntime(const Charts& charts, const ChartData& data, const Registry& registry);
    void start(ChartContext& ctx);                    // 0x787E00: root state, its entry, first evaluation
    void tick(ChartContext& ctx, float timeMult);     // 0x787E60: one game frame (timeMult = 30 Hz frames, 0.5 at 60 Hz)
    bool post(ChartContext& ctx, uint32_t event);     // 0x787CD0: an event; true when handled
    void animEnded(ChartContext& ctx);                // 0x768CF0: the requested animation ended

    const Charts& charts() const { return charts_; }
    const ChartData& data() const { return data_; }
    bool active() const { return active_; }
    int state() const { return state_; }
    int parent() const;
    int request() const;
    float stateTime() const { return stateTime_; }   // +0x508, 30 Hz frames
    float totalTime() const { return totalTime_; }   // +0x504
    int animCounter() const { return animCounter_; } // +0x24: < 0 playing, 1 the end tick, > 1 after
    bool mirrored() const { return mirror_; }
    int lastTransition() const { return lastTransition_; } // +0x518
    int queued() const { return queued_; }                 // +0x514
    uint32_t frame() const { return frame_; }              // +0x50C: condition caches go stale when it moves
    float animFrames() const { return animFrames_; }       // the requested clip's time (ours: chart-side clock)
    float animLengthFrames() const;                        // 0 = unknown / none
    bool animLoops() const;

    int value(const Cond& c) const { return charts_.value(c); }
    float valueF(const Cond& c) const { return charts_.valueF(c); }
    const Property* property(uint16_t id) const;       // the value in force (0x784B30), null when never set
    std::optional<bool> propBool(uint16_t id) const;
    std::optional<int> propInt(uint16_t id) const;
    std::optional<float> propFloat(uint16_t id) const;
    bool chainHasAction(int state, uint16_t action) const; // 0x781DC0

    // Per-condition cache, the SC enum node's {agent, frame, value} triple (+0x3C).
    struct Cache { uint32_t frame = ~0u; int value = 0; };
    Cache& cache(int16_t id) { return cache_[id]; }
    // Agent scratch fields some conditions keep (+0x70 move-dir zone, +0x74 move-speed zone, ...), by offset.
    int& field(int offset) { return fields_[offset]; }
    bool deferFlag7 = false; // agent +0x53C [U]
    // A condition function that cannot answer (an unmapped input action, a missing property): returns false and makes
    // the whole transition fail whatever the negation, counted in stats.unportedConds.
    bool unported(const Cond& c);

    struct Stats {
        std::map<int16_t, uint64_t> unportedConds;  // condition id -> evaluations
        std::map<uint16_t, uint64_t> unportedActions;
        uint64_t evaluations = 0, changes = 0;
    } stats;

private:
    bool holds(ChartContext& ctx, const Transition& t);
    int find(ChartContext& ctx, int state);      // 0x787570: transition index, -1 none
    void change(ChartContext& ctx, int t);       // 0x787890
    void evaluate(ChartContext& ctx);            // 0x787C50
    int fire(ChartContext& ctx, int state, uint32_t event, int& found); // 0x7876E0
    void applyProperties(ChartContext& ctx, int state); // 0x785E70 / 0x785CC0
    void enterAnim();                            // 0x783470
    void runExit(ChartContext& ctx, int from, int to);
    void runEnter(ChartContext& ctx, int to, int from, bool preAnim);
    void runProcess(ChartContext& ctx, int state);
    void call(ChartContext& ctx, uint16_t action, ActFn ActionFns::*which);
    const Transition* transition(int i) const;

    const Charts& charts_;
    const ChartData& data_;
    const Registry& reg_;
    bool active_ = false, mirror_ = false;
    int state_ = -1, queued_ = -1, lastTransition_ = -1, animCounter_ = 0;
    float stateTime_ = 0, totalTime_ = 0, animFrames_ = 0;
    uint32_t frame_ = 0, stamp_ = 0;
    bool unportedHit_ = false;
    std::unordered_map<uint16_t, const Property*> props_;
    std::unordered_map<uint16_t, uint32_t> propStamp_;
    std::unordered_map<int16_t, Cache> cache_;
    std::unordered_map<int, int> fields_;
};

// Shadow mode (ou-game --chart-shadow / --chart-trace): Lara's chart runs beside the hand-written controller with
// the same input and world; divergences of the requested clip from hers are logged, the contract TSV is written.
class Shadow {
public:
    Shadow(const cdc::BigfileSet& bigfile, std::optional<std::filesystem::path> trace, bool log);
    ~Shadow();
    void tick(const World& world, const Actions& a); // after World::tick
    std::string summary() const;
    const ChartRuntime& runtime() const { return *rt_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::unique_ptr<Charts> charts_;
    std::unique_ptr<ChartRuntime> rt_;
};

// ou-game chart-dump: the chart as text (one state, or all), and the coverage of conditions / actions reachable from
// Lara's root state (ported or not). Returns the process exit code.
// `names`: optional catalog JSON (W/chart/conditions.json, {"0x65": {"name": ...}}) for unported condition names.
int dump(const cdc::BigfileSet& bigfile, std::optional<int> state, bool coverage, bool upper, const std::filesystem::path* names,
         std::ostream& out);

} // namespace ou::game::chart

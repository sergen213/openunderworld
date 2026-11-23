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
#include <deque>
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
class PlayerDriver;
struct MarkupProfile;
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
    uint16_t word8 = 0; // record +0x8: 0x6EEDA0 mode 2 asks Lara's chart only when it is nonzero [U meaning]
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
    uint16_t keys = 0, keyTime = 0;         // the first clip's key count (+0x32) and time per key (+0x34; 100 = a frame)
    uint8_t mode = 1, start = 0, blend = 0; // mode 1 once, 2 loop; frames at 30 Hz
    uint8_t nextBlend = 0xFF;               // +0x1B: the next pattern's blend frames instead of its own (0xFF none)
    bool mirror = false;                    // +0x20 bit 0x80: a mirrored request (the clip reflected, XOR the agent's bit)
    // +0x17: the anim layer's blend type (layer +0xF9, anim::layerBlendWeights) and every anim of the pattern by name
    // (sub-layer i plays clips[i]; clips[0] = clip).
    uint8_t blendType = 0;
    // +0x18: the crossfade window's motion (0x441FB0 records r = *(L+0x7C) by it, 0x43B870 moves her by it each step
    // while the window runs) [O decomp]. 0: the incoming clip's root displacement over the window (r+0x10 = root at
    // start - blend, r+0x20 = root at start, 0x4358E0), spread by the curve's step. 1: lerp(|outgoing root speed|,
    // |incoming root speed at its start|) along her forward axis. 2: the same lerp with both velocities as vectors (the
    // outgoing in the incoming's frame). 3: a carried global velocity [U, not ported: falls back to 1].
    uint8_t window = 1;
    std::vector<std::string> clips;
    std::vector<uint16_t> clipKeys; // each clip's key count (+0x32; 0 unknown), for 0x433360's synced rates
    // The first clip's root speed in units per 30 Hz frame as action 501 (0x75BD30) reads it through 0x4389C0: -(header
    // +0x1C, the clip's whole root displacement y) x 100 / (keys x time per key); 0 unknown.
    float rootSpeed = 0;
    // 0x4428A0 -> 0x441FB0: the clip's time when it starts, in 30 Hz frames: start - blend, uncut, after the first clip's
    // key bump (blend > 0, a key longer than a frame, start inside the first key: start = that key, blend += the move).
    // blendOverride: the previous pattern's +0x1B (layer +0x314) when it set one.
    float startFrame(int blendOverride = -1) const {
        int st = start * 100, bl = (blendOverride >= 0 ? blendOverride : blend) * 100;
        if (bl > 0 && keyTime > 100 && st < keyTime) bl += keyTime - st, st = keyTime;
        return float(st - bl) / 100.0f;
    }
    // 0x438930: the clip length AnimationEnded tests the time against, keys x time per key rounded up to whole keys.
    float lengthFrames() const {
        if (!keys || !keyTime) return seconds * 30.0f;
        return float((int(keys) * (keyTime + 1) - 1) / keyTime * keyTime) / 100.0f;
    }
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
    // Kind-2 anim requests (0x444BA0 -> 0x443720 -> 0x43E250) name a node of the object's anim graph: ObjectBase
    // +0x4C count / +0x50 table of {u32 DTP section id, u32}; each node is a DTP of the anim graph VM's bytecode
    // (0x432B20) whose header +4 / +8 point at its enter / exit programs. code: the node's words; enter: word index.
    struct GraphNode { std::vector<uint32_t> code; uint32_t enter = 0; };
    std::vector<GraphNode> graph;
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
    // gameTracker_timeMult of the tick running (30 Hz frames; the shadow sets it before ChartRuntime::tick).
    float timeMult = 0.5f;
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
    // The stores as a whole (a save's chart state, ChartShadow.cpp).
    const std::unordered_map<int, int>& storedInts() const { return playerInt_; }
    const std::unordered_map<int, float>& storedFloats() const { return playerFloat_; }
    const std::unordered_map<int, float>& storedAgentFloats() const { return agentFloat_; }
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
    // 0x5C3AA0: segment `seg` of markup `m` (start, end); an index past the last segment gives the last one; nullopt
    // when it has none.
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
    // 0x751230 / 0x74BB50 == 1: the ledge point at `param` of markup `m` tops a wall-hug shelf; nullopt unknown.
    virtual std::optional<bool> ledgeShelf(int, float) const { return {}; }
    // 0x73DFF0's class of what lies past ledge point q along f (player +0x6C); nullopt unknown.
    virtual std::optional<int> hurdlePastClass(glm::vec3, glm::vec2) const { return {}; }
    // Effects the chart's actions have on the animation and camera (shadow: recorded only): the played clip's 2D
    // blend parameters (anim section +0x8C / +0x90), every layer's rate (0x438C40), the camera mode pushed by
    // CameraManager vfunc +0xB0 (-1 none, popped by +0xB4).
    glm::vec2 animBlend{0};
    float animRate = 1;
    int cameraMode = -1;
    // The chart's own attached markup (player +0xC94 as the chart's actions set it: the class setters of action 230 and
    // action 232 attach the jump target through 0x707B90, 0x6FDBE0 detaches; see attachChartMarkup). Engaged: it
    // answers attachedMarkup() (an empty slot = none) instead of the game; nullopt: the context's game state answers.
    std::optional<MarkupSlot> chartAttached;
    // The upper-body chart (player +0xF14) for 0x6EEF00 mode 2's fallback (0x6EEDA0); null when there is none.
    virtual const ChartRuntime* upperChart() const { return nullptr; }
    // Agent +0x345, the pole side byte 0x74C8D0 hands the anim graph VM context (0x6E9EB0, VM context +0x10) that
    // native 64 (0x6EA050) reads; nullopt until the setter ran with an answer.
    std::optional<uint8_t> vmPoleSide;
    // The anim layer the beam balance (0x743900) drives: instance +0x280 -> +0x14. Its blend type (+0xF9: 2 one axis,
    // 5 / 7 two axes; nullopt: no layer blend modelled) and its blend parameters +0x8C / +0x90. The default context
    // has none (the balance then only keeps its own state).
    virtual std::optional<int> layerBlendType() const { return std::nullopt; }
    virtual void setLayerBlend(float p8C, float p90) { (void)p8C, (void)p90; }
    // Instance +0x38 set outright (the beam balance's turn-around, 0x743D85..0x743E0A).
    virtual void setHeading(float yaw) { (void)yaw; }
    // Replay only (--chart-rng): tru.exe's generator results (0x597780, the generator object 0x8A9C78) recorded under Wine,
    // in call order, for the actions that draw from it (action 301's wobble). Empty: ours' own generator (live play).
    std::deque<uint32_t> rngFeed;
    // ---- end group B ----
    // ---- group A ----
    // Ground locomotion / landing (src/game/chart/Ground.cpp). nullopt = the game cannot say (the condition counts as
    // unported).
    // Seconds since action `id` was last pressed (the action record's +8 press time against the input clock +0x1C).
    virtual std::optional<float> sincePress(int) const { return std::nullopt; }
    // !0x7145D0: a ceiling lower than the standing height (player data +0x10 = 512.01) over her (condition 0x1F7).
    virtual std::optional<bool> lowCeiling() const { return std::nullopt; }
    // 0x1FD's "on a sliding slope" (player +0x11D0 > 0 and byte +0x11CF): drive mode asks the floor under her (a slide
    // face); nullopt = take it from the controller's state (shadow).
    virtual std::optional<bool> onSlideFloor() const { return std::nullopt; }
    // 0x6F2800: the combat stance (weapon drawn / aiming; condition 0x1FA).
    virtual std::optional<bool> combatMode() const { return std::nullopt; }
    // 0x74FD70: the sprint stamina allows a sprint (condition 0x205).
    virtual std::optional<bool> canSprint() const { return std::nullopt; }
    // 0x7594C0 == 2: a wall to plant her feet on ahead of the jump (condition 0x203).
    virtual std::optional<bool> wallJumpAttach() const { return std::nullopt; }
    // Agent +0x90 (game ms of the last message 0x1C9, too-steep ground; handler 0x76FC30) set and at most 99 ms ago
    // (condition 0x206).
    virtual std::optional<bool> wallSlideRecent() const { return std::nullopt; }
    // Player +0xF3C, the support foot (0x6EB630): animation events 0x34 / 0x36 of her clip set 1 / 2 (swapped while the
    // clip plays mirrored, 0x6EDD00), 0x35 sets 0 (handler 0x716F30). Condition 0x12D.
    virtual std::optional<int> supportFoot() const { return std::nullopt; }
    // The stick record's angle relative to her heading (0xFADFB0 -> +0xB0 -> +0x10, radians, + = her left), composed
    // as tru.exe does: the pad direction in the camera frame plus the control yaw against her heading. nullopt: derive
    // it from stick() (world space).
    virtual std::optional<float> stickRelative() const { return std::nullopt; }
    // Player +0x68 bit 30, the fly cheat (condition 0x207); ours has no such mode.
    virtual bool cheatMode() const { return false; }
    // Effects the ground actions have on her anim layer beyond group B's animBlend / animRate, recorded chart-side
    // (shadow mode does not apply them): the procedural lean (player +0xF18 controller +0x40) and the root alignment
    // blend of action 231 (0x75F550).
    struct AnimEffects {
        float lean = 0;
        struct Align { bool active = false, positionOnly = false; float duration = 0; glm::vec3 pos{0}; } align;
    } anim;
    // Lara +0x11D4 (0x208 PSCCF_OnStairs): written only by the ground check 0x5DCE20 when it runs (cleared, then set from
    // its floor hit through 0x5D3AF0(hit, 0x100, 0x20)), kept otherwise; nullopt: the last check stood on an object
    // whose own flags the context does not model.
    virtual std::optional<bool> onStairs() const { return std::nullopt; }
    // 0x198's downhill vector (0x7308B0) reads player +0x5F0, the normal of the last too-steep face (message 0x1C9,
    // kept until the next), then +0x600, the last ground check's support normal (kept while no ground is found); both
    // zero until written. nullopt: the context cannot say.
    virtual std::optional<glm::vec3> steepFaceNormal() const { return std::nullopt; }
    virtual std::optional<glm::vec3> supportFaceNormal() const { return std::nullopt; }
    // ---- end group A ----

    // ---- group J ----
    // Jump / air / combat-edge / carry / water-depth reads of the group-J conditions (src/game/chart/Jump.cpp). nullopt
    // = the context cannot answer (the condition counts as unported, never a guess).
    virtual std::optional<bool> noFloor() const { return {}; }       // instance +0x14 bit 19: 0x5DCE20 found no floor
    virtual std::optional<glm::vec3> velocity() const { return {}; } // instance +0x2A0, units per 30 Hz frame
    virtual std::optional<float> boneRise() const { return {}; }     // bone 0 z of matrices +0x90 minus previous +0x94
    virtual std::optional<int> wallJumpProbe() const { return {}; }  // 0x7594C0: 0 no wall, 1 wall not jumpable, 2 jump
    virtual std::optional<bool> wallJumpAnticipate(float) const { return {}; } // 0x759260 with that many frames' lookahead
    // 0x710380: the two markup candidate slots player +0x3A0 / +0x3E0 (ledges and hurdles, flags 0x40010000) the move
    // sweep refreshes every frame; empty slots have markup -1. nullopt: not known.
    struct Candidates { MarkupSlot a, b; };
    virtual std::optional<Candidates> jumpCandidates() const { return {}; }
    // The take-off states' jump query (0x6C0800 -> 0x7725A0 -> 0x771DB0 fill player +0xA64 while she takes off): kind 0
    // standing (q2), 1 with the stick (q43), 2 running (q26); the target with its +0xAC0 flags and +0xA80 type.
    // frames: 30 Hz frames to the hit; uncertain: +0xAC0 bits ours could not settle (their result entries' tests)
    struct Aim { MarkupSlot target; uint32_t flags = 0; int type = 0; float frames = 0; uint32_t uncertain = 0; };
    virtual std::optional<Aim> takeoffAim(int) const { return {}; }
    // ---- group A ---- tru.exe 0x7296C0 on a decoded query profile (Player::markupSearch): the target met (an empty
    // target: none) with its +0xAC0 flags and +0xA80 type; nullopt: the context cannot model this search
    // (chartMarkupSearch).
    virtual std::optional<Aim> markupSearch(const MarkupProfile&) const { return {}; }
    // The camera's axes in world space as tru.exe's camera matrix rows hold them (0x5E7850, g_camera +0x10, built by
    // 0x5E0970 -> 0x5E0900 -> 0x49B820 from the position +0x90 and the look vector +0x160: row 0 right = down x look,
    // row 1 the camera's down, row 2 the look direction) and the stick record's pad (+0x00 / +0x04: x right, y down; a
    // key direction t reads (cos t, -sin t)); nullopt: not modelled.
    struct CameraAxes { glm::vec3 right{1, 0, 0}, down{0, 0, -1}, look{0, 1, 0}; };
    virtual std::optional<CameraAxes> cameraAxes() const { return {}; }
    virtual std::optional<glm::vec2> pad() const { return {}; }
    // The chart's own jump target (player +0xA64 as the chart's actions set it: action 504's take-off query, action
    // 230's promotion); empty: the context's jumpTarget() answers.
    Aim chartTarget;
    // Player +0xA40 (the query object's +0xC0): the velocity a mode-1 search profile flies (0x76FFC0), as 0x74C850
    // writes it from 0x744310 before its profile-27 search; nullopt: the search keeps its own source (group J).
    std::optional<glm::vec3> queryVelocity;
    // Player +0x9F0, the jump target's point: clearing the target (+0xA64 = 0: the attach 0x707730, the flight's drop,
    // the take-off's new query) leaves it [O: 0x749CD0 reads it after action 236's attach cleared +0xA64]; the last
    // target's point (chartTargetPoint answers with the live target's while there is one).
    glm::vec3 targetPoint{0};
    // 0x437120's pending alignment (DAT_008C9F20 / DAT_008C7020), as a kind-1 AnimTarget setter leaves it through
    // 0x702D20: the displacement v in her frame and the turn `yaw` (radians) that the next mode-3 request's crossfade
    // window carries her through (docs "Crossfade", mode 3); taken (pending cleared) when that window starts.
    // `ladderRung`: action 505 (0x74D980) armed a LadderRungAlignAnimTarget for the state's graph-node clip;
    // `ledgeHang`: 0x74ED30's kind-2 branch armed a PrimAirAttachAnimTarget (0x74BF90) for it.
    struct Align { glm::vec3 v{0}; float yaw = 0; bool pending = false, ladderRung = false, ledgeHang = false; } align;
    // 0x702D20 for the state's kind-1 request: aim it at the hold pose of the markup just attached (the family's
    // AnimTarget: 0x74DCD0 for a ladder), so the request's window and the clip's own root motion end there. The drive
    // context answers; elsewhere nothing.
    virtual void alignToHold(const MarkupSlot&) {}
    // Action 505 on entry / re-entry (0x74D980, kind 2 -> 0x6F0FF0 with a LadderRungAlignAnimTarget, whose slot 1
    // 0x744B70 aims the graph node's next clip at the rung she holds). The drive context answers.
    virtual void ladderRungAlign() {}
    // Action 505's exit (0x734920): her root onto the rung root nearest it (0x72B8C0), facing the wall. The drive
    // context answers.
    virtual void ladderRungSnap() {}
    // Action 230's ledge class 610 (0x74ED30) for a state whose request is a graph node (kind 2): 0x74BF90 builds a
    // PrimAirAttachAnimTarget whose slot 1 (0x750780 -> 0x741130) aims the node's next clip at the hang root. The
    // drive context answers.
    virtual void ledgeHangAlign() {}
    // The game's jump-target markup the chart already handed on when its flight ended (tru.exe moves +0xA64 to the
    // attached slot +0xC94 on the attach, 0x741050 / 0x707500) [I]; ignored as +0xA64 until the game's target changes.
    int handedTarget = -1;
    virtual std::optional<bool> combatFlag(int) const { return {}; } // (player +0x504) +0x1FC bit n
    virtual std::optional<int> combatObjectMode() const { return {}; } // (player +0x504) +0x50
    virtual std::optional<bool> upperProp(uint16_t) const { return {}; } // upper chart's current-state bool property (0x6EEF00)
    virtual std::optional<bool> carrying() const { return {}; }      // player +0x858 (portable) != 0
    virtual std::optional<bool> carriedEncumbers() const { return {}; } // the carried portable's data byte +0x18 (encumbers her)
    // The jump target's query result flags (player +0x980 +0x140 = +0xAC0, cond 0x2BA) and target type (+0xA80, cond 0x294
    // group 661), both 0 without a target; nullopt: the chart-side stores (playerInt) answer.
    virtual std::optional<uint32_t> jumpQueryFlags() const { return {}; }
    virtual std::optional<int> jumpTargetType() const { return {}; }
    // The grapple component (owner queries 0x175 / 0x176): attached = query 0x17C non-null, the attach point
    // (0x4D8D90 +0x80), rope length (+0xCC) and its limit (+0xC8).
    struct Grapple {
        bool component = false, attached = false;
        glm::vec3 anchor{0};
        float rope = 0, ropeMax = 0;
    };
    virtual std::optional<Grapple> grapple() const { return {}; }
    // Water under her (0x764340 / 0x764550): inWater = player's water block present; depth = -(player +0x914) + waves.
    struct Water {
        bool inWater = false;
        float depth = 0;
    };
    virtual std::optional<Water> water() const { return {}; }
    // Effects of group-J actions and side-effect conditions on the game. Shadow mode records them (never applied to
    // the live Player); `value` is the effect's parameter.
    enum class JEffect : uint8_t {
        CameraPush, CameraPop, CarriedPost, WeaponFlag, AnimEvent, PlayResource, StateMessage, LandDamage,
        LandImpulse, HeldLaunch, InteractNotify, PairedAttack, RollNotify, WrestleHelper, HairFrame, TuningValue,
        WaterDamage, AirMeter, CollisionMask, ClearJumpTarget, DetachMarkup, Rotation, CameraVariation, YawLimit,
        ComboMeter, TargetPromote, AttachTarget, ReAim, kCount
    };
    virtual void effect(JEffect, float = 0) {}
    // Drive mode (ChartShadow.cpp Drive): called by ChartRuntime::change / enter after the new state's anim request
    // started (0x783470) and before its actions' Entry / Reentry (0x785A30), so that what the drive applies on a state
    // change (the flight's launch velocity) is in force when the new state's Entry reads it (action 510's 0x7522A0
    // tests the instance velocity's z) [I: tru.exe's launch site is not found; Wine shows the velocity on the
    // flight state's first row].
    virtual void stateEntered(int /*from*/, int /*to*/) {}
    // Player +0x31C: the move sweep's record (0x5D7FA0 -> 0x5D7A80), an index into Lara data +0x630's 0x84-byte
    // records (count +0x62C); kept from the last sweep that found one, -1 none yet. tru.exe also clears it [O, not
    // modelled]: a sweep that finds no record after the direction loop cleared local_334 (the edge / wall push block,
    // local_331 && push depth local_324 > 0) falls into 0x5D7FA0's (param_3 & 4) block, which stores +0x31C = 0 with
    // +0x320 = 1; ours keeps the old record there. Ours runs no push-depth block, so it cannot tell that path [U];
    // nullopt: the context does not run the sweep. Byte +0x2C5: the sweep's jump-candidate query (0x710380) found one.
    virtual std::optional<int> moveSweepRecord() const { return {}; }
    virtual std::optional<bool> sweepCandidates() const { return {}; }
    // Byte +0x320 (0x5D7FA0, written by group A's request): 1 when this update's sweep stored its record in +0x31C, 0
    // when it found none or did not run (0x7002D0 copies collision mask bits 0x200 / 0x800 into +0x2C1 / +0x2C0; both
    // clear: no sweep, +0x320 = 0). nullopt: this update's sweep ended on a case the context does not resolve. Not
    // modelled [U]: the tail's alternate stores (local_2e9 -> the re-cast record, local_32a -> local_2e4) and, with
    // local_334 cleared, 0x5D7FA0's (param_3 & 4) block writing +0x31C = the (empty) record with +0x320 = 1 — both
    // give the +0x320 readers a record of 0 there.
    virtual std::optional<bool> sweepFresh() const { return {}; }
    // ---- end group J ----
    // ---- group CD ----
    // Climb / ladder / pillar / perch / pole / beam / wall hug / slide / climb-out reads (src/game/chart/Climb.cpp).
    // nullopt = the context cannot say (the condition counts as unported).
    // Player +0x8EC, the water flags owner message 9 returns (0x5B36F0 -> case 9): 0x700530 stores 0x400 (dry) outside
    // water (player globals 0xFADA74 bit 0x40 clear), else 0x5D77E0 -> 0x5D3860's class (0x10 / 0x20 / 0x40 swimming,
    // 0x80, 0x100 wading, 0x200 shallow, 0x400 dry) with the volume bits 0x800 / 0x1000 / 0x2000 / 0x4000.
    virtual std::optional<uint32_t> waterFlags() const { return {}; }
    // (player +0x500) +0x134, the inventory slot in her hands: -1 none (0x6E8FC0 holsters), 0 pistols, 1 the secondary,
    // 2 Thor's hammer (weapon data +0xDC; 0x704A40 draws), 3 / 7 / 12 other items [U which].
    virtual std::optional<int> gearSlot() const { return {}; }
    // Player +0x78 is set / cleared by anim event commands 0x13 / 0x14 on the chart's anim section (0x716F30, which also
    // sets / clears agent +0x53C); 0x768C50 copies it to agent +0x6C after the chart tick. Kept chart-side in
    // playerInt(0x78) and ChartRuntime::field(0x6C) by whoever plays the chart's clip; false = nobody does.
    virtual bool tracksCriticalSection() const { return false; }
    // 0x493330 from a to b with ray flags `flags` (0x442C11E3, 0x2C19E7 ...): kind 1 = static world (record +0 == 1), 2 =
    // an instance (== 2), with the hit point (+0x10) and normal (+0x20). `stillBody`: kind 2 only, the instance has a
    // physics body (0x4D9380 +0x274) at rest (velocity and spin squared < 1, 0x4D9C80 / 0x4D8E70) and not 0x4D9300;
    // `flag8000000`: its object type flags +0x10 -> +0 bit 0x8000000. nullopt fields: unknown. No value: no hit
    // (hasCollision() tells whether there is a world to ask).
    struct RayHit {
        int kind = 1;
        glm::vec3 point{0}, normal{0};
        std::optional<bool> stillBody, flag8000000;
    };
    virtual std::optional<RayHit> rayHit(glm::vec3, glm::vec3, uint32_t) const { return {}; }
    // -(player +0x910) [U what: the catalog's floor depth, group J's head height]; 0x764550's class 805 needs it >= +0xB2.
    virtual std::optional<float> waterFloorDepth() const { return {}; }
    // Effects of group-CD actions, recorded chart-side (shadow mode never applies them): action 214's second blend slot
    // (anim section +0x3AC / +0x3B0), action 803's audio flag 0xCCDEA4 (fades a bus while set), action 1409's global
    // deadline 0x8B083C (game ms; condition 0x5AE).
    glm::vec2 blendLatch{0};
    bool audioFade = false;
    float deadline8B083C = 0;
    // ---- end group CD ----

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
// 0x707B90 after 0x6FDBE0: detach the chart's attached markup (player +0xEA0 / +0xEA4 keep it and its param) and
// attach `slot` (the jump target), when the context keeps a chart-side attached markup (ChartContext::chartAttached).
void attachChartMarkup(ChartContext& ctx, const ChartContext::MarkupSlot& slot);
void detachChartMarkup(ChartContext& ctx); // 0x6FDBE0 alone
// Action 230's target classes whose setter attaches the jump target (the callers of 0x707B90 in its class switch:
// 600 0x753510, 601 0x750F00, 603 0x74C8D0, 605 0x751130, 606 0x753F80, 610 0x74ED30 / 0x7571B0, 614 0x7503B0, 621
// 0x74E5A0, 624 / 626 0x75F1B0, 625 0x74C110, 627 0x73DD70) [O].
bool attachTargetClass(int cls);
// 0x74F480: a wall under the hang at the markup point (`slot.param`): the ray 0x744E90 in the segment's frame, at a vertex
// 40 into either segment; nullopt without collision. 0x75D6C0 makes the attach surface class (+0xED4) !result for
// ledges (no wall: 1, a free hang).
std::optional<bool> ledgeBrace(ChartContext& ctx, const ChartContext::MarkupSlot& slot);
// 0x74C8D0: the class-603 attach setter of action 230 (PreAnimEntry 0x772CA0, `alt` = the class came from property
// 0x1A4). Attaches the jump target, decides how she takes the pole (agent +0x344), the side byte agent +0x345 for the
// anim graph VM (ChartContext::vmPoleSide) and the layer mirror (+0xF4 0x20, ChartRuntime::setMirror).
void poleAttachSetter(ChartContext& ctx, bool alt);
// 0x74C110: the class-625 (beam) attach setter of action 230: attaches the jump target, sets the facing along the beam
// (+0xED0 / +0xCDC), the beam controller's segment and angle (agent +0x360 / +0x35C) and +0xCB0 / +0xCC0.
void beamAttachSetter(ChartContext& ctx);
// 0x752870: the class-618 (`high` false) / 619 (`high` true) free-climb hold attach setter of action 230: attaches the
// jump target when it is a hold (MUD 0x8000) and grips it (holdGrip).
void holdAttachSetter(ChartContext& ctx, bool high);
// 0x752870's grip on hold `s` (3 points): +0xC98 / +0xCA0 = the higher of points 1 / 2 (param 1.0 / 1.99999), +0xCC0
// = the hold's normal, and the root offset +0xCE0 / +0xCE8, so the pin (0x6FDC90) holds her root 158.025 under point 0.
void holdGrip(ChartContext& ctx, ChartContext::MarkupSlot& s, bool high);
// 0x743900: action 205's per-tick beam balance steer (init on entry): agent +0x3A0 lean, +0x3A4 walk amount, +0x28
// heading target, the layer blend (ChartContext::setLayerBlend) and mirror (src/game/chart/Ledge.cpp).
void beamBalanceTick(ChartContext& ctx, bool init);
// ---- end group B ----
// ---- group J ----
// Player +0xA64 as the chart sees it (src/game/chart/Jump.cpp): the target the chart's own actions set
// (ChartContext::chartTarget: action 504's take-off query, action 230's promotion), else the game's jumpTarget() unless
// the chart already handed that one on when its flight ended (ChartContext::handedTarget).
ChartContext::MarkupSlot chartJumpTarget(ChartContext& ctx);
// Clears the chart's jump target (+0xA64 = 0), keeping its point as +0x9F0 (ChartContext::targetPoint).
void clearChartTarget(ChartContext& ctx);
// Player +0x9F0: the live jump target's point, else the last one's.
glm::vec3 chartTargetPoint(ChartContext& ctx);
// 0x733F60 -> 0x733EC0: markup flags +0x28 & 0xA000000 and its first segment's pitch |atan2(-dz, |dxy|)| within the
// global limit *(0xE7EE50)+0x184 = 40 degrees [O-runtime: Wine, l4_traprooms] (a level enough pole).
bool markupLevel(ChartContext& ctx, const ChartContext::MarkupSlot& m);
// ---- end group J ----
// ---- group A ----
// lara.drm's query profile `id` (object data +0xDE4 / +0xDE8, tru.exe 0x6F1950) with its class (+0x94) and result
// (+0x9C) records decoded (src/game/chart/Ground.cpp); nullopt: no such profile.
std::optional<MarkupProfile> decodeProfile(const Charts& charts, int id);
// tru.exe's markup search 0x7296C0 on query profile `profile` (action 423's slots with player +0xB00 / +0xC90 = props
// 541 / its pair, ground locomotion's query 96 through prop 0x21D, ...): sets ChartContext::chartTarget (player +0xA64
// target, +0xAC0 flags, +0xA80 type) to what it finds (cleared when nothing is met). nullopt: the search could not be
// answered (no profile, or ChartContext::markupSearch cannot model it) and chartTarget is left as it was; callers count
// the condition / action unported then.
std::optional<bool> chartMarkupSearch(ChartContext& ctx, int profile);
// ---- end group A ----

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
    // Puts the chart in `state` as a change would (exit / properties / entry / anim request) without a transition:
    // tests and tools that start from a state the unported conditions cannot reach.
    void enter(ChartContext& ctx, int state);
    // Harness: the state's time and its clip clock (through the node's queue) run on `frames` as if that long had
    // passed since enter(), without evaluating or posting the end (a Wine case that begins `frames` into its state).
    void fastForward(float frames);
    // A save's chart state (ChartDrive, docs/SAVES.md "chart"): restore() puts the agent back in `state` with its
    // times, end counter, queued / last transition, mirror bit, scratch fields and clip clock, applying the state's
    // properties and anim request as a change does but running none of its actions (they ran before the save).
    struct Snapshot {
        int state = -1, queued = -1, lastTransition = -1, animCounter = 0;
        float stateTime = 0, totalTime = 0, animFrames = 0;
        bool mirror = false, deferFlag7 = false;
        std::unordered_map<int, int> fields;
    };
    Snapshot snapshot() const;
    void restore(ChartContext& ctx, const Snapshot& s);
    // Drive mode: the requested clip's clock runs at the anim layer's rate (0x438C40, ChartContext::animRate): the
    // pattern's own rate where the request starts it (0x444BA0), replaced by what actions 501 / 509 write after [O:
    // 0x438C40 stores, it does not scale]. Shadow mode leaves it off: there the clock runs at the pattern's rate.
    bool layerRate = false;

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
    void setMirror(bool m) { mirror_ = m; } // the layer's +0xF4 0x20 written outside a transition (0x74C8D0)
    int lastTransition() const { return lastTransition_; } // +0x518
    int queued() const { return queued_; }                 // +0x514
    uint32_t frame() const { return frame_; }              // +0x50C: condition caches go stale when it moves
    float animFrames() const { return animFrames_; }       // the requested clip's time (ours: chart-side clock)
    float animLengthFrames() const;                        // 0 = unknown / none (no pattern, kind-2 request)
    bool animLoops() const;
    const Request* pattern() const;                        // the pattern playing for the state's request, null when none
    int playingPattern() const { return playing_ ? playingIndex_ : -1; }
    bool resolved() const { return playing_.has_value(); } // the state's request plays a known pattern (kind 2: its node resolved)
    float clipStartFrame() const { return clipStart_; }  // the clip clock's frame when the clip started
    // 30 Hz frames until the chart hears its clip end: the playing pattern's rest plus every pattern its node queued
    // after it (0x446BD0; the end comes when the queue drains, 0x43E0C0); 0 when none plays.
    float remainingFrames() const;
    uint64_t clipStarts() const { return clipStarts_; } // counts clip (re)starts; a kind-2 node that plays nothing keeps the clip

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
        uint64_t evaluations = 0, changes = 0, chainCaps = 0; // chainCaps: frames whose 0x65 chaining hit ours' cap
    } stats;

private:
    bool holds(ChartContext& ctx, const Transition& t);
    int find(ChartContext& ctx, int state);      // 0x787570: transition index, -1 none
    void change(ChartContext& ctx, int t);       // 0x787890
    void evaluate(ChartContext& ctx);            // 0x787C50
    int fire(ChartContext& ctx, int state, uint32_t event, int& found); // 0x7876E0
    void applyProperties(ChartContext& ctx, int state); // 0x785E70 / 0x785CC0
    void enterAnim(ChartContext& ctx);           // 0x783470
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
    // The pattern the state's request resolved to (kinds 0 / 1: the request; kind 2: what its graph node played), with
    // the play mode the node gave it. Empty: none (no request, or a node that plays nothing ours can resolve).
    std::optional<Request> playing_;
    int playingIndex_ = -1;
    bool playingMirror_ = false; // the mirror request the playing clip started with
    bool resolveRequest(ChartContext& ctx);
    uint64_t clipStarts_ = 0;
    std::vector<int> queue_; // patterns a kind-2 node queued after the playing one (0x446BD0 ring)
    float clipStart_ = 0;
    int nextBlend_ = -1; // layer +0x314: the last started pattern's +0x1B, the next start's blend (-1 none)
    void startClip(const Request& r);
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
    // drive: the chart decides Lara's traversal (--traversal chart, docs/re/PLAYER_CHART.md "Drive mode"); World calls
    // driver() in the controller's place, tick() only writes the trace.
    Shadow(const cdc::BigfileSet& bigfile, std::optional<std::filesystem::path> trace, bool log, bool drive = false);
    ~Shadow();
    // After World::tick. `keys`: the move input before the move stick filter (0x483520), for the trace's inputs column,
    // which records the keys as the Wine corpus' does (the filtered stick can land a tick later); default: a.move.
    void tick(const World& world, const Actions& a, std::optional<glm::vec2> keys = std::nullopt);
    // The chart enters `state` (ChartRuntime::enter) on the first tick at or after world tick `tick` (--chart-state).
    // `at` (x, y, z, yaw degrees): where she was in the Wine case when it began in a hold (case.json start.ours); the hold's
    // markup is seeded from there [I: harness only].
    void enterAt(int state, uint64_t tick, std::optional<glm::vec4> at = std::nullopt, float elapsed = 0);
    // `grab` (x, y, z, yaw degrees; --chart-state's fifth field): where the Wine case's spawn hold put her (case.json
    // spawn.ours). tru.exe's falling grab search runs there once, on the hold's first frame, before she reaches the case
    // start (Wine cc_dice_right: 0x70F300's arc from spawn.ours' hang height) [O-runtime]; a free-climb seed runs that
    // search (profile 45) from there and holds what it takes [I: harness only].
    void grabAt(glm::vec4 grab);
    // --chart-rng (replay only, harness): tru.exe's recorded generator results, consumed in order (ChartContext::rngFeed).
    void feedRng(std::vector<uint32_t> draws);
    std::string summary() const;
    const ChartRuntime& runtime() const { return *rt_; }
    PlayerDriver* driver(); // null unless constructed with drive
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::unique_ptr<Charts> charts_;
    std::unique_ptr<ChartRuntime> rt_;
    std::unique_ptr<ChartRuntime> upper_; // the upper-body chart (agent 1), ticked after Lara's
};

// ou-game chart-dump: the chart as text (one state, or all), and the coverage of conditions / actions reachable from
// Lara's root state (ported or not). Returns the process exit code.
// `names`: optional catalog JSON (W/chart/conditions.json, {"0x65": {"name": ...}}) for unported condition names.
int dump(const cdc::BigfileSet& bigfile, std::optional<int> state, bool coverage, bool upper, const std::filesystem::path* names,
         std::ostream& out);

} // namespace ou::game::chart

// UberObjectComposite_TR8 data stored in the object section's physical-class record (+0xC8): per section a state
// machine (states, transitions), the command table and the event table (docs/formats/SCRIPTS.md "Uber objects").
#pragma once

#include "cdc/Cdc.hpp"

#include <optional>
#include <vector>

namespace ou::formats {

struct UberTarget {
    int32_t section = 0;
    uint32_t type = 0, payload = 0; // tru.exe 0x4D7680: 1 set state, 2 / 3 / 4 trigger a transition
};
struct UberCommand {
    uint32_t id = 0;
    std::vector<UberTarget> targets;
};
// Condition list entry (0x4D1360): kind 1 = section `section` is in state `value`, 2 = the section is missing.
struct UberCondition {
    int32_t section = 0;
    uint32_t kind = 0, value = 0;
};
struct UberTransition {
    uint32_t from = 0, to = 0;
    uint32_t trigger = 0, param = 0; // +0x18 / +0x1C: 1 seconds in state (float), 2 command, 3 always, 9 conditions,
                                     // 14 / 15 joint value entered / left a range (param = watch mode, 0x4D5A50),
                                     // 17 a hit of more than `param` (float) damage, 19 a hit of damage class `param`
                                     // (0x6D4690), 18 health <= `param` (float) + 0.001 (0x6D49D0); 6 / 7 / 8 contact
                                     // impulses (0x4D5560, not simulated); 20 / 21 Lara engages / lets go of a record
                                     // of kind `param` (messages 0x158 / 0x14F: 0x6D4790 / 0x6D4810) and a portable
                                     // inserted into / removed from the section (0x165 / 0x186 -> 0x6D4890: 20 or 21
                                     // with param 5, else 22 / 23 any)
    std::vector<UberCondition> conditions; // trigger 9
    int32_t joint = -1, range = -1;        // trigger 14 / 15: +0x20 joint, +0x24 range record (indices below)
    int32_t anim = -1;                     // +0x2A animation reference index, -1 none (0x4D4A00)
    uint16_t animKind = 0;                 // +0x28 its kind (0x444BA0 on the section instance's anim component):
                                           // 0 animation-list index, 1 anim-graph state, 2 anim graph
    uint32_t probability = 100, flags = 0; // +0x2C, +0x34 (1 play anim, 2 wait for its end, 0x10 random)
};
// State entry action (0x10 bytes {type, 3 words}; 0x4D86A0): 2 / 3 play an animation (value), 9 create / 10 remove
// a physics joint, 12 set its limit, 13 its motor (param), 0x14 / 0x15 add / remove a range watch on it (value = mode:
// 0 position, 1 velocity; param = range, 0x4D7380). Pointer operands are indices into UberObjectData::joints / params.
// 0x1D (UberObjectSection_TR8 0x6D5EC0) sets the section's engagement set (+0xD4, answered to query 0x53) to word +4:
// `engagement` = that set's offset in the object section, -1 none (an empty pointer clears it).
struct UberAction {
    uint32_t type = 0, value = 0;
    int32_t joint = -1, param = -1;
    int32_t engagement = -1;
};
// Physics joint record (action 9, 0x4D8000). Type 0 hinge (angle, degrees), 2 prismatic (units), 8 fixed, 11 link of
// two joints, 5 / 7 multi-axis (0x4D1040 -> engine 1 / 3 / 0xB / 0xD / 8 / 10). Bodies are section indices (each
// section's instance, 0x4D3DA0), -1 the world. Frame: object model `model`'s marker `marker`; the joint axis is its Z.
struct UberJoint {
    uint8_t type = 0;
    int32_t bodyA = -1, bodyB = -1;
    uint8_t model = 0, marker = 0;  // +0xC / +0xD
    float lo = 0, hi = 0;           // +0x2C / +0x30, used when flags bit 0 (0x4D4690)
    float speed = 0, force = 0;     // +0x34 / +0x38 motor: speed per second, impulse bound (<= 0 none), flags bit 1 (0x4D1F00)
    uint8_t flags = 0;              // +0x3C
    int32_t link1 = -1, link2 = -1; // type 11: +4 / +8 joints; joint2 = ratio * joint1 (0x4D7ED0, 0x4E17B0)
    float ratio = 0;                // +0xC (degrees per unit when joint1 is prismatic and joint2 a hinge)
    // +0x10 SoundPlex* and +0x24 where it plays (0 the pivot on body A, 1 on body B, 2 between them; 0x4F1E10): 0x4D8000
    // registers it with the joint-sound manager (0x4F2B00, docs/formats/AUDIO.md §9.16). Every record has one; most are
    // a TimeIn (silent).
    std::optional<cdc::Pointer> sound;
    uint8_t soundAt = 0;
};
// Limit / motor / range record {u8 kind, a, b, u8 flag} (kinds 0 / 2 = hinge / prismatic, 0x4D11E0 / 0x4D4510 /
// 0x4D2B00): limit lo, hi, enabled; motor speed per second, impulse bound, enabled; range lo, hi (lo <= v < hi).
struct UberParam {
    uint8_t kind = 0;
    float a = 0, b = 0;
    bool flag = false;
};
struct UberSection {
    uint32_t defaultState = 0, model = 0;
    std::vector<std::vector<uint32_t>> stateOut; // per state: indices into transitions it can leave by
    std::vector<std::vector<UberAction>> stateActions; // per state: its entry actions
    std::vector<UberTransition> transitions;
    // +0x3C health and +0x52 bit 0 "takes damage" (UberObjectSection_TR8 ctor 0x6D60B0: +0xDC = record +0x3C; 0x6D4690
    // ignores hits without the bit). Urns (*_pt_weightbreak*, ma_ei_breakable_*) 30, sh_ei_exploading_barrel 50.
    float health = 0;
    bool takesDamage = false;
    // +0x40 the engagement set the section starts with (offset in the object section, -1 none; 0x6D60B0 -> +0xD4, query
    // 0x53 0x6D59C0: where Lara engages it, physics::loadEngagementSet) and +0x4C -> byte: the model 0 marker a portable
    // inserted into the section attaches at (query 0x173), -1 none.
    int32_t engagement = -1;
    int32_t attachMarker = -1;
};
// Event (0x4D3EC0): fires when section `section` does `kind` (1 enters state, 2 leaves state, 3 starts transition,
// 4 finishes transition) with `value` (state / transition index) and every condition list holds.
struct UberEvent {
    int32_t section = 0;
    uint32_t kind = 0, value = 0;
    bool once = false;
    std::vector<std::vector<UberCondition>> conditions;
};
struct UberObjectData {
    uint16_t objectClass = 0;
    std::vector<UberSection> sections;
    std::vector<UberCommand> commands;
    std::vector<UberEvent> events;
    std::vector<std::vector<UberCondition>> queries; // +0x28 / +0x2C: `uberobjectquery` index -> condition list (0x4AB6A0)
    std::vector<UberJoint> joints;
    std::vector<UberParam> params;
};

// nullopt for non-uber objects. Throws cdc::FormatError on malformed uber data.
std::optional<UberObjectData> loadUberObject(const cdc::Drm& drm);

} // namespace ou::formats

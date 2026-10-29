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
    uint32_t trigger = 0, param = 0; // +0x18 / +0x1C: 1 seconds in state (float), 2 command, 3 always, 9 conditions
    std::vector<UberCondition> conditions; // trigger 9
    int32_t anim = -1;                     // +0x2A animation-list index, -1 none
    uint32_t probability = 100, flags = 0; // +0x2C, +0x34 (1 play anim, 2 wait for its end, 0x10 random)
};
struct UberSection {
    uint32_t defaultState = 0, model = 0;
    std::vector<std::vector<uint32_t>> stateOut; // per state: indices into transitions it can leave by
    std::vector<std::vector<uint32_t>> stateActions; // per state: its entry action types (0x4D86A0: 2/3 anims, 13 drive)
    std::vector<UberTransition> transitions;
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
};

// nullopt for non-uber objects. Throws cdc::FormatError on malformed uber data.
std::optional<UberObjectData> loadUberObject(const cdc::Drm& drm);

} // namespace ou::formats

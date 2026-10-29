// Object animation state graph (docs/formats/ANIMGRAPH.md): the physob anim state machine of physical class 6 objects
// (ObjectBase +0xC8 record), run by the instance's anim component (Instance +0x294, tru.exe 0x6A5660 / 0x6B3DD0).
// No TR8 object uses the cdc AnimGraph (ObjectBase +0x64 hasAnimGraph / +0x68 graph, 0x453100): 0 in every object.
#pragma once

#include "cdc/Cdc.hpp"

#include <optional>
#include <vector>

namespace ou::formats {

// Stimulus bits the engine posts (tru.exe 0x6B4440 / 0x6B5890 / 0x6B4390); scripts post their stimulus members' masks.
inline constexpr uint32_t kStimulusUse = 0x1;      // Lara uses the object (switches, buttress mounts)
inline constexpr uint32_t kStimulusEvent = 0x800;  // an animation event (transition +0x1C/+0x20 event ids)
inline constexpr uint32_t kStimulusTimer = 0x8000; // every update, with the 30 Hz frames spent in the state

struct AnimGraphState { // 0x28-byte record, tag 0x2714
    int16_t index = 0;    // +2: the id scripts see (modifiable 7 / 8, vt 20); a negative one gets the lowest free id
    uint16_t flags = 0;   // +4: 2 loop the state's animation (else play once and hold)
    int8_t anim = -1;     // +7: animation-list index played on entry, -1 = keep the current pose
    uint32_t message = 0; // +8: posted to the instance on entry (INSTANCE message 0x1E2)
    std::vector<uint32_t> out; // +0x14/+0x18: transitions it can leave by (indices into AnimGraph::transitions)
};
struct AnimGraphTransition { // 0x34-byte record, tag 0x2776
    int16_t index = 0;   // +2: the id of modifiable 4 / 5 / 6 and vt 19
    uint32_t mask = 0;   // +4: stimulus bits that start it
    int8_t anim = -1;    // +9: animation-list index played once; at its end the target state is entered
    float after = 0;     // +0xC: the stimulus value (timer: frames in the state) must reach this
    int32_t target = -1; // +0x14: state (index into AnimGraph::states); null = the current one again (0x6A5660)
    std::vector<uint32_t> events; // +0x1C/+0x20: animation event ids (mask 0x800)
};
struct AnimGraph {
    std::vector<AnimGraphState> states;
    std::vector<AnimGraphTransition> transitions;
    int stateById(int32_t id) const;      // -1 if none
    int transitionById(int32_t id) const; // -1 if none
};

// nullopt when the object has no class-6 state graph. Throws cdc::FormatError on malformed records.
std::optional<AnimGraph> loadAnimGraph(const cdc::Drm& object);

} // namespace ou::formats

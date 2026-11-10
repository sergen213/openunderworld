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
// A portable inserted into / removed from a receptacle: INSTANCE messages 0x165 / 0x186 (0x6C2530, 0x6B5890) post them.
inline constexpr uint32_t kStimulusInsert = 0x20, kStimulusRemove = 0x40;
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
    std::vector<uint32_t> events; // +0x1C/+0x20: hit damage classes (message 0x11C +0x50) that start it (mask 0x800)
    float window = 0;             // +0x10: > 0: the stimulus' running total since the state's entry must reach this
    // +8: clip the exe plays when this is the nearest unreached transition (0x6B3DD0 / 0x6A52E0); -1 in all data
};
struct AnimGraph {
    std::vector<AnimGraphState> states;
    std::vector<AnimGraphTransition> transitions;
    int initial = 0;                      // record +0x2C: the state (index into `states`) an instance starts in
    int stateById(int32_t id) const;      // -1 if none
    int transitionById(int32_t id) const; // -1 if none

    // Which of `state`'s transitions a stimulus starts (index into transitions, -1 none). Several eligible ones are
    // drawn with tru.exe's LCG (graphRandom, `seed` = DAT_00A038E0). `total` is the stimulus' running total since the
    // state's entry (anim component +0x14, the windows +0x10).
    // 0x6B3DD0: mask and no event ids; reached = max(value - after, window > 0 ? total - window : -1e6) >= 0; among
    // the reached ones, each replaces the pick with probability reached / (sum of reached so far).
    int pick(int state, uint32_t mask, float value, float total, uint32_t& seed) const;
    // 0x6B3BD0 (script stimulus with value < 0): any transition on the mask, thresholds and event ids ignored, uniform.
    int pickAny(int state, uint32_t mask, uint32_t& seed) const;
    // 0x6A52E0 (hit message 0x11C): mask 0x800 transitions listing the damage class, drawn as `pick` by the damage.
    int pickHit(int state, uint32_t damageClass, float damage, float total, uint32_t& seed) const;
    // 0x6B4090 (Lara's use, messages 0x166 / 0x168): the transition on the mask without event ids nearest the value
    // (|value - after|, after 0 counts 0.1), ties drawn uniformly; it starts only within 0.1.
    int pickUse(int state, uint32_t mask, float value, uint32_t& seed) const;
};

// tru.exe 0x48CA30: DAT_00A038E0 = DAT_00A038E0 * 0x41C64E6D + 0x3039, then bits 16..30 / 32767 scaled to [0, hi].
// The seed starts at 0 (.bss).
double graphRandom(uint32_t& seed, double hi);

// nullopt when the object has no class-6 state graph. Throws cdc::FormatError on malformed records.
std::optional<AnimGraph> loadAnimGraph(const cdc::Drm& object);

} // namespace ou::formats

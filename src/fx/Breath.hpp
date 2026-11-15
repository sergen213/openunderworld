// Lara's breath bubbles under water for the Enhanced under-water particle pass (render/Water.hpp UwParticlePush,
// docs/RENDERING.md "Under water: marine snow and bubbles"). Visual only and ours: no breath-bubble emitter of tru.exe
// is decoded [U] (EFFECTS.md knows the under-water-only effects as weapon trails and fx_bubbles_* objects): the puff
// clock is [I], each bubble's size, rise speed and wobble are abyssal-garden's (MIT, world.js / vsBub). Deterministic:
// puffs fall on a fixed clock of the game time (seeded per puff), each bubble keeps the mouth position it left from and
// rises straight up at its speed, so the same game time and path draw the same frame.
#pragma once

#include "render/Renderer.hpp"

#include <cstdint>
#include <deque>
#include <vector>

#include <glm/glm.hpp>

namespace ou::fx {

class Breath {
public:
    // One step at game time `t` (s): emits the bubbles whose release time passed since the last step from `mouth`
    // while `submerged` (head under water) and the mouth is below `surfaceZ`; `scuba` breathes more often [I]. A
    // backwards jump in time (load) clears everything.
    void tick(double t, const glm::vec3& mouth, bool submerged, bool scuba, float surfaceZ);
    // The live bubbles at time t (>= the last tick's), appended: rising until the surface they left under.
    void append(double t, std::vector<render::Bubble>& out) const;
    void clear() { live_.clear(), last_ = -1; }

private:
    // size (m), rise speed (m/s), wobble (m), range: the rise to 0.2 m under the surface (m)
    struct Live { glm::vec3 from; double born; float size, seed, speed, wobble, range; };
    std::deque<Live> live_;
    double last_ = -1;
};

} // namespace ou::fx

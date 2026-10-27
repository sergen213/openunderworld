// Interactive water: a deterministic ripple height field on the water surface around Lara, plus
// ballistic splash droplets. Simulated on the fixed 60 Hz game tick, renderer-agnostic (ou-game copies
// the field into render::FrameOptions). Technique: damped 2D wave equation with Gaussian impulses.
// Parameters are ours [I], not tru.exe's (its water does not simulate ripples). docs/RENDERING.md "Water".
#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <map>
#include <vector>

namespace ou::game {

class World;
using glm::vec2;
using glm::vec3;

struct SplashDrop {
    vec3 pos{0}, vel{0};
    float life = 0, maxLife = 1, radius = 8.0f;
    float alpha() const { return maxLife > 0 ? life / maxLife : 0.0f; }
};

class WaterSim {
public:
    static constexpr int kSize = 256;        // texels per side
    static constexpr float kCell = 28.0f;    // world units per texel
    static constexpr float kWaveSpeed = 420.0f; // units/s [I]
    static constexpr float kDamping = 0.994f;   // per tick [I]

    void reset();
    void tick(const World& world, float dt);
    // Impact ring + droplets at a world position on the active surface (shots, falling objects).
    void splash(vec3 pos, float strength = 1.0f);

    bool active() const { return active_; } // a water volume is near Lara
    float originX() const { return origin_.x; }
    float originY() const { return origin_.y; }
    float surfaceZ() const { return surfaceZ_; }
    const std::vector<float>& heights() const { return h_; }
    uint64_t version() const { return version_; }
    const std::vector<SplashDrop>& drops() const { return drops_; }

private:
    void impulse(float x, float y, float radius, float amount);
    void emit(vec3 pos, int count, float speed, float up, float radius = 9.0f);
    bool inside(vec3 p, float margin = 0.0f) const; // on the active surface's XY box

    std::vector<float> h_, prev_, next_;
    vec2 origin_{0};
    float surfaceZ_ = 0;
    vec3 boxLo_{0}, boxHi_{0}; // active volume (hi.z = surface)
    bool active_ = false;
    uint64_t version_ = 0;
    uint64_t tick_ = 0;
    std::vector<SplashDrop> drops_;
    std::map<int32_t, vec3> moved_; // enemy / dynamic-body positions last tick (wake sources)
    uint32_t rng_ = 0x9E3779B9u;
};

} // namespace ou::game

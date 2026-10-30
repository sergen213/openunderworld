// Interactive water: the wave field of Wars and Warriors: Joan of Arc's port (same author, GPLv3; its
// src/engine/water_waves.{hpp,cpp} and world_view_water.cpp), rescaled to TRU units, plus ballistic splash droplets.
// Simulated on the fixed 60 Hz game tick, renderer-agnostic (ou-game copies the field into render::FrameOptions;
// water.frag shades it). Parameters are ours [I], not tru.exe's (its water does not simulate ripples).
// docs/GAMEPLAY.md "Water", docs/RENDERING.md "Water".
#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <vector>

namespace ou::collision { struct Markup; }

namespace ou::game {

class World;
using glm::vec2;
using glm::vec3;

// TRU units per Joan of Arc unit: body heights (Lara ~512, a Joan soldier ~80). Every length, speed and height of
// the Joan field and shader is multiplied by it; slopes, damping and times are unchanged (water.frag: kScale).
inline constexpr float kWaterScale = 6.4f;

// A damped 2D wave equation on a kN^2 height field that follows Lara (world x, y; z up). Waves from every kick add up
// (interfere), reflect off walls - cells the `wet` predicate rejects (shores) and per-step body obstacles (wading
// legs, swimmers, enemies) - and damp out. Heights are displacements relative to whichever surface a cell lies on.
// Allocation-free after construction.
class WaveField {
public:
    static constexpr int kN = 256;
    static constexpr float kCell = 4.0f * kWaterScale;   // 25.6 units: a 6554-unit window
    static constexpr float kSpeed = 80.0f * kWaterScale; // units/s
    static constexpr float kStep = 1.0f / 60.0f;         // fixed substep: c * dt / cell = 0.33, inside CFL (1/sqrt 2)
    using Wet = std::function<bool(vec2)>;

    WaveField();
    // Scrolls the window by whole cells so `centre` is in its middle; shifted-in cells start at rest and ask `wet`
    // (open water at that x, y) once.
    void follow(vec2 centre, const Wet& wet);
    // Runs the fixed substeps owed after dt seconds; bodies added since the last call are walls for them.
    void step(float dt);
    // Displacement kick: `amplitude` down at the centre, a rim up around it (Mexican hat of width sigma: zero volume,
    // so kicks never raise or lower the mean level). Walls and this step's bodies are not touched; the volume they clip
    // off is taken evenly off the other cells.
    void kick(vec2 p, float amplitude, float sigma);
    // A body in the water moving at `velocity` (units/s) over dt: a wall for the next step() (waves scatter around it)
    // and, moving, it heaps water up ahead and leaves a trough behind (the wake). Bodies at kSpeed or faster are
    // ignored (their wake would be a Mach cone; WaterSim rings them per stroke).
    void wade(vec2 p, vec2 velocity, float dt);

    float height(vec2 p) const; // the cell holding p, 0 outside the window
    bool wall(vec2 p) const;    // the cell holding p is a static wall (or outside the window)
    vec2 origin() const { return vec2(corner_) * kCell; }
    // Empty once nothing kicked for kQuiet seconds (the field has damped to rest).
    std::span<const float> heights() const;
    void clear() { placed_ = false, idle_ = kQuiet + 1, bodies_.clear(); }

private:
    static constexpr float kQuiet = 8.0f;
    int cellIndex(vec2 p) const; // -1 outside the window
    void shift(std::vector<float>& a, int dx, int dy, float fill);

    std::vector<float> h_, prev_; // current and previous substep heights
    std::vector<float> open_;     // 1 open water, 0 static wall, -1 not asked yet
    std::vector<float> mask_;     // open_ with the bodies of the current step cut out
    std::vector<float> scratch_;
    std::vector<vec3> bodies_; // x, y, radius
    glm::ivec2 corner_{0};     // window origin in cells
    bool placed_ = false;
    float owed_ = 0.0f, idle_ = kQuiet + 1;
};

struct SplashDrop {
    vec3 pos{0}, vel{0};
    float life = 0, maxLife = 1, radius = 8.0f;
    float alpha() const { return maxLife > 0 ? life / maxLife : 0.0f; }
};

class WaterSim {
public:
    void reset();
    void tick(const World& world, float dt);
    // Impact crater + droplets (shots, falling objects); strength 1 = a projectile / body falling in.
    void splash(vec3 pos, float strength = 1.0f);

    bool active() const { return active_; } // a water volume is near Lara
    float originX() const { return field_.origin().x; }
    float originY() const { return field_.origin().y; }
    float surfaceZ() const { return surfaceZ_; } // rest surface of Lara's / the nearest water
    std::span<const float> heights() const { return active_ ? field_.heights() : std::span<const float>{}; }
    uint64_t version() const { return version_; }
    const std::vector<SplashDrop>& drops() const { return drops_; }
    const WaveField& field() const { return field_; }

private:
    void ripple(vec3 p, float strength);
    void emit(vec3 pos, int count, float speed, float up, float radius = 9.0f);
    float waterAt(vec2 p) const; // surface z of the water over p, -inf where there is none

    WaveField field_;
    const World* world_ = nullptr;                    // during tick
    std::vector<const collision::Markup*> volumes_; // water markup (type 10) this tick
    double seconds_ = 0;           // sea wave clock of this tick
    float surfaceZ_ = 0, time_ = 0, bob_ = 0;
    bool active_ = false;
    uint64_t version_ = 0;
    std::vector<SplashDrop> drops_;
    struct Body { vec3 pos; bool alive; float bob; };
    std::map<int32_t, Body> bodies_; // enemies / dynamic bodies last tick
    uint32_t rng_ = 0x9E3779B9u;
};

} // namespace ou::game

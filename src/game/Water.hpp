// Interactive water: what makes waves on the water around Lara, plus ballistic splash droplets. The waves themselves
// are simulated on the GPU (render/WaterField.hpp, shaders/water_field.comp: a spectral gravity-capillary wave field);
// this side runs on the fixed 60 Hz game tick and produces the field's inputs: its window (which cells are open water),
// displacement kicks (splashes, strokes, shots) and the pressure bodies (swimmers, waders, floating debris) whose
// motion leaves wakes. ou-game passes them through render::FrameOptions::waterField. Parameters are ours [I], not
// tru.exe's (its water does not simulate waves). docs/GAMEPLAY.md "Water", docs/RENDERING.md "Interactive waves".
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

// A displacement kick (shaders/water_surface.glsl contract C3 OuWaveKick): world x, y, the displacement at its centre
// in TRU units (negative pushes the water down) and its width sigma. The field adds a zero-volume Mexican hat
// amplitude (1 - q) e^-q, q = r^2 / (2 sigma^2): a crater with its rim for a negative kick.
struct WaveKick { float x, y, amplitude, sigma; };
// A body floating / wading in the water: a moving surface pressure (Gaussian of width `radius`, depth `draft` of
// water it pushes away, TRU units) that dips the surface and, moving, trails a Kelvin wake; `wall` (0..1) also makes
// its core reflect waves (slow bodies: legs, a standing enemy). Velocity in units/s.
struct WaveBody { float x, y, radius, draft; float vx, vy, wall, pad; };

// The window of the GPU wave field: kN^2 cells of kCell (a 40 m square) around Lara, scrolled in whole mask cells, and
// its static walls on a half-resolution mask (kMaskN^2 cells of kMaskCell): no water volume over the cell, or terrain
// within reach of its rest surface (shores, pool sides, rocks, hulls). Allocation-free after construction.
class WaveWindow {
public:
    static constexpr int kN = 512;                    // render::waterfield::kN (FFT size)
    static constexpr float kCell = 290.0f * 40.0f / kN; // 22.66 units = 7.8 cm (shaders/waves.glsl kM: 290 units/m)
    static constexpr int kMaskN = kN / 2;
    static constexpr float kMaskCell = 2.0f * kCell;
    using Wet = std::function<bool(vec2)>;

    WaveWindow();
    // Scrolls the window by whole mask cells so `centre` is in its middle; shifted-in mask cells ask `wet` (open water
    // at that x, y) once. A jump beyond the window re-asks every cell.
    void follow(vec2 centre, const Wet& wet);
    bool wall(vec2 p) const;                                  // the mask cell holding p is a wall (or outside)
    vec2 origin() const { return vec2(corner_) * kMaskCell; } // world corner of cell (0, 0)
    std::span<const uint8_t> mask() const { return mask_; }   // kMaskN^2, row major (x fastest): 255 open, 0 wall
    uint64_t version() const { return version_; }             // changes with the mask
    bool placed() const { return placed_; }
    void clear() { placed_ = false; }

private:
    std::vector<uint8_t> mask_, scratch_;
    std::vector<uint8_t> asked_; // 1 = mask_ holds the predicate's answer
    glm::ivec2 corner_{0};       // window origin in mask cells
    bool placed_ = false;
    uint64_t version_ = 0;
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
    float originX() const { return window_.origin().x; }
    float originY() const { return window_.origin().y; }
    float surfaceZ() const { return surfaceZ_; } // rest surface of Lara's / the nearest water
    float depth() const { return depth_; }       // that volume's depth (surface - floor), the waves' dispersion depth
    // Changes whenever the field must restart from rest (reset, another water surface): the renderer clears it.
    uint64_t generation() const { return generation_; }
    const WaveWindow& window() const { return window_; }
    // Kicks of the last kKeepTicks ticks; kicks()[i] has serial kickBase() + i (serials only grow, across resets too),
    // so each rendered frame takes the ones past the last serial it consumed (several ticks per frame, or none).
    std::span<const WaveKick> kicks() const { return kicks_; }
    uint64_t kickBase() const { return kickBase_; }
    std::span<const WaveBody> bodies() const { return waveBodies_; } // this tick's pressure bodies
    const std::vector<SplashDrop>& drops() const { return drops_; }

    static constexpr int kKeepTicks = 8;
    static constexpr size_t kMaxKicks = 4096; // the GPU buffer's capacity (shaders/water_surface.glsl C3)

private:
    void kick(vec2 p, float amplitude, float sigma);
    void ripple(vec3 p, float strength);
    void emit(vec3 pos, int count, float speed, float up, float radius = 9.0f);
    float waterAt(vec2 p) const; // surface z of the water over p, -inf where there is none

    WaveWindow window_;
    const World* world_ = nullptr;                    // during tick
    std::vector<const collision::Markup*> volumes_; // water markup (type 10) this tick
    double seconds_ = 0;           // sea wave clock of this tick
    float surfaceZ_ = 0, depth_ = 0, time_ = 0, bob_ = 0;
    int chosen_ = -1;              // markup index of the field's water
    bool active_ = false;
    uint64_t generation_ = 0, tick_ = 0, kickBase_ = 0;
    std::vector<WaveKick> kicks_;
    std::vector<uint64_t> kickTicks_; // the tick each kick was made on
    std::vector<WaveBody> waveBodies_;
    std::vector<SplashDrop> drops_;
    struct Body { vec3 pos; bool alive; float bob; };
    std::map<int32_t, Body> bodies_; // enemies / dynamic bodies last tick
    uint32_t rng_ = 0x9E3779B9u;
};

} // namespace ou::game

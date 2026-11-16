// Interactive water: what makes waves on the water around Lara, plus the splash bursts the Enhanced water's particle
// fluid throws (render/Fluid.hpp simulates and draws them on the GPU; its particles falling back kick the field). The
// waves themselves are simulated on the GPU (render/WaterField.hpp, shaders/water_field.comp: a spectral
// gravity-capillary wave field); this side runs on the fixed 60 Hz game tick and produces the field's inputs: its
// window (which cells are open water), displacement kicks (splashes, strokes, shots) and the pressure bodies (swimmers,
// waders, floating debris) whose motion leaves wakes. ou-game passes them through render::FrameOptions::waterField
// and the bursts through FrameOptions::fluidBursts. Parameters are ours [I], not tru.exe's (its water does not
// simulate waves). docs/GAMEPLAY.md "Water", docs/RENDERING.md "Interactive waves" / "Splash fluid".
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

// Water thrown up by an impact: what render::fluid emits on the GPU (shaders/fluid.comp, docs/RENDERING.md "Splash
// fluid"). Crown: a sheet launched off a ring around the impact (outward at `speed`, up to `up`, the higher part of the
// sheet faster: it rises as a wall that breaks into droplets); Jet: the column the collapsing cavity throws up the
// middle (a Worthington jet); Spray: droplets in a cone of half-angle `spread` (radians) around up at `speed`.
struct SplashBurst {
    enum Kind : uint8_t { Crown, Jet, Spray };
    Kind kind = Crown;
    vec3 pos{0};        // centre on the surface (spray: the emitter)
    float surfaceZ = 0; // the surface at the impact this tick (a sea's waves included): what `pos` is measured from
    vec3 carry{0};      // velocity every particle inherits (the impactor's horizontal motion), units/s
    float radius = 0;   // crown ring / jet column / spray disc radius, units
    float speed = 0;    // crown, jet: outward launch speed; spray: launch speed (units/s)
    float up = 0;       // crown, jet: upward launch speed of the fastest particles (units/s)
    float spread = 0;   // crown, jet: height the sheet / column starts over (units); spray: cone half-angle
    int count = 0;      // particles
    float delay = 0;    // seconds after the impact (the jet waits for the cavity to collapse)
    uint64_t serial = 0; // WaterSim numbering: 1, 2, ... in release order
    float restZ = 0;     // the water volume's rest surface (its box top) the renderer's waves ride on; 0: WaterSim sets it
};
// Particle spacing of the fluid, units (render::fluid::kSpacing): the crown's start height keeps it near rest density.
inline constexpr float kSplashSpacing = 7.0f;
// What Lara entering the water throws: `velocity` at the surface (units/s), headFirst = a swan dive (a streamlined
// entry: a small crown, then a tall jet). Faster entries throw more water higher; slow ones no jet. Deterministic.
std::vector<SplashBurst> entryBursts(vec3 at, vec3 velocity, bool headFirst);
// A body, object or shot hitting the water (WaterSim::splash strength: 1 = a projectile / body falling in).
std::vector<SplashBurst> impactBursts(vec3 at, float strength);

class WaterSim {
public:
    void reset();
    void tick(const World& world, float dt);
    // Impact crater + splash bursts (shots, falling objects); strength 1 = a projectile / body falling in.
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
    // The last kBurstKeep bursts released, oldest first (serials increase by 1): a renderer forwards those newer than
    // the last serial it saw, however many ticks ran between its frames.
    std::span<const SplashBurst> bursts() const { return bursts_; }
    static constexpr size_t kBurstKeep = 64;

    static constexpr int kKeepTicks = 8;
    static constexpr size_t kMaxKicks = 4096; // the GPU buffer's capacity (shaders/water_surface.glsl C3)

private:
    void kick(vec2 p, float amplitude, float sigma);
    void ripple(vec3 p, float strength);
    void release(std::vector<SplashBurst> bursts); // into pending_ (delayed) / bursts_ (now)
    float waterAt(vec2 p) const; // surface z of the water over p, -inf where there is none
    float restAt(vec2 p) const;  // rest surface (box top) of the water over p, -inf where there is none

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
    std::vector<SplashBurst> bursts_, pending_; // released; waiting for their delay (pending: release time in .delay)
    uint64_t serial_ = 0;
    bool plunged_ = false; // Lara's entry splash thrown; cleared once she is out of the water
    struct Body { vec3 pos; bool alive; float bob; };
    std::map<int32_t, Body> bodies_; // enemies / dynamic bodies last tick
};

} // namespace ou::game

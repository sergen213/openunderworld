// Enhanced water's splash fluid (docs/RENDERING.md "Splash fluid"): the game's splash bursts (FrameOptions::fluidBursts,
// game::SplashBurst) become particles of a local GPU fluid near the camera, simulated with Position Based Fluids
// (Macklin & Mueller 2013, "Position Based Fluids", ACM TOG 32(4); docs/RESEARCH.md "Splash fluid") in
// shaders/fluid.comp on both backends and drawn by the screen-space fluid passes (render/Water.hpp, splash_*).
// Particles that fall back into the water write C3 wave kicks (OuWaveKick, kKickBytes) for the interactive surface.
// This header is the CPU half shared by the backends: constants, the GPU structs, burst slots and the fixed-step clock.
#pragma once

#include "render/Renderer.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace ou::render::fluid {

inline constexpr int kCapacity = 32768;     // particle pool (a ring: new bursts overwrite the oldest slots)
inline constexpr uint32_t kHashSize = 65536; // neighbour grid buckets (hashed cells of kSupport)
inline constexpr int kMaxKicks = 4096;      // C3 kick buffer entries
inline constexpr int kMaxBursts = 64;       // bursts emitted per frame
inline constexpr int kMaxPerFrame = kCapacity / 2;
inline constexpr int kThreads = 128;        // fluid.comp local size
inline constexpr float kSpacing = 7.0f;     // particle spacing at rest, units (2.4 cm); = shaders/fluid.glsl kOuFluidSpacing
inline constexpr float kSupport = 2.0f * kSpacing; // kernel support h (and grid cell)
inline constexpr float kStep = 1.0f / 120.0f; // fixed substep, s
inline constexpr int kMaxSteps = 3;         // per frame; time beyond is dropped (slow frames slow the splash down)
inline constexpr int kIterations = 2;       // density solver iterations per step (even: the last lands in `p`)
inline constexpr float kMaxLife = 3.5f;     // s; fluid.comp gives particles 2.2..3.4
inline constexpr float kRange = 9000.0f;    // bursts farther from the eye are skipped; particles past it die
inline constexpr float kGravity = 5.72443f * 900.0f; // the game's (game::Tuning::gravity), units/s^2
inline constexpr float kKickAmplitude = 0.8f; // C3 kick depth of one particle re-entering at 1000 units/s, units
inline constexpr float kKickSigma = 24.0f;    // ... and its width, units

// shaders/fluid.glsl OuFluidBurst (std430): pos = centre xyz + the surface z at the impact; vel = carried velocity
// xyz + radius; shape = speed, up, spread, kind;
// info = first ring slot, count, seed, rest surface z (float bits).
struct GpuBurst {
    glm::vec4 pos, vel, shape;
    glm::uvec4 info;
};
static_assert(sizeof(GpuBurst) == 64);
// fluid.comp's push constants (Vulkan) / uniforms 0..3 (GL). cfg: pass, solver source (0 p -> d, 1 d -> p), bursts,
// particles emitted. sim: dt, support h, rest density (poly6 sum, support 1), relaxation eps. par: gravity, tensile
// coefficient k, poly6 at the tensile radius, XSPH viscosity. lim: capacity, kick amplitude, kick sigma, cohesion.
struct Push {
    glm::ivec4 cfg;
    glm::vec4 sim, par, lim;
};
static_assert(sizeof(Push) == 64);
enum Pass { Emit = 0, Predict = 1, Lambda = 2, Delta = 3, Finalize = 4 };

// Buffer sizes. Particles: OuFluidParticle (4 vec4) each. Grid: kHashSize bucket heads (0xFFFFFFFF = empty, cleared by
// the host before every Predict) then one `next` link per particle. Kicks: contract C3, 16-byte header (count, 3 pad)
// then kMaxKicks vec4 (world x, world y, amplitude (negative = down), sigma); the producer zeroes count at the start of
// each frame's fluid pass and count may run past kMaxKicks (readers clamp). Bursts: kMaxBursts GpuBurst per frame.
inline constexpr size_t kParticleBytes = size_t(kCapacity) * 64;
inline constexpr size_t kGridBytes = (size_t(kHashSize) + size_t(kCapacity)) * 4;
inline constexpr size_t kKickBytes = 16 + size_t(kMaxKicks) * 16;
inline constexpr size_t kBurstBytes = size_t(kMaxBursts) * sizeof(GpuBurst);

// Kernel constants in units of the support (h = 1): poly6 density of a cubic lattice of kSpacing (the rest density),
// and the PBF denominator sum |grad C|^2 of one of its particles (the relaxation is a fraction of it).
float restDensity();
float latticeGradient();

// What one frame runs. Bursts carry their ring slots (info.x).
struct Frame {
    std::vector<GpuBurst> bursts;
    int emitted = 0;     // particles the bursts emit (Emit dispatch size)
    int steps = 0;       // fixed substeps to simulate
    bool active = false; // a particle may be alive: simulate and draw
    bool clear = false;  // kill every particle first (the clock restarted, or the last ones died unobserved)
};

// One renderer's fluid clock: the game time (FrameOptions::time; a pause holds it), the ring cursor and how long the
// newest particle may live. Deterministic for a given frame sequence.
class Sim {
public:
    Frame frame(const FrameOptions& o, glm::vec3 eye);
    Push push(Pass pass, const Frame& f, int source = 0) const;

private:
    double last_ = -1.0, acc_ = 0.0, aliveUntil_ = -1.0;
    uint32_t cursor_ = 0;
    bool stale_ = true; // particles may linger (an idle-declared run, or the pool's undefined first contents)
};

// GL source (Vulkan compiles the same file with glslc -DVULKAN).
std::string computeSource();

} // namespace ou::render::fluid

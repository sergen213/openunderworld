// Enhanced water's interactive wave field (docs/RENDERING.md "Interactive waves"): the water around Lara as a linear
// gravity-capillary wave field on the GPU (shaders/water_field.comp, both backends), driven by game::WaterSim's kicks
// and pressure bodies (render::WaterField) and read by every water effect through shaders/water_surface.glsl.
//
// Method: Tessendorf's eWave ("eWave: Using an Exponential Solver on the iWave Problem", 2014; written from the paper):
// each frame the height h and vertical velocity v go to Fourier space (one complex FFT of h + i v), every wave number
// k advances by the exact solution of its damped oscillator with the full dispersion relation
//   omega^2 = (g k + sigma / rho k^3) tanh(k depth)
// (the FFT ocean's, shaders/ocean.comp), and comes back. Exact per mode, so any time step is stable and long swells
// outrun short ripples at their true group speeds (Airy rings, Kelvin wakes). Local effects are applied between the
// transforms in space: kicks, the pressure bodies (as a moving hydrostatic equilibrium: the surface oscillates around
// each body's dip, so a moving body radiates its wake), static walls (shores, from game::WaveWindow's mask) and slow
// bodies' cores (reflection), an absorbing sponge at the window edge, extra damping where the FFT ocean is rough
// (micro-breaking) and a steepness limit (breaking) so no input can make it explode.
#pragma once

#include "render/Renderer.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace ou::render::waterfield {

inline constexpr int kN = 512;          // cells per side (FFT size); game::WaveWindow::kN
inline constexpr int kLog2N = 9;
inline constexpr int kMaskN = kN / 2;   // static wall mask: 2 x 2 cells per texel
inline constexpr int kThreads = 256;    // water_field.comp local size: a 16 x 16 tile, or kN / 2 butterflies per line
inline constexpr int kTile = 16;
inline constexpr size_t kMaxKicks = 4096;  // shaders/water_surface.glsl C3
inline constexpr size_t kMaxBodies = 64;
inline constexpr float kMetre = 290.0f;    // TRU units per metre (shaders/waves.glsl kM)
inline constexpr float kGravity = 9.81f;   // m/s^2
inline constexpr float kTension = 0.073f / 1025.0f; // sigma / rho, m^3/s^2 (as shaders/ocean.comp)
// Damping (amplitude e-folding rates, 1/s) [I]: a small base rate (real metre waves barely lose energy: a splash's rings
// spread for seconds; it keeps a pool from ringing forever), an effective viscosity on k^2 (short ripples die first, as
// surface films make them), a steep k^8 term at the grid scale (keeps the cell-sized noise out), the sponge at the
// window's outer kSpongeCells and the extra rate where the FFT ocean's mean square slope is high
// (kRough x s^2 / (s^2 + 0.01)).
inline constexpr float kBaseDamping = 0.08f, kViscosity = 3e-4f /* m^2/s */, kGridDamping = 6.0f;
inline constexpr float kSponge = 8.0f, kSpongeCells = 32.0f, kRough = 0.25f;
// The waves' slope limit: a crest steeper than Stokes' limiting wave (a 120-degree crest, slope tan 30 deg) breaks and
// loses the excess. Troughs (a crater's walls) and the bodies' dips (a swimmer's trough) are not limited.
inline constexpr float kSteepMax = 0.577f;

// The GPU buffers. Kicks: contract C3 (`uint count; uint pad[3]; vec4 kicks[4096]`), the same layout as the external
// kick buffer a renderer may be given (setExternalKicks). Frame: the bodies and the wall mask (bytes packed 4 per uint).
struct KickBuffer {
    uint32_t count = 0, pad[3] = {};
    WaveKick kicks[kMaxKicks];
};
struct FrameBuffer {
    uint32_t bodyCount = 0, pad[3] = {};
    WaveBody bodies[kMaxBodies];
    uint32_t mask[kMaskN * kMaskN / 4];
};
static_assert(sizeof(WaveKick) == 16 && sizeof(WaveBody) == 32);
static_assert(sizeof(KickBuffer) == 16 + 16 * kMaxKicks);

// water_field.comp's push constants (Vulkan) / uniforms 0..5 (GL).
struct Push {
    glm::ivec4 cfg;  // pass, window shift x, y (cells; new cell c reads old cell c + shift), flags (kReset, kOcean)
    glm::vec4 par;   // dt (s), depth (m), cell (m), steepness limit
    glm::vec4 win;   // origin x, y (world), cell (world units), units per metre
    glm::vec4 phys;  // g (m/s^2), sigma / rho (m^3/s^2), sponge rate (1/s), rough-sea rate (1/s)
    glm::vec4 damp;  // base rate (1/s), viscosity (m^2/s), grid-scale rate (1/s), sponge width (cells)
    glm::vec4 ocean; // the FFT ocean's cascade tiles (world units) and the uOcean level to read (WaterBlock::ocean)
};
static_assert(sizeof(Push) <= 128);
inline constexpr int kReset = 1, kOcean = 2;
enum Pass { Prep = 0, RowsForward = 1, ColumnsForward = 2, Propagate = 3, RowsInverse = 4, ColumnsInverse = 5, Finalize = 6 };
inline constexpr Pass kPasses[] = {Prep, RowsForward, ColumnsForward, Propagate, RowsInverse, ColumnsInverse, Finalize};
// Workgroups of a pass: one per line for the FFTs, one per kTile^2 tile (cell passes) or per kThreads wave numbers.
inline constexpr uint32_t groups(Pass p) {
    return p == RowsForward || p == ColumnsForward || p == RowsInverse || p == ColumnsInverse ? kN : kN * kN / kThreads;
}

// The dispersion relation (rad/s) of wave number k (rad/m) on water `depth` metres deep, and a mode's damping rate.
float omega(float k, float depth);
float damping(float k, float cellMetres);

// Whether the frame runs the field: Enhanced water and an active field.
bool wanted(const FrameOptions& o);
// Whether a mesh bounded by the world sphere (center, radius) reaches into the frame's field window: the renderers draw
// only those water meshes through the tessellated pipeline (water.tesc; elsewhere its factors are 1 anyway).
bool touches(const FrameOptions& o, glm::vec3 center, float radius);

// Per-renderer frame bookkeeping (CPU, deterministic): window scrolling, restarts and the time step, plus the input
// buffers' contents. frame() returns the frame's Push (cfg.x set per dispatch by the caller) or nullopt when the field
// rests (its texture then is not drawn).
class Stepper {
public:
    std::optional<Push> frame(const FrameOptions& o, KickBuffer& kicks, FrameBuffer& bodies);
    glm::ivec2 corner() const { return corner_; } // window corner (cells) of the last frame

private:
    bool active_ = false;
    uint64_t generation_ = 0;
    glm::ivec2 corner_{0};
    float cell_ = 0, time_ = 0;
};

// GL source (Vulkan compiles the same file with glslc -DVULKAN).
std::string computeSource();

} // namespace ou::render::waterfield

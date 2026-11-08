// Enhanced water's open-sea FFT ocean (docs/RENDERING.md "Water" / "FFT ocean"): three Tessendorf cascades evolved
// and inverse-transformed on the GPU every frame (shaders/ocean.comp, both backends), sampled by shaders/waves.glsl so
// water.vert, water.frag and the caustics share one surface. Spectra, cascade tiles and constants after
// NeuralSpecOcean's SpectralWaves.js / FoamField.js (MIT, (c) 2026 bgbg108555 and NeuralSpecOcean Contributors;
// docs/RESEARCH.md "FFT ocean"). Visual only: gameplay keeps tru.exe's sea waves (game::waveRegisters).
#pragma once

#include "render/Renderer.hpp"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace ou::render::ocean {

inline constexpr int kCascades = 3;     // swell 240 m, gravity 28 m, chop 11.3 m
inline constexpr int kMaxN = 128;       // grid of water detail high (mip 0); standard runs 64 (mip 1)
inline constexpr int kLevels = 8;       // uOcean's mips, 128^2 .. 1^2: each frame's grid filtered down (Pass Mip)
inline constexpr float kMaxAniso = 8.0f;  // uOcean's anisotropic filtering cap (16x cost 0.07 ms more at 1080p for little gain)
inline constexpr int kLayers = 3 * kCascades; // per cascade: (Dx, Dy, h, Jxy), (sx, sy, Jxx, Jyy), (foam, J, 0, 0)
inline constexpr float kLoopSeconds = 512.0f; // angular frequencies are multiples of 2 pi / this: time wraps exactly
inline constexpr int kThreads = 64;     // ocean.comp local size (= kMaxN / 2 butterflies)
inline constexpr float kMetre = 290.0f; // TRU units per metre (shaders/waves.glsl kM)

// The GPU input: h0(k) and conj(h0(-k)) per texel (metres), cascade after cascade, the 128 grid then the 64 grid.
std::vector<glm::vec4> spectrumBuffer();
size_t spectrumOffset(int n); // vec4 index of grid n's first cascade in spectrumBuffer()
inline constexpr size_t kWorkVec4s = size_t(kCascades) * kMaxN * kMaxN * 2; // the FFT scratch (4 complex per texel)

// Per-grid constants derived from the spectra (Parseval sums, as SpectralWaves.js / demo.js syncBound).
struct Grid {
    float length[kCascades];    // tile, metres
    float choppiness[kCascades]; // horizontal displacement scale (lambda); chop: shading and folds only
    float amplitude[kCascades];  // height / slope scale
    float sigma[kCascades];      // height RMS after amplitude, metres
};
const Grid& grid(int n);

// ocean.comp's push constants (Vulkan) / uniforms 0..2 (GL).
struct Push {
    glm::ivec4 cfg; // pass (0 spectrum, 1 rows, 2 columns, 3 assemble, 4 mip, 5 up), N, log2 N, cascades
    glm::vec4 par;  // loop time (s), foam dt (s, 0 = restart the foam), foam decay factor, foam source gain
    glm::vec4 cas[kCascades]; // tile metres, choppiness, amplitude, height sigma
};
// After Assemble: grid 64 doubles into level 0 (Up: binding 2 = level 1, 3 = level 0), then each level from the grid's
// own (gridLod) down is the 2x2 mean of the one above (Mip: binding 2 = level l, 3 = level l + 1, N = kMaxN >> (l + 1)).
enum Pass { Spectrum = 0, Rows = 1, Columns = 2, Assemble = 3, Mip = 4, Up = 5 };

// Whether the frame draws the FFT ocean: Enhanced water, FrameOptions::oceanFft and a waved (tru.exe sea) volume.
bool wanted(const FrameOptions& o);
inline int gridSize(const FrameOptions& o) { return o.waterDetail ? 128 : 64; }
inline int gridLod(int n) { return n == kMaxN ? 0 : 1; } // the mip level grid n lives in

// Game-time clock of one renderer: the frame's loop time and foam step (cfg.x = Spectrum; the caller sets cfg.x per
// dispatch). A step back in time, a jump over a second (load, cut) or a grid change restarts the foam (par.y < 0); a
// repeated time (pause) holds it. So a given frame sequence always gives the same image.
struct Clock {
    double last = -1.0;
    int lastN = 0;
    Push frame(const FrameOptions& o);
};

// GL source (Vulkan compiles the same file with glslc -DVULKAN).
std::string computeSource();

} // namespace ou::render::ocean

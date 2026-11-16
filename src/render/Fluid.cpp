// See Fluid.hpp. Kernels as Mueller et al. 2003 ("Particle-Based Fluid Simulation for Interactive Applications": poly6
// density, spiky gradient) in units of the support h, as shaders/fluid.comp evaluates them.
#include "render/Fluid.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace ou::render::fluid {

namespace {
const char* kFluidComp =
#include "fluid.comp.inc"
    ;
const char* kFluidGlsl =
#include "fluid.glsl.inc"
    ;
const char* kWaterSurface =
#include "water_surface.glsl.inc"
    ;
const char* kWaves =
#include "waves.glsl.inc"
    ;
constexpr float kPi = 3.14159265358979f;
float poly6(float r2) { return r2 < 1.0f ? 315.0f / (64.0f * kPi) * (1.0f - r2) * (1.0f - r2) * (1.0f - r2) : 0.0f; }
float spikyGrad(float r) { return r < 1.0f ? 45.0f / kPi * (1.0f - r) * (1.0f - r) : 0.0f; } // |grad W|
constexpr float kLattice = kSpacing / kSupport;

// [I] tuned by eye on l4_pool: the relaxation as a fraction of the lattice denominator (Macklin & Mueller's eps 600
// is ~0.6 of theirs), the tensile term (s_corr, n = 4, dq = 0.2 h) moving a pair at half the support 1 % of h per
// iteration, XSPH c 0.02, cohesion (Akinci et al. 2013 kernel) of ~3000 units/s^2 per unit of kernel sum.
constexpr float kRelax = 0.5f, kTensileMove = 0.01f, kTensileDq = 0.2f, kXsph = 0.02f, kCohesion = 3000.0f;
} // namespace

float restDensity() {
    float rho = 0;
    for (int z = -2; z <= 2; ++z)
        for (int y = -2; y <= 2; ++y)
            for (int x = -2; x <= 2; ++x) rho += poly6(kLattice * kLattice * float(x * x + y * y + z * z));
    return rho;
}

float latticeGradient() {
    const float rho0 = restDensity();
    float sum = 0;
    glm::vec3 self(0);
    for (int z = -2; z <= 2; ++z)
        for (int y = -2; y <= 2; ++y)
            for (int x = -2; x <= 2; ++x) {
                const glm::vec3 r = kLattice * glm::vec3(x, y, z);
                const float l = glm::length(r);
                if (l <= 0 || l >= 1) continue;
                const glm::vec3 g = -spikyGrad(l) / rho0 * (r / l);
                sum += glm::dot(g, g);
                self += g;
            }
    return sum + glm::dot(self, self);
}

Frame Sim::frame(const FrameOptions& o, glm::vec3 eye) {
    Frame f;
    const double t = o.time;
    if (last_ < 0 || t < last_ || t - last_ > 1.0) { // first frame, a load, a cut: start over
        f.clear = aliveUntil_ >= 0 || stale_;
        aliveUntil_ = -1.0, acc_ = 0.0, stale_ = false;
    } else {
        acc_ += t - last_;
    }
    last_ = t;
    for (const FluidBurst& b : o.fluidBursts) {
        if (int(f.bursts.size()) >= kMaxBursts || f.emitted >= kMaxPerFrame) break;
        const glm::vec3 c(b.pos[0], b.pos[1], b.pos[2]);
        const float d = glm::distance(c, eye);
        if (d > kRange || b.count == 0) continue;
        // far splashes are small on screen: fewer, (the same burst shape) down to a quarter at the range
        const float scale = std::clamp((kRange - d) / (0.5f * kRange), 0.25f, 1.0f);
        const int n = std::min(int(std::lround(float(b.count) * scale)), kMaxPerFrame - f.emitted);
        if (n <= 0) continue;
        if (stale_ && aliveUntil_ < 0) f.clear = true, stale_ = false;
        GpuBurst g;
        g.pos = {c, b.surfaceZ};
        g.vel = {b.carry[0], b.carry[1], b.carry[2], b.radius};
        g.shape = {b.speed, b.up, b.spread, float(b.kind)};
        g.info = {cursor_, uint32_t(n), b.seed, std::bit_cast<uint32_t>(b.restZ)};
        f.bursts.push_back(g);
        cursor_ = (cursor_ + uint32_t(n)) % uint32_t(kCapacity);
        f.emitted += n;
        aliveUntil_ = t + kMaxLife;
    }
    f.steps = std::min(int(acc_ / kStep), kMaxSteps);
    acc_ = std::min(acc_ - f.steps * double(kStep), double(kStep)); // past the cap: dropped
    f.active = aliveUntil_ >= 0 && t <= aliveUntil_;
    if (aliveUntil_ >= 0 && !f.active) aliveUntil_ = -1.0, stale_ = true; // dropped steps may leave some alive
    return f;
}

Push Sim::push(Pass pass, const Frame& f, int source) const {
    static const float rho0 = restDensity(), denom = latticeGradient();
    static const float tensileK = [] {
        const float scorr = kTensileMove / (spikyGrad(0.5f) / restDensity());
        return scorr / std::pow(poly6(0.25f) / poly6(kTensileDq * kTensileDq), 4.0f);
    }();
    Push p;
    p.cfg = {int(pass), source, int(f.bursts.size()), f.emitted};
    p.sim = {kStep, kSupport, rho0, kRelax * denom};
    p.par = {kGravity, tensileK, poly6(kTensileDq * kTensileDq), kXsph};
    p.lim = {float(kCapacity), kKickAmplitude, kKickSigma, kCohesion};
    return p;
}

std::string computeSource() {
    std::string s = kFluidComp;
    const std::pair<const char*, const char*> files[] = {{"#include \"shaders/fluid.glsl\"", kFluidGlsl},
                                                         {"#include \"shaders/water_surface.glsl\"", kWaterSurface},
                                                         {"#include \"shaders/waves.glsl\"", kWaves}};
    for (const auto& [line, text] : files)
        for (size_t at; (at = s.find(line)) != std::string::npos;) s.replace(at, std::strlen(line), text);
    return s;
}

} // namespace ou::render::fluid

// See Ocean.hpp. The spectra are NeuralSpecOcean's SpectralWaves.js (MIT, (c) 2026 bgbg108555 and NeuralSpecOcean
// Contributors), ported: its seeded LCG + Box-Muller draws, the 'long' / 'short' Mitsuyasu-spread wind spectra with
// the geometry-spacing filter, the 'chop' Mitsuyasu-Donelan-Banner spectrum calibrated to RMS slope 0.205, the
// Parseval displacement-gradient bound that caps the choppiness (demo.js syncBound) and its cascade set (createSpectral
// Waves calls in demo.js). Its CPU FFT is replaced by ours on the GPU (shaders/ocean.comp).
#include "render/Ocean.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace ou::render::ocean {

namespace {
const char* kOceanComp =
#include "ocean.comp.inc"
    ;

enum class Band { Long, Short, Chop };
struct BandSpec {
    Band band;
    double length, wind;
    uint32_t seed;
    double spacing; // geometrySpacing: wavelengths under ~4 x it are filtered (mesh resolution)
};
// demo.js: long 128^2 @ 240 m wind 16 seed 84, short 64^2 @ 28 m wind 11 seed 601, chop 64^2 @ 11.3 m wind 12.5
// seed 1907; here every cascade runs the frame's grid (128 high, 64 standard).
constexpr BandSpec kBands[kCascades] = {{Band::Long, 240.0, 16.0, 84, 350.0 / 320.0},
                                        {Band::Short, 28.0, 11.0, 601, 350.0 / 320.0},
                                        {Band::Chop, 11.3, 12.5, 1907, 0.0}};
constexpr double kAmplitude = 0.64;   // demo.js goalAmplitude (long + short)
constexpr double kRequestedChop = 2.24; // demo.js requestedChop
constexpr double kChopAmplitude = 0.6;  // [I] the chop band's slopes (RMS 0.205 calibrated) toned down by look
constexpr double kPi = 3.14159265358979323846;

struct BandResult {
    std::vector<double> re, im; // h0, row-major, wrapped frequencies
    double jacobianVar = 0;     // Parseval variance of the four displacement gradients (long / short)
    double heightVar = 0;       // sum 2 |h0|^2: the height variance
};

BandResult makeBand(int n, const BandSpec& b) {
    const int N = n * n;
    BandResult r;
    r.re.assign(size_t(N), 0.0), r.im.assign(size_t(N), 0.0);
    uint32_t state = b.seed;
    auto rand = [&] {
        state = 1664525u * state + 1013904223u;
        return (double(state) + 1.0) / 4294967297.0;
    };
    auto gaussian = [&] {
        const double u = rand(), v = rand();
        return std::sqrt(-2.0 * std::log(u)) * std::cos(2.0 * kPi * v);
    };
    const double g = 9.81, L = b.wind * b.wind / g;
    const bool chop = b.band == Band::Chop;
    double chopSlopeVar = 0;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const size_t i = size_t(y * n + x);
            const double kx = double(x <= n / 2 ? x : x - n) * 2.0 * kPi / b.length;
            const double kz = double(y <= n / 2 ? y : y - n) * 2.0 * kPi / b.length;
            const double k = std::hypot(kx, kz);
            if (!(k > 0)) continue;
            double a;
            if (chop) {
                const double cMain = (0.3300 * kx + 0.9440 * kz) / k, cSkew = (-0.2800 * kx + 0.9600 * kz) / k;
                const double cBroad = (0.7600 * kx + 0.6499 * kz) / k, halfMain = 0.5 + 0.5 * cMain;
                const double broadDir = 0.26 + 0.48 * halfMain * halfMain + 0.20 * cMain * cMain +
                                        0.14 * std::max(0.0, cSkew) * std::max(0.0, cSkew) +
                                        0.12 * std::max(0.0, cBroad) * std::max(0.0, cBroad);
                const double kNyq = kPi * n / b.length;
                const double u = std::clamp((kNyq * 0.92 - k) / (kNyq * 0.30), 0.0, 1.0);
                const double taper = u * u * (3 - 2 * u);
                const double ph = std::exp(-0.26 / (k * k)) * broadDir * taper * std::exp(-k * k * 0.0011) / (k * k * k * k);
                a = std::sqrt(ph * 0.5);
            } else {
                const double cMain = (0.35 * kx + 0.93675 * kz) / k, cSub = (-0.31 * kx + 0.95074 * kz) / k;
                const double cObl = (0.82 * kx + 0.5724 * kz) / k;
                const double spread = b.band == Band::Short
                                          ? 0.24 + 0.72 * cMain * cMain + 0.26 * cSub * cSub + 0.18 * cObl * cObl
                                          : 0.11 + 1.06 * std::pow(std::abs(cMain), 4) + 0.38 * std::pow(std::abs(cSub), 4) +
                                                0.18 * cObl * cObl;
                const double ph = std::exp(-1.0 / (k * k * L * L)) * spread * std::exp(-k * k * 0.08) / (k * k * k * k);
                double blend = std::clamp((k - 0.3) / 0.4, 0.0, 1.0);
                blend = blend * blend * (3 - 2 * blend);
                const double weight = b.band == Band::Long ? 1 - blend : blend;
                const double wavelength = 2 * kPi / k;
                double f = b.spacing > 0 ? std::clamp((wavelength - 4 * b.spacing) / (4 * b.spacing), 0.0, 1.0) : 1.0;
                f = f * f * (3 - 2 * f);
                a = std::sqrt(ph * weight * f / 2) * (b.band == Band::Short ? 0.0024 : 0.00032);
            }
            const double gr = gaussian() * a;
            const double gi = gaussian() * a;
            // Nyquist row / column: no derivative there (SpectralWaves.js zeroes its slope factors); dropped here.
            if (x == n / 2 || y == n / 2) continue;
            r.re[i] = gr, r.im[i] = gi;
            const double amp2 = 2.0 * (gr * gr + gi * gi);
            if (chop) {
                chopSlopeVar += amp2 * (kx * kx + kz * kz);
            } else {
                const double fx = kx / k, fz = kz / k;
                const double xx = fx * kx, xz = fx * kz, zx = fz * kx, zz = fz * kz;
                r.jacobianVar += amp2 * (xx * xx + xz * xz + zx * zx + zz * zz);
            }
        }
    if (chop && chopSlopeVar > 1e-12) { // calibrated RMS slope 0.205
        const double s = 0.205 / std::sqrt(chopSlopeVar);
        for (size_t i = 0; i < r.re.size(); ++i) r.re[i] *= s, r.im[i] *= s;
    }
    for (size_t i = 0; i < r.re.size(); ++i) r.heightVar += 2.0 * (r.re[i] * r.re[i] + r.im[i] * r.im[i]);
    return r;
}

struct Built {
    std::vector<glm::vec4> buffer;
    Grid grids[2]; // 128, 64
};

const Built& built() {
    static const Built b = [] {
        Built out;
        for (int gi = 0; gi < 2; ++gi) {
            const int n = gi == 0 ? 128 : 64;
            Grid& grid = out.grids[gi];
            double bound = 0;
            BandResult bands[kCascades];
            for (int c = 0; c < kCascades; ++c) {
                bands[c] = makeBand(n, kBands[c]);
                if (kBands[c].band != Band::Chop) bound += 3.15 * std::sqrt(bands[c].jacobianVar);
            }
            const double lambda = std::min(kRequestedChop, 0.80 / std::max(1e-8, kAmplitude * bound)); // demo.js syncBound
            for (int c = 0; c < kCascades; ++c) {
                const bool chop = kBands[c].band == Band::Chop;
                const double amp = chop ? kChopAmplitude : kAmplitude;
                grid.length[c] = float(kBands[c].length);
                grid.choppiness[c] = chop ? 0.0f : float(lambda);
                grid.amplitude[c] = float(amp);
                grid.sigma[c] = float(amp * std::sqrt(bands[c].heightVar));
                const auto& re = bands[c].re;
                const auto& im = bands[c].im;
                for (int y = 0; y < n; ++y)
                    for (int x = 0; x < n; ++x) {
                        const size_t i = size_t(y * n + x), j = size_t(((n - y) % n) * n + (n - x) % n);
                        out.buffer.push_back(glm::vec4(float(re[i]), float(im[i]), float(re[j]), float(-im[j])));
                    }
            }
        }
        return out;
    }();
    return b;
}
} // namespace

std::vector<glm::vec4> spectrumBuffer() { return built().buffer; }

size_t spectrumOffset(int n) { return n == kMaxN ? 0 : size_t(kCascades) * kMaxN * kMaxN; }

const Grid& grid(int n) { return built().grids[n == kMaxN ? 0 : 1]; }

bool wanted(const FrameOptions& o) {
    if (o.water != WaterQuality::Enhanced || !o.oceanFft) return false;
    return std::any_of(o.waterVolumes.begin(), o.waterVolumes.end(), [](const WaterVolume& v) {
        return v.waves[3][0] != 0 || v.waves[3][1] != 0 || v.waves[3][2] != 0 || v.waves[3][3] != 0;
    });
}

Push Clock::frame(const FrameOptions& o) {
    const int n = gridSize(o);
    const Grid& g = grid(n);
    const double t = double(o.time), dt = t - last;
    float foamDt = 0.0f;
    if (last < 0 || dt < 0 || dt > 1.0 || n != lastN) foamDt = -1.0f; // restart
    else foamDt = float(std::min(dt, 0.2));                            // FoamField's dt clamp
    last = t, lastN = n;
    double loop = std::fmod(t, double(kLoopSeconds));
    if (loop < 0) loop += kLoopSeconds;
    Push p{};
    p.cfg = {Spectrum, n, n == 128 ? 7 : 6, kCascades};
    // FoamField lifetime 3 s; source max(0, 0.92 - J) x 4.8 (demo.js evalOffshoreFoamCell)
    p.par = {float(loop), foamDt, foamDt > 0 ? float(std::exp(-double(foamDt) / 3.0)) : 1.0f, 4.8f};
    for (int c = 0; c < kCascades; ++c) p.cas[c] = {g.length[c], g.choppiness[c], g.amplitude[c], g.sigma[c]};
    return p;
}

std::string computeSource() { return kOceanComp; }

} // namespace ou::render::ocean
